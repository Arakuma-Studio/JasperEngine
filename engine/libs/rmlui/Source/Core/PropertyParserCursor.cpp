#include "PropertyParserCursor.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/PropertySpecification.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include <stdlib.h>

namespace Rml {

bool PropertyParserCursor::IsCursorKeyword(const String& keyword)
{
	static const char* keywords[] = {"auto", "default", "none", "context-menu", "help", "pointer", "progress", "wait", "cell", "crosshair",
		"text", "vertical-text", "alias", "copy", "move", "no-drop", "not-allowed", "grab", "grabbing", "e-resize", "n-resize", "ne-resize",
		"nw-resize", "s-resize", "se-resize", "sw-resize", "w-resize", "ew-resize", "ns-resize", "nesw-resize", "nwse-resize", "col-resize",
		"row-resize", "all-scroll", "zoom-in", "zoom-out"};
	for (const char* candidate : keywords)
		if (keyword == candidate)
			return true;
	return false;
}

static bool IsNumber(const String& token)
{
	char* end = nullptr;
	strtof(token.c_str(), &end);
	return !token.empty() && end == token.c_str() + token.size();
}

bool PropertyParserCursor::ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const
{
	// The registered initial value is empty: the system's default cursor.
	if (StringUtilities::StripWhitespace(value).empty())
	{
		property.value = Variant(String());
		property.unit = Unit::STRING;
		return true;
	}

	StringList entries;
	SplitCommaList(value, entries);
	if (entries.empty())
		return false;

	// Image entries first: url(...) or image-set(...), with an optional hotspot of two numbers; they are dropped.
	size_t i = 0;
	for (; i < entries.size(); i++)
	{
		StringList parts;
		SplitWhitespaceList(entries[i], parts);
		if (parts.empty())
			return false;
		const String function = StringUtilities::ToLower(parts[0]);
		if (!StringUtilities::StartsWith(function, "url(") && !StringUtilities::StartsWith(function, "image-set("))
			break;
		if (function.back() != ')' || parts.size() == 2 || parts.size() > 3)
			return false;
		if (parts.size() == 3 && (!IsNumber(parts[1]) || !IsNumber(parts[2])))
			return false;
	}
	if (i == entries.size())
		return false;

	// Then the keywords: CSS takes one; more are fallbacks, the system interface takes the first it knows. A CSS
	// keyword is lowercase, a name of the application's own cursors (an identifier: 'arrow', 'resize') as written.
	String result;
	for (; i < entries.size(); i++)
	{
		const String name = StringUtilities::StripWhitespace(entries[i]);
		const String keyword = StringUtilities::ToLower(name);
		if (!IsCursorKeyword(keyword))
		{
			if (name.empty() || (name[0] >= '0' && name[0] <= '9'))
				return false;
			for (const char c : name)
			{
				if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
					return false;
			}
		}
		result += (result.empty() ? "" : ", ") + (IsCursorKeyword(keyword) ? keyword : name);
	}

	property.value = Variant(std::move(result));
	property.unit = Unit::STRING;
	return true;
}

} // namespace Rml
