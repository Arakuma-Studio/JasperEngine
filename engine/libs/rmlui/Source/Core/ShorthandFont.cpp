#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/PropertyDictionary.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "PropertyShorthandDefinition.h"
#include "ValuesShorthands.h"
#include <stdlib.h>

namespace Rml {

static bool IsOneOf(const String& word, std::initializer_list<const char*> words)
{
	for (const char* candidate : words)
		if (word == candidate)
			return true;
	return false;
}

// An absolute or relative font weight: a keyword, or a number from 1 to 1000.
static bool IsFontWeight(const String& word)
{
	if (IsOneOf(word, {"bold", "bolder", "lighter"}))
		return true;
	char* end = nullptr;
	const float number = strtof(word.c_str(), &end);
	return end == word.c_str() + word.size() && !word.empty() && number >= 1.f && number <= 1000.f;
}

bool ParseFontShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification)
{
	StringList tokens;
	SplitWhitespaceList(value, tokens);
	if (tokens.empty())
		return false;

	PropertyDictionary parsed;
	specification.SetShorthandDefaults(parsed, definition);

	// Style, variant, weight and stretch in any order, each at most once, 'normal' for any of them.
	size_t i = 0;
	int prefix_count = 0;
	bool has_style = false, has_weight = false;
	for (; i < tokens.size(); i++)
	{
		const String word = StringUtilities::ToLower(tokens[i]);
		if (word == "normal")
		{
		}
		else if (!has_style && (word == "italic" || word == "oblique"))
		{
			has_style = true;
			if (!specification.ParsePropertyDeclaration(parsed, PropertyId::FontStyle, "italic"))
				return false;
			// 'oblique <angle>' draws as italic.
			if (word == "oblique" && i + 1 < tokens.size() && StringUtilities::EndsWith(StringUtilities::ToLower(tokens[i + 1]), "deg"))
				i++;
		}
		else if (!has_weight && (IsFontWeight(word) || StringUtilities::StartsWith(word, "calc(") || StringUtilities::StartsWith(word, "min(") ||
					 StringUtilities::StartsWith(word, "max(") || StringUtilities::StartsWith(word, "clamp(")))
		{
			has_weight = true;
			if (!specification.ParsePropertyDeclaration(parsed, PropertyId::FontWeight, word))
				return false;
		}
		else
			break;
		if (++prefix_count > 4)
			return false;
	}

	// The size, with the line height after a slash ("16px/1.5", "16px / 1.5", "16px /1.5").
	if (i >= tokens.size())
		return false;
	String size = tokens[i++];
	String line_height;
	bool has_line_height = false;
	const size_t slash = size.find('/');
	if (slash != String::npos)
	{
		line_height = size.substr(slash + 1);
		size = size.substr(0, slash);
		has_line_height = true;
	}
	else if (i < tokens.size() && !tokens[i].empty() && tokens[i][0] == '/')
	{
		line_height = tokens[i++].substr(1);
		has_line_height = true;
	}
	if (has_line_height && line_height.empty())
	{
		if (i >= tokens.size())
			return false;
		line_height = tokens[i++];
	}
	if (size.empty() || !specification.ParsePropertyDeclaration(parsed, PropertyId::FontSize, size))
		return false;
	if (has_line_height && !specification.ParsePropertyDeclaration(parsed, PropertyId::LineHeight, line_height))
		return false;

	// The family list takes the rest.
	if (i >= tokens.size())
		return false;
	String family;
	for (; i < tokens.size(); i++)
		family += (family.empty() ? "" : " ") + tokens[i];
	if (!specification.ParsePropertyDeclaration(parsed, PropertyId::FontFamily, family))
		return false;

	for (const auto& id_property : parsed.GetProperties())
		dictionary.SetProperty(id_property.first, id_property.second);
	return true;
}

} // namespace Rml
