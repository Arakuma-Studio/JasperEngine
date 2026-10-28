// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// engine/core/util/WaterWaves.h, the CPU twin of library/water.glsl's waterWaves (docs/specs/
// S22-gameplay.md R28, AC10): it gives the heights of a float32 evaluation of the GLSL function
// (WaterWavesReference.inl, made by tools/gen_water_waves_reference.py), and the GLSL function is
// still the one it was translated from.

#include "util/WaterWaves.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;

#ifndef WATER_WAVES_GLSL_SHA256
#error "engine/tests/core/CMakeLists.txt defines WATER_WAVES_GLSL_SHA256 from library/water.glsl"
#endif

namespace {

    struct Reference {
        float x, z, time, directionX, directionY, height, waveLength, speed, expected;
    };

    const Reference REFERENCE[] = {
#include "WaterWavesReference.inl"
    };

}

TEST(WaterWavesTest, MatchesReferenceTable){
    ASSERT_EQ(std::size(REFERENCE), 64u);
    for (const Reference& r : REFERENCE){
        const WaterWaveSample sample = waterWaves(Vector2(r.x, r.z), r.time, Vector2(r.directionX, r.directionY), r.height, r.waveLength, r.speed);
        EXPECT_NEAR(sample.height, r.expected, 1e-5f) << "at (" << r.x << ", " << r.z << ") t " << r.time;
    }
}

TEST(WaterWavesTest, GlslTwinPinned){
    // the SHA-256 of waterWaves in engine/shaderlib/library/water.glsl (the text from its signature
    // to its closing brace) when WaterWaves.h was last made to match it
    const char* pinned = "45307ae091cb8fc4e9d19dd7690008a0db779d28f9b33195c49cc7a3420da6ee";
    EXPECT_STREQ(WATER_WAVES_GLSL_SHA256, pinned)
        << "library/water.glsl's waterWaves changed: change engine/core/util/WaterWaves.h to compute the same "
           "(line for line), regenerate engine/tests/core/util/WaterWavesReference.inl with "
           "tools/gen_water_waves_reference.py, then pin the new hash here";
}

TEST(WaterWavesTest, SlopeAndRateAreTheDerivatives){
    const Vector2 direction(1.0f, 0.3f);
    const float height = 0.5f, length = 4.0f, speed = 1.5f;
    const float x = 1.3f, z = -0.7f, t = 2.25f, h = 1e-3f;
    const WaterWaveSample sample = waterWaves(Vector2(x, z), t, direction, height, length, speed);
    const float dx = (waterWaves(Vector2(x + h, z), t, direction, height, length, speed).height -
                      waterWaves(Vector2(x - h, z), t, direction, height, length, speed).height) / (2.0f * h);
    const float dz = (waterWaves(Vector2(x, z + h), t, direction, height, length, speed).height -
                      waterWaves(Vector2(x, z - h), t, direction, height, length, speed).height) / (2.0f * h);
    const float dt = (waterWaves(Vector2(x, z), t + h, direction, height, length, speed).height -
                      waterWaves(Vector2(x, z), t - h, direction, height, length, speed).height) / (2.0f * h);
    EXPECT_NEAR(sample.slope.x, dx, 2e-3f);
    EXPECT_NEAR(sample.slope.y, dz, 2e-3f);
    EXPECT_NEAR(sample.heightRate, dt, 2e-3f);
}

TEST(WaterWavesTest, FlatWithoutHeight){
    const WaterWaveSample sample = waterWaves(Vector2(3.0f, 4.0f), 10.0f, Vector2(0.0f, 0.0f), 0.0f, 0.0f, 3.0f);
    EXPECT_EQ(sample.height, 0.0f);
    EXPECT_EQ(sample.heightRate, 0.0f);
    EXPECT_TRUE(std::isfinite(sample.slope.x) && std::isfinite(sample.slope.y));
}
