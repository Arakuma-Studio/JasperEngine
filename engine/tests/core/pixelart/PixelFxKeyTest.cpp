// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/PixelFxKey.h"

#include <gtest/gtest.h>

#include <cstdio>

using namespace jasperengine::pixelart;

// Pinned against an independent 32-bit PCG2D: the shader (includes/pixel_fx.glsl) uses the same
// constants, so a change here changes where every dissolve burns.
TEST(PixelFxKey, HashIsPinnedPcg2d){
    EXPECT_EQ(pfxHash(0, 0, 0), 417608103u);
    EXPECT_EQ(pfxHash(3, 5, 7), 228126326u);
    EXPECT_EQ(pfxHash(31, 17, 1234), 2269020740u);
    EXPECT_EQ(pfxHash01(3, 5, 7), (float)(228126326u >> 8) / 16777216.0f);
}

TEST(PixelFxKey, ValueNoiseHitsTheHashesAtWholeCells){
    for (uint32_t seed : {1u, 2u, 3u, 7u}){
        for (uint32_t c = 0; c < 6; c++){
            EXPECT_EQ(pfxValueNoise((float)(c * 4), 8.0f, 4.0f, seed), pfxHash01(c, 2, seed));
        }
        for (int i = 0; i < 64; i++){
            const float v = pfxValueNoise(i * 0.73f, i * 0.31f, 4.0f, seed);
            EXPECT_GE(v, 0.0f);
            EXPECT_LT(v, 1.0f);
        }
    }
}

namespace {

    // the edge band's height in a column: from the front (the lowest texel not burnt above a
    // burnt one) up, the texels whose distance is below the edge width; -1 for a column the front
    // is not in
    int edgeBandInColumn(int x, int width, int height, const PfxDissolve& dissolve, float progress){
        int y = 0;
        while (y < height && pfxDissolveTexel(x, y, width, height, dissolve, progress) < 0.0f)
            y++;
        if (y == 0 || y >= height)
            return -1;
        int band = 0;
        while (y < height){
            const float d = pfxDissolveTexel(x, y, width, height, dissolve, progress);
            if (d < 0.0f || d >= dissolve.edgeWidth)
                break;
            band++;
            y++;
        }
        return band;
    }

}

// AC6: the dissolve's edge band is dissolveEdgeWidth texels along the front (noise 0.35): on at
// least 95 % of the front's columns, over frame sizes, seeds, cells and progresses
TEST(PixelFxKey, BandWidth){
    int columns = 0, exact = 0;
    for (int size : {32, 64}){
        for (uint32_t seed : {1u, 2u, 3u, 7u}){
            for (float cell : {4.0f, 8.0f, 16.0f}){
                for (float edge : {1.0f, 2.0f, 3.0f}){
                    for (float progress : {0.3f, 0.5f, 0.7f}){
                        PfxDissolve dissolve;
                        dissolve.noise = 0.35f;
                        dissolve.cell = cell;
                        dissolve.edgeWidth = edge;
                        dissolve.seed = seed;
                        for (int x = 0; x < size; x++){
                            const int band = edgeBandInColumn(x, size, size, dissolve, progress);
                            if (band < 0)
                                continue;
                            columns++;
                            exact += band == (int)edge ? 1 : 0;
                        }
                    }
                }
            }
        }
    }
    ASSERT_GT(columns, 1000);
    std::printf("[dissolve] %d of %d front columns have the edge width exactly\n", exact, columns);
    EXPECT_GE(exact * 100, columns * 95);
}

// R3: progress 0 and 1 are exactly intact and gone; the front sweeps every texel once; the jitter
// keeps every key in [0, 1)
TEST(PixelFxKey, DissolveEnds){
    PfxDissolve dissolve;
    dissolve.seed = 9;
    int burntBefore = 0;
    for (float progress : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}){
        int burnt = 0;
        for (int y = 0; y < 24; y++){
            for (int x = 0; x < 16; x++){
                const float key = pfxDissolveKey(x, y, 16, 24, dissolve);
                EXPECT_GE(key, 0.0f);
                EXPECT_LT(key, 1.0f);
                burnt += pfxDissolveTexel(x, y, 16, 24, dissolve, progress) < 0.0f ? 1 : 0;
            }
        }
        if (progress == 0.0f){
            EXPECT_EQ(burnt, 0);
        }
        if (progress == 1.0f){
            EXPECT_EQ(burnt, 16 * 24);
        }
        EXPECT_GE(burnt, burntBefore) << "a burnt texel stays burnt";
        burntBefore = burnt;
    }
    // the front climbs from the bottom with the default direction
    EXPECT_LT(pfxDissolveTexel(8, 0, 16, 24, dissolve, 0.4f), pfxDissolveTexel(8, 23, 16, 24, dissolve, 0.4f));
    // noise only: still ends at both ends
    dissolve.dirX = dissolve.dirY = 0.0f;
    EXPECT_GT(pfxDissolveTexel(3, 3, 16, 24, dissolve, 0.0f), 0.0f);
    EXPECT_LT(pfxDissolveTexel(3, 3, 16, 24, dissolve, 1.0f), 0.0f);
}
