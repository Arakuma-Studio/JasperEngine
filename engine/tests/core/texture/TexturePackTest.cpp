// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// TexturePack (docs/specs/S11-materials-lighting.md R5, R14, AC5, AC11): two glTF images into
// one lobe map, channel by channel, at the larger size; and its budget, two 2048x2048 maps in
// at most 40 ms on one job-system worker. CPU only.

#include "texture/TexturePack.h"
#include "jobs/JobSystem.h"

#include "AllocationCounter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <vector>

using namespace jasperengine;

namespace {

    TexturePack::Source source(const std::vector<unsigned char>& pixels, int width, int height, int components){
        TexturePack::Source s;
        s.pixels = pixels.data();
        s.width = width;
        s.height = height;
        s.components = components;
        return s;
    }

    // the packed pixels, freed with the data
    struct Packed{
        TextureData data;
        ~Packed(){ data.releaseImageData(); }
        const unsigned char* pixels() { return static_cast<const unsigned char*>(data.getData()); }
    };

}

TEST(TexturePack, TwoSourcesRG){
    // clear coat: R from the factor map's red, G from the roughness map's green, B and A white
    const std::vector<unsigned char> factor = {10, 11, 12, 13,  20, 21, 22, 23,  30, 31, 32, 33,  40, 41, 42, 43};
    const std::vector<unsigned char> roughness = {100, 101, 102, 103,  110, 111, 112, 113,  120, 121, 122, 123,  130, 131, 132, 133};
    const TexturePack::Source a = source(factor, 2, 2, 4);
    const TexturePack::Source b = source(roughness, 2, 2, 4);
    Packed packed{TexturePack::pack(&a, &b, TexturePack::CLEARCOAT)};
    ASSERT_EQ(packed.data.getWidth(), 2);
    ASSERT_EQ(packed.data.getHeight(), 2);
    ASSERT_EQ(packed.data.getChannels(), 4);
    const std::vector<unsigned char> expected = {10, 101, 255, 255,  20, 111, 255, 255,  30, 121, 255, 255,  40, 131, 255, 255};
    EXPECT_EQ(std::vector<unsigned char>(packed.pixels(), packed.pixels() + 16), expected);

    // specular: RGB from the colour map (the second source), A from the strength map's alpha
    Packed specular{TexturePack::pack(&a, &b, TexturePack::SPECULAR)};
    EXPECT_EQ(std::vector<unsigned char>(specular.pixels(), specular.pixels() + 4), (std::vector<unsigned char>{100, 101, 102, 13}));

    // one source: the other's channels are white (a factor multiplies by 1)
    Packed single{TexturePack::pack(&a, nullptr, TexturePack::CLEARCOAT)};
    EXPECT_EQ(std::vector<unsigned char>(single.pixels(), single.pixels() + 4), (std::vector<unsigned char>{10, 255, 255, 255}));

    // a gray image is its value in R, G, B; its alpha opaque
    const std::vector<unsigned char> gray = {50, 60, 70, 80};
    const TexturePack::Source g = source(gray, 2, 2, 1);
    Packed sheen{TexturePack::pack(&g, &g, TexturePack::SHEEN)};
    EXPECT_EQ(std::vector<unsigned char>(sheen.pixels(), sheen.pixels() + 4), (std::vector<unsigned char>{50, 50, 50, 255}));

    // the id names both sources and the layout
    EXPECT_NE(TexturePack::makeId("a", "b", TexturePack::CLEARCOAT), TexturePack::makeId("b", "a", TexturePack::CLEARCOAT));
    EXPECT_NE(TexturePack::makeId("a", "b", TexturePack::CLEARCOAT), TexturePack::makeId("a", "b", TexturePack::SHEEN));
}

TEST(TexturePack, MixedSizes){
    // a 2x2 factor map and a 4x4 roughness map: the result is 4x4, the small one bilinear
    const std::vector<unsigned char> small = {0, 0, 0, 255,  200, 0, 0, 255,  0, 0, 0, 255,  200, 0, 0, 255};
    std::vector<unsigned char> large(4 * 4 * 4);
    for (int t = 0; t < 16; t++){
        large[t * 4 + 0] = 0;
        large[t * 4 + 1] = (unsigned char)(t * 10);
        large[t * 4 + 2] = 0;
        large[t * 4 + 3] = 255;
    }
    const TexturePack::Source a = source(small, 2, 2, 4);
    const TexturePack::Source b = source(large, 4, 4, 4);
    Packed packed{TexturePack::pack(&a, &b, TexturePack::IRIDESCENCE)};
    ASSERT_EQ(packed.data.getWidth(), 4);
    ASSERT_EQ(packed.data.getHeight(), 4);
    const unsigned char* p = packed.pixels();
    // G is the large map as it is
    for (int t = 0; t < 16; t++)
        EXPECT_EQ(p[t * 4 + 1], (unsigned char)(t * 10)) << "texel " << t;
    // R: the small map's 0 | 200 columns, resampled: clamped at the edges, blended between
    EXPECT_EQ(p[0 * 4 + 0], 0);
    EXPECT_EQ(p[3 * 4 + 0], 200);
    EXPECT_GT(p[1 * 4 + 0], 0);
    EXPECT_LT(p[1 * 4 + 0], 200);
    EXPECT_EQ(p[1 * 4 + 0] + p[2 * 4 + 0], 200);
}

TEST(TexturePackBench, TwoMaps2048OnOneWorker){
    const int size = 2048;
    std::vector<unsigned char> first((size_t)size * size * 4), second((size_t)size * size * 4);
    for (size_t i = 0; i < first.size(); i++){
        first[i] = (unsigned char)(i * 7);
        second[i] = (unsigned char)(i * 13);
    }
    const TexturePack::Source a = source(first, size, size, 4);
    const TexturePack::Source b = source(second, size, size, 4);

    // best of three, each on a Background worker as the engine runs it
    double best = 1e9;
    for (int run = 0; run < 3; run++){
        auto task = jobs::submit([&]() -> double {
            const auto start = std::chrono::steady_clock::now();
            TextureData packed = TexturePack::pack(&a, &b, TexturePack::CLEARCOAT);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            packed.releaseImageData();
            return ms;
        }, {jobs::Lane::Background, jobs::GateClass::Heavy, "texture-pack-bench"});
        best = std::min(best, task.get());
    }
    std::printf("[bench] TexturePack 2 x %dx%d: %.2f ms (budget 40 ms)\n", size, size, best);
    const double slack = testsupport::perfSlack();
    if (slack > 0.0) {
        EXPECT_LE(best, 40.0 * slack);
    }
}
