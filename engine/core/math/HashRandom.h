// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace jasperengine {

    // A seeded number in [0, 1): the same (seed, index) always gives the same value, and
    // neighbouring seeds and indices are well spread (splitmix64). The random starts of the
    // Vertex Animator and of actions use it (seed: the entity id), so a scene plays the same
    // every time while copies differ.
    inline double hashUnit(uint64_t seed, int64_t index) {
        uint64_t x = seed * 0x9E3779B97F4A7C15ull + (uint64_t)index * 0xBF58476D1CE4E5B9ull + 0x94D049BB133111EBull;
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBull;
        x ^= x >> 31;
        // 53 bits: exact in a double, < 1
        return (double)(x >> 11) * (1.0 / 9007199254740992.0);
    }

}
