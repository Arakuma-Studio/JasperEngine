// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The one mip recipe (spec S14 AC2, R4): sRGB colour filtered in linear light, normals
// renormalized on every level, alpha-test coverage kept, alpha borders bled, and the result
// deterministic. The raw-path / cook-path identity is TextureCookerTest.UncompressedCookMatchesRawLoad
// (editor tests), which needs the cook.

#include "texture/MipChain.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

using namespace jasperengine;

namespace {

    MipLevel checker(int size, uint8_t a, uint8_t b){
        MipLevel level;
        level.width = level.height = size;
        level.pixels.resize((size_t)size * size * 4);
        for (int y = 0; y < size; y++){
            for (int x = 0; x < size; x++){
                const uint8_t v = ((x + y) & 1) ? a : b;
                uint8_t* p = &level.pixels[((size_t)y * size + x) * 4];
                p[0] = p[1] = p[2] = v;
                p[3] = 255;
            }
        }
        return level;
    }

    // a smooth value-noise field in [0, 1]
    float valueNoise(float x, float y, uint32_t seed){
        auto hash = [seed](int ix, int iy){
            uint32_t h = (uint32_t)ix * 374761393u + (uint32_t)iy * 668265263u + seed * 2246822519u;
            h = (h ^ (h >> 13)) * 1274126177u;
            return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
        };
        const int ix = (int)std::floor(x), iy = (int)std::floor(y);
        const float fx = x - (float)ix, fy = y - (float)iy;
        const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
        const float a = hash(ix, iy), b = hash(ix + 1, iy), c = hash(ix, iy + 1), d = hash(ix + 1, iy + 1);
        return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
    }

    // foliage-like cut-out: small leaves (a few texels across) with hard edges, about a third
    // covered; filtering thins them out on every level, which is what coverage preservation fixes
    MipLevel foliage(int size){
        MipLevel level;
        level.width = level.height = size;
        level.pixels.resize((size_t)size * size * 4);
        for (int y = 0; y < size; y++){
            for (int x = 0; x < size; x++){
                float n = 0.0f, amplitude = 0.5f, frequency = 48.0f / (float)size;
                for (int octave = 0; octave < 3; octave++){
                    n += amplitude * valueNoise((float)x * frequency, (float)y * frequency, 17u + (uint32_t)octave);
                    amplitude *= 0.5f;
                    frequency *= 2.0f;
                }
                const float alpha = std::clamp((n - 0.5f) * 40.0f + 0.5f, 0.0f, 1.0f);
                uint8_t* p = &level.pixels[((size_t)y * size + x) * 4];
                p[0] = (uint8_t)(60 + 80 * n);
                p[1] = (uint8_t)(110 + 100 * n);
                p[2] = 40;
                p[3] = (uint8_t)std::lround(alpha * 255.0f);
            }
        }
        return level;
    }

    float worstCoverageError(const std::vector<MipLevel>& levels, float cutoff, int minTexels){
        const float base = MipChain::alphaCoverage(levels[0], cutoff);
        float worst = 0.0f;
        for (size_t i = 1; i < levels.size(); i++){
            if (levels[i].width * levels[i].height < minTexels)
                continue;
            worst = std::max(worst, std::fabs(MipChain::alphaCoverage(levels[i], cutoff) - base));
        }
        return worst;
    }

}

TEST(MipChain, SrgbCheckerFiltersInLinearLight){
    std::vector<MipLevel> levels = {checker(64, 0, 255)};
    MipChainOptions options;
    options.srgb = true;
    options.alphaWeighted = true;
    MipChain::build(levels, options);
    ASSERT_EQ(levels.size(), 7u);
    // half the light of white is linear 0.5: sRGB 188, never the gamma-space average 128
    for (size_t level = 1; level < levels.size(); level++){
        for (size_t i = 0; i < levels[level].pixels.size(); i += 4){
            EXPECT_NEAR(levels[level].pixels[i], 188, 1) << "level " << level;
            EXPECT_EQ(levels[level].pixels[i + 3], 255);
        }
    }
}

TEST(MipChain, LinearDataFiltersPerChannel){
    std::vector<MipLevel> levels = {checker(16, 0, 255)};
    MipChainOptions options;     // linear, 4 channels
    MipChain::build(levels, options);
    for (size_t i = 0; i < levels[1].pixels.size(); i += 4)
        EXPECT_NEAR(levels[1].pixels[i], 128, 1);
}

TEST(MipChain, AlphaWeightingKeepsTransparentColourOut){
    // opaque red next to transparent green: the filtered colour stays red
    MipLevel base;
    base.width = base.height = 8;
    base.pixels.resize(8 * 8 * 4);
    for (int i = 0; i < 64; i++){
        uint8_t* p = &base.pixels[(size_t)i * 4];
        const bool red = (i % 8) < 4;
        p[0] = red ? 255 : 0;
        p[1] = red ? 0 : 255;
        p[2] = 0;
        p[3] = red ? 255 : 0;
    }
    std::vector<MipLevel> levels = {base};
    MipChainOptions options;
    options.srgb = true;
    options.alphaWeighted = true;
    options.wrap = false;
    MipChain::build(levels, options);
    // a texel straddling the edge: partly opaque, and red, not brown
    const uint8_t* edge = &levels[1].pixels[(1 * 4 + 2) * 4];
    EXPECT_GT(edge[3], 0);
    EXPECT_GT(edge[0], 200);
    EXPECT_LT(edge[1], 30);
}

TEST(MipChain, NormalLevelsAreUnitLength){
    const int size = 128;
    MipLevel base;
    base.width = base.height = size;
    base.pixels.resize((size_t)size * size * 3 * sizeof(float));
    float* v = reinterpret_cast<float*>(base.pixels.data());
    std::mt19937 random(5);
    std::uniform_real_distribution<float> slope(-0.8f, 0.8f);
    for (int i = 0; i < size * size; i++){
        // bumpy, and deliberately not unit length (as decoded 8-bit normals are not)
        v[i * 3 + 0] = slope(random);
        v[i * 3 + 1] = slope(random);
        v[i * 3 + 2] = 0.7f;
    }
    std::vector<MipLevel> levels = {base};
    MipChainOptions options;
    options.channels = 3;
    options.type = MipPixelType::F32;
    options.normalMap = true;
    MipChain::build(levels, options);
    ASSERT_EQ(levels.size(), 8u);
    for (size_t level = 0; level < levels.size(); level++){
        const float* n = reinterpret_cast<const float*>(levels[level].pixels.data());
        for (size_t i = 0; i < (size_t)levels[level].width * levels[level].height; i++){
            const float length = std::sqrt(n[i * 3] * n[i * 3] + n[i * 3 + 1] * n[i * 3 + 1] + n[i * 3 + 2] * n[i * 3 + 2]);
            EXPECT_NEAR(length, 1.0f, 2e-3f) << "level " << level << " texel " << i;
        }
    }
}

TEST(MipChain, PreserveCoverageKeepsTheAlphaTest){
    const int size = 512;
    // AC2: within 2 % on every level of at least 8x8 texels (a smaller level cannot resolve 2 %:
    // one texel of a 4x4 level is 6 %, so those must be within one texel)
    for (bool preserve : {false, true}){
        std::vector<MipLevel> levels = {foliage(size)};
        MipChainOptions options;
        options.srgb = true;
        options.alphaWeighted = true;
        options.preserveCoverage = preserve;
        options.alphaCutoff = 0.5f;
        MipChain::build(levels, options);
        const float error = worstCoverageError(levels, 0.5f, 64);
        if (preserve){
            EXPECT_LE(error, 0.02f);
            for (const MipLevel& level : levels){
                const int texels = level.width * level.height;
                if (texels < 64){
                    EXPECT_LE(std::fabs(MipChain::alphaCoverage(level, 0.5f) - MipChain::alphaCoverage(levels[0], 0.5f)),
                              1.0f / (float)texels + 1e-6f) << level.width << "x" << level.height;
                }
            }
        }else{
            // the test is sensitive: plain filtering loses more than 10 % on some level
            EXPECT_GT(error, 0.10f);
        }
    }
}

TEST(MipChain, CoverageScalingLeavesZeroAlphaAtZero){
    std::vector<MipLevel> levels = {foliage(128)};
    MipChainOptions options;
    options.srgb = true;
    options.alphaWeighted = true;
    options.preserveCoverage = true;
    MipChain::build(levels, options);
    // a level filtered from fully transparent texels only stays fully transparent
    MipLevel clear;
    clear.width = clear.height = 16;
    clear.pixels.assign(16 * 16 * 4, 0);
    std::vector<MipLevel> empty = {clear};
    MipChain::build(empty, options);
    for (const MipLevel& level : empty)
        for (size_t i = 3; i < level.pixels.size(); i += 4)
            EXPECT_EQ(level.pixels[i], 0);
}

TEST(MipChain, BleedFillsTransparentNeighboursOnEveryLevel){
    MipLevel base;
    base.width = base.height = 32;
    base.pixels.assign(32 * 32 * 4, 0);
    // one opaque blue square in the middle, transparent black around it
    for (int y = 12; y < 20; y++){
        for (int x = 12; x < 20; x++){
            uint8_t* p = &base.pixels[((size_t)y * 32 + x) * 4];
            p[2] = 255;
            p[3] = 255;
        }
    }
    std::vector<MipLevel> levels = {base};
    MipChainOptions options;
    options.srgb = true;
    options.alphaWeighted = true;
    options.bleedAlpha = true;
    MipChain::build(levels, options);
    for (const MipLevel& level : levels){
        if (level.width < 4)
            continue;
        // every transparent texel next to an opaque one took its blue
        for (int y = 0; y < level.height; y++){
            for (int x = 0; x < level.width; x++){
                const uint8_t* p = &level.pixels[((size_t)y * level.width + x) * 4];
                if (p[3] != 0)
                    continue;
                bool nearOpaque = false;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++){
                        const int nx = x + dx, ny = y + dy;
                        if (nx >= 0 && ny >= 0 && nx < level.width && ny < level.height &&
                            level.pixels[((size_t)ny * level.width + nx) * 4 + 3] > 0)
                            nearOpaque = true;
                    }
                if (nearOpaque){
                    EXPECT_GT(p[2], 0) << level.width << "x" << level.height << " at " << x << "," << y;
                }
            }
        }
    }
}

TEST(MipChain, LevelCountsAndLimits){
    MipChainOptions options;
    EXPECT_EQ(MipChain::levelCount(1000, 600, options), 10);
    EXPECT_EQ(MipChain::levelCount(1, 1, options), 1);
    options.maxLevels = 3;
    EXPECT_EQ(MipChain::levelCount(1024, 1024, options), 3);
    std::vector<MipLevel> levels = {checker(1000, 10, 20)};
    options.maxLevels = 0;
    MipChain::build(levels, options);
    ASSERT_EQ(levels.size(), 10u);
    EXPECT_EQ(levels[1].width, 500);
    EXPECT_EQ(levels[9].width, 1);
}

TEST(MipChain, BuildsTheSameBytesTwice){
    std::vector<MipLevel> a = {foliage(256)};
    std::vector<MipLevel> b = {foliage(256)};
    MipChainOptions options;
    options.srgb = true;
    options.alphaWeighted = true;
    options.preserveCoverage = true;
    options.bleedAlpha = true;
    MipChain::build(a, options);
    MipChain::build(b, options);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); i++)
        EXPECT_EQ(a[i].pixels, b[i].pixels);
}
