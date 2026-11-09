// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S51 AC7 (the pixel scale rule), AC12 (layout against hand-computed positions) and AC13 (the
// fallback chain): BitmapText's layout and quads without a scene. The Text component's side
// (UISystem, the styled layers, NEAREST and no SDF) is editor/tests/pixelart/BitmapTextComponentTest.cpp.

#include "pixelart/BitmapFont.h"
#include "pixelart/BitmapFontPool.h"
#include "pixelart/BitmapText.h"
#include "pixelart/FontPixelReport.h"
#include "buffer/InterleavedBuffer.h"
#include "TempDir.h"
#include "TestPaths.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <fstream>
#include <set>

using namespace jasperengine;
using namespace jasperengine::pixelart;

namespace {

    std::string btFixture(const std::string& name){
        return test::testPaths().assetDir + "/fonts/" + name;
    }

    // the grid fixture's chars with every cell not in `keep` skipped (U+FFFF), as JSON
    std::string btCharsKeeping(const std::string& keep){
        std::string out;
        for (int c = 32; c < 127; c++){
            if (keep.find((char)c) != std::string::npos){
                if (c == '"' || c == '\\') out += '\\';
                out += (char)c;
            }else{
                out += "\\uffff";
            }
        }
        return out;
    }

    std::string btSettings(const std::string& chars, const std::string& extra){
        return "{\"format\": \"jasperengine.bitmapfont\", \"version\": 1, \"source\": {\"type\": \"grid\", \"image\": \"" + btFixture("grid@6x9.png") +
               "\", \"chars\": \"" + chars + "\"}" + (extra.empty() ? std::string() : ", " + extra) + "}";
    }

    std::shared_ptr<BitmapFont> btFont(const std::string& extra = ""){
        BitmapFontReport report;
        std::shared_ptr<BitmapFont> font = BitmapFont::loadSettings(btFixture("layout.bitmapfont"), btSettings("ascii", extra), &report);
        EXPECT_TRUE(font) << (report.errors.empty() ? "" : report.errors[0].message);
        return font;
    }

    int btAdvance(char c){
        const int i = c - 32;
        return std::min(4, i % 2 + (i * 7) % 5) + 1;
    }

    struct BtLayout {
        InterleavedBuffer buffer;
        std::vector<uint16_t> indices;
        std::vector<Vector2> positions;
        std::vector<CharExtent> extents;
        std::vector<TextGlyphInfo> info;
        unsigned int width = 0, height = 0;

        BtLayout(){
            buffer.addAttribute(AttributeType::POSITION, 3);
            buffer.addAttribute(AttributeType::TEXCOORD1, 2);
        }
        // the quad of glyph g: x0, y0, x1, y1
        std::array<float, 4> quad(size_t g){
            Attribute* p = buffer.getAttribute(AttributeType::POSITION);
            const Vector3 a = buffer.getVector3(p, (unsigned int)(g * 4));
            const Vector3 c = buffer.getVector3(p, (unsigned int)(g * 4 + 2));
            return {a.x, a.y, c.x, c.y};
        }
    };

    void btLayout(BitmapText& text, const std::string& s, BtLayout& out, const BitmapTextOptions& options){
        text.createText(s, &out.buffer, out.indices, out.positions, out.extents, out.width, out.height, options, &out.info);
    }

}

TEST(BitmapText, Scale){
    // R7: k = max(1, round(fontSize / nativeSize))
    EXPECT_EQ(pixelScaleFor(18, 9), 2);
    EXPECT_EQ(pixelScaleFor(16, 9), 2);
    EXPECT_EQ(pixelScaleFor(4, 9), 1);
    EXPECT_EQ(pixelScaleFor(13, 9), 1);
    EXPECT_EQ(pixelScaleFor(27, 9), 3);
    EXPECT_EQ(pixelScaleFor(0, 9), 1);
    // every quad corner is a whole number of font pixels times the unit
    std::shared_ptr<BitmapFont> font = btFont();
    ASSERT_TRUE(font);
    BitmapText text(font, "layout", 0);
    for (float unit : {1.0f, 2.0f, 3.0f, 1.459854f}){
        BtLayout layout;
        BitmapTextOptions options;
        options.unit = unit;
        btLayout(text, "Hello, World\nAgain", layout, options);
        Attribute* p = layout.buffer.getAttribute(AttributeType::POSITION);
        for (unsigned int v = 0; v < p->getCount(); v++){
            const Vector3 pos = layout.buffer.getVector3(p, v);
            EXPECT_NEAR(pos.x / unit, std::round(pos.x / unit), 1e-4f) << unit;
            EXPECT_NEAR(pos.y / unit, std::round(pos.y / unit), 1e-4f) << unit;
        }
    }
}

TEST(BitmapText, LayoutOneLineWithKerningAndCaretExtents){
    std::shared_ptr<BitmapFont> font = btFont("\"kerning\": [[\"A\", \"V\", -1]]");
    ASSERT_TRUE(font);
    BitmapText text(font, "layout", 0);
    BtLayout layout;
    btLayout(text, "AVB", layout, BitmapTextOptions());
    // pens: A 0; V adv(A) + 1 - 1; B after V + 1
    const int penV = btAdvance('A') + 1 - 1;
    const int penB = penV + btAdvance('V') + 1;
    ASSERT_EQ(layout.positions.size(), 3u);
    EXPECT_FLOAT_EQ(layout.positions[0].x, (float)btAdvance('A'));
    EXPECT_FLOAT_EQ(layout.positions[1].x, (float)(penV + btAdvance('V')));
    EXPECT_FLOAT_EQ(layout.positions[2].x, (float)(penB + btAdvance('B')));
    EXPECT_FLOAT_EQ(layout.extents[1].left, (float)penV);
    EXPECT_FLOAT_EQ(layout.extents[1].right, (float)(penV + btAdvance('V')));
    EXPECT_EQ(layout.width, (unsigned int)(penB + btAdvance('B')));
    EXPECT_EQ(layout.height, 10u);
    // 'V' (i 54): ink from column 0, rows 1 + 54 % 3 = 1 .. 6; y from the baseline (7)
    const std::array<float, 4> q = layout.quad(1);
    EXPECT_FLOAT_EQ(q[0], (float)penV);
    EXPECT_FLOAT_EQ(q[1], 1.0f - 7.0f);
    EXPECT_FLOAT_EQ(q[3], 7.0f - 7.0f);
    ASSERT_EQ(layout.info.size(), 3u);
    EXPECT_EQ(layout.info[2].cluster, 2u);
    EXPECT_FALSE(layout.info[0].back);
}

TEST(BitmapText, LayoutNewlinesAndLineHeight){
    std::shared_ptr<BitmapFont> font = btFont();
    ASSERT_TRUE(font);
    BitmapText text(font, "layout", 0);
    BtLayout layout;
    btLayout(text, "AB\nC", layout, BitmapTextOptions());
    ASSERT_EQ(layout.positions.size(), 3u);   // the newline has no entry
    EXPECT_FLOAT_EQ(layout.positions[2].x, (float)btAdvance('C'));
    EXPECT_FLOAT_EQ(layout.positions[2].y, 10.0f);
    EXPECT_FLOAT_EQ(layout.extents[2].y, 10.0f);
    EXPECT_EQ(layout.height, 20u);
    EXPECT_EQ(layout.width, (unsigned int)(btAdvance('A') + 1 + btAdvance('B')));
    ASSERT_EQ(layout.info.size(), 3u);
    EXPECT_EQ(layout.info[2].line, 1u);
    EXPECT_EQ(layout.info[2].cluster, 3u);   // codepoint index, the newline counted
    // 'C' (i 35): rows 1 + 35 % 3 = 3 .. 6, on the second line
    const std::array<float, 4> q = layout.quad(2);
    EXPECT_FLOAT_EQ(q[1], 10.0f + 3.0f - 7.0f);
}

TEST(BitmapText, LayoutWrapsAtFixedWidthOnSpaces){
    std::shared_ptr<BitmapFont> font = btFont();
    ASSERT_TRUE(font);
    BitmapText text(font, "layout", 0);
    BtLayout layout;
    BitmapTextOptions options;
    options.fixedWidth = true;
    options.multiline = true;
    layout.width = (unsigned int)(2 * btAdvance('A') + 1);   // "AA" fits, "AA A" does not
    btLayout(text, "AA AA", layout, options);
    ASSERT_EQ(layout.positions.size(), 5u);
    EXPECT_FLOAT_EQ(layout.positions[4].y, 10.0f);
    EXPECT_FLOAT_EQ(layout.extents[3].left, 0.0f);   // the second line starts at the pen's origin
    EXPECT_EQ(layout.height, 20u);
    // the break's space draws nothing and sits at the first line's end
    EXPECT_FLOAT_EQ(layout.positions[2].x, (float)(2 * btAdvance('A') + 1));
    EXPECT_EQ(layout.info.size(), 4u);
    // without multiline the same text stays on one line
    BtLayout single;
    BitmapTextOptions noWrap = options;
    noWrap.multiline = false;
    single.width = layout.width;
    btLayout(text, "AA AA", single, noWrap);
    EXPECT_FLOAT_EQ(single.positions[4].y, 0.0f);
}

TEST(BitmapText, LayoutScaledAndInverted){
    std::shared_ptr<BitmapFont> font = btFont();
    ASSERT_TRUE(font);
    BitmapText text(font, "layout", 0);
    BtLayout one, three;
    btLayout(text, "Hi!", one, BitmapTextOptions());
    BitmapTextOptions scaled;
    scaled.unit = 3.0f;
    scaled.invert = true;
    btLayout(text, "Hi!", three, scaled);
    EXPECT_EQ(three.width, one.width * 3);
    EXPECT_EQ(three.height, one.height * 3);
    for (size_t i = 0; i < one.positions.size(); i++){
        EXPECT_FLOAT_EQ(three.positions[i].x, one.positions[i].x * 3.0f);
        EXPECT_FLOAT_EQ(three.extents[i].left, one.extents[i].left * 3.0f);
    }
    for (size_t g = 0; g < one.info.size(); g++){
        const std::array<float, 4> a = one.quad(g), b = three.quad(g);
        EXPECT_FLOAT_EQ(b[0], a[0] * 3.0f);
        // y up: y0 = -y1, y1 = -y0
        EXPECT_FLOAT_EQ(b[1], -a[3] * 3.0f);
        EXPECT_FLOAT_EQ(b[3], -a[1] * 3.0f);
    }
    // an empty text still has its line and a degenerate triangle
    BtLayout empty;
    btLayout(text, "", empty, BitmapTextOptions());
    EXPECT_EQ(empty.indices.size(), 3u);
    EXPECT_EQ(empty.height, 10u);
    EXPECT_EQ(empty.width, 0u);
    // measure (Lua BitmapFont:measure) is the drawn size
    int w = 0, h = 0;
    BitmapText::measure(*font, "AB\nC", w, h);
    BtLayout drawn;
    btLayout(text, "AB\nC", drawn, BitmapTextOptions());
    EXPECT_EQ((unsigned int)w, drawn.width);
    EXPECT_EQ((unsigned int)h, drawn.height);
}

TEST(BitmapText, Fallback){
    test::TempDir dir;
    auto write = [&dir](const std::string& name, const std::string& content){
        std::ofstream out(dir.str() + "/" + name);
        out << content;
        return dir.str() + "/" + name;
    };
    // a: A and B, falling back to b (C); the Text's own fallbacks: c (D, same size), a TTF (skipped)
    const std::string a = write("a.bitmapfont", btSettings(btCharsKeeping("AB?"), "\"fallback_fonts\": [\"b.bitmapfont\"]"));
    write("b.bitmapfont", btSettings(btCharsKeeping("C"), ""));
    const std::string c = write("c.bitmapfont", btSettings(btCharsKeeping("D"), ""));
    const std::string ttf = btFixture("pixel8.ttf");

    BitmapFontPool::clear();
    std::shared_ptr<BitmapFont> own = BitmapFontPool::get(a);
    ASSERT_TRUE(own);
    EXPECT_TRUE(own->hasGlyph('A'));
    EXPECT_TRUE(own->hasGlyph('C'));    // from fallback_fonts, in a's atlas
    EXPECT_FALSE(own->hasGlyph('D'));

    std::vector<std::string> skipped;
    std::string key;
    std::shared_ptr<BitmapFont> chain = BitmapFontPool::getChain({a, c, ttf}, &skipped, &key);
    ASSERT_TRUE(chain);
    EXPECT_TRUE(chain->hasGlyph('A'));
    EXPECT_TRUE(chain->hasGlyph('C'));
    EXPECT_TRUE(chain->hasGlyph('D'));   // the Text's same-size bitmap fallback
    ASSERT_EQ(skipped.size(), 1u);       // never a vector fallback
    EXPECT_EQ(skipped[0], ttf);
    EXPECT_EQ(key, a + ";" + c);
    EXPECT_EQ(chain->getPages().size(), 1u);   // one atlas for the whole chain
    // a codepoint none has: the fallback glyph ("?" of a)
    EXPECT_EQ(chain->glyph('Z')->advance, chain->glyph('?')->advance);
    EXPECT_EQ(chain->glyph('Z')->w, chain->glyph('?')->w);
    // the pool shares the chain, and a changed file drops it
    EXPECT_EQ(BitmapFontPool::getChain({a, c, ttf}).get(), chain.get());
    const uint32_t generation = BitmapFontPool::generation(key);
    const std::vector<std::string> dropped = BitmapFontPool::invalidate(dir.str() + "/b.bitmapfont");
    EXPECT_FALSE(dropped.empty());
    EXPECT_GT(BitmapFontPool::generation(key), generation);
    BitmapFontPool::clear();
}
