// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/PixelFxKey.h"

#include <algorithm>
#include <cmath>

namespace jasperengine::pixelart {

    namespace {
        // PCG2D (Jarzynski and Olano 2020), as pfxPcg2d in pixel_fx.glsl
        void pcg2d(uint32_t& x, uint32_t& y){
            x = x * 1664525u + 1013904223u;
            y = y * 1664525u + 1013904223u;
            x += y * 1664525u;
            y += x * 1664525u;
            x ^= x >> 16u;
            y ^= y >> 16u;
            x += y * 1664525u;
            y += x * 1664525u;
            x ^= x >> 16u;
            y ^= y >> 16u;
        }
    }

    uint32_t pfxHash(uint32_t x, uint32_t y, uint32_t seed){
        uint32_t a = x ^ seed;
        uint32_t b = y ^ (seed * 2654435769u);
        pcg2d(a, b);
        return a;
    }

    float pfxHash01(uint32_t x, uint32_t y, uint32_t seed){
        return (float)(pfxHash(x, y, seed) >> 8u) * (1.0f / 16777216.0f);
    }

    float pfxValueNoise(float x, float y, float cell, uint32_t seed){
        const float c = std::max(cell, 1.0f);
        const float qx = x / c, qy = y / c;
        const float ix = std::floor(qx), iy = std::floor(qy);
        const float fx = qx - ix, fy = qy - iy;
        const uint32_t cx = (uint32_t)ix, cy = (uint32_t)iy;
        const float a = pfxHash01(cx, cy, seed);
        const float b = pfxHash01(cx + 1u, cy, seed);
        const float d = pfxHash01(cx, cy + 1u, seed);
        const float e = pfxHash01(cx + 1u, cy + 1u, seed);
        const float lo = a + (b - a) * fx;
        const float hi = d + (e - d) * fx;
        return lo + (hi - lo) * fy;
    }

}
