// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The renderer's CPU parts (docs/specs/S80-title-art-core.md R16, R27, R43, §6.4, §6.7): the
// adjustments' identity rule and tone LUTs (Photoshop's curves, levels and modern
// brightness/contrast with a fixed pivot), gradient and curve baking, the flood step lists
// bounded by the reach, the blur pyramid plan, and the uv maps between targets.

#include "titleart/render/Adjustments.h"
#include "titleart/render/TitleFields.h"
#include "titleart/render/TitlePlanner.h"
#include "titleart/render/TitleRenderServices.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;

TEST(TitleArtAdjustmentsTest, IdentityRule) {
    EXPECT_TRUE(adjustmentIsIdentity(Json{{"type", "brightness_contrast"}, {"brightness", 0}, {"contrast", 0}}));
    EXPECT_FALSE(adjustmentIsIdentity(Json{{"type", "brightness_contrast"}, {"brightness", 10}, {"contrast", 0}}));
    EXPECT_TRUE(adjustmentIsIdentity(Json{{"type", "hue_saturation"}, {"hue", 360}, {"saturation", 0}, {"lightness", 0}, {"colorize", false}}));
    EXPECT_FALSE(adjustmentIsIdentity(Json{{"type", "hue_saturation"}, {"hue", 0}, {"saturation", 0}, {"lightness", 0}, {"colorize", true}}));
    EXPECT_TRUE(adjustmentIsIdentity(Json{{"type", "effects_only"}}));
    EXPECT_TRUE(adjustmentIsIdentity(Json{{"type", "photo_filter"}, {"density", 0}}));
    EXPECT_FALSE(adjustmentIsIdentity(Json{{"type", "invert"}}));
    EXPECT_FALSE(adjustmentIsIdentity(Json{{"type", "threshold"}, {"level", 128}}));
    // a curve on the diagonal is the identity
    const Json diagonal{{"points", Json::array({{{"x", 0}, {"y", 0}}, {{"x", 0.5}, {"y", 0.5}}, {{"x", 1}, {"y", 1}}})}};
    EXPECT_TRUE(adjustmentIsIdentity(Json{{"type", "curves"}, {"channels", {{"rgb", diagonal}, {"r", diagonal}, {"g", diagonal}, {"b", diagonal}}}}));
}

TEST(TitleArtAdjustmentsTest, ToneLuts) {
    float lut[256 * 4];
    // levels: input black 64 maps 64 to 0 and keeps white
    const Json level{{"in_black", 64}, {"in_white", 255}, {"gamma", 1}, {"out_black", 0}, {"out_white", 255}};
    const Json identity{{"in_black", 0}, {"in_white", 255}, {"gamma", 1}, {"out_black", 0}, {"out_white", 255}};
    bakeToneLut(Json{{"type", "levels"}, {"channels", {{"rgb", level}, {"r", identity}, {"g", identity}, {"b", identity}}}}, lut);
    EXPECT_NEAR(lut[64 * 4], 0.0f, 1e-6f);
    EXPECT_NEAR(lut[255 * 4], 1.0f, 1e-6f);
    EXPECT_NEAR(lut[(64 + 191 / 2) * 4], 95.0f / 191.0f, 1e-3f);
    // brightness/contrast pivots at 50 %: contrast leaves mid grey and both ends
    bakeToneLut(Json{{"type", "brightness_contrast"}, {"brightness", 0}, {"contrast", 60}}, lut);
    EXPECT_NEAR(lut[0], 0.0f, 1e-6f);
    EXPECT_NEAR(lut[255 * 4 + 1], 1.0f, 1e-6f);
    EXPECT_NEAR(lut[128 * 4 + 2], 0.5f, 0.02f);
    EXPECT_LT(lut[64 * 4], 64.0f / 255.0f);
    // curves: each channel's curve, then the composite's
    const Json up{{"points", Json::array({{{"x", 0}, {"y", 0}}, {{"x", 0.5}, {"y", 0.75}}, {{"x", 1}, {"y", 1}}})}};
    const Json line{{"points", Json::array({{{"x", 0}, {"y", 0}}, {{"x", 1}, {"y", 1}}})}};
    bakeToneLut(Json{{"type", "curves"}, {"channels", {{"rgb", line}, {"r", up}, {"g", line}, {"b", line}}}}, lut);
    EXPECT_NEAR(lut[128 * 4 + 0], 0.75f, 0.01f);
    EXPECT_NEAR(lut[128 * 4 + 1], 128.0f / 255.0f, 1e-3f);
}

TEST(TitleLutsTest, GradientMidpointsAndVivid) {
    TitleGradient g;
    TitleGradientStop a, b;
    a.pos = 0;
    parseTitleColor(Json("#000000"), a.color);
    b.pos = 1;
    parseTitleColor(Json("#ffffff"), b.color);
    b.alpha = 0.0;
    g.stops = {a, b};
    uint8_t texels[256 * 4];
    bakeGradient(g, texels);
    EXPECT_EQ(texels[0], 0);
    EXPECT_EQ(texels[255 * 4], 255);
    EXPECT_EQ(texels[3], 255);
    EXPECT_EQ(texels[255 * 4 + 3], 0);
    // sRGB-encoded channels lerped (interp rgb), Pachislop's code
    EXPECT_NEAR(texels[128 * 4], 128, 1);
    // a midpoint at 0.25 puts the half-way colour a quarter of the way
    g.stops[0].mid = 0.25;
    bakeGradient(g, texels);
    EXPECT_NEAR(texels[64 * 4], 128, 2);
    // vivid: blue to white stays blue through the middle (OKLCH), rgb greys it
    parseTitleColor(Json("#0000ff"), g.stops[0].color);
    g.stops[0].mid = 0.5;
    g.stops[1].alpha = 1.0;
    uint8_t rgb[256 * 4];
    bakeGradient(g, rgb);
    g.vivid = true;
    bakeGradient(g, texels);
    const int mid = 128 * 4;
    EXPECT_GT(texels[mid + 2] - texels[mid + 0], rgb[mid + 2] - rgb[mid + 0] - 1);
    EXPECT_NE(gradientKey(g), 0u);
}

TEST(TitleLutsTest, Curve16) {
    TitleCurve c;
    c.points = {{0.0, 0.0, false}, {1.0, 1.0, false}};
    uint8_t texels[256 * 4];
    bakeCurve16(c, texels);
    EXPECT_EQ(texels[0], 0);
    EXPECT_EQ(texels[1], 0);
    EXPECT_EQ(texels[255 * 4], 255);
    EXPECT_EQ(texels[255 * 4 + 1], 255);
    // 16 bits: 128 / 255 is 0x8080
    EXPECT_EQ(texels[128 * 4], 0x80);
    EXPECT_EQ(texels[128 * 4 + 1], 0x80);
}

TEST(TitleFieldsTest, FloodStepsAreBoundedByTheReach) {
    // R43: at most ceil(log2(R + 2)) + 1 passes, reaching 2^k - 1 >= R + 1 px
    for (float reach : {1.0f, 6.0f, 10.0f, 62.0f, 300.0f}) {
        const std::vector<float> steps = floodSteps(reach, 2048, 1024);
        const int k = (int)std::ceil(std::log2(reach + 2.0));
        EXPECT_EQ((int)steps.size(), k + 1) << reach;
        float reached = 0.0f;
        for (size_t i = 0; i + 1 < steps.size(); i++) reached += steps[i];
        EXPECT_GE(reached, reach + 1.0f) << reach;
        EXPECT_EQ(steps.back(), 1.0f);
    }
    // unbounded: the frame's longer side, Pachislop's count
    const std::vector<float> all = floodSteps(0.0f, 1000, 300);
    EXPECT_EQ((int)all.size(), (int)std::ceil(std::log2(1002.0)) + 1);
    EXPECT_EQ(all.front(), 512.0f);
}

TEST(TitleFieldsTest, BlurPlan) {
    EXPECT_TRUE(blurPlan(0.5f, 100, 100).copy);
    const TitleBlurPlan small = blurPlan(12.0f, 100, 100);
    EXPECT_FALSE(small.copy);
    EXPECT_EQ(small.levels, 0);
    EXPECT_FLOAT_EQ(small.sigma, 4.0f);
    // sigma 40: two halvings bring it to 10, three to 5 <= 6
    const TitleBlurPlan big = blurPlan(120.0f, 300, 101);
    EXPECT_EQ(big.levels, 3);
    EXPECT_FLOAT_EQ(big.sigma, 5.0f);
    ASSERT_EQ(big.sizes.size(), 3u);
    EXPECT_EQ(big.sizes[0], std::make_pair(150, 51));
    EXPECT_EQ(big.sizes[2], std::make_pair(38, 13));
}

TEST(TitleFieldsTest, UvMaps) {
    // a target at (100, 50) reading one at (80, 40) of 200 x 100: pixel (0.5, 0.5) samples
    // (20.5, 10.5) of it
    const std::array<float, 4> m = uvMap({100, 50, 64, 64}, {80, 40, 200, 100});
    EXPECT_NEAR(0.5f * m[0] + m[2], 20.5f / 200.0f, 1e-6f);
    EXPECT_NEAR(0.5f * m[1] + m[3], 10.5f / 100.0f, 1e-6f);
}
