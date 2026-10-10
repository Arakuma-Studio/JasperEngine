#include "PropertyParserAlignment.h"
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "../../Include/RmlUi/Core/PropertyDictionary.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "PropertyShorthandDefinition.h"

namespace Rml {

static bool IsPositional(const String& keyword)
{
	return keyword == "center" || keyword == "start" || keyword == "end" || keyword == "self-start" || keyword == "self-end" ||
		keyword == "flex-start" || keyword == "flex-end" || keyword == "left" || keyword == "right";
}

bool PropertyParserAlignment::ParseValue(Property& property, const String& value, const ParameterMap& parameters) const
{
	StringList words;
	SplitWhitespaceList(StringUtilities::ToLower(value), words);
	if (words.empty() || words.size() > 2)
		return false;

	String keyword = words[0];
	int flags = 0;
	if (words.size() == 2)
	{
		const String& first = words[0];
		const String& second = words[1];
		if (first == "first" && second == "baseline")
			keyword = "baseline";
		else if (first == "last" && second == "baseline")
			keyword = "last baseline";
		else if ((first == "legacy" && (second == "left" || second == "right" || second == "center")) ||
			(second == "legacy" && (first == "left" || first == "right" || first == "center")))
			keyword = "legacy";
		else if ((first == "safe" || first == "unsafe") && IsPositional(second))
		{
			keyword = second;
			flags = (first == "safe" ? AlignmentSafe : AlignmentUnsafe);
		}
		else
			return false;
	}

	// The prefixed forms are listed with their flags, so that a property prints as it was declared.
	const auto it = parameters.find(flags ? words[0] + ' ' + keyword : keyword);
	if (it == parameters.end())
		return false;

	property.value = Variant(it->second);
	property.unit = Unit::KEYWORD;
	return true;
}

String MakeAlignmentKeywords(const char* keywords, int first)
{
	StringList list;
	StringUtilities::ExpandString(list, keywords);
	String result;
	int value = first;
	auto Append = [&result](const String& name, int name_value) {
		if (!result.empty())
			result += ", ";
		result += name + '=' + ToString(name_value);
	};
	for (const String& keyword : list)
		Append(keyword, value++);
	value = first;
	for (const String& keyword : list)
	{
		if (IsPositional(keyword))
		{
			Append("safe " + keyword, value | PropertyParserAlignment::AlignmentSafe);
			Append("unsafe " + keyword, value | PropertyParserAlignment::AlignmentUnsafe);
		}
		value++;
	}
	return result;
}

bool ParsePlaceShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification)
{
	if (definition.items.size() != 2 || definition.items[0].type != ShorthandItemType::Property ||
		definition.items[1].type != ShorthandItemType::Property)
		return false;
	const PropertyId align_id = definition.items[0].property_id;
	const PropertyId justify_id = definition.items[1].property_id;

	StringList words;
	SplitWhitespaceList(value, words);
	if (words.empty())
		return false;

	// The align value takes one or two words ('safe center', 'last baseline'); the justify value the rest, or the same.
	for (size_t align_words = 1; align_words <= words.size() && align_words <= 2; align_words++)
	{
		String align_value, justify_value;
		for (size_t i = 0; i < words.size(); i++)
		{
			String& part = (i < align_words ? align_value : justify_value);
			part += (part.empty() ? "" : " ") + words[i];
		}

		PropertyDictionary parsed;
		if (!specification.ParsePropertyDeclaration(parsed, align_id, align_value))
			continue;

		if (justify_value.empty())
		{
			justify_value = align_value;
			// place-content: a baseline alignment has no justify-content counterpart, 'start' is used.
			if (justify_id == PropertyId::JustifyContent &&
				(align_value == "baseline" || align_value == "first baseline" || align_value == "last baseline"))
				justify_value = "start";
		}
		if (!specification.ParsePropertyDeclaration(parsed, justify_id, justify_value))
			continue;

		for (const auto& id_property : parsed.GetProperties())
			dictionary.SetProperty(id_property.first, id_property.second);
		return true;
	}
	return false;
}

} // namespace Rml
