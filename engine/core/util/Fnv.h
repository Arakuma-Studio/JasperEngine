// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// FNV-1a: the field ids of cooked scenes (32-bit) and the entry checks of .jpak content packs
// (64-bit) (docs/specs/S30-shipping.md D2, D6). Constexpr, so ids can be compile-time constants.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace jasperengine {

    constexpr uint32_t FNV1A32_SEED = 0x811c9dc5u;
    constexpr uint64_t FNV1A64_SEED = 0xcbf29ce484222325ull;

    constexpr uint32_t fnv1a32(std::string_view data, uint32_t hash = FNV1A32_SEED) {
        for (char c : data) {
            hash ^= static_cast<uint8_t>(c);
            hash *= 0x01000193u;
        }
        return hash;
    }

    inline uint64_t fnv1a64(const void* data, size_t size, uint64_t hash = FNV1A64_SEED) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; i++) {
            hash ^= bytes[i];
            hash *= 0x100000001b3ull;
        }
        return hash;
    }

    constexpr uint64_t fnv1a64(std::string_view data, uint64_t hash = FNV1A64_SEED) {
        for (char c : data) {
            hash ^= static_cast<uint8_t>(c);
            hash *= 0x100000001b3ull;
        }
        return hash;
    }

}
