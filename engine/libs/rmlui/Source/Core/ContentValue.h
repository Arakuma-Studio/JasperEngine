#pragma once

#include "../../Include/RmlUi/Core/PropertyParser.h"
#include "../../Include/RmlUi/Core/PropertySpecification.h"
#include "../../Include/RmlUi/Core/StyleValue.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

/**
    The generated-content values of phase 3 (docs/html-ui/phase-3-fork-patches.md 3.6): 'content', 'quotes' and the
    counter properties, as StyleValues (Unit::STYLE_VALUE). 'normal'/'none'/'auto' are keywords of the properties.
 */

/// One item of a 'content' list.
struct ContentSegment {
	enum class Type : uint8_t { String, Attr, Url, Counter, Counters, OpenQuote, CloseQuote, NoOpenQuote, NoCloseQuote };
	Type type = Type::String;
	String text;      // String: the text (escapes decoded); Attr: the attribute name; Url: the URL; Counter(s): the counter
	String separator; // Counters: the string between the levels
	String style;     // Counter(s): the counter style ("decimal" when omitted)

	bool operator==(const ContentSegment& other) const
	{
		return type == other.type && text == other.text && separator == other.separator && style == other.style;
	}
};

/// 'content: <list>' (strings, attr(), url(), counter(), counters(), quote keywords); the alternative text after '/' is
/// dropped.
class ContentValue final : public StyleValue {
public:
	Vector<ContentSegment> segments;

	String ToString() const override;
	bool Equals(const StyleValue& other) const override;

	bool UsesCounters() const;
	bool UsesAttribute(const String& name) const;
	bool UsesAttributes() const;
	bool UsesQuotes() const;
};

/// 'quotes: [<string> <string>]+' (the open and close quote of each nesting level).
class QuotesValue final : public StyleValue {
public:
	Vector<Pair<String, String>> pairs;

	String ToString() const override;
	bool Equals(const StyleValue& other) const override;
};

/// 'counter-reset', 'counter-increment', 'counter-set': [<custom-ident> <integer>?]+. 'counter-reset' also takes
/// 'reversed(<custom-ident>)' (CSS Lists 3, reversed counters).
class CounterListValue final : public StyleValue {
public:
	struct Item {
		String name;
		int value = 0;
		bool has_value = false; // The integer was given (the default depends on the property).
		bool reversed = false;  // reversed(name), counter-reset only.
		bool operator==(const Item& other) const
		{
			return name == other.name && value == other.value && has_value == other.has_value && reversed == other.reversed;
		}
	};
	Vector<Item> items;

	String ToString() const override;
	bool Equals(const StyleValue& other) const override;
};

/// Parser of 'content' lists (the keywords 'normal' and 'none' are parsed by a keyword parser before it).
class PropertyParserContent : public PropertyParser {
public:
	bool ParseValue(Property& property, const String& value, const ParameterMap& parameters) const override;
};

/// Parser of 'quotes' pairs (the keywords 'auto' and 'none' come before it).
class PropertyParserQuotes : public PropertyParser {
public:
	bool ParseValue(Property& property, const String& value, const ParameterMap& parameters) const override;
};

/// Parser of counter lists (the keyword 'none' comes before it). Parameter "reversed": accept reversed(name).
class PropertyParserCounterList : public PropertyParser {
public:
	bool ParseValue(Property& property, const String& value, const ParameterMap& parameters) const override;
};

/// Parser of 'list-style-type': its keywords (by index in the keyword list given as parameters), a <string> (Unit::STRING,
/// escapes decoded), or another counter style name, which counts in decimal (the fallback of every counter style).
class PropertyParserListStyleType : public PropertyParser {
public:
	bool ParseValue(Property& property, const String& value, const ParameterMap& parameters) const override;
};

/// Parser of an image URL, 'url(...)' (Unit::STRING, the URL as written).
class PropertyParserUrl : public PropertyParser {
public:
	bool ParseValue(Property& property, const String& value, const ParameterMap& parameters) const override;
};

/// The 'list-style' shorthand (ShorthandType::Custom): list-style-type, list-style-position and list-style-image in any
/// order; 'none' sets the type when it is not given otherwise, else the image; omitted ones take their initial value.
bool ParseListStyleShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);

namespace ContentParsing {
	/// Reads a CSS string token at 'i' (a quote first): decodes the escapes (\hex, \char, an escaped newline) into UTF-8.
	/// @return False when it is not a string or is unterminated; on success 'i' is after the closing quote.
	bool ReadString(const String& value, size_t& i, String& result);
	/// Serializes a string as a double-quoted CSS string.
	String QuoteString(const String& text);
	/// Splits a value at top-level white space: strings and parenthesized groups are kept whole.
	void SplitTokens(const String& value, StringList& tokens);
} // namespace ContentParsing

} // namespace Rml
