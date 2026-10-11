#include "CounterResolver.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/Profiling.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "ContentValue.h"
#include "ElementCascade.h"
#include "PseudoElements.h"
#include <stdlib.h>

namespace Rml {

// The CSS alphabetic system: a b ... z aa ab ...
static String Alphabetic(int value, const char* const* symbols, int count)
{
	String result;
	unsigned int remaining = (unsigned int)value;
	while (remaining > 0)
	{
		remaining--;
		result.insert(0, symbols[remaining % (unsigned int)count]);
		remaining /= (unsigned int)count;
	}
	return result;
}

static String Roman(int value, bool upper)
{
	static const int values[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
	static const char* const lower_symbols[] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
	static const char* const upper_symbols[] = {"M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I"};
	String result;
	for (int i = 0; i < 13; i++)
	{
		while (value >= values[i])
		{
			result += upper ? upper_symbols[i] : lower_symbols[i];
			value -= values[i];
		}
	}
	return result;
}

bool CounterResolver::IsSymbolStyle(const String& style)
{
	return style == "disc" || style == "circle" || style == "square" || style == "disclosure-open" || style == "disclosure-closed";
}

String CounterResolver::FormatCounter(int value, const String& style)
{
	static const char* const lower_latin[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s",
		"t", "u", "v", "w", "x", "y", "z"};
	static const char* const upper_latin[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S",
		"T", "U", "V", "W", "X", "Y", "Z"};
	// lower-greek: 24 letters, no final sigma
	static const char* const greek[] = {"\xCE\xB1", "\xCE\xB2", "\xCE\xB3", "\xCE\xB4", "\xCE\xB5", "\xCE\xB6", "\xCE\xB7", "\xCE\xB8",
		"\xCE\xB9", "\xCE\xBA", "\xCE\xBB", "\xCE\xBC", "\xCE\xBD", "\xCE\xBE", "\xCE\xBF", "\xCF\x80", "\xCF\x81", "\xCF\x83", "\xCF\x84",
		"\xCF\x85", "\xCF\x86", "\xCF\x87", "\xCF\x88", "\xCF\x89"};

	if (!style.empty() && style[0] == '"')
		return style.substr(1);
	if (style == "none")
		return String();
	if (style == "disc")
		return "\xE2\x80\xA2"; // U+2022
	if (style == "circle")
		return "\xE2\x97\xA6"; // U+25E6
	if (style == "square")
		return "\xE2\x96\xAA"; // U+25AA
	if (style == "disclosure-open")
		return "\xE2\x96\xBE"; // U+25BE
	if (style == "disclosure-closed")
		return "\xE2\x96\xB8"; // U+25B8

	// Outside their range alphabetic and roman styles count in decimal, their fallback.
	if (style == "lower-alpha" || style == "lower-latin")
		return value >= 1 ? Alphabetic(value, lower_latin, 26) : Rml::ToString(value);
	if (style == "upper-alpha" || style == "upper-latin")
		return value >= 1 ? Alphabetic(value, upper_latin, 26) : Rml::ToString(value);
	if (style == "lower-greek")
		return value >= 1 ? Alphabetic(value, greek, 24) : Rml::ToString(value);
	if (style == "lower-roman" || style == "upper-roman")
		return (value >= 1 && value <= 3999) ? Roman(value, style == "upper-roman") : Rml::ToString(value);
	if (style == "decimal-leading-zero")
	{
		// The minus sign counts in the two digits: -4 is "-4".
		const String digits = Rml::ToString(value);
		return digits.size() < 2 ? "0" + digits : digits;
	}
	return Rml::ToString(value);
}

int CounterResolver::GetCounterValue(const CssCounterSet& counters, const String& name)
{
	for (auto it = counters.rbegin(); it != counters.rend(); ++it)
	{
		if (it->name == name)
			return it->value;
	}
	return 0;
}

String CounterResolver::FormatCounters(const CssCounterSet& counters, const String& name, const String& separator, const String& style)
{
	String result;
	bool any = false;
	for (const CssCounter& counter : counters)
	{
		if (counter.name != name)
			continue;
		if (any)
			result += separator;
		result += FormatCounter(counter.value, style);
		any = true;
	}
	return any ? result : FormatCounter(0, style);
}

namespace {

	struct ResolveState {
		CssCounterSet last_in_order; // the counters of the previous element in tree order
		int quote_depth = 0;
	};

	const CounterListValue* GetCounterList(Element& element, PropertyId id)
	{
		const Property* property = element.GetProperty(id);
		if (!property || property->unit != Unit::STYLE_VALUE)
			return nullptr;
		return static_cast<const CounterListValue*>(property->Get<StyleValuePtr>().get());
	}

	CssCounter* Innermost(CssCounterSet& counters, const String& name)
	{
		for (auto it = counters.rbegin(); it != counters.rend(); ++it)
		{
			if (it->name == name)
				return &*it;
		}
		return nullptr;
	}

	bool IsPreviousSibling(const Element* candidate, const Element* element)
	{
		const Element* parent = element->GetParentNode();
		if (!candidate || !parent || candidate->GetParentNode() != parent)
			return false;
		for (int i = 0; i < parent->GetNumChildren(true); i++)
		{
			const Element* child = parent->GetChild(i);
			if (child == element)
				return false;
			if (child == candidate)
				return true;
		}
		return false;
	}

	// CSS Lists 3, instantiating a counter: the innermost counter of the name goes when this element or a previous
	// sibling created it.
	void Instantiate(CssCounterSet& counters, const Element* element, const String& name, int value, bool reversed)
	{
		for (auto it = counters.rbegin(); it != counters.rend(); ++it)
		{
			if (it->name != name)
				continue;
			if (it->origin == element || IsPreviousSibling(it->origin, element))
				counters.erase(std::next(it).base());
			break;
		}
		counters.push_back(CssCounter{name, element, value, reversed});
	}

	bool IsListItem(Element& element)
	{
		return element.GetComputedValues().display() == Style::Display::ListItem;
	}

	bool ReadInteger(const String& text, int& value)
	{
		const String stripped = StringUtilities::StripWhitespace(text);
		if (stripped.empty())
			return false;
		char* end = nullptr;
		const long number = strtol(stripped.c_str(), &end, 10);
		if (!end || *end != '\0')
			return false;
		value = (int)Math::Clamp(number, -2147483647L, 2147483647L);
		return true;
	}

	// The number of list items an <ol reversed> counts down from: its list-item children.
	int CountListItems(Element& list)
	{
		int count = 0;
		for (int i = 0; i < list.GetNumChildren(); i++)
		{
			Element* child = list.GetChild(i);
			if (child->GetTagName() != "#text" && IsListItem(*child))
				count++;
		}
		return count;
	}

	void ApplyCounterProperties(Element& element, CssCounterSet& counters)
	{
		const String& tag = element.GetTagName();
		const bool html_list = (tag == "ol" || tag == "ul" || tag == "menu");

		// counter-reset
		if (const CounterListValue* resets = GetCounterList(element, PropertyId::CounterReset))
		{
			for (const CounterListValue::Item& item : resets->items)
			{
				bool reversed = item.reversed;
				int value = item.value;
				if (item.name == "list-item" && html_list && !item.has_value)
				{
					// <ol start> and <ol reversed>: HTML's numbering of the items.
					reversed = reversed || (tag == "ol" && element.HasAttribute("reversed"));
					int start = 0;
					const bool has_start = (tag == "ol" && ReadInteger(element.GetAttribute<String>("start", ""), start));
					if (reversed)
						value = (has_start ? start : CountListItems(element)) + 1;
					else
						value = (has_start ? start - 1 : 0);
				}
				else if (reversed && !item.has_value)
				{
					value = CountListItems(element) + 1;
				}
				Instantiate(counters, &element, item.name, value, reversed);
			}
		}

		// counter-increment, and the implicit increment of 'list-item' by list items
		bool incremented_list_item = false;
		if (const CounterListValue* increments = GetCounterList(element, PropertyId::CounterIncrement))
		{
			for (const CounterListValue::Item& item : increments->items)
			{
				CssCounter* counter = Innermost(counters, item.name);
				if (!counter)
				{
					Instantiate(counters, &element, item.name, 0, false);
					counter = &counters.back();
				}
				counter->value += (item.has_value ? item.value : 1);
				incremented_list_item = incremented_list_item || item.name == "list-item";
			}
		}
		const bool list_item = IsListItem(element) && element.GetPseudoElementType() == PseudoElementType::None;
		if (list_item && !incremented_list_item)
		{
			CssCounter* counter = Innermost(counters, "list-item");
			if (!counter)
			{
				Instantiate(counters, &element, "list-item", 0, false);
				counter = &counters.back();
			}
			counter->value += (counter->reversed ? -1 : 1);
		}

		// counter-set, and <li value>
		if (const CounterListValue* sets = GetCounterList(element, PropertyId::CounterSet))
		{
			for (const CounterListValue::Item& item : sets->items)
			{
				CssCounter* counter = Innermost(counters, item.name);
				if (!counter)
				{
					Instantiate(counters, &element, item.name, 0, false);
					counter = &counters.back();
				}
				counter->value = (item.has_value ? item.value : 0);
			}
		}
		int value = 0;
		if (list_item && tag == "li" && ReadInteger(element.GetAttribute<String>("value", ""), value))
		{
			if (CssCounter* counter = Innermost(counters, "list-item"))
				counter->value = value;
		}
	}

	CssCounterSet Walk(Element& element, const CssCounterSet& parent_counters, const CssCounterSet* sibling_counters, ResolveState& state)
	{
		// Inheriting counters (CSS Lists 3 §4.4.1): the parent's, those of the preceding sibling whose name is not there yet,
		// and the values of the previous element in tree order.
		CssCounterSet counters = parent_counters;
		if (sibling_counters)
		{
			for (const CssCounter& counter : *sibling_counters)
			{
				const bool has_name =
					std::any_of(counters.begin(), counters.end(), [&](const CssCounter& existing) { return existing.name == counter.name; });
				if (!has_name)
					counters.push_back(counter);
			}
		}
		for (const CssCounter& source : state.last_in_order)
		{
			for (CssCounter& counter : counters)
			{
				if (counter.name == source.name && counter.origin == source.origin)
					counter.value = source.value;
			}
		}

		ApplyCounterProperties(element, counters);
		state.last_in_order = counters;

		if (element.GetPseudoElementType() != PseudoElementType::None)
		{
			ElementPseudo& pseudo_element = static_cast<ElementPseudo&>(element);
			pseudo_element.OnCountersResolved(counters, state.quote_depth);
		}

		// The children in tree order: ::marker, ::before, the element children, ::after.
		CssCounterSet previous_sibling;
		bool has_previous = false;
		auto Visit = [&](Element* child) {
			if (!child || child->GetTagName() == "#text" || child->GetComputedValues().display() == Style::Display::None)
				return;
			CssCounterSet child_counters = Walk(*child, counters, has_previous ? &previous_sibling : nullptr, state);
			previous_sibling = std::move(child_counters);
			has_previous = true;
		};
		Visit(element.GetPseudoElement(PseudoElementType::Marker));
		Visit(element.GetPseudoElement(PseudoElementType::Before));
		const int num_children = element.GetNumChildren();
		for (int i = 0; i < num_children; i++)
			Visit(element.GetChild(i));
		Visit(element.GetPseudoElement(PseudoElementType::After));

		return counters;
	}

} // namespace

void CounterResolver::Resolve(ElementDocument& document)
{
	RMLUI_ZoneScoped;
	ResolveState state;
	Walk(document, CssCounterSet(), nullptr, state);
}

} // namespace Rml
