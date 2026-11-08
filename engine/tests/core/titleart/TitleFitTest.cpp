// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Auto-fit by geometry (docs/specs/S87-title-localisation-interchange.md R8-R11, D13, AC4):
// shrink, condense then shrink, balanced wrap (Latin and the kinsoku table), vertical columns, the
// min_scale floor, determinism, at most 9 measures, and the extent against the rims.

#include "gtest/gtest.h"

#include "titleart/support/TitleTestAssets.h"

#include "titleart/TitleFit.h"
#include "titleart/geometry/TitleGeometry.h"

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    class TitleFitTest : public ::testing::Test {
    protected:
        TestAssetResolver assets;
        TitleArtDoc doc;

        Json text(const std::string& value, Json fit, Json extra = Json::object()) {
            Json layer = Json{{"id", "0000000a"}, {"type", "text"}, {"text", value}, {"size", 100}, {"font", fontRef(OSWALD)}, {"fit", fit}};
            for (auto it = extra.begin(); it != extra.end(); ++it) layer[it.key()] = it.value();
            doc = documentWith({layer});
            clearTitleFitCache();
            return doc.layers()[0];
        }
    };

}

TEST_F(TitleFitTest, ShrinkFindsTheLargestScale) {
    Json layer = text("GROSSER GEWINN", Json{{"mode", "none"}});
    const TitleVec2 natural = measureTitleTextExtent(layer, doc, &assets);
    ASSERT_GT(natural.x, 100);
    layer = text("GROSSER GEWINN", Json{{"mode", "shrink"}, {"box", Json::array({natural.x * 0.7, natural.y * 2})}});
    const TitleFitResult r = computeTitleFit(layer, doc, &assets);
    EXPECT_TRUE(r.active);
    EXPECT_TRUE(r.fits);
    EXPECT_LE(r.extent.x, natural.x * 0.7 + 1e-6);
    EXPECT_NEAR(r.scale, 0.7, 1.0 / 256 + 0.01) << "the extent scales with the size";
    EXPECT_LE(r.measures, 9);
    // the next step up would not fit (largest to 1/256)
    Json bigger = layer;
    bigger["size"] = 100 * (r.scale + 2.0 / 256);
    EXPECT_GT(measureTitleTextExtent(bigger, doc, &assets).x, natural.x * 0.7);
}

TEST_F(TitleFitTest, FitsAlreadyMeansOneMeasure) {
    const Json layer = text("WIN", Json{{"mode", "shrink"}, {"box", Json::array({4000, 4000})}});
    const TitleFitResult r = computeTitleFit(layer, doc, &assets);
    EXPECT_TRUE(r.fits);
    EXPECT_DOUBLE_EQ(r.scale, 1.0);
    EXPECT_EQ(r.measures, 1);
}

TEST_F(TitleFitTest, TheMinScaleFloorReportsNoFit) {
    Json layer = text("GROSSER GEWINN", Json{{"mode", "none"}});
    const TitleVec2 natural = measureTitleTextExtent(layer, doc, &assets);
    layer = text("GROSSER GEWINN", Json{{"mode", "shrink"}, {"box", Json::array({natural.x * 0.3, natural.y * 2})}, {"min_scale", 0.6}});
    const TitleFitResult r = computeTitleFit(layer, doc, &assets);
    EXPECT_FALSE(r.fits);
    EXPECT_DOUBLE_EQ(r.scale, 0.6);
    EXPECT_GT(r.overflowPx, 0.0);
}

TEST_F(TitleFitTest, CondenseThenShrink) {
    Json layer = text("GROSSER GEWINN", Json{{"mode", "none"}});
    const TitleVec2 natural = measureTitleTextExtent(layer, doc, &assets);
    // 0.9 of the width: condensing alone fits
    layer = text("GROSSER GEWINN", Json{{"mode", "condense"}, {"box", Json::array({natural.x * 0.9, natural.y * 2})}});
    TitleFitResult r = computeTitleFit(layer, doc, &assets);
    EXPECT_TRUE(r.fits);
    EXPECT_DOUBLE_EQ(r.scale, 1.0);
    EXPECT_LT(r.hScale, 1.0);
    EXPECT_GE(r.hScale, 0.8);
    // 0.5: condensed to its floor, then shrunk
    layer = text("GROSSER GEWINN", Json{{"mode", "condense"}, {"box", Json::array({natural.x * 0.5, natural.y * 2})}});
    r = computeTitleFit(layer, doc, &assets);
    EXPECT_TRUE(r.fits);
    EXPECT_NEAR(r.hScale, 0.8, 1e-9);
    EXPECT_LT(r.scale, 1.0);
}

TEST_F(TitleFitTest, WrapBalancesTheLines) {
    Json layer = text("ONE TWO THREE FOUR", Json{{"mode", "none"}});
    const TitleVec2 natural = measureTitleTextExtent(layer, doc, &assets);
    layer = text("ONE TWO THREE FOUR", Json{{"mode", "wrap"}, {"box", Json::array({natural.x * 0.6, natural.y * 4})}, {"max_lines", 3}});
    const TitleFitResult r = computeTitleFit(layer, doc, &assets);
    EXPECT_TRUE(r.fits);
    EXPECT_EQ(r.lines, 2) << "the fewest lines that fit";
    EXPECT_EQ(r.text, "ONE TWO\nTHREE FOUR") << "the longest line as short as possible";
    Json applied = layer;
    applyTitleFit(applied, r);
    EXPECT_EQ(applied["text"], "ONE TWO\nTHREE FOUR");
}

TEST_F(TitleFitTest, KinsokuBreaks) {
    // between two ideographs or kana: yes; before small kana and closing marks, after opening ones: no
    EXPECT_TRUE(titleFitCanBreakBetween(0x5927, 0x5f53));      // 大当
    EXPECT_FALSE(titleFitCanBreakBetween(0x30ad, 0x30e3));     // キャ: never before a small kana
    EXPECT_FALSE(titleFitCanBreakBetween(0x5927, 0x3002));     // 大。
    EXPECT_FALSE(titleFitCanBreakBetween(0x300c, 0x5927));     // 「大
    EXPECT_FALSE(titleFitCanBreakBetween(0x30b9, 0x30fc));     // スー: never before the long vowel mark
    EXPECT_FALSE(titleFitCanBreakBetween('A', 'B')) << "Latin breaks only at spaces";
    // a CJK title wraps between characters, CharAdjust keys after the break move with them
    Json layer = text("\xe5\xa4\xa7\xe5\xbd\x93\xe3\x82\x8a\xe7\xa2\xba\xe5\xae\x9a", Json{{"mode", "none"}},
                      Json{{"font", fontRef(DELA_GOTHIC)}, {"chars", Json{{"4", Json{{"rotation", 10}}}}}});
    const TitleVec2 natural = measureTitleTextExtent(layer, doc, &assets);
    layer = text("\xe5\xa4\xa7\xe5\xbd\x93\xe3\x82\x8a\xe7\xa2\xba\xe5\xae\x9a", Json{{"mode", "wrap"}, {"box", Json::array({natural.x * 0.7, natural.y * 3})}},
                 Json{{"font", fontRef(DELA_GOTHIC)}, {"chars", Json{{"4", Json{{"rotation", 10}}}}}});
    const TitleFitResult r = computeTitleFit(layer, doc, &assets);
    EXPECT_TRUE(r.fits);
    EXPECT_EQ(r.lines, 2);
    Json applied = layer;
    applyTitleFit(applied, r);
    EXPECT_TRUE(applied["chars"].contains("5")) << "the character after an inserted newline keeps its adjustment";
}

TEST_F(TitleFitTest, VerticalMeasuresTheColumn) {
    Json layer = text("\xe5\xa4\xa7\xe5\xbd\x93\xe3\x82\x8a\xe7\xa2\xba\xe5\xae\x9a", Json{{"mode", "none"}},
                      Json{{"font", fontRef(DELA_GOTHIC)}, {"vertical", true}});
    const TitleVec2 natural = measureTitleTextExtent(layer, doc, &assets);
    EXPECT_GT(natural.y, natural.x) << "a column";
    layer = text("\xe5\xa4\xa7\xe5\xbd\x93\xe3\x82\x8a\xe7\xa2\xba\xe5\xae\x9a", Json{{"mode", "shrink"}, {"box", Json::array({natural.x * 2, natural.y * 0.75})}},
                 Json{{"font", fontRef(DELA_GOTHIC)}, {"vertical", true}});
    const TitleFitResult r = computeTitleFit(layer, doc, &assets);
    EXPECT_TRUE(r.fits);
    EXPECT_LT(r.scale, 0.8);
}

TEST_F(TitleFitTest, Deterministic) {
    Json layer = text("GROSSER GEWINN", Json{{"mode", "condense"}, {"box", Json::array({300, 200})}});
    const TitleFitResult a = computeTitleFit(layer, doc, &assets);
    clearTitleFitCache();
    const TitleFitResult b = computeTitleFit(layer, doc, &assets);
    EXPECT_EQ(a.scale, b.scale);
    EXPECT_EQ(a.hScale, b.hScale);
    EXPECT_EQ(a.text, b.text);
}

// the extent equals the outline bounds grown by the cumulative rim width (round joins)
TEST_F(TitleFitTest, ExtentIncludesTheRims) {
    Json style = Json{{"rims", Json::array({Json{{"width", 5}, {"join", "round"}}, Json{{"width", 3}, {"join", "round"}}})}};
    const Json bare = text("WIN", Json{{"mode", "none"}});
    const TitleVec2 a = measureTitleTextExtent(bare, doc, &assets);
    const Json rimmed = text("WIN", Json{{"mode", "none"}}, Json{{"style", style}});
    const TitleVec2 b = measureTitleTextExtent(rimmed, doc, &assets);
    // rims are % of the size (100): 8 % = 8 px a side
    EXPECT_NEAR(b.x - a.x, 16.0, 1.0);
    EXPECT_NEAR(b.y - a.y, 16.0, 1.0);
}
