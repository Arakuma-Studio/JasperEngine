// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The S13 side of the post-processing presets and inspector model (docs/specs/S13-post-fx.md):
// the film, vhs and mobile_adaptive presets (R10), the row rules (R15), the cost readout (R17),
// the dynamic resolution readout (R16) and .cube as a colour LUT asset (R18, S04).

#include <gtest/gtest.h>

#include "PostProcessingPresets.h"
#include "asset/AssetTypes.h"
#include "util/CubeLut.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    bool hasPass(const PostProcessingCostSide& side, const std::string& label) {
        return std::any_of(side.passes.begin(), side.passes.end(), [&](const PostProcessingCostPass& pass) { return pass.label == label; });
    }

    bool contains(const std::vector<std::string>& list, const std::string& text) {
        return std::any_of(list.begin(), list.end(), [&](const std::string& item) { return item.find(text) != std::string::npos; });
    }

}

TEST(PostProcessingPresets, S13PresetsAndTheirLooks) {
    const PostProcessingPreset* film = findPostProcessingPreset("film");
    const PostProcessingPreset* vhs = findPostProcessingPreset("VHS Tape");
    const PostProcessingPreset* adaptive = findPostProcessingPreset("mobile_adaptive");
    const PostProcessingPreset* mobile = findPostProcessingPreset("mobile");
    ASSERT_NE(film, nullptr);
    ASSERT_NE(vhs, nullptr);
    ASSERT_NE(adaptive, nullptr);
    ASSERT_NE(mobile, nullptr);
    EXPECT_EQ(vhs->id, "vhs");

    // Film: neutral curve, warm grade, Lens grain, halation, light damage without a gate mask, vignette
    EXPECT_EQ(film->settings.tonemap, TonemapMode::NEUTRAL);
    EXPECT_TRUE(film->settings.colorGradingEnabled);
    EXPECT_GT(film->settings.temperature, 0.0f);
    EXPECT_TRUE(film->settings.filmGrainEnabled);
    EXPECT_EQ(film->settings.filmGrainType, FilmGrainType::LENS);
    EXPECT_TRUE(film->settings.halationEnabled);
    EXPECT_TRUE(film->settings.filmDamageEnabled);
    EXPECT_EQ(film->settings.filmGateMask, FilmGateMask::NONE);
    EXPECT_TRUE(film->settings.vignetteEnabled);

    // VHS Tape: the VHS defaults, classic grain 0.1, a desaturating grade, no CRT
    EXPECT_TRUE(vhs->settings.vhsEnabled);
    EXPECT_FLOAT_EQ(vhs->settings.vhsIntensity, PostProcessingSettings().vhsIntensity);
    EXPECT_TRUE(vhs->settings.filmGrainEnabled);
    EXPECT_EQ(vhs->settings.filmGrainType, FilmGrainType::CLASSIC);
    EXPECT_FLOAT_EQ(vhs->settings.filmGrainIntensity, 0.1f);
    EXPECT_LT(vhs->settings.saturation, 1.0f);
    EXPECT_FALSE(vhs->settings.crtEnabled);

    // Mobile Adaptive: Mobile plus dynamic resolution at min 0.6
    PostProcessingSettings expected = mobile->settings;
    expected.dynamicResolutionEnabled = true;
    expected.dynamicResolutionMinScale = 0.6f;
    EXPECT_TRUE(adaptive->settings == expected);
    EXPECT_FALSE(mobile->settings.dynamicResolutionEnabled);

    // every preset is recognised from its own settings (saved scenes keep their preset name)
    for (const PostProcessingPreset& preset : getPostProcessingPresets()) {
        const PostProcessingPreset* matched = matchPostProcessingPreset(preset.settings);
        ASSERT_NE(matched, nullptr) << preset.id;
        EXPECT_EQ(matched->id, preset.id);
    }
}

TEST(PostProcessingPresets, RowStatesExplainInactiveRows) {
    PostProcessingSettings settings;
    EXPECT_FALSE(getPostProcessingRowState(settings, "lut_input_encoding", false).shown);
    settings.lutTexture = "luts/Look.CUBE";
    EXPECT_TRUE(getPostProcessingRowState(settings, "lut_input_encoding", false).shown);
    settings.lutTexture = "luts/strip.png";
    EXPECT_FALSE(getPostProcessingRowState(settings, "lut_input_encoding", false).shown);

    EXPECT_FALSE(getPostProcessingRowState(settings, "film_grain_size", false).shown);
    EXPECT_FALSE(getPostProcessingRowState(settings, "film_grain_colored", false).shown);
    settings.filmGrainType = FilmGrainType::LENS;
    EXPECT_TRUE(getPostProcessingRowState(settings, "film_grain_size", false).shown);
    EXPECT_FALSE(getPostProcessingRowState(settings, "film_grain_colored", false).shown);
    settings.filmGrainType = FilmGrainType::FILMIC;
    EXPECT_TRUE(getPostProcessingRowState(settings, "film_grain_colored", false).shown);

    EXPECT_EQ(getPostProcessingRowState(settings, "fxaa_enabled", false).inactiveReason, "");
    settings.vhsEnabled = true;
    EXPECT_NE(getPostProcessingRowState(settings, "fxaa_enabled", false).inactiveReason.find("VHS"), std::string::npos);
    EXPECT_NE(getPostProcessingRowState(settings, "sharpen_enabled", false).inactiveTag, "");

    EXPECT_EQ(getPostProcessingRowState(settings, "halation_threshold", false).inactiveReason, "");
    settings.bloomEnabled = true;
    EXPECT_NE(getPostProcessingRowState(settings, "halation_threshold", false).inactiveReason.find("Bloom"), std::string::npos);

    EXPECT_EQ(getPostProcessingRowState(settings, "dynamic_resolution_enabled", false).inactiveReason, "");
    EXPECT_NE(getPostProcessingRowState(settings, "dynamic_resolution_min_scale", true).inactiveReason.find("Fixed Resolution"),
              std::string::npos);
}

TEST(PostProcessingPresets, CostCountsHalationAndListsS13Effects) {
    PostProcessingSettings settings;
    settings.halationEnabled = true;
    PostProcessingCost cost = estimatePostProcessingCost(settings, {}, false);
    EXPECT_TRUE(hasPass(cost.engine, "halation prefilter"));
    EXPECT_TRUE(hasPass(cost.engine, "halation down"));
    EXPECT_TRUE(cost.chain);
    EXPECT_TRUE(contains(cost.urpCustom, "halation"));
    const int alone = cost.engine.passCount;

    // with bloom: bloom's levels, no pass of its own
    settings.bloomEnabled = true;
    cost = estimatePostProcessingCost(settings, {}, false);
    EXPECT_FALSE(hasPass(cost.engine, "halation prefilter"));
    PostProcessingSettings bloomOnly;
    bloomOnly.bloomEnabled = true;
    EXPECT_EQ(cost.engine.passCount, estimatePostProcessingCost(bloomOnly, {}, false).engine.passCount);
    EXPECT_EQ(alone, estimatePostProcessingCost(PostProcessingSettings(), {}, false).engine.passCount + 3);   // 2 + the uber pass

    PostProcessingSettings stylised;
    stylised.vhsEnabled = true;
    stylised.filmDamageEnabled = true;
    cost = estimatePostProcessingCost(stylised, {}, false);
    EXPECT_TRUE(contains(cost.urpCustom, "VHS"));
    EXPECT_TRUE(contains(cost.urpCustom, "film damage"));
    EXPECT_EQ(cost.engine.passCount, 1);        // the uber pass alone

    PostProcessingSettings graded;
    graded.colorGradingEnabled = true;
    graded.lutTexture = "luts/look.cube";
    EXPECT_TRUE(contains(estimatePostProcessingCost(graded, {}, false).notes, "Color Lookup"));

    PostProcessingSettings adaptive;
    adaptive.dynamicResolutionEnabled = true;
    EXPECT_TRUE(contains(estimatePostProcessingCost(adaptive, {}, false).notes, "min^2 and max^2"));
}

TEST(PostProcessingPresets, DynamicResolutionReadout) {
    std::vector<std::string> warnings;
    DynamicResolutionStatus status;
    EXPECT_EQ(describeDynamicResolution(status, warnings), "");

    status.enabled = true;
    status.running = true;
    status.scale = 0.75f;
    status.source = "gpu";
    status.smoothedMs = 14.2f;
    status.targetMs = 16.7f;
    EXPECT_EQ(describeDynamicResolution(status, warnings), "Dynamic resolution 0.75 (GPU 14.2 / 16.7 ms)");
    EXPECT_TRUE(warnings.empty());

    status.source = "frame";
    status.probeSeconds = 3.2f;
    EXPECT_EQ(describeDynamicResolution(status, warnings), "Dynamic resolution 0.75 (frame law: probing in 4 s)");

    status.saturatedLow = true;
    status.ineffective = true;
    describeDynamicResolution(status, warnings);
    EXPECT_EQ(warnings.size(), 2u);

    warnings.clear();
    status.overridden = true;
    status.scale = 0.5f;
    EXPECT_EQ(describeDynamicResolution(status, warnings), "Dynamic resolution 0.50 (pinned)");
}

TEST(PostProcessingPresets, CubeFilesAreColorLutAssets) {
    const AssetTypeInfo* info = AssetTypes::find("assets/luts/look.cube", "assets");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->type, AssetType::ColorLut);
    EXPECT_EQ(std::string(assetTypeName(AssetType::ColorLut)), "color_lut");
    EXPECT_EQ(assetTypeFromName("color_lut"), AssetType::ColorLut);

    CubeLut lut;
    std::string error;
    ASSERT_TRUE(parseCubeLut("TITLE \"ACEScct look\"\nLUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n", lut, error)) << error;
    const std::string text = describeColorLut(&lut, "");
    EXPECT_NE(text.find("Title: ACEScct look"), std::string::npos);
    EXPECT_NE(text.find("3D table: 2^3"), std::string::npos);
    EXPECT_NE(text.find("Auto: ACEScct"), std::string::npos);
    EXPECT_NE(describeColorLut(nullptr, "line 3: bad").find("line 3: bad"), std::string::npos);
}

TEST(PostProcessingPresets, S43PalettePresetsAndRows) {
    // R22: Palette Quantize with a built-in, named by its alias until applied in a project (R7)
    const PostProcessingPreset* handheld = findPostProcessingPreset("handheld_4");
    const PostProcessingPreset* pico = findPostProcessingPreset("pico_8");
    ASSERT_NE(handheld, nullptr);
    ASSERT_NE(pico, nullptr);
    EXPECT_TRUE(handheld->settings.paletteEnabled);
    EXPECT_EQ(handheld->settings.palette.path, "builtin:game-boy");
    EXPECT_EQ(handheld->settings.paletteDither, PaletteDither::BAYER4);
    EXPECT_TRUE(pico->settings.paletteEnabled);
    EXPECT_EQ(pico->settings.palette.path, "builtin:pico-8");
    EXPECT_EQ(pico->settings.paletteDither, PaletteDither::NONE);
    EXPECT_EQ(getPostProcessingCostTier(handheld->settings), PostProcessingCostTier::UBER);

    // the rows: Dither Strength only with a dither; the anti-banding Dither and an effect
    // without a palette say why they do nothing
    PostProcessingSettings settings;
    settings.paletteEnabled = true;
    EXPECT_NE(getPostProcessingRowState(settings, "palette_enabled", false).inactiveReason, "");
    EXPECT_EQ(getPostProcessingRowState(settings, "dither_enabled", false).inactiveReason, "");
    settings.palette = "Palettes/hero.palette";
    EXPECT_EQ(getPostProcessingRowState(settings, "palette_enabled", false).inactiveReason, "");
    EXPECT_NE(getPostProcessingRowState(settings, "dither_enabled", false).inactiveReason, "");
    EXPECT_TRUE(getPostProcessingRowState(settings, "palette_dither_strength", false).shown);
    settings.paletteDither = PaletteDither::NONE;
    EXPECT_FALSE(getPostProcessingRowState(settings, "palette_dither_strength", false).shown);
}
