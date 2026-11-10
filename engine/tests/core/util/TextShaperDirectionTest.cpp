// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S80 R20 (§6.6 "TextShaper additions"): top-to-bottom shaping with vertical forms, feature
// lists and per-face font overrides; the defaults shape as before (yAdvance 0, the face's own font).

#include "gtest/gtest.h"

#include "TestPaths.h"

#include "util/FontFace.h"
#include "util/TextShaper.h"

#include "hb.h"

#include <filesystem>
#include <fstream>

using namespace jasperengine;

namespace {

    std::shared_ptr<FontFace> testFont(const char* file) {
        const std::filesystem::path path = std::filesystem::path(test::testPaths().sourceDir) / "titleart" / "fonts" / file;
        std::ifstream in(path, std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return FontFace::loadMemory(bytes.data(), bytes.size(), 0, true, file);
    }

    std::vector<ShapedGlyph> shape(const FontFace& face, const std::string& utf8, const ShapeOptions& options) {
        std::vector<uint32_t> cps;
        TextShaper::decodeUtf8(utf8.data(), utf8.size(), cps);
        std::vector<ShapedGlyph> out;
        TextShaper::shapeRange({&face}, {1.0f}, cps, 0, cps.size(), 0, options, out);
        return out;
    }

}

TEST(TextShaperDirectionTest, VerticalUsesVerticalFormsAndAdvancesDown) {
    const std::shared_ptr<FontFace> dela = testFont("DelaGothicOne-Regular.ttf");
    ASSERT_TRUE(dela);
    const std::vector<ShapedGlyph> across = shape(*dela, "確、ー", ShapeOptions{});
    ShapeOptions down;
    down.direction = ShapeDirection::Vertical;
    const std::vector<ShapedGlyph> column = shape(*dela, "確、ー", down);
    ASSERT_EQ(across.size(), 3u);
    ASSERT_EQ(column.size(), 3u);
    for (const ShapedGlyph& g : across) EXPECT_EQ(g.yAdvance, 0.0f) << "horizontal text has no vertical advance";
    for (const ShapedGlyph& g : column) EXPECT_LT(g.yAdvance, 0.0f) << "top to bottom: HarfBuzz's y up advance is negative";
    EXPECT_EQ(column[0].glyphIndex, across[0].glyphIndex) << "a kanji has no vertical form";
    EXPECT_NE(column[1].glyphIndex, across[1].glyphIndex) << "the comma takes its vert form";
    EXPECT_NE(column[2].glyphIndex, across[2].glyphIndex) << "the long vowel mark turns";
}

TEST(TextShaperDirectionTest, FeaturesApplyOnTopOfTheDefaults) {
    const std::shared_ptr<FontFace> sans = testFont("SourceSans3-Regular.otf");
    const std::shared_ptr<FontFace> oswald = testFont("Oswald-wght.ttf");
    ASSERT_TRUE(sans && oswald);
    // a default feature turned off: kerning of A V
    const std::vector<ShapedGlyph> kerned = shape(*oswald, "AV", ShapeOptions{});
    ShapeOptions noKern;
    noKern.features = {{HB_TAG('k', 'e', 'r', 'n'), 0}};
    const std::vector<ShapedGlyph> spaced = shape(*oswald, "AV", noKern);
    ASSERT_EQ(kerned.size(), 2u);
    EXPECT_LT(kerned[0].xAdvance, spaced[0].xAdvance);
    ShapeOptions smallCaps;
    smallCaps.features = {{HB_TAG('s', 'm', 'c', 'p'), 1}};
    EXPECT_NE(shape(*sans, "a", smallCaps)[0].glyphIndex, shape(*sans, "a", ShapeOptions{})[0].glyphIndex);
}

TEST(TextShaperDirectionTest, OptionalRasterizerOpensCff2) {
    // stb_truetype cannot read CFF2: the atlases' fonts still refuse it, Title Art's open it
    const std::filesystem::path path = std::filesystem::path(test::testPaths().sourceDir) / "titleart" / "fonts" / "SourceSans3VF-Upright.otf";
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string error;
    EXPECT_FALSE(FontFace::loadMemory(bytes.data(), bytes.size(), 0, true, "vf", &error));
    EXPECT_FALSE(error.empty());
    const std::shared_ptr<FontFace> vf = FontFace::loadMemory(bytes.data(), bytes.size(), 0, true, "vf", nullptr, FontFaceRasterizer::Optional);
    ASSERT_TRUE(vf);
    EXPECT_FALSE(vf->canRasterize());
    EXPECT_NE(vf->getFamilyName().find("SourceSans"), std::string::npos) << vf->getFamilyName();
    EXPECT_TRUE(vf->hasCodepoint('a'));
    EXPECT_GT(vf->getAscender(), 0);
    EXPECT_LT(vf->getDescender(), 0);
    EXPECT_GT(vf->getGlyphAdvance(vf->getGlyphIndex('a'), 1.0f), 0.0f);
    std::vector<unsigned char> pixels;
    int w = 1, h = 1, x = 0, y = 0;
    EXPECT_FALSE(vf->rasterizeGlyph(vf->getGlyphIndex('a'), 0.1f, pixels, w, h, x, y));
    EXPECT_EQ(w, 0);
    // a font stb_truetype reads is unchanged by the option
    const std::shared_ptr<FontFace> cff = testFont("SourceSans3-Regular.otf");
    ASSERT_TRUE(cff);
    EXPECT_TRUE(cff->canRasterize());
}

TEST(TextShaperDirectionTest, PerFaceFontOverride) {
    const std::shared_ptr<FontFace> oswald = testFont("Oswald-wght.ttf");
    ASSERT_TRUE(oswald);
    hb_font_t* bold = hb_font_create(hb_font_get_face(oswald->getHbFont()));
    hb_font_set_scale(bold, oswald->getUnitsPerEm(), oswald->getUnitsPerEm());
    const hb_variation_t wght = {HB_TAG('w', 'g', 'h', 't'), 700};
    hb_font_set_variations(bold, &wght, 1);
    ShapeOptions options;
    options.fonts = {bold};
    const float boldAdvance = shape(*oswald, "H", options)[0].xAdvance;
    options.fonts = {nullptr};
    const float regularAdvance = shape(*oswald, "H", options)[0].xAdvance;
    EXPECT_GT(boldAdvance, regularAdvance) << "the sub-font's variation shapes the run";
    EXPECT_EQ(regularAdvance, shape(*oswald, "H", ShapeOptions{})[0].xAdvance) << "a null entry is the face's own font";
    hb_font_destroy(bold);
}
