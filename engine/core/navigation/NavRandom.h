// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_RANDOM_H
#define NAV_RANDOM_H

// The random stream of navigation's random point queries (docs/specs/S55-navigation-gameplay-api.md
// R2, D2): splitmix64, a fixed integer recipe, so a seed gives the same draws on every platform and
// toolchain (std::mt19937's distributions are implementation-defined). Header only.

#include <cstdint>

namespace jasperengine::nav {

    class NavRandom {
    public:
        explicit NavRandom(uint64_t seed = 0x5EED): state(seed) {}

        uint64_t next() {
            uint64_t x = (state += 0x9E3779B97F4A7C15ull);
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
            return x ^ (x >> 31);
        }

        // [0, 1): the top 24 bits, exact in a float
        float nextFloat() {
            return (float)(next() >> 40) * (1.0f / 16777216.0f);
        }

        uint64_t state;
    };

}

#endif // NAV_RANDOM_H
