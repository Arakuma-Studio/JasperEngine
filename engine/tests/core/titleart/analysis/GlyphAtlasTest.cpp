// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Magic Circle's glyph atlas (analysis/GlyphAtlas.h, docs/specs/S83-title-effects-generators.md
// §6.7.6, risk 8), on the CPU with the engine's built-in face:
// - GlyphAtlas.RunesInCells: the rune set (Greek capitals) fills 10 cells of 8 columns, each
//   glyph's ink inside its own cell and centred in it; the cells past the set stay blank.
// - GlyphAtlas.MissingGlyphs: characters the face lacks are listed once each (the generator
//   reports them as glyph_missing); a face that has them lists none.
// - GlyphAtlas.Fonts: without a resolver the library font falls back to the built-in face; a
//   `font` asset that cannot be had says so.
// - GlyphAtlas.Utf8: code points and back, the HUD string's 42 slots.

#include <gtest/gtest.h>

#include "titleart/analysis/GlyphAtlas.h"

#include "util/FontFace.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::analysis;

namespace {

    struct CellInk {
        double sum = 0.0;
        double cx = 0.0, cy = 0.0;      // the ink's centroid in the cell, px
        bool touchesEdge = false;       // ink on the cell's outermost pixels
    };

    CellInk inkOf(const GlyphAtlas& atlas, int index) {
        CellInk ink;
        const int width = atlas.cols * atlas.cell;
        const int x0 = (index % atlas.cols) * atlas.cell, y0 = (index / atlas.cols) * atlas.cell;
        for (int y = 0; y < atlas.cell; y++) {
            for (int x = 0; x < atlas.cell; x++) {
                const double a = atlas.coverage[(size_t)(y0 + y) * width + (x0 + x)] / 255.0;
                ink.sum += a;
                ink.cx += a * (x + 0.5);
                ink.cy += a * (y + 0.5);
                if (a > 0.0 && (x == 0 || y == 0 || x == atlas.cell - 1 || y == atlas.cell - 1)) ink.touchesEdge = true;
            }
        }
        if (ink.sum > 0.0) {
            ink.cx /= ink.sum;
            ink.cy /= ink.sum;
        }
        return ink;
    }

}

TEST(GlyphAtlas, RunesInCells) {
    const GlyphFont font = resolveGlyphFont(nullptr, {}, {});
    ASSERT_TRUE(font.face);
    const GlyphAtlas atlas = buildGlyphAtlas(*font.face, utf8Codepoints("ΨΓΔΘΛΞΠΣΦΩ"), 64);
    EXPECT_EQ(atlas.count, 10);
    EXPECT_EQ(atlas.cols, 8);
    EXPECT_EQ(atlas.rows, 2);
    ASSERT_EQ(atlas.coverage.size(), (size_t)(8 * 64) * (2 * 64));
    EXPECT_TRUE(atlas.missing.empty()) << "the built-in face has Greek capitals";
    for (int i = 0; i < 10; i++) {
        SCOPED_TRACE(i);
        const CellInk ink = inkOf(atlas, i);
        EXPECT_GT(ink.sum, 0.03 * 64 * 64) << "a glyph's ink";
        EXPECT_FALSE(ink.touchesEdge) << "the em at 84 % of the cell keeps the ink inside it";
        // centred on its advance and on the ascender-to-descender height (capitals sit a little
        // above the middle: the descender's share is below the baseline)
        EXPECT_NEAR(ink.cx, 32.0, 8.0);
        EXPECT_NEAR(ink.cy, 32.0, 12.0);
    }
    for (int i = 10; i < 16; i++) EXPECT_EQ(inkOf(atlas, i).sum, 0.0) << "cell " << i << " is past the set";
}

TEST(GlyphAtlas, MissingGlyphs) {
    const GlyphFont font = resolveGlyphFont(nullptr, {}, {});
    ASSERT_TRUE(font.face);
    // two trigram names, one repeated, and a letter: each missing code point once, in order
    const std::vector<uint32_t> chars = utf8Codepoints("乾兌乾A");
    const GlyphAtlas atlas = buildGlyphAtlas(*font.face, chars, 32);
    EXPECT_EQ(atlas.count, 4);
    ASSERT_EQ(atlas.missing.size(), 2u);
    EXPECT_EQ(atlas.missing[0], 0x4E7Eu);
    EXPECT_EQ(atlas.missing[1], 0x514Cu);
    EXPECT_GT(inkOf(atlas, 3).sum, 0.0) << "the covered character still draws";
    // a custom string keeps its first 64 characters
    const GlyphAtlas capped = buildGlyphAtlas(*font.face, std::vector<uint32_t>(100, 'A'), 32);
    EXPECT_EQ(capped.count, GLYPH_ATLAS_MAX_GLYPHS);
    EXPECT_EQ(capped.rows, 8);
}

TEST(GlyphAtlas, Fonts) {
    const GlyphFont library = resolveGlyphFont(nullptr, {}, TitleAssetRef{"TitleArt/Fonts/zen-old-mincho-black/ZenOldMincho-Black.ttf", ""});
    ASSERT_TRUE(library.face);
    EXPECT_EQ(library.key.rfind("builtin:", 0), 0u) << "no resolver: the built-in face draws";
    EXPECT_TRUE(library.error.empty()) << "a library font that is not in the project is not an asset problem";
    const GlyphFont named = resolveGlyphFont(nullptr, TitleAssetRef{"fonts/Missing.ttf", ""}, {});
    ASSERT_TRUE(named.face);
    EXPECT_FALSE(named.error.empty()) << "the layer's own font could not be had";
}

TEST(GlyphAtlas, Utf8) {
    const std::vector<uint32_t> cps = utf8Codepoints("ΨA臨");
    ASSERT_EQ(cps.size(), 3u);
    EXPECT_EQ(cps[0], 0x3A8u);
    EXPECT_EQ(cps[1], 0x41u);
    EXPECT_EQ(cps[2], 0x81E8u);
    EXPECT_EQ(utf8Char(cps[0]) + utf8Char(cps[1]) + utf8Char(cps[2]), "ΨA臨");
    EXPECT_EQ(utf8Codepoints("\xC3").front(), 0xFFFDu) << "a cut sequence";
    // Pachislop's HUD row has one slot per character of its string
    EXPECT_EQ(utf8Codepoints("SYSTEM ONLINE // TARGET LOCK 0451-7F3A // ").size(), 42u);
}
