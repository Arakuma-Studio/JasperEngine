// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TEXTSHAPER_H
#define TEXTSHAPER_H

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {

    class FontFace;

    // a glyph the shaper picked, already in visual order inside its line
    struct ShapedGlyph { uint16_t face; bool rtl; uint32_t glyphIndex; float xOffset, yOffset, xAdvance; size_t cluster; unsigned int line; };

    struct ShapeOptions {
        bool kerning = true;          // false: hb feature "kern" = 0
        float letterSpacing = 0.0f;   // pixels added to each glyph advance; != 0 also turns off liga/clig/dlig
        int baseDirection = 0;        // 0 auto (SBLevelDefaultLTR, today's behavior), 1 LTR, 2 RTL
        std::string language;         // empty: guessed as STBText does
    };

    // HarfBuzz + SheenBidi shaping over a fallback chain (moved out of STBText)
    class JASPERENGINE_API TextShaper {
    public:
        // faces = chain (primary first), scales[i] = pixels per font unit of face i
        static unsigned int selectFace(const std::vector<const FontFace*>& faces, uint32_t codepoint, unsigned int current);
        static void shapeRange(const std::vector<const FontFace*>& faces, const std::vector<float>& scales,
                               const std::vector<uint32_t>& codepoints, size_t offset, size_t length,
                               unsigned int line, const ShapeOptions& options, std::vector<ShapedGlyph>& out);
        // total advance of shapeRange
        static float measure(const std::vector<const FontFace*>& faces, const std::vector<float>& scales,
                             const std::vector<uint32_t>& codepoints, size_t offset, size_t length,
                             const ShapeOptions& options);
        static void decodeUtf8(const char* text, size_t length, std::vector<uint32_t>& codepoints);
    };

}

#endif //TEXTSHAPER_H
