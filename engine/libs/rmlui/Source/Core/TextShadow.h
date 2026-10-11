#pragma once

#include "../../Include/RmlUi/Core/PropertyParser.h"
#include "../../Include/RmlUi/Core/StyleValue.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class Element;

/*
    'text-shadow' (CSS Text Decoration 3 §4; phase 3, p3-text): a list of shadows, each 'offset-x offset-y blur? color?'.
    The value of the property is a TextShadowListValue (Unit::STYLE_VALUE), or the keyword 'none'. ElementText draws the
    shadows behind its text, the first one on top, a blurred one through a layer and the 'blur' filter.
*/

struct TextShadow {
	LengthPercentageValue offset_x, offset_y, blur;
	Colourb colour;
	bool current_colour = true; // no colour given, or 'currentcolor': the text's colour
	String colour_mix;          // with current_colour: a color-mix() of 'currentcolor' (phase 3, values)

	bool operator==(const TextShadow& other) const
	{
		return offset_x == other.offset_x && offset_y == other.offset_y && blur == other.blur && current_colour == other.current_colour &&
			colour_mix == other.colour_mix && (current_colour || colour == other.colour);
	}
};

class TextShadowListValue final : public StyleValue {
public:
	Vector<TextShadow> shadows;

	String ToString() const override;
	bool Equals(const StyleValue& other) const override;
	SharedPtr<const StyleValue> Interpolate(const StyleValue& to, float alpha, Element& element) const override;
	Units GetUnitDependencies() const override;
};

// A shadow resolved for an element: pixels, and the colour with 'currentcolor' replaced by the element's colour.
struct ResolvedTextShadow {
	Vector2f offset;
	float blur = 0.f; // the CSS blur radius; the Gaussian's standard deviation is half of it
	Colourb colour;
};

/// The element's text shadows, in the order they are listed (the first one is painted on top). Empty for 'none'.
Vector<ResolvedTextShadow> ResolveTextShadows(Element& element);

/// The parser "text_shadow" (a comma list of shadows; 'none' is the property's keyword).
PropertyParser& GetTextShadowParser();

} // namespace Rml
