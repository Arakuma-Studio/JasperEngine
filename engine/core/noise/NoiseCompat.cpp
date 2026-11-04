// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "noise/NoiseCompat.h"

#include <cmath>

namespace jasperengine { namespace noise { namespace compat {

    float hash01(int x, int y, uint32_t seed) {
        uint32_t h = seed ^ 0x9E3779B9u;
        h ^= static_cast<uint32_t>(x) + 0x85EBCA6Bu + (h << 6) + (h >> 2);
        h ^= static_cast<uint32_t>(y) + 0xC2B2AE35u + (h << 6) + (h >> 2);
        h ^= h >> 16;
        h *= 0x7FEB352Du;
        h ^= h >> 15;
        h *= 0x846CA68Bu;
        h ^= h >> 16;
        return static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x00FFFFFFu);
    }

    float value(float x, float y, uint32_t seed) {
        const int x0 = static_cast<int>(std::floor(x));
        const int y0 = static_cast<int>(std::floor(y));
        const float fx = x - static_cast<float>(x0);
        const float fy = y - static_cast<float>(y0);
        const float tx = fx * fx * (3.0f - 2.0f * fx);
        const float ty = fy * fy * (3.0f - 2.0f * fy);

        const float a = hash01(x0, y0, seed);
        const float b = hash01(x0 + 1, y0, seed);
        const float c = hash01(x0, y0 + 1, seed);
        const float d = hash01(x0 + 1, y0 + 1, seed);

        const float lower = a + (b - a) * tx;
        const float upper = c + (d - c) * tx;
        return lower + (upper - lower) * ty;
    }

    float fractal(float x, float y, uint32_t seed, int octaves) {
        float sum = 0.0f;
        float weight = 1.0f;
        float weightSum = 0.0f;
        float frequency = 1.0f;

        for (int o = 0; o < octaves; o++) {
            sum += (value(x * frequency, y * frequency, seed + static_cast<uint32_t>(o * 1013)) * 2.0f - 1.0f) * weight;
            weightSum += weight;
            weight *= 0.5f;
            frequency *= 2.0f;
        }

        return weightSum > 0.0f ? sum / weightSum : 0.0f;
    }

}}}
