// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ContentHash.h"

// every symbol static in this file: no clash with Zstandard's prefixed copy
#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"

using namespace jasperengine;

ContentHash128 jasperengine::hashContent128(const void* data, size_t size, uint64_t seed){
    const XXH128_hash_t hash = XXH3_128bits_withSeed(data, size, seed);
    ContentHash128 result;
    result.lo = hash.low64;
    result.hi = hash.high64;
    return result;
}

uint64_t jasperengine::hashContent64(const void* data, size_t size, uint64_t seed){
    return XXH3_64bits_withSeed(data, size, seed);
}
