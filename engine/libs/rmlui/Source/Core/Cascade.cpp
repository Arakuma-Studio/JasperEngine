#include "Cascade.h"
#include "../../Include/RmlUi/Core/PropertyDictionary.h"
#include "../../Include/RmlUi/Core/PropertySpecification.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"

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
