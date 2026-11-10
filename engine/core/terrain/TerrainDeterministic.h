// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_DETERMINISTIC_H
#define TERRAIN_DETERMINISTIC_H

// The arithmetic every terrain kernel shares (T1 R8): the same bytes in the same order give the
// same result on every target and thread count, so no wall clock, no global RNG, no
// std::*_distribution, and no libm beyond sqrt, floor, ceil, round and fabs (those are exact or
// correctly rounded everywhere; sin, cos and pow are not). Sources including this header build with
// no floating-point contraction (TerrainSources.cmake).

#include <cmath>
#include <cstdint>

namespace jasperengine::terraincore {

    // PCG32 (XSH-RR, M. E. O'Neill, pcg-random.org): a small generator with a stated output on every
    // platform
    class Pcg32 {
    public:
        explicit Pcg32(uint64_t seed, uint64_t stream = 0x5851F42D4C957F2Dull): state(0), increment((stream << 1u) | 1u) {
            next();
            state += seed;
            next();
        }

        uint32_t next() {
            const uint64_t previous = state;
            state = previous * 6364136223846793005ull + increment;
            const uint32_t xorshifted = static_cast<uint32_t>(((previous >> 18u) ^ previous) >> 27u);
            const uint32_t rotation = static_cast<uint32_t>(previous >> 59u);
            return (xorshifted >> rotation) | (xorshifted << ((32u - rotation) & 31u));
        }

        // [0, 1)
        float nextFloat() {
            return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f);
        }

    private:
        uint64_t state;
        uint64_t increment;
    };

    inline constexpr float DET_PI = 3.14159265358979f;
    inline constexpr float DET_TWO_PI = 6.28318530717959f;

    // A Taylor polynomial on [-pi/2, pi/2], within 2e-6 of the true value at any turn the float angle holds
    inline float detSin(float radians) {
        float turns = radians / DET_TWO_PI;
        turns -= std::floor(turns + 0.5f);
        float x = turns * DET_TWO_PI;
        if (x > DET_PI * 0.5f) x = DET_PI - x;
        if (x < -DET_PI * 0.5f) x = -DET_PI - x;
        const float x2 = x * x;
        return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f + x2 * (-1.0f / 39916800.0f))))));
    }

    inline float detCos(float radians) {
        return detSin(radians + DET_PI * 0.5f);
    }

    inline float detRadians(float degrees) {
        return degrees * (DET_PI / 180.0f);
    }

    inline float detClamp01(float value) {
        // NaN compares false on both and lands on 0: a map never stores it
        if (!(value > 0.0f)) return 0.0f;
        return value < 1.0f ? value : 1.0f;
    }

    inline float detSmooth(float t) {
        return t * t * (3.0f - 2.0f * t);
    }

    // 0 at edge0 and below, 1 at edge1 and above; edge0 > edge1 runs the other way
    inline float detSmoothstep(float edge0, float edge1, float value) {
        if (edge0 == edge1) return value >= edge1 ? 1.0f : 0.0f;
        return detSmooth(detClamp01((value - edge0) / (edge1 - edge0)));
    }

    // One value in [0, 1] per integer cell, the same everywhere: random_noise's field (T1 R19)
    inline float detHash01(int x, int y, uint32_t seed) {
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

}

#endif // TERRAIN_DETERMINISTIC_H
