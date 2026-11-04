// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T2 R36, N13, AC27: noise::compat returns what the removed TerrainNoise.h returned, for every input
// of the table captured from it before its removal, and the library's Value noise extends it.

#include "noise/NoiseCompat.h"
#include "noise/NoiseKernels.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::noise;

namespace {

    struct CompatHashCase { int x, y; uint32_t seed, bits; };
    struct CompatValueCase { uint32_t x, y, seed, bits; };
    struct CompatFractalCase { uint32_t x, y, seed; int octaves; uint32_t bits; };

#include "TerrainNoiseCompatTable.inc"

    float fromBits(uint32_t bits) { float f; std::memcpy(&f, &bits, sizeof(f)); return f; }
    uint32_t toBits(float f) { uint32_t b; std::memcpy(&b, &f, sizeof(b)); return b; }

}

TEST(TerrainNoiseCompat, MatchesTheOldHeaderTable) {
    for (const CompatHashCase& c : kCompatHash) {
        ASSERT_EQ(toBits(compat::hash01(c.x, c.y, c.seed)), c.bits) << c.x << " " << c.y << " " << c.seed;
    }
    for (const CompatValueCase& c : kCompatValue) {
        ASSERT_EQ(toBits(compat::value(fromBits(c.x), fromBits(c.y), c.seed)), c.bits) << fromBits(c.x) << " " << fromBits(c.y) << " " << c.seed;
    }
    for (const CompatFractalCase& c : kCompatFractal) {
        ASSERT_EQ(toBits(compat::fractal(fromBits(c.x), fromBits(c.y), c.seed, c.octaves)), c.bits) << fromBits(c.x) << " " << fromBits(c.y) << " " << c.octaves;
    }
}

// N13: on [0, 65535) the Value node is compat::value * 2 - 1. The last cell differs on purpose: the
// node's lattice wraps there, and the old one had no wrap.
TEST(TerrainNoiseCompat, ValueNodeExtendsTheOldValue) {
    for (uint32_t seed : {0u, 7u, 0xFFFFFFFFu}) {
        for (float x : {0.0f, 0.25f, 3.75f, 17.125f, 255.5f, 40000.7f, 65534.5f}) {
            for (float z : {0.0f, 0.75f, 9.125f, 511.25f, 65000.25f}) {
                const float expected = compat::value(x, z, seed) * 2.0f - 1.0f;
                EXPECT_EQ(toBits(value2(x, z, seed)), toBits(expected)) << x << " " << z << " " << seed;
            }
        }
    }
}

TEST(TerrainNoiseCompat, FbmOfValueIsTheOldFractal) {
    FractalParams params;
    params.basis.basis = Basis::Value;
    params.basis.frequency = 1.0f;
    params.lacunarity = 2.0f;
    params.gain = 0.5f;
    for (int octaves : {1, 2, 4, 6}) {
        params.octaves = octaves;
        for (uint32_t seed : {1u, 7u}) {
            params.basis.seed = seed;
            for (float x : {0.25f, 3.75f, 17.125f, 255.5f}) {
                for (float z : {0.75f, 9.125f, 200.5f}) {
                    EXPECT_EQ(toBits(fbmPoint(params, x, 0.0f, z)), toBits(compat::fractal(x, z, seed, octaves))) << x << " " << z << " " << octaves;
                }
            }
        }
    }
}
