// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef CONTENT_HASH_H
#define CONTENT_HASH_H

#include "Export.h"

#include <cstddef>
#include <cstdint>

namespace jasperengine{

    // A 128-bit content hash: the key of bytes shared by content (MeshBufferPool's GPU buffers),
    // where a collision would draw the wrong data. Two different byte strings collide with a
    // probability near 2^-128 per pair (2^-64 over the birthday bound).
    struct ContentHash128{
        uint64_t lo = 0;
        uint64_t hi = 0;

        bool operator==(const ContentHash128& rhs) const { return lo == rhs.lo && hi == rhs.hi; }
        bool operator!=(const ContentHash128& rhs) const { return !(*this == rhs); }
        bool operator<(const ContentHash128& rhs) const { return hi < rhs.hi || (hi == rhs.hi && lo < rhs.lo); }
        bool isZero() const { return lo == 0 && hi == 0; }
    };

    // XXH3 (engine/libs/xxhash): every input bit reaches every output bit, unlike the word FNV-1a
    // it replaces, whose high-bit flips in an even count of words cancel
    JASPERENGINE_API ContentHash128 hashContent128(const void* data, size_t size, uint64_t seed = 0);
    JASPERENGINE_API uint64_t hashContent64(const void* data, size_t size, uint64_t seed = 0);

}

#endif // CONTENT_HASH_H
