#include "Cascade.h"
#include "../../Include/RmlUi/Core/PropertyDictionary.h"
#include "../../Include/RmlUi/Core/PropertySpecification.h"
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "PropertyShorthandDefinition.h"

namespace Rml {
namespace Cascade {

	// Finds a trailing '!important' outside strings and parentheses; 'stripped' receives the value before it.
	static bool StripImportant(const String& value, String& stripped)
	{
		size_t bang = String::npos;
		int depth = 0;
		char quote = 0;
		for (size_t i = 0; i < value.size(); i++)
		{
			const char c = value[i];
			if (quote)
			{
				if (c == '\\')
					i++;
				else if (c == quote)
					quote = 0;
				continue;
			}
			if (c == '"' || c == '\'')
				quote = c;
			else if (c == '(')
				depth++;
			else if (c == ')' && depth > 0)
				depth--;
			else if (c == '!' && depth == 0)
				bang = i;
		}
		if (bang == String::npos)
			return false;

		const String keyword = StringUtilities::StripWhitespace(StringView(value, bang + 1, value.size() - bang - 1));
		if (keyword.size() != 9)
			return false;
		static const char important[] = "important";
		for (size_t i = 0; i < 9; i++)
		{
			char c = keyword[i];
			if (c >= 'A' && c <= 'Z')
				c = char(c - 'A' + 'a');
			if (c != important[i])
				return false;
		}

		stripped = StringUtilities::StripWhitespace(StringView(value, 0, bang));
		return true;
	}

	bool ParseImportantDeclaration(const PropertySpecification& specification, PropertyDictionary& dictionary, const String& name,
		const String& value, bool& result)
	{
		// Only the declarations of style rules and inline styles; at-rules with their own specification (decorators,
		// font effects, media queries) know no importance.
		if (&specification != &StyleSheetSpecification::GetPropertySpecification())
			return false;
		String stripped;
		if (!StripImportant(value, stripped))
			return false;

		result = false;
		// '!important' alone, or twice, is invalid.
		if (stripped.empty() || stripped.back() == '!')
			return true;
		String twice;
		if (StripImportant(stripped, twice))
			return true;

		PropertyDictionary parsed;
		if (!specification.ParsePropertyDeclaration(parsed, name, stripped))
			return true;

		for (const auto& [id, property] : parsed.GetProperties())
		{
			Property important = property;
			important.important = true;
			dictionary.SetProperty(id, important);
		}
		for (const auto& [custom_name, property] : parsed.GetCustomProperties())
		{
			Property important = property;
			important.important = true;
			dictionary.SetCustomProperty(custom_name, std::move(important));
		}
		for (const auto& [shorthand_id, property] : parsed.GetVarShorthands())
		{
			Property important = property;
			important.important = true;
			dictionary.SetVarShorthand(shorthand_id, std::move(important));
		}

		result = true;
		return true;
	}

	bool ParseKeyword(const String& value, CssWideKeyword& keyword)
	{
		const String word = StringUtilities::ToLower(StringUtilities::StripWhitespace(value));
		if (word == "inherit")
			keyword = CssWideKeyword::Inherit;
		else if (word == "initial")
			keyword = CssWideKeyword::Initial;
		else if (word == "unset")
			keyword = CssWideKeyword::Unset;
		else if (word == "revert" || word == "revert-layer")
			keyword = CssWideKeyword::Revert;
		else
			return false;
		return true;
	}

	static Property KeywordProperty(CssWideKeyword keyword)
	{
		return Property(int(keyword), Unit::CSS_WIDE_KEYWORD);
	}

	static void SetShorthandKeyword(PropertyDictionary& dictionary, const ShorthandDefinition& definition, CssWideKeyword keyword)
	{
		for (const ShorthandItem& item : definition.items)
		{
			if (item.type == ShorthandItemType::Property)
				dictionary.SetProperty(item.property_id, KeywordProperty(keyword));
			else if (item.type == ShorthandItemType::Shorthand && item.shorthand_definition)
				SetShorthandKeyword(dictionary, *item.shorthand_definition, keyword);
		}
	}

	void SetAllProperties(const PropertySpecification& specification, PropertyDictionary& dictionary, CssWideKeyword keyword)
	{
		for (const PropertyId id : specification.GetRegisteredProperties())
		{
			const PropertyDefinition* definition = specification.GetProperty(id);
			if (!definition)
				continue;
			const String& name = StyleSheetSpecification::GetPropertyName(id);
			if (StringUtilities::StartsWith(name, "-rmlui-"))
				continue;
			dictionary.SetProperty(id, KeywordProperty(keyword));
		}
	}

	bool ParseAllShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& /*definition*/, const String& value,
		const PropertySpecification& specification)
	{
		CssWideKeyword keyword;
		if (!ParseKeyword(value, keyword))
			return false;
		SetAllProperties(specification, dictionary, keyword);
		return true;
	}

	bool ParseKeywordDeclaration(const PropertySpecification& specification, PropertyDictionary& dictionary, const String& name,
		const String& value)
	{
		if (&specification != &StyleSheetSpecification::GetPropertySpecification())
			return false;
		CssWideKeyword keyword;
		if (!ParseKeyword(value, keyword))
			return false;

		if (const PropertyDefinition* definition = specification.GetProperty(name))
		{
			dictionary.SetProperty(definition->GetId(), KeywordProperty(keyword));
			return true;
		}
		if (const ShorthandDefinition* shorthand = specification.GetShorthand(name))
		{
			if (shorthand->id == ShorthandId::All)
				SetAllProperties(specification, dictionary, keyword);
			else
				SetShorthandKeyword(dictionary, *shorthand, keyword);
			return true;
		}
		if (StringUtilities::StartsWith(name, "--"))
		{
			dictionary.SetCustomProperty(name, KeywordProperty(keyword));
			return true;
		}
		return false;
	}

	// The generated-content properties read their value as written: strings keep their quotes (the generic value split
	// unquotes a single string and rejects several). A value with var() takes the usual path.
	static bool ParseRawDeclaration(const PropertySpecification& specification, PropertyDictionary& dictionary, const String& name,
		const String& value, bool& result)
	{
		const String stripped = StringUtilities::StripWhitespace(value);
		if (stripped.find("var(") != String::npos)
			return false;

		if (const PropertyDefinition* definition = specification.GetProperty(name))
		{
			switch (definition->GetId())
			{
			case PropertyId::Content:
			case PropertyId::Quotes:
			case PropertyId::CounterReset:
			case PropertyId::CounterIncrement:
			case PropertyId::CounterSet:
			case PropertyId::ListStyleType:
			case PropertyId::ListStyleImage: break;
			default: return false;
			}
			Property property;
			result = definition->ParseValue(property, stripped);
			if (result)
				dictionary.SetProperty(definition->GetId(), property);
			return true;
		}

		const ShorthandDefinition* shorthand = specification.GetShorthand(name);
		if (!shorthand || shorthand->id != ShorthandId::ListStyle || !shorthand->parser)
			return false;
		PropertyDictionary parsed;
		result = shorthand->parser(parsed, *shorthand, stripped, specification);
		if (result)
		{
			for (const auto& [id, property] : parsed.GetProperties())
				dictionary.SetProperty(id, property);
		}
		return true;
	}

	bool ParseDeclaration(const PropertySpecification& specification, PropertyDictionary& dictionary, const String& name, const String& value,
		bool& result)
	{
		if (&specification != &StyleSheetSpecification::GetPropertySpecification())
			return false;
		if (ParseImportantDeclaration(specification, dictionary, name, value, result))
			return true;
		if (!dictionary.HasImportantProperties())
		{
			if (ParseKeywordDeclaration(specification, dictionary, name, value))
			{
				result = true;
				return true;
			}
			return ParseRawDeclaration(specification, dictionary, name, value, result);
		}

		// A normal declaration does not override an important one of the same block.
		PropertyDictionary parsed;
		result = specification.ParsePropertyDeclaration(parsed, name, value);
		if (!result)
			return true;
		for (const auto& [id, property] : parsed.GetProperties())
		{
			const Property* existing = dictionary.GetProperty(id);
			if (!existing || !existing->important)
				dictionary.SetProperty(id, property);
		}
		for (const auto& [custom_name, property] : parsed.GetCustomProperties())
		{
			const Property* existing = dictionary.GetCustomProperty(custom_name);
			if (!existing || !existing->important)
				dictionary.SetCustomProperty(custom_name, property);
		}
		for (const auto& [shorthand_id, property] : parsed.GetVarShorthands())
		{
			const Property* existing = dictionary.GetVarShorthand(shorthand_id);
			if (!existing || !existing->important)
				dictionary.SetVarShorthand(shorthand_id, property);
		}
		return true;
	}

	void RemoveImportantDeclarations(PropertyDictionary& dictionary)
	{
		if (!HasImportantDeclarations(dictionary))
			return;

		PropertyDictionary normal;
		for (const auto& [id, property] : dictionary.GetProperties())
		{
			if (!property.important)
				normal.SetProperty(id, property);
		}
		for (const auto& [name, property] : dictionary.GetCustomProperties())
		{
			if (!property.important)
				normal.SetCustomProperty(name, property);
		}
		for (const auto& [id, property] : dictionary.GetVarShorthands())
		{
			if (!property.important)
				normal.SetVarShorthand(id, property);
		}
		dictionary = std::move(normal);
	}

	bool HasImportantDeclarations(const PropertyDictionary& dictionary)
	{
		for (const auto& entry : dictionary.GetProperties())
		{
			if (entry.second.important)
				return true;
		}
		for (const auto& entry : dictionary.GetCustomProperties())
		{
			if (entry.second.important)
				return true;
		}
		for (const auto& entry : dictionary.GetVarShorthands())
		{
			if (entry.second.important)
				return true;
		}
		return false;
	}

} // namespace Cascade
} // namespace Rml
