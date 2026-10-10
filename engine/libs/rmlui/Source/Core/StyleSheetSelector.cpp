#include "StyleSheetSelector.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "StyleSheetNode.h"
#include <tuple>

namespace Rml {

static inline bool IsTextElement(const Element* element)
{
	return element->GetTagName() == "#text";
}

// Returns true if a positive integer can be found for n in the equation an + b = count.
static bool IsNth(int a, int b, int count)
{
	int x = count;
	x -= b;
	if (a != 0)
		x /= a;

	return (x >= 0 && x * a + b == count);
}

bool operator==(const AttributeSelector& a, const AttributeSelector& b)
{
	return a.type == b.type && a.case_insensitive == b.case_insensitive && a.name == b.name && a.value == b.value;
}
bool operator<(const AttributeSelector& a, const AttributeSelector& b)
{
	return std::tie(a.type, a.case_insensitive, a.name, a.value) < std::tie(b.type, b.case_insensitive, b.name, b.value);
}

bool operator==(const StructuralSelector& a, const StructuralSelector& b)
{
	// Currently sub-selectors (selector_tree) are only superficially compared. This mainly has the consequence that selectors with a sub-selector
	// which are instantiated separately will never compare equal, even if they have the exact same sub-selector expression. This further results in
	// such selectors not being de-duplicated. This should not lead to any functional differences but leads to potentially missed memory/performance
	// optimizations. E.g. 'div a, div b' will combine the two div nodes, while ':not(div) a, :not(div) b' will not combine the two not-div nodes.
	return a.type == b.type && a.a == b.a && a.b == b.b && a.selector_tree == b.selector_tree;
}
bool operator<(const StructuralSelector& a, const StructuralSelector& b)
{
	return std::tie(a.type, a.a, a.b, a.selector_tree) < std::tie(b.type, b.a, b.b, b.selector_tree);
}

bool operator==(const CompoundSelector& a, const CompoundSelector& b)
{
	if (a.tag != b.tag)
		return false;
	if (a.id != b.id)
		return false;
	if (a.class_names != b.class_names)
		return false;
	if (a.pseudo_class_names != b.pseudo_class_names)
		return false;
	if (a.attributes != b.attributes)
		return false;
	if (a.structural_selectors != b.structural_selectors)
		return false;
	if (a.combinator != b.combinator)
		return false;
	if (a.pseudo_element != b.pseudo_element)
		return false;
	return true;
}

// True when any selector of the tree matches the element.
static bool MatchesSelectorTree(const Element* element, const SelectorTree& tree, const Element* scope)
{
	for (const StyleSheetNode* node : tree.leafs)
	{
		if (node->IsApplicable(element, scope))
			return true;
	}
	return false;
}

// Calls 'callback' for every element of the subtree below 'element' (DOM children only: pseudo-elements and widget
// parts are not part of the tree :has() looks at) until it returns true.
template <typename Callback>
static bool AnyDescendant(const Element* element, Callback&& callback)
{
	const int num_children = element->GetNumChildren();
	for (int i = 0; i < num_children; i++)
	{
		const Element* child = element->GetChild(i);
		if (IsTextElement(child))
			continue;
		if (callback(child) || AnyDescendant(child, callback))
			return true;
	}
	return false;
}

// :has(): a relative selector matches some element relative to the subject (its scope).
static bool MatchesRelativeSelectorTree(const Element* subject, const SelectorTree& tree)
{
	for (size_t i = 0; i < tree.leafs.size(); i++)
	{
		const StyleSheetNode* node = tree.leafs[i];
		auto matches = [node, subject](const Element* candidate) { return node->IsApplicable(candidate, subject); };
		const bool sibling = (i < tree.sibling_relative.size() && tree.sibling_relative[i]);
		if (!sibling)
		{
			if (AnyDescendant(subject, matches))
				return true;
			continue;
		}

		const Element* parent = subject->GetParentNode();
		if (!parent)
			continue;
		bool after_subject = false;
		const int num_children = parent->GetNumChildren();
		for (int j = 0; j < num_children; j++)
		{
			const Element* sibling_element = parent->GetChild(j);
			if (sibling_element == subject)
			{
				after_subject = true;
				continue;
			}
			if (!after_subject || IsTextElement(sibling_element))
				continue;
			if (matches(sibling_element) || AnyDescendant(sibling_element, matches))
				return true;
		}
	}
	return false;
}

// The elements :enabled and :disabled apply to (HTML's form controls, as the compatibility layer maps them).
static bool IsFormControl(const Element* element)
{
	const String& tag = element->GetTagName();
	return tag == "input" || tag == "textarea" || tag == "select" || tag == "button" || tag == "option" || tag == "optgroup" ||
		tag == "fieldset";
}

// Disabled by its own state or attribute, or by a disabled <fieldset> ancestor unless it is inside that fieldset's first
// <legend>; an <option> also by a disabled <optgroup> parent.
bool IsElementDisabled(const Element* element)
{
	if (element->IsPseudoClassSet("disabled"))
		return true;
	if (!IsFormControl(element))
		return false;
	if (element->HasAttribute("disabled"))
		return true;

	const Element* child = element;
	for (const Element* ancestor = element->GetParentNode(); ancestor; child = ancestor, ancestor = ancestor->GetParentNode())
	{
		const String& tag = ancestor->GetTagName();
		if (tag == "optgroup" && child == element && element->GetTagName() == "option" && ancestor->HasAttribute("disabled"))
			return true;
		if (tag != "fieldset" || !ancestor->HasAttribute("disabled"))
			continue;

		// The first legend child of the fieldset stays enabled.
		const Element* first_legend = nullptr;
		for (int i = 0; i < ancestor->GetNumChildren(); i++)
		{
			const Element* fieldset_child = ancestor->GetChild(i);
			if (fieldset_child->GetTagName() == "legend")
			{
				first_legend = fieldset_child;
				break;
			}
		}
		if (!first_legend || child != first_legend)
			return true;
	}
	return false;
}

// A text field the user can edit: :read-write; everything else is :read-only.
static bool IsReadWrite(const Element* element)
{
	const String& tag = element->GetTagName();
	if (tag == "textarea")
		return !element->HasAttribute("readonly") && !IsElementDisabled(element);
	if (tag != "input")
		return false;
	const String type = StringUtilities::ToLower(element->GetAttribute<String>("type", "text"));
	static const char* const text_types[] = {"text", "password", "search", "email", "url", "tel", "number", "date", "time",
		"datetime-local", "month", "week", ""};
	bool text = false;
	for (const char* text_type : text_types)
		text = text || (type == text_type);
	return text && !element->HasAttribute("readonly") && !IsElementDisabled(element);
}

// The elements :required and :optional apply to.
static bool IsRequirable(const Element* element)
{
	const String& tag = element->GetTagName();
	if (tag == "select" || tag == "textarea")
		return true;
	if (tag != "input")
		return false;
	const String type = StringUtilities::ToLower(element->GetAttribute<String>("type", "text"));
	return type != "hidden" && type != "range" && type != "color" && type != "submit" && type != "reset" && type != "button" && type != "image";
}

// Some radio button of the element's group (same name, same parent form or document) is checked.
static bool IsRadioGroupChecked(const Element* element)
{
	const String name = element->GetAttribute<String>("name", "");
	if (name.empty())
		return false;
	const Element* form = element->GetParentNode();
	while (form && form->GetTagName() != "form" && form->GetParentNode())
		form = form->GetParentNode();
	if (!form)
		return false;
	bool checked = false;
	AnyDescendant(form, [&](const Element* candidate) {
		if (candidate->GetTagName() == "input" && candidate->GetAttribute<String>("name", "") == name &&
			StringUtilities::ToLower(candidate->GetAttribute<String>("type", "")) == "radio" &&
			(candidate->HasAttribute("checked") || candidate->IsPseudoClassSet("checked")))
			checked = true;
		return checked;
	});
	return checked;
}

// :default: checkboxes and radio buttons checked by default, options selected by default, and a form's first submit
// button.
static bool IsDefault(const Element* element)
{
	const String& tag = element->GetTagName();
	if (tag == "option")
		return element->HasAttribute("selected");
	if (tag == "input")
	{
		const String type = StringUtilities::ToLower(element->GetAttribute<String>("type", ""));
		if (type == "checkbox" || type == "radio")
			return element->HasAttribute("checked");
		if (type != "submit" && type != "image")
			return false;
	}
	else if (tag == "button")
	{
		const String type = StringUtilities::ToLower(element->GetAttribute<String>("type", "submit"));
		if (type != "submit")
			return false;
	}
	else
	{
		return false;
	}

	// The first submit button of its form.
	const Element* form = element->GetParentNode();
	while (form && form->GetTagName() != "form")
		form = form->GetParentNode();
	if (!form)
		return false;
	const Element* first = nullptr;
	AnyDescendant(form, [&](const Element* candidate) {
		const String& candidate_tag = candidate->GetTagName();
		const String candidate_type = StringUtilities::ToLower(candidate->GetAttribute<String>("type", candidate_tag == "button" ? "submit" : ""));
		if ((candidate_tag == "button" && candidate_type == "submit") ||
			(candidate_tag == "input" && (candidate_type == "submit" || candidate_type == "image")))
			first = candidate;
		return first != nullptr;
	});
	return first == element;
}

bool IsSelectorApplicable(const Element* element, const StructuralSelector& selector, const Element* scope)
{
	RMLUI_ASSERT(element);

	switch (selector.type)
	{
	case StructuralSelectorType::Nth_Child:
	{
		Element* parent = element->GetParentNode();
		if (!parent)
			return false;

		// ':nth-child(An+B of S)' counts the siblings that match S, and the element must match it.
		if (selector.selector_tree && !MatchesSelectorTree(element, *selector.selector_tree, scope))
			return false;

		// Start counting elements until we find this one.
		int element_index = 1;
		for (int i = 0; i < parent->GetNumChildren(); i++)
		{
			Element* child = parent->GetChild(i);

			// Skip text nodes.
			if (IsTextElement(child))
				continue;
			if (child != element && selector.selector_tree && !MatchesSelectorTree(child, *selector.selector_tree, scope))
				continue;

			// If we've found our element, then break; the current index is our element's index.
			if (child == element)
				break;

			element_index++;
		}

		return IsNth(selector.a, selector.b, element_index);
	}
	break;
	case StructuralSelectorType::Nth_Last_Child:
	{
		Element* parent = element->GetParentNode();
		if (!parent)
			return false;

		if (selector.selector_tree && !MatchesSelectorTree(element, *selector.selector_tree, scope))
			return false;

		// Start counting elements until we find this one.
		int element_index = 1;
		for (int i = parent->GetNumChildren() - 1; i >= 0; --i)
		{
			Element* child = parent->GetChild(i);

			// Skip text nodes.
			if (IsTextElement(child))
				continue;
			if (child != element && selector.selector_tree && !MatchesSelectorTree(child, *selector.selector_tree, scope))
				continue;

			// If we've found our element, then break; the current index is our element's index.
			if (child == element)
				break;

			element_index++;
		}

		return IsNth(selector.a, selector.b, element_index);
	}
	break;
	case StructuralSelectorType::Nth_Of_Type:
	{
		Element* parent = element->GetParentNode();
		if (!parent)
			return false;

		// Start counting elements until we find this one.
		int element_index = 1;
		const int num_children = parent->GetNumChildren();
		for (int i = 0; i < num_children; i++)
		{
			Element* child = parent->GetChild(i);

			// If we've found our element, then break; the current index is our element's index.
			if (child == element)
				break;

			// Skip nodes that don't share our tag.
			if (child->GetTagName() != element->GetTagName())
				continue;

			element_index++;
		}

		return IsNth(selector.a, selector.b, element_index);
	}
	break;
	case StructuralSelectorType::Nth_Last_Of_Type:
	{
		Element* parent = element->GetParentNode();
		if (!parent)
			return false;

		// Start counting elements until we find this one.
		int element_index = 1;
		for (int i = parent->GetNumChildren() - 1; i >= 0; --i)
		{
			Element* child = parent->GetChild(i);

			// If we've found our element, then break; the current index is our element's index.
			if (child == element)
				break;

			// Skip nodes that don't share our tag.
			if (child->GetTagName() != element->GetTagName())
				continue;

			element_index++;
		}

		return IsNth(selector.a, selector.b, element_index);
	}
	break;
	case StructuralSelectorType::First_Child:
	{
		Element* parent = element->GetParentNode();
		if (!parent)
			return false;

		int child_index = 0;
		while (child_index < parent->GetNumChildren())
		{
			// If this child (the first non-text child) is our element, then the selector succeeds.
			Element* child = parent->GetChild(child_index);
			if (child == element)
				return true;

			// If this child is not a text element, then the selector fails; this element is non-trivial.
			if (!IsTextElement(child))
				return false;

			// Otherwise, skip over the text element to find the last non-trivial element.
			child_index++;
		}

		return false;
	}
	break;
	case StructuralSelectorType::Last_Child:
	{
		Element* parent = element->GetParentNode();
		if (!parent)
			return false;

		int child_index = parent->GetNumChildren() - 1;
		while (child_index >= 0)
		{
			// If this child (the last non-text child) is our element, then the selector succeeds.
			Element* child = parent->GetChild(child_index);
			if (child == element)
				return true;

			// If this child is not a text element, then the selector fails; this element is non-trivial.
			if (!IsTextElement(child))
				return false;

			// Otherwise, skip over the text element to find the last non-trivial element.
			child_index--;
		}

		return false;
	}
	break;
	case StructuralSelectorType::First_Of_Type:
	{
		Element* parent = element->GetParentNode();
		if (!parent)
			return false;

		int child_index = 0;
		while (child_index < parent->GetNumChildren())
		{
			// If this child is our element, then it's the first one we've found with our tag; the selector succeeds.
			Element* child = parent->GetChild(child_index);
			if (child == element)
				return true;

			// Otherwise, if this child shares our element's tag, then our element is not the first tagged child; the selector fails.
			if (child->GetTagName() == element->GetTagName())
				return false;

			child_index++;
		}

		return false;
	}
	break;
	case StructuralSelectorType::Last_Of_Type:
	{
		Element* parent = element->GetParentNode();
		if (!parent)
			return false;

		int child_index = parent->GetNumChildren() - 1;
		while (child_index >= 0)
		{
			// If this child is our element, then it's the first one we've found with our tag; the selector succeeds.
			Element* child = parent->GetChild(child_index);
			if (child == element)
				return true;

			// Otherwise, if this child shares our element's tag, then our element is not the first tagged child; the selector fails.
			if (child->GetTagName() == element->GetTagName())
				return false;

			child_index--;
		}

		return false;
	}
	break;
	case StructuralSelectorType::Only_Child:
	{
		Element* parent = element->GetParentNode();
		if (!parent)
			return false;

		const int num_children = parent->GetNumChildren();
		for (int i = 0; i < num_children; i++)
		{
			Element* child = parent->GetChild(i);

			// Skip the child if it is our element.
			if (child == element)
				continue;

			// Skip the child if it is trivial.
			if (IsTextElement(child))
				continue;

			return false;
		}

		return true;
	}
	break;
	case StructuralSelectorType::Only_Of_Type:
	{
		Element* parent = element->GetParentNode();
		if (!parent)
			return false;

		const int num_children = parent->GetNumChildren();
		for (int i = 0; i < num_children; i++)
		{
			Element* child = parent->GetChild(i);

			// Skip the child if it is our element.
			if (child == element)
				continue;

			// Skip the child if it does not share our tag.
			if (child->GetTagName() != element->GetTagName())
				continue;

			// We've found a similarly-tagged child to our element; selector fails.
			return false;
		}

		return true;
	}
	break;
	case StructuralSelectorType::Empty:
	{
		return element->GetNumChildren() == 0;
	}
	break;
	case StructuralSelectorType::Not:
	{
		if (!selector.selector_tree)
		{
			RMLUI_ERROR;
			return false;
		}

		bool inner_selector_matches = false;

		for (const StyleSheetNode* node : selector.selector_tree->leafs)
		{
			if (node->IsApplicable(element, scope))
			{
				inner_selector_matches = true;
				break;
			}
		}

		return !inner_selector_matches;
	}
	break;
	case StructuralSelectorType::Scope:
	{
		return scope && element == scope;
	}
	break;
	case StructuralSelectorType::Root:
	{
		// A document is its own owner document.
		return element == element->GetOwnerDocument();
	}
	break;
	case StructuralSelectorType::Is:
	case StructuralSelectorType::Where:
	{
		if (!selector.selector_tree)
		{
			RMLUI_ERROR;
			return false;
		}
		return MatchesSelectorTree(element, *selector.selector_tree, scope);
	}
	break;
	case StructuralSelectorType::Has:
	{
		if (!selector.selector_tree)
		{
			RMLUI_ERROR;
			return false;
		}
		return MatchesRelativeSelectorTree(element, *selector.selector_tree);
	}
	break;
	case StructuralSelectorType::Enabled: return IsFormControl(element) && !IsElementDisabled(element);
	case StructuralSelectorType::Disabled: return IsElementDisabled(element);
	case StructuralSelectorType::AnyLink:
	{
		const String& tag = element->GetTagName();
		return (tag == "a" || tag == "area") && element->HasAttribute("href");
	}
	case StructuralSelectorType::Never: return false;
	case StructuralSelectorType::ReadWrite: return IsReadWrite(element);
	case StructuralSelectorType::ReadOnly: return !IsReadWrite(element);
	case StructuralSelectorType::Required: return IsRequirable(element) && element->HasAttribute("required");
	case StructuralSelectorType::Optional: return IsRequirable(element) && !element->HasAttribute("required");
	case StructuralSelectorType::Indeterminate:
	{
		const String& tag = element->GetTagName();
		if (tag == "progress")
			return !element->HasAttribute("value");
		if (tag == "input")
		{
			const String type = StringUtilities::ToLower(element->GetAttribute<String>("type", ""));
			if (type == "checkbox")
				return element->HasAttribute("indeterminate");
			if (type == "radio")
				return !element->HasAttribute("checked") && !IsRadioGroupChecked(element);
		}
		return false;
	}
	case StructuralSelectorType::Default: return IsDefault(element);
	case StructuralSelectorType::Target:
	{
		const String& id = element->GetId();
		const ElementDocument* document = element->GetOwnerDocument();
		return !id.empty() && document && document->GetTarget() == id;
	}
	case StructuralSelectorType::Invalid:
	{
		RMLUI_ERROR;
	}
	break;
	}

	return false;
}

} // namespace Rml
