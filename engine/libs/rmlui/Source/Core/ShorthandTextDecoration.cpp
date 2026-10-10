#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/PropertyDictionary.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "PropertyShorthandDefinition.h"
#include "TextProperties.h"

namespace Rml {

// 'text-decoration-line' and the 'text-decoration' shorthand (CSS Text Decoration 3; phase 3, p3-text).

static bool AddDecorationLine(const String& keyword, int& lines, bool& none)
{
	int line = 0;
	if (keyword == "underline")
		line = (int)Style::TextDecoration::Underline;
	else if (keyword == "overline")
		line = (int)Style::TextDecoration::Overline;
	else if (keyword == "line-through")
		line = (int)Style::TextDecoration::LineThrough;
	else if (keyword == "blink")
		line = 0x80; // accepted, draws nothing; kept apart so that it can be given once
	else if (keyword == "none")
	{
		if (none || lines != 0)
			return false;
		none = true;
		return true;
	}
	else
		return false;

	if (none || (lines & line) != 0)
		return false;
	lines |= line;
	return true;
}

bool PropertyParserTextDecorationLine::ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const
{
	StringList words;
	SplitWhitespaceList(value, words);
	if (words.empty())
		return false;

	int lines = 0;
	bool none = false;
	for (const String& word : words)
	{
		if (!AddDecorationLine(StringUtilities::ToLower(word), lines, none))
			return false;
	}

	property.value = Variant(lines & 0x7f);
	property.unit = Unit::KEYWORD;
	return true;
}

bool ParseTextDecorationShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification)
{
	StringList words;
	SplitWhitespaceList(value, words);
	if (words.empty())
		return false;

	String line_value;
	int lines = 0;
	bool none = false;
	bool have_style = false, have_color = false, have_thickness = false;

	PropertyDictionary parsed;
	specification.SetShorthandDefaults(parsed, definition);

	for (const String& word : words)
	{
		const String keyword = StringUtilities::ToLower(word);
		if (AddDecorationLine(keyword, lines, none))
		{
			line_value += (line_value.empty() ? "" : " ") + keyword;
			continue;
		}
		if (!have_style && specification.ParsePropertyDeclaration(parsed, PropertyId::TextDecorationStyle, word))
		{
			have_style = true;
			continue;
		}
		if (!have_thickness && specification.ParsePropertyDeclaration(parsed, PropertyId::TextDecorationThickness, word))
		{
			have_thickness = true;
			continue;
		}
		if (!have_color && specification.ParsePropertyDeclaration(parsed, PropertyId::TextDecorationColor, word))
		{
			have_color = true;
			continue;
		}
		return false;
	}

	if (!line_value.empty() && !specification.ParsePropertyDeclaration(parsed, PropertyId::TextDecoration, line_value))
		return false;

	for (const auto& id_property : parsed.GetProperties())
		dictionary.SetProperty(id_property.first, id_property.second);
	return true;
}

} // namespace Rml
