#include "../../Include/RmlUi/Core/PropertyDictionary.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "PropertyShorthandDefinition.h"
#include "TextProperties.h"

namespace Rml {

// 'white-space' and 'text-wrap' as the shorthands of CSS Text 4 (phase 3, p3-text).

bool ParseWhiteSpaceShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification)
{
	StringList words;
	SplitWhitespaceList(value, words);
	if (words.empty() || words.size() > 2)
		return false;

	String collapse = "collapse", wrap_mode = "wrap";
	if (words.size() == 1)
	{
		// The CSS 2.1 keywords.
		const String keyword = StringUtilities::ToLower(words[0]);
		if (keyword == "normal")
		{}
		else if (keyword == "pre")
			collapse = "preserve", wrap_mode = "nowrap";
		else if (keyword == "nowrap")
			wrap_mode = "nowrap";
		else if (keyword == "pre-wrap")
			collapse = "preserve";
		else if (keyword == "pre-line")
			collapse = "preserve-breaks";
		else if (keyword == "break-spaces")
			collapse = "break-spaces";
		else if (keyword == "wrap" || keyword == "nowrap")
			wrap_mode = keyword;
		else
			collapse = keyword;
	}
	else
	{
		// '<white-space-collapse> || <text-wrap-mode>'
		bool have_collapse = false, have_wrap_mode = false;
		for (const String& word : words)
		{
			const String keyword = StringUtilities::ToLower(word);
			if (keyword == "wrap" || keyword == "nowrap")
			{
				if (have_wrap_mode)
					return false;
				wrap_mode = keyword;
				have_wrap_mode = true;
			}
			else
			{
				if (have_collapse)
					return false;
				collapse = keyword;
				have_collapse = true;
			}
		}
	}

	PropertyDictionary parsed;
	if (!specification.ParsePropertyDeclaration(parsed, PropertyId::WhiteSpaceCollapse, collapse) ||
		!specification.ParsePropertyDeclaration(parsed, PropertyId::TextWrapMode, wrap_mode))
		return false;

	(void)definition;
	for (const auto& id_property : parsed.GetProperties())
		dictionary.SetProperty(id_property.first, id_property.second);
	return true;
}

bool ParseTextWrapShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification)
{
	StringList words;
	SplitWhitespaceList(value, words);
	if (words.empty() || words.size() > 2)
		return false;

	String wrap_mode = "wrap";
	bool have_wrap_mode = false, have_style = false;
	for (const String& word : words)
	{
		const String keyword = StringUtilities::ToLower(word);
		if (keyword == "wrap" || keyword == "nowrap")
		{
			if (have_wrap_mode)
				return false;
			wrap_mode = keyword;
			have_wrap_mode = true;
		}
		else if (keyword == "auto" || keyword == "balance" || keyword == "pretty" || keyword == "stable")
		{
			// 'text-wrap-style': lines are filled greedily whatever the style.
			if (have_style)
				return false;
			have_style = true;
		}
		else
			return false;
	}

	PropertyDictionary parsed;
	if (!specification.ParsePropertyDeclaration(parsed, PropertyId::TextWrapMode, wrap_mode))
		return false;

	(void)definition;
	for (const auto& id_property : parsed.GetProperties())
		dictionary.SetProperty(id_property.first, id_property.second);
	return true;
}

} // namespace Rml
