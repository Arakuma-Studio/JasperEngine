#include "ContentValue.h"
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "../../Include/RmlUi/Core/PropertyDictionary.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include <stdlib.h>

namespace Rml {

namespace ContentParsing {

	static int HexValue(char c)
	{
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	}

	bool ReadString(const String& value, size_t& i, String& result)
	{
		if (i >= value.size() || (value[i] != '"' && value[i] != '\''))
			return false;
		const char quote = value[i++];
		result.clear();
		while (i < value.size())
		{
			const char c = value[i];
			if (c == quote)
			{
				i++;
				return true;
			}
			if (c == '\n')
				return false;
			if (c != '\\')
			{
				result += c;
				i++;
				continue;
			}

			// An escape.
			i++;
			if (i >= value.size())
				return false;
			if (value[i] == '\n')
			{
				i++;
				continue;
			}
			if (HexValue(value[i]) >= 0)
			{
				uint32_t code_point = 0;
				int digits = 0;
				while (i < value.size() && digits < 6 && HexValue(value[i]) >= 0)
				{
					code_point = code_point * 16 + (uint32_t)HexValue(value[i]);
					i++;
					digits++;
				}
				// One white space after a hex escape belongs to it.
				if (i < value.size() && (value[i] == ' ' || value[i] == '\t' || value[i] == '\n'))
					i++;
				if (code_point == 0 || code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF))
					code_point = 0xFFFD;
				result += StringUtilities::ToUTF8(Character(code_point));
				continue;
			}
			result += value[i];
			i++;
		}
		return false;
	}

	String QuoteString(const String& text)
	{
		String result = "\"";
		for (const char c : text)
		{
			if (c == '"' || c == '\\')
				result += '\\';
			if (c == '\n')
			{
				result += "\\a ";
				continue;
			}
			result += c;
		}
		result += '"';
		return result;
	}

	void SplitTokens(const String& value, StringList& tokens)
	{
		tokens.clear();
		String token;
		int depth = 0;
		char quote = 0;
		for (size_t i = 0; i < value.size(); i++)
		{
			const char c = value[i];
			if (quote)
			{
				token += c;
				if (c == '\\' && i + 1 < value.size())
					token += value[++i];
				else if (c == quote)
					quote = 0;
				continue;
			}
			if (StringUtilities::IsWhitespace(c) && depth == 0)
			{
				if (!token.empty())
					tokens.push_back(std::move(token));
				token.clear();
				continue;
			}
			if (c == '"' || c == '\'')
				quote = c;
			else if (c == '(')
				depth++;
			else if (c == ')' && depth > 0)
				depth--;
			token += c;
		}
		if (!token.empty())
			tokens.push_back(std::move(token));
	}

} // namespace ContentParsing

using namespace ContentParsing;

static bool IsIdentifier(const String& name)
{
	if (name.empty())
		return false;
	for (size_t i = 0; i < name.size(); i++)
	{
		const char c = name[i];
		const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (unsigned char)c >= 0x80;
		const bool digit = (c >= '0' && c <= '9');
		if (!(letter || c == '-' || (digit && i > 0)))
			return false;
	}
	return !(name.size() >= 2 && name[0] == '-' && name[1] >= '0' && name[1] <= '9');
}

// 'name(arguments)': the lower-case name and the arguments, split at top-level commas and stripped.
static bool ReadFunction(const String& token, String& name, StringList& arguments)
{
	const size_t open = token.find('(');
	if (open == String::npos || open == 0 || token.back() != ')')
		return false;
	name = StringUtilities::ToLower(token.substr(0, open));
	const String inner = token.substr(open + 1, token.size() - open - 2);
	arguments.clear();
	String item;
	int depth = 0;
	char quote = 0;
	for (size_t i = 0; i < inner.size(); i++)
	{
		const char c = inner[i];
		if (quote)
		{
			item += c;
			if (c == '\\' && i + 1 < inner.size())
				item += inner[++i];
			else if (c == quote)
				quote = 0;
			continue;
		}
		if (c == ',' && depth == 0)
		{
			arguments.push_back(StringUtilities::StripWhitespace(item));
			item.clear();
			continue;
		}
		if (c == '"' || c == '\'')
			quote = c;
		else if (c == '(')
			depth++;
		else if (c == ')' && depth > 0)
			depth--;
		item += c;
	}
	arguments.push_back(StringUtilities::StripWhitespace(item));
	return true;
}

// A whole token that is one string.
static bool ReadStringToken(const String& token, String& result)
{
	size_t i = 0;
	return ReadString(token, i, result) && i == token.size();
}

bool PropertyParserContent::ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const
{
	StringList tokens;
	SplitTokens(value, tokens);
	if (tokens.empty())
		return false;

	auto content = MakeShared<ContentValue>();
	for (size_t t = 0; t < tokens.size(); t++)
	{
		const String& token = tokens[t];
		// The alternative text ('/ "alt"') is for accessibility only.
		if (token == "/")
			break;
		if (token[0] == '/')
			break;

		ContentSegment segment;
		if (token[0] == '"' || token[0] == '\'')
		{
			// A string may be followed directly by '/'.
			size_t i = 0;
			if (!ReadString(token, i, segment.text))
				return false;
			content->segments.push_back(std::move(segment));
			if (i != token.size())
			{
				if (token[i] != '/')
					return false;
				break;
			}
			continue;
		}

		const String lower = StringUtilities::ToLower(token);
		if (lower == "open-quote" || lower == "close-quote" || lower == "no-open-quote" || lower == "no-close-quote")
		{
			segment.type = (lower == "open-quote" ? ContentSegment::Type::OpenQuote
					: lower == "close-quote"      ? ContentSegment::Type::CloseQuote
					: lower == "no-open-quote"    ? ContentSegment::Type::NoOpenQuote
												  : ContentSegment::Type::NoCloseQuote);
			content->segments.push_back(std::move(segment));
			continue;
		}

		String name;
		StringList arguments;
		if (!ReadFunction(token, name, arguments))
			return false;

		if (name == "url")
		{
			if (arguments.size() != 1 || arguments[0].empty())
				return false;
			String url = arguments[0];
			if (url[0] == '"' || url[0] == '\'')
			{
				String unquoted;
				if (!ReadStringToken(arguments[0], unquoted))
					return false;
				url = std::move(unquoted);
			}
			segment.type = ContentSegment::Type::Url;
			segment.text = std::move(url);
		}
		else if (name == "attr")
		{
			// attr(name) of the originating element; a type or fallback (CSS Values 5) is not supported.
			if (arguments.size() != 1 || !IsIdentifier(arguments[0]))
				return false;
			segment.type = ContentSegment::Type::Attr;
			segment.text = arguments[0];
		}
		else if (name == "counter")
		{
			if (arguments.empty() || arguments.size() > 2 || !IsIdentifier(arguments[0]))
				return false;
			segment.type = ContentSegment::Type::Counter;
			segment.text = arguments[0];
			segment.style = (arguments.size() == 2 ? arguments[1] : String("decimal"));
			if (segment.style.empty())
				return false;
		}
		else if (name == "counters")
		{
			if (arguments.size() < 2 || arguments.size() > 3 || !IsIdentifier(arguments[0]))
				return false;
			segment.type = ContentSegment::Type::Counters;
			segment.text = arguments[0];
			if (!ReadStringToken(arguments[1], segment.separator))
				return false;
			segment.style = (arguments.size() == 3 ? arguments[2] : String("decimal"));
			if (segment.style.empty())
				return false;
		}
		else
		{
			return false;
		}

		// A counter style is a keyword or a string (symbols(): not supported).
		if (segment.type == ContentSegment::Type::Counter || segment.type == ContentSegment::Type::Counters)
		{
			if (segment.style[0] == '"' || segment.style[0] == '\'')
			{
				String symbol;
				if (!ReadStringToken(segment.style, symbol))
					return false;
				segment.style = "\"" + symbol;
			}
			else if (!IsIdentifier(segment.style))
				return false;
			else
				segment.style = StringUtilities::ToLower(segment.style);
		}
		content->segments.push_back(std::move(segment));
	}

	if (content->segments.empty())
		return false;

	property.value = Variant(StyleValuePtr(std::move(content)));
	property.unit = Unit::STYLE_VALUE;
	return true;
}

String ContentValue::ToString() const
{
	String result;
	for (const ContentSegment& segment : segments)
	{
		if (!result.empty())
			result += ' ';
		switch (segment.type)
		{
		case ContentSegment::Type::String: result += QuoteString(segment.text); break;
		case ContentSegment::Type::Attr: result += "attr(" + segment.text + ")"; break;
		case ContentSegment::Type::Url: result += "url(" + QuoteString(segment.text) + ")"; break;
		case ContentSegment::Type::Counter:
		case ContentSegment::Type::Counters:
		{
			const String style = (!segment.style.empty() && segment.style[0] == '"' ? QuoteString(segment.style.substr(1)) : segment.style);
			if (segment.type == ContentSegment::Type::Counter)
				result += "counter(" + segment.text + (style == "decimal" ? String() : ", " + style) + ")";
			else
				result += "counters(" + segment.text + ", " + QuoteString(segment.separator) + (style == "decimal" ? String() : ", " + style) + ")";
		}
		break;
		case ContentSegment::Type::OpenQuote: result += "open-quote"; break;
		case ContentSegment::Type::CloseQuote: result += "close-quote"; break;
		case ContentSegment::Type::NoOpenQuote: result += "no-open-quote"; break;
		case ContentSegment::Type::NoCloseQuote: result += "no-close-quote"; break;
		}
	}
	return result;
}

bool ContentValue::Equals(const StyleValue& other) const
{
	return segments == static_cast<const ContentValue&>(other).segments;
}

bool ContentValue::UsesCounters() const
{
	for (const ContentSegment& segment : segments)
	{
		if (segment.type == ContentSegment::Type::Counter || segment.type == ContentSegment::Type::Counters)
			return true;
	}
	return false;
}

bool ContentValue::UsesAttribute(const String& name) const
{
	for (const ContentSegment& segment : segments)
	{
		if (segment.type == ContentSegment::Type::Attr && segment.text == name)
			return true;
	}
	return false;
}

bool ContentValue::UsesAttributes() const
{
	for (const ContentSegment& segment : segments)
	{
		if (segment.type == ContentSegment::Type::Attr)
			return true;
	}
	return false;
}

bool ContentValue::UsesQuotes() const
{
	for (const ContentSegment& segment : segments)
	{
		if (segment.type >= ContentSegment::Type::OpenQuote)
			return true;
	}
	return false;
}

bool PropertyParserQuotes::ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const
{
	StringList tokens;
	SplitTokens(value, tokens);
	if (tokens.empty() || tokens.size() % 2 != 0)
		return false;
	auto quotes = MakeShared<QuotesValue>();
	for (size_t i = 0; i < tokens.size(); i += 2)
	{
		String open, close;
		if (!ReadStringToken(tokens[i], open) || !ReadStringToken(tokens[i + 1], close))
			return false;
		quotes->pairs.emplace_back(std::move(open), std::move(close));
	}
	property.value = Variant(StyleValuePtr(std::move(quotes)));
	property.unit = Unit::STYLE_VALUE;
	return true;
}

String QuotesValue::ToString() const
{
	String result;
	for (const auto& pair : pairs)
	{
		if (!result.empty())
			result += ' ';
		result += QuoteString(pair.first) + ' ' + QuoteString(pair.second);
	}
	return result;
}

bool QuotesValue::Equals(const StyleValue& other) const
{
	return pairs == static_cast<const QuotesValue&>(other).pairs;
}

bool PropertyParserCounterList::ParseValue(Property& property, const String& value, const ParameterMap& parameters) const
{
	const bool accept_reversed = (parameters.find("reversed") != parameters.end());
	StringList tokens;
	SplitTokens(value, tokens);
	if (tokens.empty())
		return false;

	auto list = MakeShared<CounterListValue>();
	for (size_t i = 0; i < tokens.size(); i++)
	{
		CounterListValue::Item item;
		const String& token = tokens[i];
		String function_name;
		StringList arguments;
		if (accept_reversed && ReadFunction(token, function_name, arguments) && function_name == "reversed")
		{
			if (arguments.size() != 1 || !IsIdentifier(arguments[0]))
				return false;
			item.name = arguments[0];
			item.reversed = true;
		}
		else
		{
			// The CSS-wide keywords and 'none' are not counter names.
			const String lower = StringUtilities::ToLower(token);
			if (!IsIdentifier(token) || lower == "none" || lower == "inherit" || lower == "initial" || lower == "unset" || lower == "default")
				return false;
			item.name = token;
		}

		if (i + 1 < tokens.size())
		{
			const String& next = tokens[i + 1];
			char* end = nullptr;
			const long number = strtol(next.c_str(), &end, 10);
			if (end && *end == '\0' && !next.empty())
			{
				item.value = (int)Math::Clamp(number, -2147483647L, 2147483647L);
				item.has_value = true;
				i++;
			}
		}
		list->items.push_back(std::move(item));
	}

	property.value = Variant(StyleValuePtr(std::move(list)));
	property.unit = Unit::STYLE_VALUE;
	return true;
}

bool PropertyParserListStyleType::ParseValue(Property& property, const String& value, const ParameterMap& parameters) const
{
	const String stripped = StringUtilities::StripWhitespace(value);
	if (stripped.empty())
		return false;
	if (stripped[0] == '"' || stripped[0] == '\'')
	{
		String text;
		if (!ReadStringToken(stripped, text))
			return false;
		property.value = Variant(text);
		property.unit = Unit::STRING;
		return true;
	}
	if (!IsIdentifier(stripped))
		return false;
	const String keyword = StringUtilities::ToLower(stripped);
	auto it = parameters.find(keyword);
	if (it == parameters.end())
	{
		// Another counter style: decimal, its fallback. The CSS-wide keywords never reach here.
		it = parameters.find("decimal");
		if (it == parameters.end())
			return false;
	}
	property.value = Variant(it->second);
	property.unit = Unit::KEYWORD;
	return true;
}

bool PropertyParserUrl::ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const
{
	// url(...), or as RCSS writes its other sources: a string or a bare path.
	const String stripped = StringUtilities::StripWhitespace(value);
	if (stripped.empty())
		return false;
	String url = stripped;
	String name;
	StringList arguments;
	if (ReadFunction(stripped, name, arguments))
	{
		if (name != "url" || arguments.size() != 1 || arguments[0].empty())
			return false;
		url = arguments[0];
	}
	else if (stripped.find_first_of(" \t\n()") != String::npos && stripped[0] != '"' && stripped[0] != '\'')
		return false;
	if (url[0] == '"' || url[0] == '\'')
	{
		String unquoted;
		if (!ReadStringToken(url, unquoted) || unquoted.empty())
			return false;
		url = std::move(unquoted);
	}
	property.value = Variant(url);
	property.unit = Unit::STRING;
	return true;
}

// The shorthand takes its image as url() only: a bare word there is a counter style.
static bool IsUrlFunction(const String& token)
{
	return token.size() > 4 && StringUtilities::ToLower(token.substr(0, 4)) == "url(";
}

bool ParseListStyleShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification)
{
	StringList tokens;
	SplitTokens(value, tokens);
	if (tokens.empty() || tokens.size() > 3)
		return false;

	const PropertyDefinition* type = specification.GetProperty(PropertyId::ListStyleType);
	const PropertyDefinition* position = specification.GetProperty(PropertyId::ListStylePosition);
	const PropertyDefinition* image = specification.GetProperty(PropertyId::ListStyleImage);
	if (!type || !position || !image)
		return false;

	specification.SetShorthandDefaults(dictionary, definition);
	bool has_type = false, has_position = false, has_image = false;
	int nones = 0;
	for (const String& token : tokens)
	{
		Property property;
		if (StringUtilities::ToLower(token) == "none")
		{
			// Assigned once the others are known: 'none' fills the type, then the image.
			nones++;
			continue;
		}
		if (!has_position && position->ParseValue(property, token))
		{
			dictionary.SetProperty(PropertyId::ListStylePosition, property);
			has_position = true;
		}
		else if (!has_image && IsUrlFunction(token) && image->ParseValue(property, token))
		{
			dictionary.SetProperty(PropertyId::ListStyleImage, property);
			has_image = true;
		}
		else if (!has_type && type->ParseValue(property, token))
		{
			dictionary.SetProperty(PropertyId::ListStyleType, property);
			has_type = true;
		}
		else
			return false;
	}
	for (int i = 0; i < nones; i++)
	{
		Property property;
		if (!has_type && type->ParseValue(property, "none"))
		{
			dictionary.SetProperty(PropertyId::ListStyleType, property);
			has_type = true;
		}
		else if (!has_image && image->ParseValue(property, "none"))
		{
			dictionary.SetProperty(PropertyId::ListStyleImage, property);
			has_image = true;
		}
		else
			return false;
	}
	return true;
}

String CounterListValue::ToString() const
{
	String result;
	for (const Item& item : items)
	{
		if (!result.empty())
			result += ' ';
		result += item.reversed ? "reversed(" + item.name + ")" : item.name;
		if (item.has_value)
			result += ' ' + Rml::ToString(item.value);
	}
	return result;
}

bool CounterListValue::Equals(const StyleValue& other) const
{
	return items == static_cast<const CounterListValue&>(other).items;
}

} // namespace Rml
