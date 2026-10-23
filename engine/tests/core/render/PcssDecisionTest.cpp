// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The PCSS decisions of includes/shadow_pcss_decisions.glsl (spec S10 AC8), ported from
// OpenEngine's ShadowPcssDecisionTests with the depth inverted (nearer the light is smaller
// here): the shader's own functions, compiled as C++ through the vec2 subset below.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

namespace pcss {

    struct vec2 {
        float x, y;
        vec2(float ax, float ay) : x(ax), y(ay) {}
    };
    inline float dot(vec2 a, vec2 b){ return a.x * b.x + a.y * b.y; }
    inline float max(float a, float b){ return a > b ? a : b; }
    inline float clamp(float v, float lo, float hi){ return v < lo ? lo : (v > hi ? hi : v); }
    inline int clamp(int v, int lo, int hi){ return v < lo ? lo : (v > hi ? hi : v); }

    #include "../../../shaderlib/includes/shadow_pcss_decisions.glsl"

}

using namespace pcss;

TEST(PcssDecision, SlopedReceiverIsNotItsOwnBlocker){
    // a receiver sloping away from the light along u: its own surface a tap away is deeper by
    // the gradient, and that is no blocker
    EXPECT_FALSE(shadowPcssIsBlocker(0.502f, 0.5f, vec2(0.01f, 0.0f), vec2(0.2f, 0.0f))) << "receiver surface down-slope";
    EXPECT_TRUE(shadowPcssIsBlocker(0.49f, 0.5f, vec2(0.01f, 0.0f), vec2(0.2f, 0.0f))) << "caster in front of the receiver plane";
    // with the bias off the reference is flat
    EXPECT_FALSE(shadowPcssIsBlocker(0.502f, 0.5f, vec2(0.01f, 0.0f), vec2(0.0f, 0.0f)));
    EXPECT_TRUE(shadowPcssIsBlocker(0.498f, 0.5f, vec2(0.01f, 0.0f), vec2(0.0f, 0.0f)));
    // the plane at the tap, not at the centre: a tap up-slope compares against a nearer depth
    EXPECT_FALSE(shadowPcssIsBlocker(0.497f, 0.5f, vec2(-0.02f, 0.0f), vec2(0.2f, 0.0f)));
}

TEST(PcssDecision, PenumbraClampsToOneTexelAndTheCap){
    const float texelWorld = 0.05f;
    // contact: no depth between receiver and blocker gives the minimum kernel
    EXPECT_FLOAT_EQ(shadowPcssPenumbraTexels(shadowPcssDirectionalPenumbraWorld(0.5f, 0.5f, 100.0f, 0.0046f), texelWorld), 1.0f);
    // a blocker behind the receiver is no depth either
    EXPECT_FLOAT_EQ(shadowPcssDirectionalPenumbraWorld(0.6f, 0.5f, 100.0f, 0.0046f), 0.0f);
    // physical: 0.1 of a 100 m span at tan 0.01 is 0.1 m, two texels
    EXPECT_NEAR(shadowPcssPenumbraTexels(shadowPcssDirectionalPenumbraWorld(0.4f, 0.5f, 100.0f, 0.01f), texelWorld), 2.0f, 1e-4f);
    // huge: the cap
    EXPECT_FLOAT_EQ(shadowPcssPenumbraTexels(shadowPcssDirectionalPenumbraWorld(0.0f, 1.0f, 1000.0f, 0.1f), texelWorld), SHADOW_PCSS_MAX_TEXELS);
    // spot: similar triangles of the light's radius
    EXPECT_NEAR(shadowPcssSpotPenumbraWorld(10.0f, 5.0f, 0.1f), 0.1f, 1e-6f);
    EXPECT_FLOAT_EQ(shadowPcssSpotPenumbraWorld(5.0f, 5.0f, 0.1f), 0.0f);
    // the search: at least a texel, at most the cap
    EXPECT_FLOAT_EQ(shadowPcssSearchTexels(0.2f), 1.0f);
    EXPECT_FLOAT_EQ(shadowPcssSearchTexels(7.0f), 7.0f);
    EXPECT_FLOAT_EQ(shadowPcssSearchTexels(400.0f), SHADOW_PCSS_MAX_TEXELS);
}

TEST(PcssDecision, TapsGrowWithTheKernelWithinBounds){
    EXPECT_EQ(shadowPcssTapsForRadius(1.0f), SHADOW_PCSS_MIN_TAPS);
    EXPECT_EQ(shadowPcssTapsForRadius(16.0f), 16);
    EXPECT_EQ(shadowPcssTapsForRadius(20.0f), 20);
    EXPECT_EQ(shadowPcssTapsForRadius(24.0f), 24);
    EXPECT_EQ(shadowPcssTapsForRadius(100.0f), SHADOW_PCSS_MAX_TAPS);
    int previous = 0;
    for (float r = 1.0f; r <= 24.0f; r += 0.25f){
        const int taps = shadowPcssTapsForRadius(r);
        EXPECT_GE(taps, previous);
        EXPECT_GE(taps, SHADOW_PCSS_MIN_TAPS);
        EXPECT_LE(taps, SHADOW_PCSS_MAX_TAPS);
        previous = taps;
    }
}
