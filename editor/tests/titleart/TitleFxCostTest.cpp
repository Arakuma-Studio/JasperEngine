// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S83 §6.4, R37, R46: the static cost readout render_title {options: {cost}} returns. Every
// visible generator layer and enabled effect is a row with its class, passes, FRE and predicted
// ms on both tiers; a hidden generator and a disabled effect cost nothing (the readout equals the
// document without them); the mobile tier reads each definition's mobile estimate.

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/fx/FxCost.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;

namespace {

    titleart::TitleArtDoc parse(const std::string& text) {
        titleart::TitleArtParseResult parsed = titleart::parseTitleArt(text);
        EXPECT_TRUE(parsed.ok) << parsed.error.toString();
        return parsed.doc;
    }

    const char* WITH_FX = R"({"format": "jasperengine.titleart", "version": 1,
      "canvas": {"width": 1920, "height": 1080},
      "layers": [
        {"id": "00000001", "type": "text", "name": "Title", "size": 200,
         "effects": [{"id": "00000021", "type": "outer_glow"}, {"id": "00000022", "type": "drop_shadow", "enabled": false}]},
        {"id": "00000002", "type": "generator", "name": "Rings", "generator": {"type": "shockwave"}},
        {"id": "00000003", "type": "generator", "name": "Noise", "visible": false, "generator": {"type": "fractal_noise"}}
      ]})";

    const char* WITHOUT_FX = R"({"format": "jasperengine.titleart", "version": 1,
      "canvas": {"width": 1920, "height": 1080},
      "layers": [
        {"id": "00000001", "type": "text", "name": "Title", "size": 200, "effects": [{"id": "00000021", "type": "outer_glow"}]},
        {"id": "00000002", "type": "generator", "name": "Rings", "generator": {"type": "shockwave"}}
      ]})";

}

TEST(TitleFxCostTest, RowsPerEffectAndGenerator) {
    const Json cost = estimateTitleFxCost(parse(WITH_FX), titleart::FxTier::Desktop);
    ASSERT_EQ(cost["items"].size(), 2u) << cost.dump();
    const Json& glow = cost["items"][0];
    const Json& rings = cost["items"][1];
    EXPECT_EQ(glow["entry"], "effect");
    EXPECT_EQ(glow["type"], "outer_glow");
    EXPECT_EQ(rings["entry"], "generator");
    EXPECT_EQ(rings["type"], "shockwave");
    EXPECT_EQ(rings["live"]["kind"], "kernel");
    for (const char* key : {"class", "declared", "passes", "fre", "taps", "instances", "cpu_ms", "sdf", "analysis", "readback", "desktop_ms", "mobile_ms", "live"}) {
        EXPECT_TRUE(rings.contains(key)) << key;
    }
    EXPECT_GT(rings["mobile_ms"].get<double>(), rings["desktop_ms"].get<double>());
    EXPECT_GE(cost["total"]["passes"].get<int>(), 2);
    const std::string summary = titleFxCostSummary(cost);
    EXPECT_EQ(summary.rfind("cost: C", 0), 0u) << summary;
    EXPECT_NE(summary.find("Rings: shockwave"), std::string::npos) << summary;
}

TEST(TitleFxCostTest, DisabledAndHiddenCostNothing) {
    // R37: the readout with a disabled effect and a hidden generator equals the one without them
    for (titleart::FxTier tier : {titleart::FxTier::Desktop, titleart::FxTier::Mobile}) {
        EXPECT_EQ(estimateTitleFxCost(parse(WITH_FX), tier), estimateTitleFxCost(parse(WITHOUT_FX), tier));
    }
}

TEST(TitleFxCostTest, ClassesFollowTheStaticLimits) {
    titleart::FxCost c;
    c.fre = 1.0;
    c.maxTapsPerPixel = 16;
    EXPECT_EQ(titleFxCostClass(c), titleart::CostClass::C0);
    c.fre = 5.0;
    EXPECT_EQ(titleFxCostClass(c), titleart::CostClass::C2);
    c.fre = 2.0;
    c.sdf = true;
    EXPECT_EQ(titleFxCostClass(c), titleart::CostClass::C3) << "an SDF makes it C3";
    c.sdf = false;
    c.fre = 50.0;
    EXPECT_EQ(titleFxCostClass(c), titleart::CostClass::C4);
}
