// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S80 R20-R23, R48 (AC21, AC31): shaping over the fallback chain, variable axes, .otf / .ttc,
// horizontal and vertical layout, proportional spacing, per-character transforms, missing glyphs
// and the shaping budget.

#include "gtest/gtest.h"

#include "titleart/support/TitleTestAssets.h"

#include "titleart/text/TitleTextLayout.h"
#include "util/FontFace.h"

#include "hb.h"

#include <chrono>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    class TitleTextLayoutTest : public ::testing::Test {
    protected:
        TestAssetResolver assets;

        // a normalized Text layer with `members`
        Json textLayer(Json members) {
            members["type"] = "text";
            if (!members.contains("id")) members["id"] = "0000000a";
            doc = documentWith({members});
            return doc.layers()[0];
        }

        TitleArtDoc doc;
    };

    double totalAdvance(const LaidOutText& t, uint32_t line = 0) {
        double sum = 0;
        for (const LaidOutCluster& c : t.clusters) {
            if (c.line == line) sum += c.advance;
        }
        return sum;
    }

    const LaidOutCluster* clusterAt(const LaidOutText& t, uint32_t index) {
        for (const LaidOutCluster& c : t.clusters) {
            if (c.index == index) return &c;
        }
        return nullptr;
    }

    bool hasVerb(const geometry::Path& p, geometry::PathVerb verb) {
        return std::find(p.verbList().begin(), p.verbList().end(), verb) != p.verbList().end();
    }

    const TitleProblem* problemOf(const LaidOutText& t, const std::string& code) {
        for (const TitleProblem& p : t.problems) {
            if (p.code == code) return &p;
        }
        return nullptr;
    }

}

TEST_F(TitleTextLayoutTest, ShapesOverTheFallbackChain) {
    TitleTextLayout layout(&assets);
    const Json layer = textLayer({{"text", "AB確定"}, {"font", fontRef(OSWALD)}, {"fallback_fonts", Json::array({fontRef(DELA_GOTHIC)})}});
    const LaidOutText& t = layout.layout(layer);
    ASSERT_EQ(t.faces.size(), 3u) << "font, fallback font, built-in face";
    EXPECT_EQ(t.faces[0]->getFamilyName(), "Oswald");
    EXPECT_EQ(t.faces[1]->getFamilyName(), "Dela Gothic One");
    ASSERT_EQ(t.clusters.size(), 4u);
    EXPECT_EQ(t.clusters[0].face, 0);
    EXPECT_EQ(t.clusters[1].face, 0);
    EXPECT_EQ(t.clusters[2].face, 1) << "確 is not in Oswald: the fallback draws it";
    EXPECT_EQ(t.clusters[3].face, 1);
    for (const LaidOutCluster& c : t.clusters) {
        EXPECT_FALSE(c.missing);
        ASSERT_EQ(c.glyphs.size(), 1u);
        EXPECT_NE(c.glyphs[0].glyphIndex, 0u);
        EXPECT_FALSE(c.glyphs[0].outline.empty());
    }
    EXPECT_EQ(problemOf(t, "glyph_missing"), nullptr);
    // left to right, the frame centred on the origin
    EXPECT_LT(t.clusters[0].pen.x, t.clusters[3].pen.x);
    EXPECT_NEAR(t.frame.x + t.frame.width / 2, 0.0, 1e-9);
    EXPECT_NEAR(t.frame.y + t.frame.height / 2, 0.0, 1e-9);
}

TEST_F(TitleTextLayoutTest, VariableAxesChangeOutlinesAndAdvances) {
    // glyf variations (Oswald) and CFF2 (Source Sans 3 VF): the sub-font shapes and draws
    for (const char* font : {OSWALD, SOURCE_SANS_VF}) {
        SCOPED_TRACE(font);
        TitleTextLayout layout(&assets);
        const LaidOutText light = layout.layout(textLayer({{"text", "HO"}, {"font", fontRef(font)}, {"variations", {{"wght", 300}}}}));
        const LaidOutText bold = layout.layout(textLayer({{"text", "HO"}, {"font", fontRef(font)}, {"variations", {{"wght", 700}}}}));
        const TitleProblem* unknown = problemOf(light, "variation_axis_unknown");
        EXPECT_EQ(unknown, nullptr) << (unknown ? unknown->message + " " + unknown->detail.dump() : "");
        ASSERT_EQ(light.clusters.size(), 2u);
        ASSERT_EQ(bold.clusters.size(), 2u);
        EXPECT_GT(bold.clusters[0].advance, light.clusters[0].advance + 1.0) << "a bolder H is wider";
        const double lightArea = std::abs(light.clusters[0].glyphs[0].outline.signedArea(0.01));
        const double boldArea = std::abs(bold.clusters[0].glyphs[0].outline.signedArea(0.01));
        EXPECT_GT(boldArea, lightArea * 1.2) << "the outline follows the axis";
        // the advance matches the outline: a heavier stem moves the right side bearing too
        EXPECT_GT(bold.clusters[0].inkBox.width, light.clusters[0].inkBox.width);
        if (std::string(font) == SOURCE_SANS_VF) {
            EXPECT_TRUE(hasVerb(bold.clusters[1].glyphs[0].outline, geometry::PathVerb::Cubic)) << "CFF2 cubics";
        }
    }
}

TEST_F(TitleTextLayoutTest, UnknownAxisIsReportedAndOutOfRangeIsClamped) {
    TitleTextLayout layout(&assets);
    const LaidOutText unknown = layout.layout(textLayer({{"text", "H"}, {"font", fontRef(OSWALD)}, {"variations", {{"wdth", 80}}}}));
    const TitleProblem* p = problemOf(unknown, "variation_axis_unknown");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->path, "variations.wdth");
    EXPECT_EQ(p->layer, "0000000a");
    // wght above Oswald's 700 draws as 700
    const double clamped = layout.layout(textLayer({{"text", "H"}, {"font", fontRef(OSWALD)}, {"variations", {{"wght", 1000}}}})).clusters[0].advance;
    const double top = layout.layout(textLayer({{"text", "H"}, {"font", fontRef(OSWALD)}, {"variations", {{"wght", 700}}}})).clusters[0].advance;
    EXPECT_DOUBLE_EQ(clamped, top);
}

TEST_F(TitleTextLayoutTest, CffOutlinesStayCubic) {
    TitleTextLayout layout(&assets);
    const LaidOutText& cff = layout.layout(textLayer({{"text", "a"}, {"font", fontRef(SOURCE_SANS)}}));
    ASSERT_EQ(cff.clusters.size(), 1u);
    const geometry::Path& a = cff.clusters[0].glyphs[0].outline;
    EXPECT_TRUE(hasVerb(a, geometry::PathVerb::Cubic));
    EXPECT_FALSE(hasVerb(a, geometry::PathVerb::Quad));
    const LaidOutText& ttf = layout.layout(textLayer({{"text", "a"}, {"font", fontRef(OSWALD)}}));
    EXPECT_TRUE(hasVerb(ttf.clusters[0].glyphs[0].outline, geometry::PathVerb::Quad)) << "TrueType keeps its quadratics";
}

TEST_F(TitleTextLayoutTest, CollectionMemberByFaceIndex) {
    assets.addFile("fonts/pair.ttc", buildCollection({readFileBytes(titleFontsDir() + "/Oswald-wght.ttf"),
                                                      readFileBytes(titleFontsDir() + "/SourceSans3-Regular.otf")}));
    TitleTextLayout layout(&assets);
    const LaidOutText& first = layout.layout(textLayer({{"text", "a"}, {"font", fontRef("fonts/pair.ttc")}, {"face_index", 0}}));
    ASSERT_FALSE(first.faces.empty());
    EXPECT_EQ(first.faces[0]->getFamilyName(), "Oswald");
    const LaidOutText& second = layout.layout(textLayer({{"text", "a"}, {"font", fontRef("fonts/pair.ttc")}, {"face_index", 1}}));
    ASSERT_FALSE(second.faces.empty());
    EXPECT_EQ(second.faces[0]->getFamilyName(), "Source Sans 3");
    EXPECT_TRUE(hasVerb(second.clusters[0].glyphs[0].outline, geometry::PathVerb::Cubic));
}

TEST_F(TitleTextLayoutTest, KerningTrackingAndFeatures) {
    TitleTextLayout layout(&assets);
    const double kerned = totalAdvance(layout.layout(textLayer({{"text", "AV"}, {"font", fontRef(OSWALD)}})));
    const double plain = totalAdvance(layout.layout(textLayer({{"text", "AV"}, {"font", fontRef(OSWALD)}, {"kerning", false}})));
    EXPECT_LT(kerned, plain - 1.0) << "GPOS kerning pulls A and V together";
    // tracking: thousandths of an em after every cluster but the last
    const LaidOutText& base = layout.layout(textLayer({{"text", "HHHH"}, {"font", fontRef(OSWALD)}}));
    const double pen3 = base.clusters[3].pen.x - base.clusters[0].pen.x;
    const LaidOutText& tracked = layout.layout(textLayer({{"text", "HHHH"}, {"font", fontRef(OSWALD)}, {"tracking", 100}}));
    EXPECT_NEAR(tracked.clusters[3].pen.x - tracked.clusters[0].pen.x, pen3 + 3 * 0.1 * 200, 1e-6);
    // features on top of the defaults: small capitals
    const uint32_t lower = layout.layout(textLayer({{"text", "a"}, {"font", fontRef(SOURCE_SANS)}})).clusters[0].glyphs[0].glyphIndex;
    const uint32_t smcp = layout.layout(textLayer({{"text", "a"}, {"font", fontRef(SOURCE_SANS)}, {"features", {{"smcp", 1}}}}))
                              .clusters[0].glyphs[0].glyphIndex;
    EXPECT_NE(lower, smcp);
}

TEST_F(TitleTextLayoutTest, LinesAlignAndLineHeight) {
    TitleTextLayout layout(&assets);
    for (const char* align : {"start", "center", "end"}) {
        SCOPED_TRACE(align);
        const LaidOutText& t = layout.layout(textLayer({{"text", "HHHH\nH"}, {"font", fontRef(OSWALD)}, {"align", align}}));
        ASSERT_EQ(t.lines.size(), 2u);
        const LaidOutCluster& first = t.clusters[t.lines[0].firstCluster];
        const LaidOutCluster& second = t.clusters[t.lines[1].firstCluster];
        const double left0 = first.pen.x, left1 = second.pen.x;
        const double right0 = t.clusters[t.lines[0].firstCluster + 3].pen.x + t.clusters[t.lines[0].firstCluster + 3].advance;
        const double right1 = left1 + second.advance;
        if (std::string(align) == "start") {
            EXPECT_NEAR(left0, left1, 1e-9);
        }
        if (std::string(align) == "center") {
            EXPECT_NEAR((left0 + right0) / 2, (left1 + right1) / 2, 1e-9);
        }
        if (std::string(align) == "end") {
            EXPECT_NEAR(right0, right1, 1e-9);
        }
        // uniform lines step by line_height x size
        EXPECT_NEAR(first.pen.y - second.pen.y, 1.1 * 200, 1e-9);
        EXPECT_GT(t.lines[0].box.y, t.lines[1].box.y);
    }
}

TEST_F(TitleTextLayoutTest, ScalesReflowTheLine) {
    TitleTextLayout layout(&assets);
    const LaidOutText base = layout.layout(textLayer({{"text", "HH"}, {"font", fontRef(OSWALD)}}));
    const LaidOutText wide = layout.layout(textLayer({{"text", "HH"}, {"font", fontRef(OSWALD)}, {"h_scale", 2}}));
    EXPECT_NEAR(wide.clusters[0].advance, 2 * base.clusters[0].advance, 1e-9);
    EXPECT_NEAR(wide.clusters[0].inkBox.height, base.clusters[0].inkBox.height, 1e-6);
    // a smaller character moves the rest of the line, not just itself
    const LaidOutText small = layout.layout(textLayer({{"text", "HHH"}, {"font", fontRef(OSWALD)}, {"chars", {{"1", {{"scale", 0.5}}}}}}));
    const LaidOutText even = layout.layout(textLayer({{"text", "HHH"}, {"font", fontRef(OSWALD)}}));
    EXPECT_NEAR(small.clusters[1].advance, even.clusters[1].advance / 2, 1e-9);
    EXPECT_LT(small.frame.width, even.frame.width);
}

TEST_F(TitleTextLayoutTest, PerCharacterRotationKeepsNeighboursAndFrame) {
    TitleTextLayout layout(&assets);
    const LaidOutText base = layout.layout(textLayer({{"text", "ABC"}, {"font", fontRef(OSWALD)}}));
    const LaidOutText turned =
        layout.layout(textLayer({{"text", "ABC"}, {"font", fontRef(OSWALD)}, {"chars", {{"1", {{"rotation", 30}, {"offset", {10, 20}}}}}}}));
    for (int c : {0, 2}) {
        EXPECT_DOUBLE_EQ(turned.clusters[c].glyphs[0].position.x, base.clusters[c].glyphs[0].position.x);
        EXPECT_DOUBLE_EQ(turned.clusters[c].glyphs[0].position.y, base.clusters[c].glyphs[0].position.y);
    }
    EXPECT_DOUBLE_EQ(turned.frame.x, base.frame.x);
    EXPECT_DOUBLE_EQ(turned.frame.width, base.frame.width);
    EXPECT_DOUBLE_EQ(turned.frame.height, base.frame.height);
    // B turned about its em-cell centre, then moved by (10 %, 20 %) of the size
    const LaidOutCluster& b0 = base.clusters[1];
    const LaidOutCluster& b1 = turned.clusters[1];
    EXPECT_NEAR(b1.emCell.x + b1.emCell.width / 2, b0.emCell.x + b0.emCell.width / 2 + 20, 1e-9);
    EXPECT_NEAR(b1.emCell.y + b1.emCell.height / 2, b0.emCell.y + b0.emCell.height / 2 + 40, 1e-9);
    EXPECT_EQ(b1.glyphs[0].rotation, 30);
    EXPECT_NE(b1.inkBox.width, b0.inkBox.width);
    EXPECT_DOUBLE_EQ(b1.pen.x, b0.pen.x) << "the caret stays where the layout put it";
    // the transform maps the font outline onto the drawn one
    const geometry::Path& outline = b1.glyphs[0].outline;
    ASSERT_FALSE(outline.pointList().empty());
}

TEST_F(TitleTextLayoutTest, ShearLeansAboutTheCentreLine) {
    TitleTextLayout layout(&assets);
    const LaidOutText upright = layout.layout(textLayer({{"text", "I"}, {"font", fontRef(OSWALD)}}));
    const LaidOutText leaning = layout.layout(textLayer({{"text", "I"}, {"font", fontRef(OSWALD)}, {"shear", 20}}));
    EXPECT_GT(leaning.frame.width, upright.frame.width);
    EXPECT_NEAR(leaning.frame.x + leaning.frame.width / 2, 0.0, 1e-9) << "the sheared frame is centred again";
    // positive shear leans right: the top of the I moves right of its bottom
    const TitleRect ink = leaning.clusters[0].inkBox;
    double topX = 0, bottomX = 0, topY = -1e9, bottomY = 1e9;
    for (const TitleVec2& p : leaning.clusters[0].glyphs[0].outline.pointList()) {
        if (p.y > topY) topY = p.y, topX = p.x;
        if (p.y < bottomY) bottomY = p.y, bottomX = p.x;
    }
    EXPECT_GT(topX, bottomX);
    EXPECT_GT(ink.width, upright.clusters[0].inkBox.width);
}

TEST_F(TitleTextLayoutTest, VerticalColumnsUseVerticalForms) {
    TitleTextLayout layout(&assets);
    const Json members = {{"text", "確定、\n確"}, {"font", fontRef(DELA_GOTHIC)}, {"vertical", true}};
    const LaidOutText& t = layout.layout(textLayer(members));
    ASSERT_TRUE(t.vertical);
    ASSERT_EQ(t.lines.size(), 2u);
    const LaidOutCluster* kaku = clusterAt(t, 0);
    const LaidOutCluster* tei = clusterAt(t, 1);
    const LaidOutCluster* comma = clusterAt(t, 2);
    const LaidOutCluster* next = clusterAt(t, 4);
    ASSERT_TRUE(kaku && tei && comma && next);
    // top to bottom inside a column, columns right to left
    EXPECT_GT(kaku->emCell.y, tei->emCell.y);
    EXPECT_GT(tei->emCell.y, comma->emCell.y);
    EXPECT_NEAR(kaku->emCell.x + kaku->emCell.width / 2, tei->emCell.x + tei->emCell.width / 2, 1e-9);
    EXPECT_LT(next->emCell.x, kaku->emCell.x - 100) << "the second column is to the left";
    EXPECT_NEAR(kaku->pen.y - tei->pen.y, 200, 1e-9) << "one em cell per character";
    EXPECT_TRUE(kaku->glyphs[0].upright);
    // the comma takes its vertical form (vert / vrt2 through top-to-bottom shaping): not the
    // horizontal glyph
    const LaidOutText& horizontal = layout.layout(textLayer({{"text", "確定、\n確"}, {"font", fontRef(DELA_GOTHIC)}}));
    EXPECT_NE(comma->glyphs[0].glyphIndex, clusterAt(horizontal, 2)->glyphs[0].glyphIndex);
    EXPECT_EQ(kaku->glyphs[0].glyphIndex, clusterAt(horizontal, 0)->glyphs[0].glyphIndex);
}

TEST_F(TitleTextLayoutTest, TateChuYokoSetsShortRunsUpright) {
    TitleTextLayout layout(&assets);
    const LaidOutText& t = layout.layout(textLayer({{"text", "第12回"}, {"font", fontRef(DELA_GOTHIC)}, {"vertical", true}}));
    ASSERT_EQ(t.clusters.size(), 3u) << "第, the 12 cell, 回";
    const LaidOutCluster& run = t.clusters[1];
    ASSERT_EQ(run.codepoints.size(), 2u);
    ASSERT_EQ(run.glyphs.size(), 2u);
    EXPECT_DOUBLE_EQ(run.glyphs[0].position.y, run.glyphs[1].position.y) << "side by side across the column";
    EXPECT_LT(run.glyphs[0].position.x, run.glyphs[1].position.x);
    // centred on the column, not wider than one em
    const double column = t.clusters[0].emCell.x + t.clusters[0].emCell.width / 2;
    EXPECT_NEAR(run.inkBox.x + run.inkBox.width / 2, column, 15);
    EXPECT_LE(run.inkBox.width, 200 + 1e-6);
    // a run longer than tate_chu_yoko_max gets one cell per character
    const LaidOutText& longer = layout.layout(textLayer({{"text", "第1234回"}, {"font", fontRef(DELA_GOTHIC)}, {"vertical", true}}));
    EXPECT_EQ(longer.clusters.size(), 6u);
    const LaidOutText& off = layout.layout(textLayer({{"text", "第12回"}, {"font", fontRef(DELA_GOTHIC)}, {"vertical", true}, {"tate_chu_yoko", false}}));
    EXPECT_EQ(off.clusters.size(), 4u);
}

TEST_F(TitleTextLayoutTest, ProportionalNarrowsKanaOnly) {
    // Stick has palt / vpal; Dela Gothic One has neither, so its advances fit the ink
    for (const char* font : {STICK, DELA_GOTHIC}) {
        SCOPED_TRACE(font);
        TitleTextLayout layout(&assets);
        const LaidOutText full = layout.layout(textLayer({{"text", "ッ確A"}, {"font", fontRef(font)}}));
        const LaidOutText tight = layout.layout(textLayer({{"text", "ッ確A"}, {"font", fontRef(font)}, {"proportional", 1}}));
        ASSERT_EQ(full.clusters.size(), 3u);
        EXPECT_LT(tight.clusters[0].advance, full.clusters[0].advance - 2) << "the kana closes up";
        EXPECT_DOUBLE_EQ(tight.clusters[1].advance, full.clusters[1].advance) << "kanji keeps its metrics";
        EXPECT_DOUBLE_EQ(tight.clusters[2].advance, full.clusters[2].advance) << "Latin keeps its metrics";
        const LaidOutText half = layout.layout(textLayer({{"text", "ッ確A"}, {"font", fontRef(font)}, {"proportional", 0.5}}));
        EXPECT_NEAR(half.clusters[0].advance, (full.clusters[0].advance + tight.clusters[0].advance) / 2, 1e-9);
        // vertically, the kana's cell shortens
        const LaidOutText vfull = layout.layout(textLayer({{"text", "ッ確"}, {"font", fontRef(font)}, {"vertical", true}}));
        const LaidOutText vtight = layout.layout(textLayer({{"text", "ッ確"}, {"font", fontRef(font)}, {"vertical", true}, {"proportional", 1}}));
        EXPECT_LT(vtight.clusters[0].advance, vfull.clusters[0].advance - 2);
        EXPECT_DOUBLE_EQ(vtight.clusters[1].advance, vfull.clusters[1].advance);
    }
}

TEST_F(TitleTextLayoutTest, MissingGlyphIsReportedWithCoveringFonts) {
    assets.setProjectFonts({OSWALD, DELA_GOTHIC, STICK, SOURCE_SANS});
    TitleTextLayout layout(&assets);
    const LaidOutText& t = layout.layout(textLayer({{"text", "Aあ"}, {"font", fontRef(OSWALD)}}));
    const TitleProblem* p = problemOf(t, "glyph_missing");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->layer, "0000000a");
    ASSERT_EQ(p->detail["chars"].size(), 1u);
    EXPECT_EQ(p->detail["chars"][0]["index"], 1);
    EXPECT_EQ(p->detail["chars"][0]["codepoint"], 0x3042);
    EXPECT_EQ(p->detail["chars"][0]["char"], "あ");
    EXPECT_EQ(p->detail["fonts_with_it"], Json::array({DELA_GOTHIC, STICK}));
    // drawn as the primary face's .notdef, which keeps its advance so the layout does not jump
    ASSERT_EQ(t.clusters.size(), 2u);
    EXPECT_FALSE(t.clusters[0].missing);
    EXPECT_TRUE(t.clusters[1].missing);
    EXPECT_EQ(t.clusters[1].glyphs[0].glyphIndex, 0u);
    EXPECT_EQ(t.clusters[1].face, 0);
    const double upem = t.faces[0]->getUnitsPerEm();
    const double notdef = hb_font_get_glyph_h_advance(t.faces[0]->getHbFont(), 0) * 200 / upem;
    EXPECT_NEAR(t.clusters[1].advance, notdef, 1e-4) << "single-precision shaper advances";
    EXPECT_GT(t.clusters[1].advance, 0);
    // a fallback font that has it clears the problem
    const LaidOutText& fixed = layout.layout(textLayer({{"text", "Aあ"}, {"font", fontRef(OSWALD)}, {"fallback_fonts", Json::array({fontRef(STICK)})}}));
    EXPECT_EQ(problemOf(fixed, "glyph_missing"), nullptr);
}

TEST_F(TitleTextLayoutTest, MissingFontIsReportedAndTheChainContinues) {
    TitleTextLayout layout(&assets);
    const LaidOutText& t = layout.layout(textLayer({{"text", "A"}, {"font", fontRef("fonts/Gone.ttf")}}));
    const TitleProblem* p = problemOf(t, "asset_missing");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->path, "font");
    ASSERT_EQ(t.faces.size(), 1u) << "the built-in face draws it";
    EXPECT_NE(t.clusters[0].glyphs[0].glyphIndex, 0u);
}

TEST_F(TitleTextLayoutTest, OverridesApplyToTheirLayerOnly) {
    TitleTextLayout layout(&assets);
    const Json layer = textLayer({{"text", "AB"}, {"font", fontRef(OSWALD)}});
    const LaidOutText& overridden = layout.layout(layer, {FieldOverride{"0000000a", "text", "ABC"}, FieldOverride{"ffffffff", "text", "X"}});
    EXPECT_EQ(overridden.clusters.size(), 3u);
    EXPECT_EQ(layout.layout(layer).clusters.size(), 2u) << "the layer itself is untouched";
}

TEST_F(TitleTextLayoutTest, LayoutsAreCachedByTheirInputs) {
    TitleTextLayout layout(&assets);
    const Json a = textLayer({{"text", "確定"}, {"font", fontRef(DELA_GOTHIC)}});
    const LaidOutText* first = &layout.layout(a);
    const uint64_t hash = first->hash;
    EXPECT_EQ(&layout.layout(a), first);
    // a colour of the face is not a layout input
    Json recoloured = a;
    recoloured["style"]["face"]["fill"]["color"] = "#ff0000";
    EXPECT_EQ(layout.layout(recoloured).hash, hash);
    Json other = a;
    other["text"] = "確";
    EXPECT_NE(layout.layout(other).hash, hash);
    // the same inputs in a fresh object give the same layout (deterministic)
    TitleTextLayout again(&assets);
    const LaidOutText& b = again.layout(a);
    EXPECT_EQ(b.hash, hash);
    ASSERT_EQ(b.clusters.size(), 2u);
    EXPECT_EQ(b.clusters[1].glyphs[0].outline.pointList().size(), layout.layout(a).clusters[1].glyphs[0].outline.pointList().size());
    EXPECT_DOUBLE_EQ(b.clusters[1].glyphs[0].position.x, layout.layout(a).clusters[1].glyphs[0].position.x);
}

TEST_F(TitleTextLayoutTest, Budget) {
    // R48: shaping and outlines of 64 CJK glyphs with two fallback fonts within 6 ms (Release),
    // then cached; fonts are loaded once per resolver, so they are loaded before the clock
    std::string text;
    const char* kanji[] = {"確", "定", "激", "熱", "大", "当", "連", "荘", "金", "銀", "銅", "赤", "青", "緑", "黄", "白"};
    for (int i = 0; i < 64; i++) text += kanji[i % 16];
    const Json layer = textLayer({{"text", text}, {"font", fontRef(OSWALD)}, {"fallback_fonts", Json::array({fontRef(STICK), fontRef(DELA_GOTHIC)})}});
    {
        TitleTextLayout warm(&assets);
        warm.layout(layer);
    }
    TitleTextLayout layout(&assets);
    const auto start = std::chrono::steady_clock::now();
    const LaidOutText& t = layout.layout(layer);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    ASSERT_EQ(t.clusters.size(), 64u);
    for (const LaidOutCluster& c : t.clusters) EXPECT_FALSE(c.glyphs[0].outline.empty());
    const auto cachedStart = std::chrono::steady_clock::now();
    layout.layout(layer);
    const double cachedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cachedStart).count();
    std::printf("[ budget   ] 64 CJK glyphs, two fallback fonts: %.3f ms, cached %.3f ms\n", ms, cachedMs);
#ifdef NDEBUG
    EXPECT_LE(ms, 6.0);
#else
    EXPECT_LE(ms, 60.0);
#endif
    EXPECT_LT(cachedMs, ms);
}
