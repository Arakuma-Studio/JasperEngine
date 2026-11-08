// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S51 AC1 (grid), AC4 (BMFont text, XML, binary) and AC5 (pixel TrueType): the sources of a
// bitmap font. Fixtures in engine/tests/core/fixtures/fonts/, written by
// tools/pixelart/make_font_fixtures.py.

#include "pixelart/BitmapFont.h"
#include "pixelart/BitmapFontParsers.h"
#include "util/FontFace.h"
#include "util/FontRegistry.h"
#include "TestPaths.h"

#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::pixelart;

namespace {

    std::string bfpFixture(const std::string& name){
        return test::testPaths().assetDir + "/fonts/" + name;
    }

    std::vector<uint8_t> bfpRead(const std::string& path){
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    DecodedPage bfpImage(const std::string& name){
        DecodedPage page;
        EXPECT_TRUE(decodeImage(bfpRead(bfpFixture(name)), page)) << name;
        return page;
    }

    // the fixture's glyph i (codepoint 32 + i): ink columns i % 2 .. min(4, i % 2 + (i * 7) % 5)
    int bfpInkRight(int i){
        return std::min(4, i % 2 + (i * 7) % 5);
    }

    std::vector<uint32_t> bfpAscii(){
        return expandFontChars("ascii", "ascii");
    }

    std::string bfpFirstErrorKind(const std::vector<FontError>& errors){
        return errors.empty() ? std::string() : errors[0].kind;
    }

}

TEST(BitmapFontParsers, GridCellFromTheFileName){
    int w = 0, h = 0;
    EXPECT_TRUE(cellFromFileName("fonts/hud@6x9.png", w, h));
    EXPECT_EQ(w, 6);
    EXPECT_EQ(h, 9);
    EXPECT_TRUE(cellFromFileName("C:\\art\\font@12X16.PNG", w, h));
    EXPECT_EQ(w, 12);
    EXPECT_EQ(h, 16);
    EXPECT_FALSE(cellFromFileName("fonts/hud.png", w, h));
    EXPECT_FALSE(cellFromFileName("fonts/hud@6.png", w, h));
    EXPECT_FALSE(cellFromFileName("fonts/hud@ax9.png", w, h));
}

TEST(BitmapFontParsers, GridReadsTheCellsInSheetOrder){
    const DecodedPage sheet = bfpImage("grid@6x9.png");
    GridOptions options;
    options.imagePath = "fonts/grid@6x9.png";
    options.chars = bfpAscii();
    RawFont raw;
    std::vector<FontError> errors;
    ASSERT_TRUE(parseGrid(sheet.rgba.data(), sheet.width, sheet.height, options, raw, errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(raw.cellWidth, 6);
    EXPECT_EQ(raw.cellHeight, 9);
    EXPECT_EQ(raw.cellsTotal, 96);
    ASSERT_EQ(raw.glyphs.size(), 95u);
    ASSERT_FALSE(raw.detections.empty());
    EXPECT_NE(raw.detections[0].find("file name"), std::string::npos);
    for (int i = 0; i < 95; i++){
        EXPECT_EQ(raw.glyphs[(size_t)i].codepoint, (uint32_t)(32 + i));
        EXPECT_EQ(raw.glyphs[(size_t)i].cell, i);
    }
    // 'A' (i 33): ink from column 1, rows 1 + 33 % 3 = 1 .. 6
    const RawGlyph& a = raw.glyphs[33];
    auto alpha = [&a](int x, int y){ return a.rgba[((size_t)y * 6 + (size_t)x) * 4 + 3]; };
    EXPECT_EQ(alpha(0, 3), 0);
    EXPECT_EQ(alpha(1, 1), 255);
    EXPECT_EQ(alpha(bfpInkRight(33), 6), 255);
    EXPECT_EQ(alpha(1, 0), 0);
}

TEST(BitmapFontParsers, GridCellMarginGapRowsAndSkips){
    // the gutter sheet read with an explicit cell and gap
    const DecodedPage sheet = bfpImage("gutter.png");
    GridOptions options;
    options.imagePath = "fonts/gutter.png";
    options.cellWidth = 6;
    options.cellHeight = 9;
    options.gapX = 1;
    options.gapY = 1;
    options.chars = bfpAscii();
    RawFont raw;
    std::vector<FontError> errors;
    ASSERT_TRUE(parseGrid(sheet.rgba.data(), sheet.width, sheet.height, options, raw, errors));
    EXPECT_EQ(raw.cellsTotal, 96);
    ASSERT_EQ(raw.glyphs.size(), 95u);
    const DecodedPage plain = bfpImage("grid@6x9.png");
    GridOptions plainOptions;
    plainOptions.imagePath = "fonts/grid@6x9.png";
    plainOptions.chars = bfpAscii();
    RawFont plainRaw;
    ASSERT_TRUE(parseGrid(plain.rgba.data(), plain.width, plain.height, plainOptions, plainRaw, errors));
    for (size_t i = 0; i < 95; i++)
        EXPECT_EQ(raw.glyphs[i].rgba, plainRaw.glyphs[i].rgba) << i;

    // a margin moves the first cell: margin 6 skips the first column of cells
    GridOptions margin = plainOptions;
    margin.marginX = 6;
    margin.chars = {'X'};
    RawFont marginRaw;
    ASSERT_TRUE(parseGrid(plain.rgba.data(), plain.width, plain.height, margin, marginRaw, errors));
    EXPECT_EQ(marginRaw.cellsTotal, 15 * 6);
    EXPECT_EQ(marginRaw.glyphs[0].rgba, plainRaw.glyphs[1].rgba);

    // rows: a short row does not shift the next; U+FFFF skips a cell
    GridOptions rows = plainOptions;
    rows.chars.clear();
    rows.rows = {{'a', 0xFFFF, 'c'}, {'q'}};
    RawFont rowsRaw;
    ASSERT_TRUE(parseGrid(plain.rgba.data(), plain.width, plain.height, rows, rowsRaw, errors));
    ASSERT_EQ(rowsRaw.glyphs.size(), 3u);
    EXPECT_EQ(rowsRaw.glyphs[0].cell, 0);
    EXPECT_EQ(rowsRaw.glyphs[1].codepoint, (uint32_t)'c');
    EXPECT_EQ(rowsRaw.glyphs[1].cell, 2);
    EXPECT_EQ(rowsRaw.glyphs[2].codepoint, (uint32_t)'q');
    EXPECT_EQ(rowsRaw.glyphs[2].cell, 16);
    EXPECT_EQ(rowsRaw.glyphs[2].rgba, plainRaw.glyphs[16].rgba);
}

TEST(BitmapFontParsers, GridOverflowIsATypedErrorWithTheCounts){
    const DecodedPage sheet = bfpImage("grid@6x9.png");
    GridOptions options;
    options.imagePath = "fonts/grid@6x9.png";
    options.cellWidth = 6;
    options.cellHeight = 10;   // 5 rows of 16: 80 cells
    options.chars = bfpAscii();
    RawFont raw;
    std::vector<FontError> errors;
    EXPECT_FALSE(parseGrid(sheet.rgba.data(), sheet.width, sheet.height, options, raw, errors));
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].kind, "font_chars_overflow");
    EXPECT_EQ(errors[0].message, "95 chars but 80 cells (16 x 5 of 6x10)");
    EXPECT_EQ(errors[0].fix, "check the cell size in the file name or set cell");
    EXPECT_EQ(errors[0].field, "source.chars");
}

TEST(BitmapFontParsers, GridGutterDetectionFindsTheCellAndReportsIt){
    const DecodedPage sheet = bfpImage("gutter.png");
    int w = 0, h = 0, gx = 0, gy = 0;
    ASSERT_TRUE(detectGridCell(sheet.rgba.data(), sheet.width, sheet.height, w, h, gx, gy));
    EXPECT_EQ(w, 6);
    EXPECT_EQ(h, 9);
    EXPECT_EQ(gx, 1);
    EXPECT_EQ(gy, 1);

    GridOptions options;
    options.imagePath = "fonts/gutter.png";
    options.chars = bfpAscii();
    RawFont raw;
    std::vector<FontError> errors;
    ASSERT_TRUE(parseGrid(sheet.rgba.data(), sheet.width, sheet.height, options, raw, errors));
    EXPECT_EQ(raw.cellWidth, 6);
    EXPECT_EQ(raw.cellHeight, 9);
    ASSERT_EQ(raw.detections.size(), 1u);
    EXPECT_NE(raw.detections[0].find("gutters"), std::string::npos) << raw.detections[0];

    // a sheet with no repeating transparent rows cannot be guessed: a typed error, never silent
    std::vector<uint8_t> solid((size_t)20 * 20 * 4, 255);
    GridOptions none;
    none.imagePath = "fonts/solid.png";
    none.chars = {'a'};
    RawFont noneRaw;
    errors.clear();
    EXPECT_FALSE(parseGrid(solid.data(), 20, 20, none, noneRaw, errors));
    EXPECT_EQ(bfpFirstErrorKind(errors), "font_cell_unknown");
}

TEST(BitmapFontParsers, BMFontEncodingsGiveTheSameFont){
    const DecodedPage page = bfpImage("bm_0.png");
    std::vector<std::shared_ptr<BitmapFont>> fonts;
    const BMFontFile::Encoding expected[] = {BMFontFile::Encoding::TEXT, BMFontFile::Encoding::XML, BMFontFile::Encoding::BINARY};
    int index = 0;
    for (const char* name : {"bm.fnt", "bm_xml.fnt", "bm_bin.fnt"}){
        BMFontFile file;
        std::vector<FontError> errors;
        ASSERT_TRUE(parseBMFont(bfpRead(bfpFixture(name)), file, errors)) << name << ": " << bfpFirstErrorKind(errors);
        EXPECT_EQ(file.encoding, expected[index++]) << name;
        EXPECT_EQ(file.size, 9);
        EXPECT_EQ(file.lineHeight, 10);
        EXPECT_EQ(file.base, 7);
        ASSERT_EQ(file.pages.size(), 1u);
        EXPECT_EQ(file.pages[0], "bm_0.png");
        EXPECT_EQ(file.chars.size(), 95u);
        EXPECT_EQ(file.kerning.size(), 2u);
        BitmapFontReport report;
        std::shared_ptr<BitmapFont> font = BitmapFont::load(bfpFixture(name), &report);
        ASSERT_TRUE(font) << name << ": " << bfpFirstErrorKind(report.errors);
        fonts.push_back(font);
    }
    for (size_t f = 1; f < fonts.size(); f++){
        EXPECT_EQ(fonts[f]->getNativeSize(), fonts[0]->getNativeSize());
        EXPECT_EQ(fonts[f]->getLineHeight(), fonts[0]->getLineHeight());
        EXPECT_EQ(fonts[f]->getBaseline(), fonts[0]->getBaseline());
        EXPECT_EQ(fonts[f]->getCodepoints(), fonts[0]->getCodepoints());
        EXPECT_EQ(fonts[f]->getKerningTable(), fonts[0]->getKerningTable());
        EXPECT_EQ(fonts[f]->toCooked(), fonts[0]->toCooked()) << f;
    }
    // the file's metrics, as given: xadvance (advance "file"), base, lineHeight, kerning
    EXPECT_EQ(fonts[0]->getNativeSize(), 9);
    EXPECT_EQ(fonts[0]->getLineHeight(), 10);
    EXPECT_EQ(fonts[0]->getBaseline(), 7);
    EXPECT_EQ(fonts[0]->glyph('A')->advance, bfpInkRight(33) + 2);
    EXPECT_EQ(fonts[0]->kerning('A', 'V'), -1);
    EXPECT_EQ(fonts[0]->kerning('T', 'o'), -1);
    (void)page;
}

TEST(BitmapFontParsers, BMFontFromHieroReadsChannelZeroAsAllChannels){
    // libGDX Hiero writes chnl=0 for glyphs in every channel: the same font as chnl=15
    BitmapFontReport report;
    std::shared_ptr<BitmapFont> hiero = BitmapFont::load(bfpFixture("bm_hiero.fnt"), &report);
    ASSERT_TRUE(hiero) << bfpFirstErrorKind(report.errors);
    std::shared_ptr<BitmapFont> plain = BitmapFont::load(bfpFixture("bm.fnt"));
    ASSERT_TRUE(plain);
    EXPECT_EQ(hiero->getCodepoints(), plain->getCodepoints());
    EXPECT_EQ(hiero->toCooked(), plain->toCooked());
}

TEST(BitmapFontParsers, BMFontErrorsAreTyped){
    const std::string base = "info face=\"x\" size=9\ncommon lineHeight=10 base=7 scaleW=96 scaleH=54 pages=1 packed=0\n";
    auto load = [](const std::string& text, const std::string& path){
        BitmapFontReport report;
        std::shared_ptr<BitmapFont> font = BitmapFont::loadBMFont(path, std::vector<uint8_t>(text.begin(), text.end()), nullptr, &report);
        return std::make_pair(font, report);
    };
    // chnl 3 (two channels): only all four or one is read
    auto badChannel = load(base + "page id=0 file=\"bm_0.png\"\nchar id=65 x=0 y=0 width=2 height=2 xoffset=0 yoffset=0 xadvance=3 page=0 chnl=3\n",
                           bfpFixture("inline.fnt"));
    EXPECT_FALSE(badChannel.first);
    EXPECT_EQ(bfpFirstErrorKind(badChannel.second.errors), "font_bmfont_channel");
    EXPECT_EQ(badChannel.second.errors[0].field, "chars");
    // a page that is not there
    auto missing = load(base + "page id=0 file=\"nowhere.png\"\nchar id=65 x=0 y=0 width=2 height=2 xoffset=0 yoffset=0 xadvance=3 page=0 chnl=15\n",
                        bfpFixture("inline.fnt"));
    EXPECT_FALSE(missing.first);
    EXPECT_EQ(bfpFirstErrorKind(missing.second.errors), "font_bmfont_page_missing");
    EXPECT_EQ(missing.second.errors[0].fix, "copy the page next to the .fnt");
    // an id beyond U+10FFFF
    auto beyond = load(base + "page id=0 file=\"bm_0.png\"\nchar id=1114112 x=0 y=0 width=2 height=2 xoffset=0 yoffset=0 xadvance=3 page=0 chnl=15\n",
                       bfpFixture("inline.fnt"));
    EXPECT_FALSE(beyond.first);
    EXPECT_EQ(bfpFirstErrorKind(beyond.second.errors), "font_bmfont_codepoint");
    // packed with outline channels
    auto packed = load("info face=\"x\" size=9\ncommon lineHeight=10 base=7 scaleW=96 scaleH=54 pages=1 packed=1 alphaChnl=1 redChnl=0 greenChnl=0 blueChnl=0\n",
                       bfpFixture("inline.fnt"));
    EXPECT_FALSE(packed.first);
    EXPECT_EQ(bfpFirstErrorKind(packed.second.errors), "font_bmfont_packed");

    // a binary file cut anywhere inside a block
    const std::vector<uint8_t> binary = bfpRead(bfpFixture("bm_bin.fnt"));
    for (size_t cut : {(size_t)6, (size_t)30, binary.size() / 2, binary.size() - 3}){
        std::vector<uint8_t> truncated(binary.begin(), binary.begin() + (std::ptrdiff_t)cut);
        BMFontFile file;
        std::vector<FontError> errors;
        EXPECT_FALSE(parseBMFont(truncated, file, errors)) << cut;
        EXPECT_EQ(bfpFirstErrorKind(errors), "font_bmfont_truncated") << cut;
    }
    // not a BMFont at all
    BMFontFile file;
    std::vector<FontError> errors;
    const std::string junk = "hello";
    EXPECT_FALSE(parseBMFont(std::vector<uint8_t>(junk.begin(), junk.end()), file, errors));
    EXPECT_EQ(bfpFirstErrorKind(errors), "font_bmfont_invalid");
}

namespace {

    // codepoint -> 8 rows of 5 columns, the bitmaps pixel8.ttf was drawn from
    std::map<uint32_t, std::vector<std::string>> bfpTtfBitmaps(){
        std::map<uint32_t, std::vector<std::string>> out;
        std::ifstream in(bfpFixture("pixel8_bitmaps.txt"));
        std::string line;
        while (std::getline(in, line)){
            if (line.empty() || line[0] == '#') continue;
            std::istringstream fields(line);
            uint32_t c = 0;
            fields >> c;
            std::vector<std::string> rows;
            std::string row;
            while (fields >> row) rows.push_back(row);
            out[c] = rows;
        }
        return out;
    }

}

TEST(BitmapFontParsers, TrueTypePixelFontIsDetectedAndRasterisedExactly){
    std::string error;
    std::shared_ptr<FontFace> face = FontFace::loadFile(bfpFixture("pixel8.ttf"), 0, &error);
    ASSERT_TRUE(face) << error;
    EXPECT_EQ(detectPixelSize(*face), 8);

    const std::map<uint32_t, std::vector<std::string>> bitmaps = bfpTtfBitmaps();
    ASSERT_GT(bitmaps.size(), 60u);
    TrueTypeOptions options;
    for (const auto& entry : bitmaps) options.chars.push_back(entry.first);
    RawFont raw;
    std::vector<FontError> errors;
    ASSERT_TRUE(rasterTrueType(*face, options, raw, errors)) << bfpFirstErrorKind(errors);
    EXPECT_EQ(raw.baseline, 7);
    EXPECT_EQ(raw.nativeSize, 8);
    ASSERT_EQ(raw.glyphs.size(), bitmaps.size());
    for (const RawGlyph& g : raw.glyphs){
        const std::vector<std::string>& rows = bitmaps.at(g.codepoint);
        EXPECT_EQ(g.fileAdvance, 6) << g.codepoint;
        // every texel of the 5x8 cell: the raster put where the bitmap has ink, nothing else
        for (int y = 0; y < 8; y++){
            for (int x = 0; x < 5; x++){
                const int gx = x - g.offsetX, gy = y - g.offsetY;
                uint8_t a = 0;
                if (gx >= 0 && gy >= 0 && gx < g.width && gy < g.height)
                    a = g.rgba[((size_t)gy * (size_t)g.width + (size_t)gx) * 4 + 3];
                EXPECT_EQ(a, rows[(size_t)y][(size_t)x] == '#' ? 255 : 0) << "U+" << g.codepoint << " " << x << "," << y;
            }
        }
        // no partial alpha ever
        for (size_t i = 3; i < g.rgba.size(); i += 4)
            EXPECT_TRUE(g.rgba[i] == 0 || g.rgba[i] == 255);
    }
    // the kern table, rounded to whole pixels
    bool kern = false;
    for (const BitmapFontSettings::Kern& k : raw.kerning)
        if (k.left == 'A' && k.right == 'V' && k.amount == -1) kern = true;
    EXPECT_TRUE(kern);
}

TEST(BitmapFontParsers, TrueTypeThresholdChangesTheResultPredictably){
    std::shared_ptr<FontFace> face = FontFace::loadFile(bfpFixture("pixel8.ttf"));
    ASSERT_TRUE(face);
    // U+00A4: column 0 is 25 % covered, column 1 75 %
    auto columns = [&face](float threshold){
        TrueTypeOptions options;
        options.pixelSize = 8;
        options.threshold = threshold;
        options.chars = {0xA4};
        RawFont raw;
        std::vector<FontError> errors;
        EXPECT_TRUE(rasterTrueType(*face, options, raw, errors));
        std::vector<int> on(2, 0);
        const RawGlyph& g = raw.glyphs.at(0);
        for (int y = 0; y < g.height; y++)
            for (int x = 0; x < g.width; x++)
                if (g.rgba[((size_t)y * (size_t)g.width + (size_t)x) * 4 + 3] && x + g.offsetX >= 0 && x + g.offsetX < 2)
                    on[(size_t)(x + g.offsetX)] = 1;
        return on;
    };
    EXPECT_EQ(columns(0.5f), (std::vector<int>{0, 1}));
    EXPECT_EQ(columns(0.2f), (std::vector<int>{1, 1}));
    EXPECT_EQ(columns(0.8f), (std::vector<int>{0, 0}));
}

TEST(BitmapFontParsers, TrueTypeSmoothFontReportsPixelSizeUnknown){
    std::shared_ptr<FontFace> roboto = FontRegistry::getBuiltInLatin();
    ASSERT_TRUE(roboto);
    std::vector<int> candidates;
    EXPECT_EQ(detectPixelSize(*roboto, &candidates), 0);
    EXPECT_FALSE(candidates.empty());
    TrueTypeOptions options;
    options.chars = {'A'};
    RawFont raw;
    std::vector<FontError> errors;
    EXPECT_FALSE(rasterTrueType(*roboto, options, raw, errors));
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].kind, "font_pixel_size_unknown");
    EXPECT_EQ(errors[0].field, "source.pixel_size");
    EXPECT_NE(errors[0].message.find("closest"), std::string::npos);
}
