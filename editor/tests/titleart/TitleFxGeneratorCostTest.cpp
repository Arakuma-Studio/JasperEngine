// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S83 §6.4, R37, R46: the generator rows of the static cost readout render_title {options: {cost}}
// returns (FxGeneratorCost.h inside FxCost.h's estimate). Every visible generator layer is a row with its class, passes, FRE and predicted
// ms on both tiers; a hidden generator and a disabled effect cost nothing (the readout equals the
// document without them); the mobile tier reads each definition's mobile estimate.

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/fx/FxCost.h"
#include "titleart/fx/FxGeneratorCost.h"

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

TEST(TitleFxGeneratorCostTest, OneRowPerVisibleGenerator) {
    const TitleFxCost cost = estimateTitleFxCost(parse(WITH_FX), titleart::FxTier::Desktop);
    const TitleFxCostRow* rings = nullptr;
    int generators = 0;
    for (const TitleFxCostRow& r : cost.rows) {
        if (r.path != "gen") continue;
        generators++;
        if (r.type == "shockwave") rings = &r;
    }
    EXPECT_EQ(generators, 1) << "the hidden Fractal Noise adds no row";
    ASSERT_TRUE(rings);
    EXPECT_EQ(rings->layer, "00000002");
    EXPECT_EQ(rings->liveKind, titleart::LiveKind::Kernel);
    EXPECT_GE(rings->cost.passes, 1);
    EXPECT_GT(rings->mobileMs, rings->desktopMs);
    const Json json = cost.toJson();
    bool found = false;
    for (const Json& r : json["rows"]) found = found || r["type"] == "shockwave";
    EXPECT_TRUE(found);
    EXPECT_NE(cost.summary().find("shockwave"), std::string::npos) << cost.summary();
}

TEST(TitleFxGeneratorCostTest, HiddenGeneratorsAndDisabledEffectsCostNothing) {
    // R37: the readout with a disabled effect and a hidden generator equals the one without them
    for (titleart::FxTier tier : {titleart::FxTier::Desktop, titleart::FxTier::Mobile}) {
        EXPECT_EQ(estimateTitleFxCost(parse(WITH_FX), tier).toJson(), estimateTitleFxCost(parse(WITHOUT_FX), tier).toJson());
    }
}

TEST(TitleFxGeneratorCostTest, MobileReadsTheMobileEstimate) {
    // Lens Flare draws 8 quads on the mobile tier (C0) and a full-canvas pass on the desktop one
    const char* flare = R"({"format": "jasperengine.titleart", "version": 1, "canvas": {"width": 1920, "height": 1080},
      "layers": [{"id": "00000002", "type": "generator", "name": "Flare", "generator": {"type": "lens_flare"}}]})";
    const TitleFxCost desktop = estimateTitleFxCost(parse(flare), titleart::FxTier::Desktop);
    const TitleFxCost mobile = estimateTitleFxCost(parse(flare), titleart::FxTier::Mobile);
    ASSERT_EQ(desktop.rows.size(), 1u);
    ASSERT_EQ(mobile.rows.size(), 1u);
    EXPECT_EQ(mobile.rows[0].costClass, titleart::CostClass::C0);
    EXPECT_LT(mobile.rows[0].cost.fre, desktop.rows[0].cost.fre);
}
