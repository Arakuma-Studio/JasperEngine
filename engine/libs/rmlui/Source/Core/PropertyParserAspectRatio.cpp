#include "PropertyParserAspectRatio.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/PropertySpecification.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include <cmath>
#include <stdlib.h>

namespace Rml {

static bool ParseNonNegative(const String& token, float& value)
{
	char* end = nullptr;
	value = strtof(token.c_str(), &end);
	return !token.empty() && end == token.c_str() + token.size() && std::isfinite(value) && value >= 0.f;
}

bool PropertyParserAspectRatio::ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const
{
	// Tokens with '/' on its own: '16/9', '16 / 9', '16/ 9'.
	String spaced;
	for (const char c : value)
	{
		if (c == '/')
			spaced += " / ";
		else
			spaced += c;
	}
	StringList tokens;
	SplitWhitespaceList(spaced, tokens);

	bool has_auto = false;
	bool has_ratio = false;
	float width = 0.f, height = 1.f;
	for (size_t i = 0; i < tokens.size(); i++)
	{
		if (StringUtilities::ToLower(tokens[i]) == "auto")
		{
			if (has_auto)
				return false;
			has_auto = true;
			continue;
		}
		if (has_ratio || !ParseNonNegative(tokens[i], width))
			return false;
		has_ratio = true;
		height = 1.f;
		if (i + 1 < tokens.size() && tokens[i + 1] == "/")
		{
			if (i + 2 >= tokens.size() || !ParseNonNegative(tokens[i + 2], height))
				return false;
			i += 2;
		}
	}

	if (!has_ratio)
	{
		if (!has_auto)
			return false;
		property.value = Variant(0);
		property.unit = Unit::KEYWORD;
		return true;
	}
	property.value = Variant(has_auto ? Vector2f(-width, -height) : Vector2f(width, height));
	property.unit = Unit::RATIO;
	return true;
}

} // namespace Rml
