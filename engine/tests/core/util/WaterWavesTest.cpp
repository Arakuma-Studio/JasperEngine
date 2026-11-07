// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// engine/core/util/WaterWaves.h, the CPU twin of library/water.glsl's waterWaves (docs/specs/
// S22-gameplay.md R28, AC10): it gives the heights of a float32 evaluation of the GLSL function
// (WaterWavesReference.inl, made by tools/gen_water_waves_reference.py), and the GLSL function is
// still the one it was translated from.

#include "util/WaterWaves.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

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

#ifndef WATER_GERSTNER_GLSL_SHA256
#error "engine/tests/core/CMakeLists.txt defines WATER_GERSTNER_GLSL_SHA256 from library/water.glsl"
#endif

namespace {

    struct GerstnerReference {
        float x, z, time, directionX, directionY, height, waveLength, speed, steepness, offsetX, offsetY, offsetZ;
    };

    const GerstnerReference GERSTNER_REFERENCE[] = {
#include "WaterGerstnerReference.inl"
    };

    WaterWaveParams gerstnerParams(float height, float length, float speed, float steepness){
        WaterWaveParams params;
        params.waves = true;
        params.gerstner = true;
        params.direction = Vector2(1.0f, 0.3f);
        params.height = height;
        params.length = length;
        params.speed = speed;
        params.steepness = steepness;
        return params;
    }

}

TEST(WaterWavesTest, GerstnerMatchesReferenceTable){
    ASSERT_EQ(std::size(GERSTNER_REFERENCE), 64u);
    for (const GerstnerReference& r : GERSTNER_REFERENCE){
        const WaterGerstnerSample sample = waterGerstner(Vector2(r.x, r.z), r.time, Vector2(r.directionX, r.directionY), r.height,
                                                         r.waveLength, r.speed, r.steepness);
        const float tolerance = 2e-5f * std::max(1.0f, std::max(r.height, r.waveLength));
        EXPECT_NEAR(sample.offset.x, r.offsetX, tolerance) << "at (" << r.x << ", " << r.z << ") t " << r.time;
        EXPECT_NEAR(sample.offset.y, r.offsetY, tolerance) << "at (" << r.x << ", " << r.z << ") t " << r.time;
        EXPECT_NEAR(sample.offset.z, r.offsetZ, tolerance) << "at (" << r.x << ", " << r.z << ") t " << r.time;
    }
}

TEST(WaterWavesTest, GerstnerGlslTwinPinned){
    // the SHA-256 of waterGerstnerWave and waterGerstner in engine/shaderlib/library/water.glsl (from
    // the first's signature to the second's closing brace) when WaterWaves.h was last made to match
    const char* pinned = "e8ae841eaea0cc063e73590e066c11781fd7ab70b26fe1443b1c02f913677e08";
    EXPECT_STREQ(WATER_GERSTNER_GLSL_SHA256, pinned)
        << "library/water.glsl's waterGerstner changed: change engine/core/util/WaterWaves.h to compute the same "
           "(line for line), regenerate engine/tests/core/util/WaterGerstnerReference.inl with "
           "tools/gen_water_waves_reference.py --gerstner, then pin the new hash here";
}

TEST(WaterWavesTest, GerstnerTangentsAndVelocityAreTheDerivatives){
    const Vector2 direction(1.0f, 0.3f);
    const float height = 0.4f, length = 5.0f, speed = 1.2f, steepness = 0.6f;
    const float x = 1.3f, z = -0.7f, t = 2.25f, h = 1e-3f;
    auto at = [&](float px, float pz, float pt){
        return waterGerstner(Vector2(px, pz), pt, direction, height, length, speed, steepness);
    };
    const WaterGerstnerSample sample = at(x, z, t);
    const Vector3 dx = ((Vector3(x + h, 0.0f, z) + at(x + h, z, t).offset) - (Vector3(x - h, 0.0f, z) + at(x - h, z, t).offset)) / (2.0f * h);
    const Vector3 dz = ((Vector3(x, 0.0f, z + h) + at(x, z + h, t).offset) - (Vector3(x, 0.0f, z - h) + at(x, z - h, t).offset)) / (2.0f * h);
    const Vector3 dt = (at(x, z, t + h).offset - at(x, z, t - h).offset) / (2.0f * h);
    for (int i = 0; i < 3; i++){
        EXPECT_NEAR(sample.dPdx[i], dx[i], 3e-3f) << "dPdx " << i;
        EXPECT_NEAR(sample.dPdz[i], dz[i], 3e-3f) << "dPdz " << i;
        EXPECT_NEAR(sample.velocity[i], dt[i], 3e-3f) << "velocity " << i;
    }
}

TEST(WaterWavesTest, GerstnerInversion){
    // the rest point waterSurfaceAt finds is moved by the waves onto the asked xz, at any steepness
    // the surface does not fold at
    uint32_t seed = 12345u;
    auto random = [&seed](float low, float high){
        seed = seed * 1664525u + 1013904223u;
        return low + (high - low) * (float)(seed >> 8) / 16777216.0f;
    };
    for (float steepness : {0.0f, 0.3f, 0.6f, 0.9f}){
        const WaterWaveParams params = gerstnerParams(0.5f, 6.0f, 1.5f, steepness);
        for (int i = 0; i < 30; i++){
            const Vector2 xz(random(-50.0f, 50.0f), random(-50.0f, 50.0f));
            const float t = random(0.0f, 100.0f);
            const WaterWaveSample surface = waterSurfaceAt(params, xz, t);
            // the height found is the waves' at a rest point moved onto xz: search that rest point
            // by brute force along the sideways reach and compare
            float best = 1e9f, bestHeight = 0.0f;
            const float reach = params.getHorizontalReach() + 0.01f;
            for (int a = 0; a <= 40; a++){
                for (int b = 0; b <= 40; b++){
                    const Vector2 rest(xz.x - reach + 2.0f * reach * a / 40.0f, xz.y - reach + 2.0f * reach * b / 40.0f);
                    const WaterGerstnerSample point = waterGerstner(rest, t, params.direction, params.height, params.length, params.speed, params.steepness);
                    const float ex = rest.x + point.offset.x - xz.x, ez = rest.y + point.offset.z - xz.y;
                    if (ex * ex + ez * ez < best){
                        best = ex * ex + ez * ez;
                        bestHeight = point.offset.y;
                    }
                }
            }
            EXPECT_NEAR(surface.height, bestHeight, 0.02f) << "steepness " << steepness << " at (" << xz.x << ", " << xz.y << ")";
            EXPECT_GT(surface.normal.y, 0.0f);
        }
    }
}

TEST(WaterWavesTest, SurfaceAtFollowsTheSines){
    WaterWaveParams params = gerstnerParams(0.5f, 4.0f, 1.5f, 0.0f);
    params.gerstner = false;
    const WaterWaveSample sines = waterWaves(Vector2(2.0f, 3.0f), 4.0f, params.direction, params.height, params.length, params.speed);
    const WaterWaveSample surface = waterSurfaceAt(params, Vector2(2.0f, 3.0f), 4.0f);
    EXPECT_EQ(surface.height, sines.height);
    EXPECT_EQ(surface.velocity.y, sines.heightRate);
    EXPECT_NEAR(surface.normal.y, Vector3(-sines.slope.x, 1.0f, -sines.slope.y).normalized().y, 1e-6f);
    params.waves = false;
    EXPECT_EQ(waterSurfaceAt(params, Vector2(2.0f, 3.0f), 4.0f).height, 0.0f);
    EXPECT_EQ(params.getBand(), 0.0f);
}
