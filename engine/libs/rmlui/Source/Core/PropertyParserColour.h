#pragma once

#include "../../Include/RmlUi/Core/PropertyParser.h"
#include "../../Include/RmlUi/Core/Types.h"
#include "ControlledLifetimeResource.h"

namespace Rml {

namespace ColorSpaces {
	struct Color;
}

/**
    A property parser that parses a colour value: CSS Color 4 and 5 (phase 3, values). Hex colors, the CSS named colors
    and 'transparent', rgb()/rgba()/hsl()/hsla() in the comma and the space syntax, hwb(), lab(), lch(), oklab(), oklch(),
    color() in the srgb, srgb-linear and xyz spaces, and color-mix(), all case-insensitive. Components out of range are
    clamped, 'none' is zero, the alpha is a number from 0 to 1 or a percentage.
 */

class PropertyParserColour : public PropertyParser {
public:
	PropertyParserColour();
	virtual ~PropertyParserColour();

	/// Called to parse a RCSS colour declaration.
	/// @param[out] property The property to set the parsed value on.
	/// @param[in] value The raw value defined for this property.
	/// @param[in] parameters The parameters defined for this property; not used for this parser.
	/// @return True if the value was parsed successfully, false otherwise.
	bool ParseValue(Property& property, const String& value, const ParameterMap& parameters) const override;

	/// Parse a colour directly.
	static bool ParseColour(Colourb& colour, const String& value);

	static void Initialize();
	static void Shutdown();

private:
	static ControlledLifetimeResource<struct PropertyParserColourData> parser_data;

	// Parses any colour into its color space; 'depth' limits nested color-mix().
	static bool ParseColor(ColorSpaces::Color& color, const String& value, int depth);
	static bool ParseHexColour(Colourb& colour, const String& value);
	static bool ParseColorFunction(ColorSpaces::Color& color, const String& name, const String& arguments, int depth);
	static bool ParseColorMix(ColorSpaces::Color& color, const String& arguments, int depth);
};

} // namespace Rml
