// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The key of mesh detail levels and smooth normals (MeshLodPool::hash): a word FNV-1a let two
// flips of a word's top bit cancel (the odd multiply keeps bit 63 where it is, the next xor takes
// it out again), so tight vec2 streams that differ only in the sign of x collided.

#include "pool/MeshLodPool.h"
#include "util/ContentHash.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

using namespace jasperengine;

TEST(MeshLodPoolHash, SeparatesTopBitFlipsOfTwoWords){
    // two vec2 (x, y as floats): the second stream has the sign of both y values flipped, which in
    // 8-byte words is bit 63 of each word
    float a[4] = {1.5f, 2.0f, 3.25f, 4.0f};
    float b[4] = {1.5f, -2.0f, 3.25f, -4.0f};
    EXPECT_NE(MeshLodPool::hash(0, a, sizeof(a)), MeshLodPool::hash(0, b, sizeof(b)));

    unsigned char zero[16] = {};
    unsigned char flipped[16] = {};
    flipped[7] = 0x80;
    flipped[15] = 0x80;
    EXPECT_NE(MeshLodPool::hash(0, zero, sizeof(zero)), MeshLodPool::hash(0, flipped, sizeof(flipped)));
}

TEST(MeshLodPoolHash, ChainsThroughTheSeedAndIsNeverZero){
    const char data[] = "positions";
    const uint64_t first = MeshLodPool::hash(0, data, sizeof(data));
    EXPECT_NE(first, 0u);
    EXPECT_NE(MeshLodPool::hash(first, data, sizeof(data)), first);
    EXPECT_EQ(MeshLodPool::hash(first, data, sizeof(data)), MeshLodPool::hash(first, data, sizeof(data)));
}

TEST(ContentHash, Separates128BitKeys){
    unsigned char zero[32] = {};
    unsigned char one[32] = {};
    one[31] = 1;
    EXPECT_NE(hashContent128(zero, sizeof(zero)), hashContent128(one, sizeof(one)));
    EXPECT_NE(hashContent128(zero, 16), hashContent128(zero, 32));
    EXPECT_EQ(hashContent128(one, sizeof(one)), hashContent128(one, sizeof(one)));
}
