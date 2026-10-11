#include "PropertyParserFontFamily.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/PropertySpecification.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"

namespace Rml {

static bool IsSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// One family: a quoted string (escapes taken literally), or unquoted words whose white space collapses to one space.
static bool CanonicalFamily(const String& item, String& family)
{
	family.clear();
	if (item.empty())
		return false;

	const char first = item[0];
	if (first == '"' || first == '\'')
	{
		size_t i = 1;
		for (; i < item.size(); i++)
		{
			const char c = item[i];
			if (c == '\\' && i + 1 < item.size())
				family += item[++i];
			else if (c == first)
				break;
			else
				family += c;
		}
		// Nothing may follow the closing quote.
		if (i != item.size() - 1)
			return false;
	}
	else
	{
		bool pending_space = false;
		for (const char c : item)
		{
			if (IsSpace(c))
			{
				pending_space = !family.empty();
				continue;
			}
			// Unquoted names are identifiers: no quotes or parentheses inside.
			if (c == '"' || c == '\'' || c == '(' || c == ')' || c == ';')
				return false;
			if (pending_space)
				family += ' ';
			pending_space = false;
			family += c;
		}
	}

	// White space collapses (quoted names too: the font engine matches names by their canonical form).
	String collapsed;
	bool pending_space = false;
	for (const char c : family)
	{
		if (IsSpace(c))
		{
			pending_space = !collapsed.empty();
			continue;
		}
		if (pending_space)
			collapsed += ' ';
		pending_space = false;
		collapsed += c;
	}
	family = StringUtilities::ToLower(std::move(collapsed));
	// The list separator cannot be part of a canonical name.
	return !family.empty() && family.find(',') == String::npos;
}

bool PropertyParserFontFamily::CanonicalFamilyList(const String& value, String& canonical)
{
	canonical.clear();
	StringList items;
	SplitCommaList(value, items);
	if (items.empty())
		return false;

	String family;
	for (const String& item : items)
	{
		if (!CanonicalFamily(item, family))
			return false;
		if (!canonical.empty())
			canonical += ',';
		canonical += family;
	}
	return true;
}

bool PropertyParserFontFamily::ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const
{
	// The registered initial value is empty: the font engine's default font.
	String canonical;
	if (!StringUtilities::StripWhitespace(value).empty() && !CanonicalFamilyList(value, canonical))
		return false;
	property.value = Variant(std::move(canonical));
	property.unit = Unit::STRING;
	return true;
}

} // namespace Rml
