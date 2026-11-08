// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S51 AC3 (metrics), AC6 (determinism), AC15 (the built-in font), AC22 (.jfont round trip) and
// R5/R33 (the .bitmapfont settings and their canonical writer).

#include "pixelart/BitmapFont.h"
#include "pixelart/BitmapFontPool.h"
#include "pixelart/BuiltinPixelFont.h"
#include "TempDir.h"
#include "TestPaths.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

using namespace jasperengine;
using namespace jasperengine::pixelart;

namespace {

    std::string bfFixture(const std::string& name){
        return test::testPaths().assetDir + "/fonts/" + name;
    }

    // a .bitmapfont over the grid fixture with extra keys, written next to the fixtures' copy
    std::shared_ptr<BitmapFont> bfGrid(const std::string& extra, BitmapFontReport* report = nullptr, const std::string& image = "grid@6x9.png"){
        const std::string json = "{\"format\": \"jasperengine.bitmapfont\", \"version\": 1, \"source\": {\"type\": \"grid\", \"image\": \"" + image + "\"}" +
                                 (extra.empty() ? std::string() : ", " + extra) + "}";
        BitmapFontReport local;
        std::shared_ptr<BitmapFont> font = BitmapFont::loadSettings(bfFixture("test.bitmapfont"), json, report ? report : &local);
        return font;
    }

    int bfInkLeft(int i){ return i % 2; }
    int bfInkRight(int i){ return std::min(4, i % 2 + (i * 7) % 5); }

}

TEST(BitmapFont, MetricsAutoAdvanceIsTheRightmostInkColumnPlusOne){
    BitmapFontReport report;
    std::shared_ptr<BitmapFont> font = bfGrid("", &report);
    ASSERT_TRUE(font) << (report.errors.empty() ? "" : report.errors[0].message);
    // the table: glyph i of the fixture has its last ink column at min(4, i % 2 + 7i % 5)
    static const int expected[95] = {
        3, 4, 5, 3, 4, 2, 3, 5, 2, 5, 1, 4, 5, 3, 4, 2, 3, 5, 2, 5, 1, 4, 5, 3, 4, 2, 3, 5, 2, 5, 1, 4,
        5, 3, 4, 2, 3, 5, 2, 5, 1, 4, 5, 3, 4, 2, 3, 5, 2, 5, 1, 4, 5, 3, 4, 2, 3, 5, 2, 5, 1, 4, 5, 3,
        4, 2, 3, 5, 2, 5, 1, 4, 5, 3, 4, 2, 3, 5, 2, 5, 1, 4, 5, 3, 4, 2, 3, 5, 2, 5, 1, 4, 5, 3, 4};
    for (int i = 1; i < 95; i++){
        EXPECT_EQ(font->glyph((uint32_t)(32 + i))->advance, bfInkRight(i) + 1) << i;
        EXPECT_EQ(font->glyph((uint32_t)(32 + i))->advance, expected[i]) << i;
    }
    // spryte's defaults: spacing 1, space round(h / 3), line height h + 1, fallback "?"
    EXPECT_EQ(font->getSpacing(), 1);
    EXPECT_EQ(font->getSpaceWidth(), 3);
    EXPECT_EQ(font->glyph(' ')->advance, 3);
    EXPECT_EQ(font->getLineHeight(), 10);
    EXPECT_EQ(font->getNativeSize(), 9);
    // the most common bottom ink row of A-Z and 0-9 (row 6) plus 1
    EXPECT_EQ(font->getBaseline(), 7);
    EXPECT_EQ(font->glyph(0x4E00)->advance, font->glyph('?')->advance);
    EXPECT_FALSE(font->isColored());
    EXPECT_EQ(report.glyphs, 95);
    EXPECT_EQ(report.cellsUsed, 95);
    EXPECT_EQ(report.cellsEmpty, 1);
    EXPECT_TRUE(report.missingAscii.empty());
    EXPECT_TRUE(report.emptyGlyphs.empty());
    EXPECT_EQ(report.partialAlphaShare, 0.0f);
}

TEST(BitmapFont, MetricsTrimMonospaceIntegerAndOverrides){
    std::shared_ptr<BitmapFont> both = bfGrid("\"trim\": \"both\"");
    ASSERT_TRUE(both);
    for (int i = 1; i < 95; i++){
        const BitmapGlyph* g = both->glyph((uint32_t)(32 + i));
        EXPECT_EQ(g->advance, bfInkRight(i) - bfInkLeft(i) + 1) << i;
        EXPECT_EQ(g->offsetX, 0) << i;   // the leftmost ink column moves to the pen
    }
    std::shared_ptr<BitmapFont> mono = bfGrid("\"advance\": \"monospace\"");
    ASSERT_TRUE(mono);
    EXPECT_EQ(mono->glyph('i')->advance, 6);
    EXPECT_EQ(mono->glyph('W')->advance, 6);
    std::shared_ptr<BitmapFont> fixed = bfGrid("\"advance\": 4");
    ASSERT_TRUE(fixed);
    EXPECT_EQ(fixed->glyph('M')->advance, 4);

    BitmapFontReport report;
    std::shared_ptr<BitmapFont> custom = bfGrid("\"spacing\": 2, \"space_width\": 5, \"line_height\": 12, \"baseline\": 8, \"fallback\": \"#\", "
                                                "\"glyphs\": {\"i\": {\"advance\": 2}, \"j\": {\"offset\": [-1, 1]}}, \"kerning\": [[\"T\", \"o\", -2]]",
                                                &report);
    ASSERT_TRUE(custom);
    EXPECT_EQ(custom->getSpacing(), 2);
    EXPECT_EQ(custom->getSpaceWidth(), 5);
    EXPECT_EQ(custom->getLineHeight(), 12);
    EXPECT_EQ(custom->getBaseline(), 8);
    EXPECT_EQ(custom->glyph('i')->advance, 2);
    std::shared_ptr<BitmapFont> plain = bfGrid("");
    EXPECT_EQ(custom->glyph('j')->offsetX, plain->glyph('j')->offsetX - 1);
    EXPECT_EQ(custom->glyph('j')->offsetY, plain->glyph('j')->offsetY + 1);
    EXPECT_EQ(custom->kerning('T', 'o'), -2);
    EXPECT_EQ(custom->glyph(0x4E00)->advance, custom->glyph('#')->advance);
    EXPECT_EQ(report.overridesApplied, 3);
    // measure: advances + spacing + kerning, no trailing spacing
    const std::u32string to = U"To";
    EXPECT_EQ(custom->measure(to), custom->glyph('T')->advance + 2 - 2 + custom->glyph('o')->advance);
}

TEST(BitmapFont, MetricsAlphaCutoffAndColored){
    BitmapFontReport partial;
    std::shared_ptr<BitmapFont> sloppy = bfGrid("", &partial, "partial@6x9.png");
    ASSERT_TRUE(sloppy);
    EXPECT_GT(partial.partialAlphaShare, 0.0f);
    BitmapFontReport cut;
    ASSERT_TRUE(bfGrid("\"alpha_cutoff\": 128", &cut, "partial@6x9.png"));
    EXPECT_EQ(cut.partialAlphaShare, 0.0f);
    BitmapFontReport colored;
    std::shared_ptr<BitmapFont> art = bfGrid("", &colored, "colored@6x9.png");
    ASSERT_TRUE(art);
    EXPECT_TRUE(art->isColored());
    EXPECT_EQ(art->getPages()[0].channels, 4);
    EXPECT_FALSE(bfGrid("\"colored\": false", nullptr, "colored@6x9.png")->isColored());
    EXPECT_EQ(sloppy->getPages()[0].channels, 1);
}

TEST(BitmapFont, MetricsFallbackBoxAndMissingGlyphs){
    BitmapFontReport report;
    std::shared_ptr<BitmapFont> font = bfGrid("\"source\": {\"type\": \"grid\", \"image\": \"grid@6x9.png\", \"chars\": \"AB C\"}", &report);
    ASSERT_TRUE(font);
    EXPECT_EQ(report.glyphs, 4);   // A, B, space, C
    EXPECT_FALSE(font->hasGlyph('?'));
    // no "?": a hollow box of the cell
    const BitmapGlyph& box = font->getFallbackGlyph();
    EXPECT_EQ(box.w, 5);
    EXPECT_EQ(box.h, 7);
    EXPECT_EQ(font->glyph('z'), &font->getFallbackGlyph());
    EXPECT_EQ(report.missingAscii.size(), 95u - 4u);
}

TEST(BitmapFont, SettingsErrorsAreTypedWithTheirField){
    BitmapFontSettings settings;
    std::vector<FontError> errors;
    EXPECT_FALSE(parseBitmapFontSettings("not json", settings, errors));
    EXPECT_EQ(errors.at(0).kind, "font_invalid_json");
    errors.clear();
    EXPECT_FALSE(parseBitmapFontSettings("{\"format\": \"jasperengine.bitmapfont\", \"version\": 1}", settings, errors));
    EXPECT_EQ(errors.at(0).kind, "font_no_source");
    errors.clear();
    EXPECT_FALSE(parseBitmapFontSettings("{\"format\": \"jasperengine.bitmapfont\", \"source\": {\"type\": \"grid\", \"image\": \"a.png\", \"cell\": [6]}, "
                                         "\"spacing\": \"x\", \"kerning\": [[\"T\", \"o\"]]}", settings, errors));
    std::set<std::string> fields;
    for (const FontError& e : errors) fields.insert(e.field);
    EXPECT_TRUE(fields.count("source.cell"));
    EXPECT_TRUE(fields.count("spacing"));
    EXPECT_TRUE(fields.count("kerning[0]"));
    BitmapFontReport report;
    EXPECT_FALSE(BitmapFont::loadSettings(bfFixture("x.bitmapfont"), "{\"format\": \"jasperengine.bitmapfont\", \"source\": {\"type\": \"grid\", \"image\": \"nowhere.png\"}}", &report));
    EXPECT_EQ(report.errors.at(0).kind, "font_source_missing");
    EXPECT_EQ(report.errors.at(0).field, "source.image");
}

TEST(BitmapFont, SettingsCanonicalWriterSortsAndKeepsUnknownKeys){
    const std::string in = "{\"kerning\": [[\"T\", \"o\", -1], [\"A\", \"V\", -1]], \"glyphs\": {\"j\": {\"offset\": [-1, 0]}, \"i\": {\"advance\": 2}},"
                           " \"zz_custom\": 5, \"source\": {\"image\": \"hud@6x9.png\", \"type\": \"grid\", \"note\": \"kept\"}, \"version\": 1,"
                           " \"format\": \"jasperengine.bitmapfont\", \"spacing\": 1}";
    const std::string out = canonicalBitmapFontJson(in);
    EXPECT_LT(out.find("\"format\""), out.find("\"version\""));
    EXPECT_LT(out.find("\"version\""), out.find("\"source\""));
    EXPECT_LT(out.find("\"type\""), out.find("\"image\""));
    EXPECT_LT(out.find("\"image\""), out.find("\"note\""));
    EXPECT_LT(out.find("\"spacing\""), out.find("\"glyphs\""));
    EXPECT_LT(out.find("\"i\""), out.find("\"j\""));
    EXPECT_LT(out.find("[\"A\", \"V\", -1]"), out.find("[\"T\", \"o\", -1]"));
    EXPECT_NE(out.find("\"zz_custom\": 5"), std::string::npos);
    EXPECT_EQ(canonicalBitmapFontJson(out), out);
}

TEST(BitmapFont, Deterministic){
    for (const char* name : {"grid", "bmfont", "truetype"}){
        std::shared_ptr<BitmapFont> a, b;
        BitmapFontReport ra, rb;
        if (std::string(name) == "grid"){
            a = bfGrid("\"kerning\": [[\"A\", \"V\", -1]]", &ra);
            b = bfGrid("\"kerning\": [[\"A\", \"V\", -1]]", &rb);
        }else if (std::string(name) == "bmfont"){
            a = BitmapFont::load(bfFixture("bm.fnt"), &ra);
            b = BitmapFont::load(bfFixture("bm.fnt"), &rb);
        }else{
            const std::string json = "{\"format\": \"jasperengine.bitmapfont\", \"source\": {\"type\": \"truetype\", \"file\": \"pixel8.ttf\"}}";
            a = BitmapFont::loadSettings(bfFixture("t.bitmapfont"), json, &ra);
            b = BitmapFont::loadSettings(bfFixture("t.bitmapfont"), json, &rb);
        }
        ASSERT_TRUE(a && b) << name;
        EXPECT_EQ(a->toCooked(), b->toCooked()) << name;
        EXPECT_EQ(ra.glyphs, rb.glyphs);
        EXPECT_EQ(ra.baseline, rb.baseline);
        EXPECT_EQ(ra.detections, rb.detections);
        EXPECT_EQ(ra.partialAlphaShare, rb.partialAlphaShare);
        EXPECT_EQ(ra.missingAscii, rb.missingAscii);
    }
}

TEST(BitmapFont, JfontRoundTrip){
    std::shared_ptr<BitmapFont> font = bfGrid("\"kerning\": [[\"A\", \"V\", -1]], \"glyphs\": {\"i\": {\"advance\": 2}}");
    ASSERT_TRUE(font);
    const std::vector<uint8_t> cooked = font->toCooked();
    ASSERT_GT(cooked.size(), 8u);
    EXPECT_EQ(std::string(cooked.begin(), cooked.begin() + 4), "JFNT");
    BitmapFontReport report;
    std::shared_ptr<BitmapFont> back = BitmapFont::loadCooked(cooked, &report);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->toCooked(), cooked);
    EXPECT_EQ(back->getCodepoints(), font->getCodepoints());
    EXPECT_EQ(back->getPages()[0].pixels, font->getPages()[0].pixels);
    EXPECT_EQ(back->kerning('A', 'V'), -1);
    EXPECT_EQ(back->glyph('i')->advance, 2);
    EXPECT_EQ(report.glyphs, 95);
    // a damaged file is a typed error, never a crash
    std::vector<uint8_t> damaged(cooked.begin(), cooked.begin() + (std::ptrdiff_t)(cooked.size() / 2));
    EXPECT_FALSE(BitmapFont::loadCooked(damaged, &report));
    EXPECT_EQ(report.errors.at(0).kind, "font_cooked_invalid");
    // through load(): a .jfont file on disk
    test::TempDir dir;
    const std::string path = dir.str() + "/hud.jfont";
    {
        std::ofstream out(path, std::ios::binary);
        out.write((const char*)cooked.data(), (std::streamsize)cooked.size());
    }
    std::shared_ptr<BitmapFont> loaded = BitmapFont::load(path);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->toCooked(), cooked);
}

TEST(BitmapFont, BuiltIn){
    std::shared_ptr<BitmapFont> font = BitmapFont::load(kBuiltinPixelFont);
    ASSERT_TRUE(font);
    EXPECT_EQ(fontKindOf(kBuiltinPixelFont), FontKind::BITMAP);
    EXPECT_EQ(font->getNativeSize(), 9);
    EXPECT_EQ(font->getBaseline(), 7);
    EXPECT_EQ(font->getLineHeight(), 10);
    for (uint32_t c = 0x20; c <= 0x7E; c++)
        EXPECT_TRUE(font->hasGlyph(c)) << c;
    for (uint32_t c = 0xC0; c <= 0xFF; c++)
        EXPECT_TRUE(font->hasGlyph(c)) << c;
    // 5x7 glyphs in 6x9 cells: ink within 5 columns; above the baseline at most 7 rows; nothing
    // below row 8
    for (uint32_t c : font->getCodepoints()){
        const std::vector<std::string> rows = builtinPixelGlyphRows(c);
        ASSERT_EQ(rows.size(), 9u) << c;
        for (const std::string& row : rows) EXPECT_EQ(row.size(), 5u) << c;
        const BitmapGlyph* g = font->glyph(c);
        EXPECT_LE(g->offsetX + g->w, 5) << c;
        EXPECT_GE(g->offsetY, 0) << c;
        EXPECT_LE(g->offsetY + g->h, 9) << c;
        if (c != ' ') { EXPECT_GT(g->w, 0) << c; }
    }
    // composed letters: the base letter's body under a mark
    const std::vector<std::string> e = builtinPixelGlyphRows('e');
    const std::vector<std::string> eAcute = builtinPixelGlyphRows(0xE9);
    for (int y = 2; y < 9; y++) EXPECT_EQ(eAcute[(size_t)y], e[(size_t)y]) << y;
    EXPECT_NE(eAcute[0], std::string("....."));
    EXPECT_FALSE(font->isColored());
    EXPECT_EQ(font->getReport().partialAlphaShare, 0.0f);
}

namespace {
    // The classic 5x7 character-LCD glyph table (HD44780 ROM A00 style, the shapes most 5x7 fonts
    // repeat), U+0021..U+007E, five column bytes each, bit 0 the top row: the reference the built-in
    // font must not copy (R16, AC15)
    const uint8_t kForbiddenLcd5x7[94][5] = {
        {0x00, 0x00, 0x5F, 0x00, 0x00}, // !
        {0x00, 0x07, 0x00, 0x07, 0x00}, // "
        {0x14, 0x7F, 0x14, 0x7F, 0x14}, // #
        {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // $
        {0x23, 0x13, 0x08, 0x64, 0x62}, // %
        {0x36, 0x49, 0x55, 0x22, 0x50}, // &
        {0x00, 0x05, 0x03, 0x00, 0x00}, // '
        {0x00, 0x1C, 0x22, 0x41, 0x00}, // (
        {0x00, 0x41, 0x22, 0x1C, 0x00}, // )
        {0x14, 0x08, 0x3E, 0x08, 0x14}, // *
        {0x08, 0x08, 0x3E, 0x08, 0x08}, // +
        {0x00, 0x50, 0x30, 0x00, 0x00}, // ,
        {0x08, 0x08, 0x08, 0x08, 0x08}, // -
        {0x00, 0x60, 0x60, 0x00, 0x00}, // .
        {0x20, 0x10, 0x08, 0x04, 0x02}, // /
        {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
        {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
        {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
        {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
        {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
        {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
        {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
        {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
        {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
        {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
        {0x00, 0x36, 0x36, 0x00, 0x00}, // :
        {0x00, 0x56, 0x36, 0x00, 0x00}, // ;
        {0x08, 0x14, 0x22, 0x41, 0x00}, // <
        {0x14, 0x14, 0x14, 0x14, 0x14}, // =
        {0x00, 0x41, 0x22, 0x14, 0x08}, // >
        {0x02, 0x01, 0x51, 0x09, 0x06}, // ?
        {0x32, 0x49, 0x79, 0x41, 0x3E}, // @
        {0x7E, 0x11, 0x11, 0x11, 0x7E}, // A
        {0x7F, 0x49, 0x49, 0x49, 0x36}, // B
        {0x3E, 0x41, 0x41, 0x41, 0x22}, // C
        {0x7F, 0x41, 0x41, 0x22, 0x1C}, // D
        {0x7F, 0x49, 0x49, 0x49, 0x41}, // E
        {0x7F, 0x09, 0x09, 0x09, 0x01}, // F
        {0x3E, 0x41, 0x49, 0x49, 0x7A}, // G
        {0x7F, 0x08, 0x08, 0x08, 0x7F}, // H
        {0x00, 0x41, 0x7F, 0x41, 0x00}, // I
        {0x20, 0x40, 0x41, 0x3F, 0x01}, // J
        {0x7F, 0x08, 0x14, 0x22, 0x41}, // K
        {0x7F, 0x40, 0x40, 0x40, 0x40}, // L
        {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // M
        {0x7F, 0x04, 0x08, 0x10, 0x7F}, // N
        {0x3E, 0x41, 0x41, 0x41, 0x3E}, // O
        {0x7F, 0x09, 0x09, 0x09, 0x06}, // P
        {0x3E, 0x41, 0x51, 0x21, 0x5E}, // Q
        {0x7F, 0x09, 0x19, 0x29, 0x46}, // R
        {0x46, 0x49, 0x49, 0x49, 0x31}, // S
        {0x01, 0x01, 0x7F, 0x01, 0x01}, // T
        {0x3F, 0x40, 0x40, 0x40, 0x3F}, // U
        {0x1F, 0x20, 0x40, 0x20, 0x1F}, // V
        {0x3F, 0x40, 0x38, 0x40, 0x3F}, // W
        {0x63, 0x14, 0x08, 0x14, 0x63}, // X
        {0x07, 0x08, 0x70, 0x08, 0x07}, // Y
        {0x61, 0x51, 0x49, 0x45, 0x43}, // Z
        {0x00, 0x7F, 0x41, 0x41, 0x00}, // [
        {0x02, 0x04, 0x08, 0x10, 0x20}, // backslash
        {0x00, 0x41, 0x41, 0x7F, 0x00}, // ]
        {0x04, 0x02, 0x01, 0x02, 0x04}, // ^
        {0x40, 0x40, 0x40, 0x40, 0x40}, // _
        {0x00, 0x01, 0x02, 0x04, 0x00}, // `
        {0x20, 0x54, 0x54, 0x54, 0x78}, // a
        {0x7F, 0x48, 0x44, 0x44, 0x38}, // b
        {0x38, 0x44, 0x44, 0x44, 0x20}, // c
        {0x38, 0x44, 0x44, 0x48, 0x7F}, // d
        {0x38, 0x54, 0x54, 0x54, 0x18}, // e
        {0x08, 0x7E, 0x09, 0x01, 0x02}, // f
        {0x0C, 0x52, 0x52, 0x52, 0x3E}, // g
        {0x7F, 0x08, 0x04, 0x04, 0x78}, // h
        {0x00, 0x44, 0x7D, 0x40, 0x00}, // i
        {0x20, 0x40, 0x44, 0x3D, 0x00}, // j
        {0x7F, 0x10, 0x28, 0x44, 0x00}, // k
        {0x00, 0x41, 0x7F, 0x40, 0x00}, // l
        {0x7C, 0x04, 0x18, 0x04, 0x78}, // m
        {0x7C, 0x08, 0x04, 0x04, 0x78}, // n
        {0x38, 0x44, 0x44, 0x44, 0x38}, // o
        {0x7C, 0x14, 0x14, 0x14, 0x08}, // p
        {0x08, 0x14, 0x14, 0x18, 0x7C}, // q
        {0x7C, 0x08, 0x04, 0x04, 0x08}, // r
        {0x48, 0x54, 0x54, 0x54, 0x20}, // s
        {0x04, 0x3F, 0x44, 0x40, 0x20}, // t
        {0x3C, 0x40, 0x40, 0x20, 0x7C}, // u
        {0x1C, 0x20, 0x40, 0x20, 0x1C}, // v
        {0x3C, 0x40, 0x30, 0x40, 0x3C}, // w
        {0x44, 0x28, 0x10, 0x28, 0x44}, // x
        {0x0C, 0x50, 0x50, 0x50, 0x3C}, // y
        {0x44, 0x64, 0x54, 0x4C, 0x44}, // z
        {0x00, 0x08, 0x36, 0x41, 0x00}, // {
        {0x00, 0x00, 0x7F, 0x00, 0x00}, // |
        {0x00, 0x41, 0x36, 0x08, 0x00}, // }
        {0x10, 0x08, 0x08, 0x10, 0x08}, // ~
    };

    // a glyph's rows (9 x 5, true = ink) moved left to its first ink column
    std::vector<std::vector<bool>> bfOriginalLeft(std::vector<std::vector<bool>> rows){
        int first = 5;
        for (const auto& row : rows)
            for (int x = 0; x < 5; x++) if (row[(size_t)x]) first = std::min(first, x);
        if (first == 5) return rows;
        for (auto& row : rows){
            std::vector<bool> moved(5, false);
            for (int x = first; x < 5; x++) moved[(size_t)(x - first)] = row[(size_t)x];
            row = moved;
        }
        return rows;
    }
}

TEST(BitmapFont, BuiltInIsOriginal){
    // every printable ASCII glyph against the table, both moved to their first ink column: a glyph
    // is its own when 3 or more of its 45 pixels differ. Straight-stroke shapes the grid forces
    // (H, I, L, T, |, -) may coincide; most glyphs must not
    int own = 0;
    std::string alike;
    for (uint32_t c = 0x21; c <= 0x7E; c++){
        const std::vector<std::string> art = builtinPixelGlyphRows(c);
        std::vector<std::vector<bool>> ours(9, std::vector<bool>(5, false)), lcd(9, std::vector<bool>(5, false));
        for (int y = 0; y < 9; y++)
            for (int x = 0; x < 5; x++){
                ours[(size_t)y][(size_t)x] = art[(size_t)y][(size_t)x] == '#';
                if (y < 7) lcd[(size_t)y][(size_t)x] = (kForbiddenLcd5x7[c - 0x21][x] >> y) & 1;
            }
        ours = bfOriginalLeft(ours);
        lcd = bfOriginalLeft(lcd);
        int differ = 0;
        for (int y = 0; y < 9; y++)
            for (int x = 0; x < 5; x++) differ += ours[(size_t)y][(size_t)x] != lcd[(size_t)y][(size_t)x];
        if (differ >= 3) own++;
        else alike += (char)c;
    }
    EXPECT_GE(own * 4, 94 * 3) << own << " of 94 glyphs are the font's own; alike: " << alike;
}

TEST(BitmapFont, PoolReloadsAnEditedFnt){
    // a .fnt used directly depends on itself and its pages; the watcher's absolute path finds it
    // whatever path the font was loaded by (R33, AC23)
    test::TempDir dir;
    std::filesystem::copy_file(test::testPaths().assetDir + "/fonts/bm_0.png", dir.file("bm_0.png"));
    std::ifstream in(test::testPaths().assetDir + "/fonts/bm.fnt", std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    dir.write("bm.fnt", text);
    const std::string path = dir.str() + "/./bm.fnt";
    std::shared_ptr<BitmapFont> before = BitmapFontPool::get(path);
    ASSERT_TRUE(before);
    EXPECT_EQ(before->advanceOf('A'), 4);
    const std::string from = "id=65 x=7 y=19 width=2 height=6 xoffset=1 yoffset=1 xadvance=4";
    ASSERT_NE(text.find(from), std::string::npos);
    text.replace(text.find(from), from.size(), "id=65 x=7 y=19 width=2 height=6 xoffset=1 yoffset=1 xadvance=6");
    dir.write("bm.fnt", text);
    EXPECT_FALSE(BitmapFontPool::invalidate(dir.file("bm.fnt").string()).empty());
    std::shared_ptr<BitmapFont> after = BitmapFontPool::get(path);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->advanceOf('A'), 6);
    BitmapFontPool::clear();
}
