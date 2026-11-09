// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/PixelFxKey.h"

#include <gtest/gtest.h>

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
