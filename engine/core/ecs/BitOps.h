// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ECS_BITOPS_H
#define ECS_BITOPS_H

#include <bit>
#include <cstdint>

namespace jasperengine::ecs {

    // Index of the lowest set bit; the word must not be zero
    inline unsigned countTrailingZeros(uint64_t word){
        return (unsigned)std::countr_zero(word);
    }

    inline unsigned popCount(uint64_t word){
        return (unsigned)std::popcount(word);
    }

}

#endif // ECS_BITOPS_H
