// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_NOISE_H
#define TERRAIN_NOISE_H

// Moved from editor/util with its bytes unchanged (T1 R22). Whichever of T1 and T2 lands second
// deletes this header for noise::compat (T2 R36, N13).

#include "terrain/TerrainDeterministic.h"

#include <cmath>
#include <cstdint>

namespace jasperengine::terrain {

    // Value noise on an integer lattice, shared by the noise brush and the generators.
    class TerrainNoise {

    public:

        inline static float hash(int x, int y, uint32_t seed) {
            return detHash01(x, y, seed);
        }

        inline static float value(float x, float y, uint32_t seed) {
            const int x0 = static_cast<int>(std::floor(x));
            const int y0 = static_cast<int>(std::floor(y));
            const float tx = detSmooth(x - static_cast<float>(x0));
            const float ty = detSmooth(y - static_cast<float>(y0));

            const float a = hash(x0, y0, seed);
            const float b = hash(x0 + 1, y0, seed);
            const float c = hash(x0, y0 + 1, seed);
            const float d = hash(x0 + 1, y0 + 1, seed);

            const float lower = a + (b - a) * tx;
            const float upper = c + (d - c) * tx;
            return lower + (upper - lower) * ty;
        }

        // Octaves at doubling frequency and halving weight, normalized to [-1, 1]
        inline static float fractal(float x, float y, uint32_t seed, int octaves) {
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
    };

}

#endif // TERRAIN_NOISE_H
