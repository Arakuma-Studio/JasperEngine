// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_HASH_H
#define NAV_HASH_H

// A streaming 64-bit content hash for bake inputs (each tile's triangles, areas, links and
// settings, spec R4) and file checksums. Content-only and endian-fixed: the same values give the
// same hash in any order of jobs, on any thread. Not cryptographic.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace jasperengine::nav {

    class NavHasher {
    public:
        static constexpr uint64_t SEED = 0x9E3779B97F4A7C15ull;

        explicit NavHasher(uint64_t seed = SEED): state(seed ^ 0x6A09E667F3BCC908ull) {}

        void u64(uint64_t value) {
            state ^= mix(value + 0x9E3779B97F4A7C15ull + count);
            state = rotl(state, 27) * 0x94D049BB133111EBull + 0x52DCE729ull;
            count++;
        }
        void u32(uint32_t value) { u64(value); }
        void i32(int32_t value) { u64((uint64_t)(uint32_t)value); }
        void u8(uint8_t value) { u64(value); }
        void boolean(bool value) { u64(value ? 1u : 0u); }
        // by bits: -0 and 0 differ, as they would in a tile
        void f32(float value) {
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            u64(bits);
        }
        void floats(const float* values, size_t count) {
            for (size_t i = 0; i < count; i++)
                f32(values[i]);
        }
        void bytes(const void* data, size_t size) {
            const uint8_t* p = static_cast<const uint8_t*>(data);
            u64(size);
            while (size >= 8) {
                uint64_t word = 0;
                for (int b = 0; b < 8; b++)
                    word |= (uint64_t)p[b] << (8 * b);
                u64(word);
                p += 8;
                size -= 8;
            }
            uint64_t tail = 0;
            for (size_t b = 0; b < size; b++)
                tail |= (uint64_t)p[b] << (8 * b);
            u64(tail);
        }
        void string(std::string_view text) { bytes(text.data(), text.size()); }

        uint64_t value() const { return mix(state ^ count); }

        static uint64_t mix(uint64_t x) {
            x ^= x >> 30;
            x *= 0xBF58476D1CE4E5B9ull;
            x ^= x >> 27;
            x *= 0x94D049BB133111EBull;
            x ^= x >> 31;
            return x;
        }

    private:
        static uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

        uint64_t state;
        uint64_t count = 0;
    };

    inline uint64_t hashBytes(const void* data, size_t size) {
        NavHasher hasher;
        hasher.bytes(data, size);
        return hasher.value();
    }

}

#endif // NAV_HASH_H
