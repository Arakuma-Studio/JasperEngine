#pragma once

#include "StyleTypes.h"
#include "Types.h"

namespace Rml {

/*
    The space a justified line adds around one character (p6-justify: 'text-align: justify', CSS Text 3 §7.3, as Chromium's
    ShapeResult::ApplyExpansion places it). The character starting at the byte 'offset' of the string advances 'before' +
    'after' further, and its glyphs are drawn 'before' to the right of where they would be.
*/
struct JustificationSpacing {
	int offset = 0;
	float before = 0.f;
	float after = 0.f;
};

/*
    A directional run of a string as a font engine lays it out (fork, P6-L1: the line-clamp ellipsis cuts the runs as
    Chromium's LineTruncator cuts a line's items): the bytes [begin, end) of the string, right to left when its bidi
    embedding level is odd.
*/
struct TextBidiRun {
	int begin = 0;
	int end = 0;
	bool rtl = false;
};

/*
    Data extracted from the properties of an element to help provide context for text shaping and spacing.
*/
struct TextShapingContext {
	const String& language;
	Style::Direction text_direction = Style::Direction::Auto;
	Style::FontKerning font_kerning = Style::FontKerning::Auto;
	float letter_spacing = 0.0f; // Measured in pixels.
	float word_spacing = 0.0f;   // Measured in pixels, added to the advance of each space: U+0020, U+00A0 and U+1361 (CSS Text 3 §8.1).
	// The space of a justified line, by the byte offset of the character it follows (sorted by offset, at most one entry per
	// character, only at the first code point of a grapheme cluster; empty when the string is not justified). A font engine
	// applies it in GetStringWidth and GenerateString.
	Span<const JustificationSpacing> justification = {};
	// A per-character text animation (fork, T21-2): the state the engine's text effect provider gave the text (null: none,
	// see ElementText::SetTextEffectProvider), and the index of the first visible character (not white space) of the string
	// in the text's inline formatting context, so an effect runs on across elements and lines.
	const void* text_effect = nullptr;
	int glyph_index_base = 0;
};

} // namespace Rml
