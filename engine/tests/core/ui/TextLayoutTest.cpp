// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S93 R37 / AC43: native Text alignment and Auto Size. The line x offsets of Left, Center, Right
// and Justified, the vertical offsets, and the auto-size fit against a brute-force one over the
// built-in fonts' real layout (STBText::measure).

#include "util/STBText.h"
#include "util/StringUtils.h"
#include "util/TextAlignment.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    // glyphs of 10-px advances laid out from x = 0 on each line, as STBText does
    struct LaidOut {
        std::vector<TextGlyphInfo> glyphs;
        std::vector<uint32_t> codepoints;
    };

    // lines: the characters of each line in logical order; breaks: true where the line ends a
    // paragraph with a newline (else it is a wrap that swallowed a space)
    LaidOut layOut(const std::vector<std::string>& lines, const std::vector<bool>& newlineAfter){
        LaidOut out;
        for (size_t l = 0; l < lines.size(); l++){
            float pen = 0.0f;
            for (char c : lines[l]){
                TextGlyphInfo glyph;
                glyph.cluster = (uint32_t)out.codepoints.size();
                glyph.line = (uint32_t)l;
                glyph.penLeft = pen;
                glyph.penRight = pen + 10.0f;
                glyph.space = c == ' ';
                pen += 10.0f;
                out.glyphs.push_back(glyph);
                out.codepoints.push_back((uint32_t)(unsigned char)c);
            }
            if (l + 1 < lines.size())
                out.codepoints.push_back(newlineAfter[l] ? 10u : 32u);
        }
        return out;
    }

    // the offset of each line's first glyph
    std::vector<float> firstOffsets(const LaidOut& text, const std::vector<float>& offsets){
        std::vector<float> first;
        for (size_t i = 0; i < text.glyphs.size(); i++){
            if (text.glyphs[i].line >= first.size())
                first.push_back(offsets[i]);
        }
        return first;
    }

}

TEST(TextLayout, HorizontalAlignmentPerLine){
    // "abcd" (40 px) wraps before "ab" (20 px) and "a b c" (50 px, its last paragraph line),
    // in a box 100 px wide; the first line has a trailing space that does not count
    const LaidOut text = layOut({"abcd ", "ab", "a b c"}, {false, true});

    const std::vector<float> left = TextAlignment::horizontalOffsets(text.glyphs, text.codepoints, 100.0f, TextHorizontalAlignment::LEFT);
    for (float offset : left)
        EXPECT_EQ(offset, 0.0f);

    const std::vector<float> center = firstOffsets(text, TextAlignment::horizontalOffsets(text.glyphs, text.codepoints, 100.0f,
                                                                                        TextHorizontalAlignment::CENTER));
    ASSERT_EQ(center.size(), 3u);
    EXPECT_FLOAT_EQ(center[0], 30.0f);   // (100 - 40) / 2
    EXPECT_FLOAT_EQ(center[1], 40.0f);   // (100 - 20) / 2
    EXPECT_FLOAT_EQ(center[2], 25.0f);   // (100 - 50) / 2

    const std::vector<float> right = firstOffsets(text, TextAlignment::horizontalOffsets(text.glyphs, text.codepoints, 100.0f,
                                                                                       TextHorizontalAlignment::RIGHT));
    EXPECT_FLOAT_EQ(right[0], 60.0f);
    EXPECT_FLOAT_EQ(right[1], 80.0f);
    EXPECT_FLOAT_EQ(right[2], 50.0f);
}

TEST(TextLayout, JustifiedSpreadsWrappedLinesAtTheirSpaces){
    // "aa bb cc" (80 px, two inner spaces) wraps; "dd" ends the paragraph; "ee ff" ends the text
    const LaidOut text = layOut({"aa bb cc", "dd", "ee ff"}, {false, true});
    const std::vector<float> offsets = TextAlignment::horizontalOffsets(text.glyphs, text.codepoints, 120.0f,
                                                                        TextHorizontalAlignment::JUSTIFIED);
    // the 40 px left over go to the two gaps: the second word 20 px on, the third 40
    EXPECT_FLOAT_EQ(offsets[0], 0.0f);    // a
    EXPECT_FLOAT_EQ(offsets[2], 0.0f);    // the first space keeps its place
    EXPECT_FLOAT_EQ(offsets[3], 20.0f);   // b
    EXPECT_FLOAT_EQ(offsets[5], 20.0f);   // the second space
    EXPECT_FLOAT_EQ(offsets[6], 40.0f);   // c
    EXPECT_FLOAT_EQ(offsets[7], 40.0f);   // the line's last glyph ends on the box's right edge
    EXPECT_FLOAT_EQ(text.glyphs[7].penRight + offsets[7], 120.0f);
    // a paragraph's last line and the text's last line stay left
    for (size_t i = 8; i < offsets.size(); i++)
        EXPECT_EQ(offsets[i], 0.0f) << "glyph " << i;
}

TEST(TextLayout, VerticalAlignmentInTheBox){
    // two lines of 20 px, the baseline 15 px below a line's top, in a box 100 px high
    EXPECT_FLOAT_EQ(TextAlignment::verticalOffset(2, 20.0f, 15.0f, 100.0f, TextVerticalAlignment::TOP), 0.0f);
    EXPECT_FLOAT_EQ(TextAlignment::verticalOffset(2, 20.0f, 15.0f, 100.0f, TextVerticalAlignment::MIDDLE), 30.0f);
    EXPECT_FLOAT_EQ(TextAlignment::verticalOffset(2, 20.0f, 15.0f, 100.0f, TextVerticalAlignment::BOTTOM), 60.0f);
    // the first baseline on the middle: 50 - 15
    EXPECT_FLOAT_EQ(TextAlignment::verticalOffset(2, 20.0f, 15.0f, 100.0f, TextVerticalAlignment::BASELINE), 35.0f);
}

TEST(TextLayout, AutoSizeFitsFixedBox){
    STBText font;
    ASSERT_NE(font.load("", {}, 20), nullptr) << "the built-in fonts";

    struct Case { std::string text; unsigned int width, height; bool multiline; };
    const std::vector<Case> cases = {
        {"GAME OVER", 200, 40, false},
        {"Press any key to continue the adventure", 160, 90, true},
        {"A", 300, 300, true},
        {"Supercalifragilistic", 50, 100, true},
    };
    for (const Case& c : cases){
        auto fits = [&](unsigned int size){
            float width = 0.0f, height = 0.0f;
            font.measure(c.text, size, c.width, true, c.multiline, width, height);
            return width <= (float)c.width && height <= (float)c.height;
        };
        // brute force: the largest whole size that fits, else the minimum
        unsigned int expected = 10;
        for (unsigned int size = 10; size <= 72; size++){
            if (fits(size))
                expected = size;
        }
        EXPECT_EQ(TextAlignment::fitSize(10, 72, fits), expected) << c.text;
        // the result does fit (unless nothing does), the next size does not
        const unsigned int fitted = TextAlignment::fitSize(10, 72, fits);
        if (fitted < 72){
            EXPECT_FALSE(fits(fitted + 1)) << c.text;
        }
    }

    // the measure follows the size: twice the size, about twice the width
    float small = 0.0f, large = 0.0f, h = 0.0f;
    font.measure("HELLO", 20, 0, false, false, small, h);
    font.measure("HELLO", 40, 0, false, false, large, h);
    EXPECT_NEAR(large, 2.0f * small, 2.0f);
    EXPECT_GT(small, 0.0f);
}
