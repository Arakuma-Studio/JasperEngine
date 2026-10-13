#pragma once

#include "StyleTypes.h"
#include "Types.h"

namespace Rml {

/*
    Data extracted from the properties of an element to help provide context for text shaping and spacing.
*/
struct TextShapingContext {
	const String& language;
	Style::Direction text_direction = Style::Direction::Auto;
	Style::FontKerning font_kerning = Style::FontKerning::Auto;
	float letter_spacing = 0.0f; // Measured in pixels.
	float word_spacing = 0.0f;   // Measured in pixels, added to the advance of each space: U+0020, U+00A0 and U+1361 (CSS Text 3 §8.1).
};

} // namespace Rml
