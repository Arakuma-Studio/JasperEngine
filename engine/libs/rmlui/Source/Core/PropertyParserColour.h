#pragma once

#include "../../Include/RmlUi/Core/PropertyParser.h"
#include "../../Include/RmlUi/Core/StyleValue.h"
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

	/// Parse a colour directly. 'currentcolor' is not a colour here: it has no value without an element.
	static bool ParseColour(Colourb& colour, const String& value);

	/// The keyword value 'currentcolor' parses to (Unit::KEYWORD) where a property, a box shadow or a color stop takes a
	/// colour: the element's 'color', resolved when the values are computed (ElementStyle) or the shadow and gradient are
	/// generated. No keyword list uses this value.
	static constexpr int CurrentColor = 0x43430000;
	static bool IsCurrentColor(const Property& property);

	/// A color-mix() with a 'currentcolor' operand (CSS Color 5) has no value without an element either: it parses to a
	/// CurrentColorMixValue (Unit::STYLE_VALUE) and is resolved where 'currentcolor' is; a box shadow, a color stop or a
	/// text shadow keeps its text with the currentcolor flag.
	static bool IsCurrentColorMix(const Property& property);
	/// 'currentcolor' or a color-mix() of it.
	static bool DependsOnCurrentColor(const Property& property);
	/// The colour of a colour property for an element whose 'color' is 'current_color'.
	static Colourb Resolve(const Property& property, Colourb current_color);
	/// The colour of a 'currentcolor' or of the text of a color-mix() of it ('mix' empty: 'currentcolor' itself).
	static Colourb ResolveCurrentColor(const String& mix, Colourb current_color);

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

/// The value of a colour property that is a color-mix() with a 'currentcolor' operand.
class CurrentColorMixValue final : public StyleValue {
public:
	RMLUI_RTTI_DeclareWithParent(CurrentColorMixValue, StyleValue)

	explicit CurrentColorMixValue(String text) : text(std::move(text)) {}
	String text; // the color-mix() as written (lower case)

	String ToString() const override { return text; }
	bool Equals(const StyleValue& other) const override { return text == static_cast<const CurrentColorMixValue&>(other).text; }
};

} // namespace Rml
