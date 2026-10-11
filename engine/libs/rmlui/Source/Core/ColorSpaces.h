#pragma once

#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

/**
    CSS Color 4 color spaces and CSS Color 5 color-mix(), for the colour parser (phase 3, values).

    Components are in the units of each space: sRGB and linear sRGB 0..1, HSL hue in degrees with saturation and lightness
    0..1, HWB hue in degrees with whiteness and blackness 0..1, CIE Lab/LCH lightness 0..100, Oklab/Oklch lightness 0..1,
    hues of the polar spaces in degrees, XYZ with Y 0..1. Conversions follow the sample code of CSS Color 4 section 18.
 */

namespace ColorSpaces {

	enum class Space { SRGB, SRGBLinear, HSL, HWB, Lab, LCH, Oklab, Oklch, XYZD50, XYZD65 };
	enum class HueInterpolation { Shorter, Longer, Increasing, Decreasing };

	struct Color {
		Space space = Space::SRGB;
		float components[3] = {};
		bool missing[3] = {}; // 'none' components, and powerless hues
		float alpha = 1.f;
		bool alpha_missing = false;
	};

	// Parses a color space name of color-mix() ("srgb", "oklch", ...).
	bool ParseSpace(const String& name, Space& space);
	bool IsPolar(Space space);
	// The index of the hue component of a polar space.
	int HueIndex(Space space);

	// Converts a color to another space. Missing components count as zero, except that a powerless or missing hue stays
	// missing in a polar destination.
	Color Convert(const Color& color, Space destination);

	// Gamut-clips to sRGB and rounds to 8 bits per channel.
	Colourb ToColourb(const Color& color);
	Color FromColourb(Colourb colour);

	// CSS Color 5 color-mix(): mixes 'a' with weight 'weight_a' (0..1) and 'b' with 1 - weight_a in 'space', the result's
	// alpha multiplied by 'alpha_multiplier' (the sum of the percentages when it is below 100%).
	Color Mix(Space space, HueInterpolation hue_interpolation, const Color& a, const Color& b, float weight_a, float alpha_multiplier);

} // namespace ColorSpaces
} // namespace Rml
