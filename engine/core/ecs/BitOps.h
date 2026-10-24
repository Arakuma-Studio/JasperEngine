// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ECS_BITOPS_H
#define ECS_BITOPS_H

#include <bit>
#include <cstddef>
#include <cstdint>

// Without -mpopcnt GCC and Clang call libgcc's table-driven __popcountdi2 for std::popcount;
// the SWAR count stays inline (Signature::count is on the metadata getter path)
#if (defined(__x86_64__) || defined(__i386__)) && !defined(__POPCNT__) && (defined(__GNUC__) || defined(__clang__))
#define JE_ECS_SWAR_POPCOUNT 1
#else
#define JE_ECS_SWAR_POPCOUNT 0
#endif

// The accessors on the lookup paths (entity -> slot -> component or metadata): inlined
// even into large callers, where the compiler's size limits would otherwise leave a call
#if defined(_MSC_VER)
#define JE_ECS_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define JE_ECS_INLINE inline __attribute__((always_inline))
#else
#define JE_ECS_INLINE inline
#endif

namespace jasperengine::ecs {

    // Index of the lowest set bit; the word must not be zero
    inline unsigned countTrailingZeros(uint64_t word){
        return (unsigned)std::countr_zero(word);
    }

    JE_ECS_INLINE unsigned popCount(uint64_t word){
#if JE_ECS_SWAR_POPCOUNT
        word = word - ((word >> 1) & 0x5555555555555555ull);
        word = (word & 0x3333333333333333ull) + ((word >> 2) & 0x3333333333333333ull);
        word = (word + (word >> 4)) & 0x0F0F0F0F0F0F0F0Full;
        return (unsigned)((word * 0x0101010101010101ull) >> 56);
#else
        return (unsigned)std::popcount(word);
#endif
    }

    // popCount of each word, summed
    JE_ECS_INLINE unsigned popCountWords(const uint64_t* words, size_t count){
#if JE_ECS_SWAR_POPCOUNT
        // the byte counts of up to 31 words fit a byte each: one multiply at the end
        uint64_t bytes = 0;
        unsigned total = 0;
        for (size_t i = 0; i < count; i++){
            uint64_t word = words[i];
            word = word - ((word >> 1) & 0x5555555555555555ull);
            word = (word & 0x3333333333333333ull) + ((word >> 2) & 0x3333333333333333ull);
            bytes += (word + (word >> 4)) & 0x0F0F0F0F0F0F0F0Full;
            if ((i & 31) == 30 || i + 1 == count){
                total += (unsigned)((bytes * 0x0101010101010101ull) >> 56);
                bytes = 0;
            }
        }
        return total;
#else
        unsigned total = 0;
        for (size_t i = 0; i < count; i++){
            total += (unsigned)std::popcount(words[i]);
        }
        return total;
#endif
    }

}

#endif // ECS_BITOPS_H
