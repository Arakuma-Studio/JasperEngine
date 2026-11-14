// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TEXTALIGNMENT_H
#define TEXTALIGNMENT_H

// Native Text alignment and Auto Size (S93 R37, D15), TextMeshPro's names: pure layout math over
// the glyph quads STBText and BitmapText lay out from the left and the top (UISystem applies it).

#include "Export.h"
#include "util/TextGlyphInfo.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace jasperengine {

    // each line inside a fixed width; Justified spreads a wrapped line over the width at its
    // spaces (a paragraph's last line, and a line without spaces, stay left)
    enum class TextHorizontalAlignment { LEFT, CENTER, RIGHT, JUSTIFIED };
    // the lines inside a fixed height; Baseline puts the first line's baseline on the middle
    enum class TextVerticalAlignment { TOP, MIDDLE, BOTTOM, BASELINE };

    class JASPERENGINE_API TextAlignment {
    public:
        // The x offset of each glyph (glyphs[i] is the quad's info: its line, its pen span, its
        // character), in a box boxWidth wide. A line spans its glyphs' pens, without its trailing
        // spaces. codepoints: the text, which says where a paragraph ends (a newline or the end
        // after a line's last character). LEFT gives 0 everywhere.
        static std::vector<float> horizontalOffsets(const std::vector<TextGlyphInfo>& glyphs, const std::vector<uint32_t>& codepoints,
                                                    float boxWidth, TextHorizontalAlignment alignment);
        // The y offset (down) of the lines of lineHeight, the first baseline ascent below their
        // top, in a box boxHeight high. TOP gives 0.
        static float verticalOffset(unsigned int lineCount, float lineHeight, float ascent, float boxHeight, TextVerticalAlignment alignment);
        // The largest whole size in [minSize, maxSize] that fits (a bounded binary search: fits
        // grows false with the size); minSize when none does
        static unsigned int fitSize(unsigned int minSize, unsigned int maxSize, const std::function<bool(unsigned int)>& fits);
    };

}

#endif //TEXTALIGNMENT_H
