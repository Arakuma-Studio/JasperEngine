#include "StyleSheetFactory.h"
#include "../../Include/RmlUi/Core/Log.h"
#include "../../Include/RmlUi/Core/StyleSheetContainer.h"
#include "StreamFile.h"
#include "StyleSheetNode.h"
#include "StyleSheetParser.h"
#include "StyleSheetSelector.h"
#include <limits.h>

namespace Rml {

static UniquePtr<StyleSheetFactory> instance;

StyleSheetFactory::StyleSheetFactory() :
	selectors{
		{"nth-child", StructuralSelectorType::Nth_Child},
		{"nth-last-child", StructuralSelectorType::Nth_Last_Child},
		{"nth-of-type", StructuralSelectorType::Nth_Of_Type},
		{"nth-last-of-type", StructuralSelectorType::Nth_Last_Of_Type},
		{"first-child", StructuralSelectorType::First_Child},
		{"last-child", StructuralSelectorType::Last_Child},
		{"first-of-type", StructuralSelectorType::First_Of_Type},
		{"last-of-type", StructuralSelectorType::Last_Of_Type},
		{"only-child", StructuralSelectorType::Only_Child},
		{"only-of-type", StructuralSelectorType::Only_Of_Type},
		{"empty", StructuralSelectorType::Empty},
		{"not", StructuralSelectorType::Not},
		{"scope", StructuralSelectorType::Scope},
		// Phase 3, cascade (docs/html-ui/phase-3-fork-patches.md 3.5)
		{"root", StructuralSelectorType::Root},
		{"is", StructuralSelectorType::Is},
		{"matches", StructuralSelectorType::Is},
		{"-webkit-any", StructuralSelectorType::Is},
		{"-moz-any", StructuralSelectorType::Is},
		{"where", StructuralSelectorType::Where},
		{"has", StructuralSelectorType::Has},
		{"enabled", StructuralSelectorType::Enabled},
		{"disabled", StructuralSelectorType::Disabled},
		{"link", StructuralSelectorType::AnyLink},
		{"any-link", StructuralSelectorType::AnyLink},
		{"-webkit-any-link", StructuralSelectorType::AnyLink},
		{"visited", StructuralSelectorType::Never},
		{"read-only", StructuralSelectorType::ReadOnly},
		{"read-write", StructuralSelectorType::ReadWrite},
		{"required", StructuralSelectorType::Required},
		{"optional", StructuralSelectorType::Optional},
		{"indeterminate", StructuralSelectorType::Indeterminate},
		{"default", StructuralSelectorType::Default},
		{"target", StructuralSelectorType::Target},
	},
	pseudo_classes{"hover", "active", "focus", "focus-visible", "focus-within", "checked", "selected", "placeholder-shown", "drag",
		"-rmlui-starting-style"}
{}

StyleSheetFactory::~StyleSheetFactory() {}

bool StyleSheetFactory::Initialise()
{
	RMLUI_ASSERT(instance == nullptr);
	instance = UniquePtr<StyleSheetFactory>(new StyleSheetFactory);
	return true;
}

void StyleSheetFactory::Shutdown()
{
	instance.reset();
}

const StyleSheetContainer* StyleSheetFactory::GetStyleSheetContainer(const String& sheet_name)
{
	// Look up the sheet definition in the cache
	auto it = instance->stylesheets.find(sheet_name);
	if (it != instance->stylesheets.end())
		return it->second.get();

	// Don't currently have the sheet, attempt to load it
	UniquePtr<const StyleSheetContainer> sheet = instance->LoadStyleSheetContainer(sheet_name);
	if (!sheet)
		return nullptr;

	const StyleSheetContainer* result = sheet.get();

	// Add it to the cache.
	instance->stylesheets[sheet_name] = std::move(sheet);

	return result;
}

void StyleSheetFactory::ClearStyleSheetCache()
{
	instance->stylesheets.clear();
}

bool StyleSheetFactory::IsPseudoClass(const String& name)
{
	return instance->pseudo_classes.count(name) != 0;
}

void StyleSheetFactory::RegisterPseudoClass(const String& name)
{
	instance->pseudo_classes.insert(StringUtilities::ToLower(name));
}

bool StyleSheetFactory::ParseAnB(const String& text, int& a, int& b)
{
	const String s = StringUtilities::ToLower(StringUtilities::StripWhitespace(text));
	if (s == "odd")
	{
		a = 2;
		b = 1;
		return true;
	}
	if (s == "even")
	{
		a = 2;
		b = 0;
		return true;
	}

	size_t i = 0;
	auto ReadInteger = [&](int& value) {
		const size_t begin = i;
		long long result = 0;
		while (i < s.size() && s[i] >= '0' && s[i] <= '9')
		{
			result = Math::Min(result * 10 + (s[i] - '0'), (long long)INT_MAX);
			i++;
		}
		value = (int)result;
		return i > begin;
	};
	auto SkipWhitespace = [&]() {
		while (i < s.size() && StringUtilities::IsWhitespace(s[i]))
			i++;
	};

	int sign = 1;
	if (i < s.size() && (s[i] == '+' || s[i] == '-'))
	{
		sign = (s[i] == '-' ? -1 : 1);
		i++;
	}
	int number = 0;
	const bool has_number = ReadInteger(number);
	if (i >= s.size() || s[i] != 'n')
	{
		// B alone
		if (!has_number || i != s.size())
			return false;
		a = 0;
		b = sign * number;
		return true;
	}

	a = sign * (has_number ? number : 1);
	b = 0;
	i++; // 'n'
	SkipWhitespace();
	if (i == s.size())
		return true;
	if (s[i] != '+' && s[i] != '-')
		return false;
	const int b_sign = (s[i] == '-' ? -1 : 1);
	i++;
	SkipWhitespace();
	int b_number = 0;
	if (!ReadInteger(b_number) || i != s.size())
		return false;
	b = b_sign * b_number;
	return true;
}

// The selector list of a functional pseudo-class argument, in a tree of its own. 'forgiving': an invalid selector of
// the list is left out instead of invalidating the list (Selectors 4, :is() and :where()).
static SharedPtr<SelectorTree> MakeSelectorTree(const String& selectors, bool forgiving, int& specificity)
{
	auto tree = MakeShared<SelectorTree>();
	tree->root = MakeUnique<StyleSheetNode>();

	StringList selector_list;
	StringUtilities::ExpandString(selector_list, selectors, ',', '(', ')');
	specificity = 0;
	for (const String& selector : selector_list)
	{
		StyleSheetNode* leaf = StyleSheetParser::ConstructNode(*tree->root, selector);
		if (!leaf || leaf == tree->root.get())
		{
			if (forgiving)
				continue;
			return nullptr;
		}
		tree->leafs.push_back(leaf);
		specificity = Math::Max(specificity, leaf->GetSpecificity());
	}
	if (tree->leafs.empty() && !forgiving)
		return nullptr;
	return tree;
}

StructuralSelector StyleSheetFactory::GetSelector(const String& name)
{
	SelectorMap::const_iterator it;
	const size_t parameter_start = name.find('(');

	if (parameter_start == String::npos)
		it = instance->selectors.find(name);
	else
		it = instance->selectors.find(name.substr(0, parameter_start));

	const StructuralSelector invalid(StructuralSelectorType::Invalid, 0, 0);
	if (it == instance->selectors.end())
		return invalid;

	const StructuralSelectorType selector_type = it->second;

	bool requires_parameter = false;
	switch (selector_type)
	{
	case StructuralSelectorType::Nth_Child:
	case StructuralSelectorType::Nth_Last_Child:
	case StructuralSelectorType::Nth_Of_Type:
	case StructuralSelectorType::Nth_Last_Of_Type:
	case StructuralSelectorType::Not:
	case StructuralSelectorType::Is:
	case StructuralSelectorType::Where:
	case StructuralSelectorType::Has: requires_parameter = true; break;
	default: break;
	}

	const size_t parameter_end = name.rfind(')');
	const bool has_parameter = (parameter_start != String::npos && parameter_end != String::npos && parameter_start < parameter_end);

	if (requires_parameter != has_parameter)
	{
		Log::Message(Log::LT_WARNING, "Invalid selector ':%s' encountered, expected %s parameters", name.c_str(),
			requires_parameter ? "parenthesized" : "no");
		return invalid;
	}

	if (!has_parameter)
		return StructuralSelector(selector_type, 1, 0);

	const String parameters = StringUtilities::StripWhitespace(StringView(name, parameter_start + 1, parameter_end - (parameter_start + 1)));

	switch (selector_type)
	{
	case StructuralSelectorType::Not:
	case StructuralSelectorType::Is:
	case StructuralSelectorType::Where:
	{
		// Forgiving selector lists, except for :not() (Selectors 4).
		int specificity = 0;
		SharedPtr<SelectorTree> tree = MakeSelectorTree(parameters, selector_type != StructuralSelectorType::Not, specificity);
		if (!tree)
			return invalid;
		if (selector_type == StructuralSelectorType::Where)
			specificity = 0;
		return StructuralSelector(selector_type, std::move(tree), specificity);
	}
	case StructuralSelectorType::Has:
	{
		// Relative selectors: each argument is matched relative to the subject (':scope' in front), and :has() does not
		// nest.
		const String lower = StringUtilities::ToLower(parameters);
		if (lower.find(":has(") != String::npos)
			return invalid;

		StringList relative_list;
		StringUtilities::ExpandString(relative_list, parameters, ',', '(', ')');
		String scoped;
		Vector<bool> sibling_relative;
		for (const String& relative : relative_list)
		{
			const String trimmed = StringUtilities::StripWhitespace(relative);
			if (trimmed.empty())
				return invalid;
			if (!scoped.empty())
				scoped += ", ";
			scoped += ":scope " + trimmed;
			sibling_relative.push_back(trimmed[0] == '+' || trimmed[0] == '~');
		}
		int specificity = 0;
		SharedPtr<SelectorTree> tree = MakeSelectorTree(scoped, false, specificity);
		if (!tree || tree->leafs.size() != sibling_relative.size())
			return invalid;
		tree->sibling_relative = std::move(sibling_relative);
		// The prepended ':scope' does not count.
		return StructuralSelector(selector_type, std::move(tree), Math::Max(0, specificity - (int)SelectorSpecificity::PseudoClass));
	}
	default: break;
	}

	// ':nth-child(An+B [of S])', ':nth-last-child(An+B [of S])', ':nth-of-type(An+B)', ':nth-last-of-type(An+B)'.
	String an_b = parameters;
	SharedPtr<SelectorTree> of_tree;
	int of_specificity = 0;
	if (selector_type == StructuralSelectorType::Nth_Child || selector_type == StructuralSelectorType::Nth_Last_Child)
	{
		const String lower = StringUtilities::ToLower(parameters);
		for (size_t i = lower.find("of"); i != String::npos; i = lower.find("of", i + 1))
		{
			const bool word_begin = (i > 0 && StringUtilities::IsWhitespace(lower[i - 1]));
			const bool word_end = (i + 2 < lower.size() && StringUtilities::IsWhitespace(lower[i + 2]));
			if (!word_begin || !word_end)
				continue;
			an_b = parameters.substr(0, i);
			of_tree = MakeSelectorTree(StringUtilities::StripWhitespace(parameters.substr(i + 2)), false, of_specificity);
			if (!of_tree)
				return invalid;
			break;
		}
	}

	int a = 0, b = 0;
	if (!ParseAnB(an_b, a, b))
	{
		Log::Message(Log::LT_WARNING, "Invalid selector ':%s' encountered, the argument is not of the form An+B", name.c_str());
		return invalid;
	}

	if (of_tree)
	{
		StructuralSelector result(selector_type, std::move(of_tree), (int)SelectorSpecificity::PseudoClass + of_specificity);
		result.a = a;
		result.b = b;
		return result;
	}
	return StructuralSelector(selector_type, a, b);
}

UniquePtr<const StyleSheetContainer> StyleSheetFactory::LoadStyleSheetContainer(const String& sheet)
{
	UniquePtr<StyleSheetContainer> new_style_sheet;

	// Open stream, construct new sheet and pass the stream into the sheet
	auto stream = MakeUnique<StreamFile>();
	if (stream->Open(sheet))
	{
		new_style_sheet = MakeUnique<StyleSheetContainer>();
		if (!new_style_sheet->LoadStyleSheetContainer(stream.get()))
		{
			new_style_sheet.reset();
		}
	}

	return new_style_sheet;
}

} // namespace Rml
