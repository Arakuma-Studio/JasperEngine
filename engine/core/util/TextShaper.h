// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TEXTSHAPER_H
#define TEXTSHAPER_H

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

//HarfBuzz font, opaque as in FontFace.h
struct hb_font_t;

namespace jasperengine {

    class FontFace;

    // a glyph the shaper picked, already in visual order inside its line; offsets and
    // advance in pixels (font units times the face's scale), cluster = index of the first
    // codepoint it came from (shared when a ligature covers several). yAdvance is 0 in
    // horizontal text; vertical text (ShapeDirection::Vertical) advances down by -yAdvance
    // (HarfBuzz's y-up sign) and its offsets place the glyph's horizontal origin.
    struct ShapedGlyph { uint16_t face; bool rtl; uint32_t glyphIndex; float xOffset, yOffset, xAdvance; size_t cluster; unsigned int line; float yAdvance = 0.0f; };

    // HarfBuzz's direction for a range: left to right / bidi (today's), or top to bottom (TTB:
    // the font's vertical forms through vert/vrt2 and its vertical metrics, no bidi reordering)
    enum class ShapeDirection : uint8_t { Horizontal, Vertical };

    // an OpenType feature over the whole range, on top of the defaults (`value` 0 turns one off)
    struct ShapeFeature { uint32_t tag; uint32_t value; };

    struct ShapeOptions {
        bool kerning = true;          // false: hb feature "kern" = 0
        float letterSpacing = 0.0f;   // pixels added after each cluster (not after a default-ignorable one); != 0 also turns off liga/clig/dlig
        float wordSpacing = 0.0f;     // pixels added after each space: U+0020, U+00A0, U+1361 (CSS Text 3 §8.1)
        int baseDirection = 0;        // 0 auto (SBLevelDefaultLTR, today's behavior), 1 LTR, 2 RTL
        std::string language;         // BCP 47; empty: guessed by HarfBuzz from the script, as STBText does
        // a justified line (RmlUi's TextShapingContext::justification): per codepoint of the shaped
        // vector (empty: none), the space added to its cluster's advance and the part of it its
        // glyphs move right by (the space before it)
        std::vector<float> justifyAdvance;
        std::vector<float> justifyOffset;
        ShapeDirection direction = ShapeDirection::Horizontal;
        std::vector<ShapeFeature> features;   // after kerning's and letter spacing's own, so they win
        // per face of the chain (empty, or a null entry: the face's own FontFace::getHbFont()), the
        // font to shape with instead: a sub-font of that face with variation coordinates (Title
        // Art's variable fonts), scaled like the face's (font units)
        std::vector<hb_font_t*> fonts;
    };

    // HarfBuzz + SheenBidi shaping over a fallback chain (moved out of STBText): bidi runs
    // in visual order, each split again per face (the first face covering each grapheme
    // cluster, as Chromium falls back: after a fallback, the primary again) and per script, so
    // letters only join inside their own font and script.
    class JASPERENGINE_API TextShaper {
    public:
        // faces = chain (primary first), scales[i] = pixels per font unit of face i.
        // A default-ignorable codepoint (zero-width space, joiners, word joiner, soft hyphen,
        // variation selectors, bidi controls) keeps the current face: it draws nothing and
        // takes no space, so a face without a glyph for it is no reason to switch fonts.
        // Any other codepoint takes the first face in the chain that has it (none: the primary,
        // its .notdef box).
        static unsigned int selectFace(const std::vector<const FontFace*>& faces, uint32_t codepoint, unsigned int current);
        // The face of the cluster codepoints[begin, end): the first face that has all of its
        // visible codepoints (none: the face of its first one); only default-ignorables: `current`.
        static unsigned int selectClusterFace(const std::vector<const FontFace*>& faces, const std::vector<uint32_t>& codepoints,
                                              size_t begin, size_t end, unsigned int current);
        // The end of the grapheme cluster that starts at `begin` (UAX #29's extended grapheme
        // clusters as font fallback needs them: marks, joiners, variation selectors, emoji
        // modifiers and ZWJ sequences, flag pairs, Hangul jamo, CR LF), at most `end`.
        static size_t clusterEnd(const std::vector<uint32_t>& codepoints, size_t begin, size_t end);
        // appends the glyphs of codepoints[offset, offset + length) to `out`, left to right
        static void shapeRange(const std::vector<const FontFace*>& faces, const std::vector<float>& scales,
                               const std::vector<uint32_t>& codepoints, size_t offset, size_t length,
                               unsigned int line, const ShapeOptions& options, std::vector<ShapedGlyph>& out);
        // total advance of shapeRange
        static float measure(const std::vector<const FontFace*>& faces, const std::vector<float>& scales,
                             const std::vector<uint32_t>& codepoints, size_t offset, size_t length,
                             const ShapeOptions& options);
        // Unicode's Default_Ignorable_Code_Point (DerivedCoreProperties.txt, Unicode 15.1):
        // invisible, zero advance (HarfBuzz hides them in any face that has a space glyph)
        static bool isDefaultIgnorable(uint32_t codepoint);
        // invalid sequences become U+FFFD
        static void decodeUtf8(const char* text, size_t length, std::vector<uint32_t>& codepoints);
    };

}

#endif //TEXTSHAPER_H
