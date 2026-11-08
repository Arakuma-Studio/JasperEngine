// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S51 AC3 (metrics), AC6 (determinism), AC15 (the built-in font), AC22 (.jfont round trip) and
// R5/R33 (the .bitmapfont settings and their canonical writer).

#include "pixelart/BitmapFont.h"
#include "pixelart/BuiltinPixelFont.h"
#include "TempDir.h"
#include "TestPaths.h"

#include <gtest/gtest.h>

#include <fstream>
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
        if (c != ' ') EXPECT_GT(g->w, 0) << c;
    }
    // composed letters: the base letter's body under a mark
    const std::vector<std::string> e = builtinPixelGlyphRows('e');
    const std::vector<std::string> eAcute = builtinPixelGlyphRows(0xE9);
    for (int y = 2; y < 9; y++) EXPECT_EQ(eAcute[(size_t)y], e[(size_t)y]) << y;
    EXPECT_NE(eAcute[0], std::string("....."));
    EXPECT_FALSE(font->isColored());
    EXPECT_EQ(font->getReport().partialAlphaShare, 0.0f);
}
