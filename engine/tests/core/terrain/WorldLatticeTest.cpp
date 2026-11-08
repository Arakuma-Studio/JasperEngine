// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A Terrain World's lattice and the shader's kernels mirrored on the CPU (T4 R2, R3, R6, R11, D3, D5;
// AC1, AC9)

#include "TerrainWorldTestSupport.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstring>

using namespace terrainworldtest;

namespace {

    uint32_t bitsOf(float value) { return std::bit_cast<uint32_t>(value); }

    // A 2 x 2 world of the fixture written through the store, as an op would: gutters follow
    struct FourTiles {
        WorldLattice lattice = smallLattice(64, 1.0f);
        WorldMaps maps;
        MemoryTileAccess access;
        TiledMapStore store;

        FourTiles(): store(lattice, maps, access, TiledStoreOptions{true}) {
            const int n = lattice.tileQuads;
            const int m = lattice.heightOrigin();
            for (const TileCoord tile : {TileCoord(-1, -1), TileCoord(0, -1), TileCoord(-1, 0), TileCoord(0, 0)}) {
                EXPECT_TRUE(store.addTile(tile));
            }
            const TexelRect all{m - n, m - n, 2 * n + 1, 2 * n + 1};
            std::vector<uint8_t> bytes(static_cast<size_t>(all.area()) * 2);
            for (int y = 0; y < all.h; y++) {
                for (int x = 0; x < all.w; x++) {
                    const uint16_t value = fixtureSample(all.x + x - m, all.y + y - m);
                    bytes[(static_cast<size_t>(y) * all.w + x) * 2] = static_cast<uint8_t>(value & 0xFF);
                    bytes[(static_cast<size_t>(y) * all.w + x) * 2 + 1] = static_cast<uint8_t>(value >> 8);
                }
            }
            EXPECT_TRUE(store.writeBytes(MapId{MapKind::Height, 0}, all, bytes).ok);
        }
    };

    // Compares every shared border sample of two neighbours, heights and normals, bit for bit;
    // returns how many normals differ
    int compareBorder(const WorldTile& a, const WorldTile& b, int n, float maxHeight, float spacing, bool alongX) {
        int differing = 0;
        for (int k = 0; k <= n; k++) {
            // a's last shared column (or row) is b's first
            const int ax = alongX ? n + 1 : k + 1, az = alongX ? k + 1 : n + 1;
            const int bx = alongX ? 1 : k + 1, bz = alongX ? k + 1 : 1;
            EXPECT_EQ(bitsOf(tileHeightAt(a, static_cast<float>(ax), static_cast<float>(az), maxHeight)),
                      bitsOf(tileHeightAt(b, static_cast<float>(bx), static_cast<float>(bz), maxHeight)));
            if (k < n) {
                // halfway along the border: the bilerp's weight on the far side is exactly 0
                const float half = 0.5f;
                EXPECT_EQ(bitsOf(tileHeightAt(a, ax + (alongX ? 0.0f : half), az + (alongX ? half : 0.0f), maxHeight)),
                          bitsOf(tileHeightAt(b, bx + (alongX ? 0.0f : half), bz + (alongX ? half : 0.0f), maxHeight)));
            }
            for (int delta : {1, 2, 4, 8}) {
                const LatticeNormal na = tileNormalAt(a, ax, az, delta, maxHeight, spacing);
                const LatticeNormal nb = tileNormalAt(b, bx, bz, delta, maxHeight, spacing);
                if (!(bitsOf(na.x) == bitsOf(nb.x) && bitsOf(na.y) == bitsOf(nb.y) && bitsOf(na.z) == bitsOf(nb.z))) differing++;
            }
        }
        return differing;
    }

}

TEST(WorldLattice, CoordinatesExactAtTheLimit) {
    for (int n : {64, 128, 256, 512, 1024}) {
        for (float h : {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f}) {
            WorldLattice lattice = smallLattice(n, h);
            std::string why;
            ASSERT_TRUE(validateLattice(lattice, &why)) << why;
            const double size = static_cast<double>(n) * h;
            EXPECT_EQ(bitsOf(lattice.tileSize()), bitsOf(static_cast<float>(size)));
            EXPECT_EQ(bitsOf(lattice.nodeSize(lattice.tileLevel())), bitsOf(lattice.tileSize())) << "level T is a tile";
            EXPECT_EQ(bitsOf(lattice.nodeSize(0)), bitsOf(32.0f * h));
            EXPECT_EQ(lattice.rootLevel(), lattice.tileLevel() + 8);

            for (int tile : {WORLD_TILE_MIN, -77, -1, 0, 1, 93, WORLD_TILE_MAX}) {
                const double exact = static_cast<double>(tile) * size;
                const TileCoord coord(tile, -tile - 1);
                if (std::fabs(exact) > WORLD_EXTENT_LIMIT || std::fabs(exact + size) > WORLD_EXTENT_LIMIT) {
                    EXPECT_FALSE(tileWithinLimit(lattice, TileCoord(tile, 0))) << "R6 refuses past +-16384 m";
                    continue;
                }
                EXPECT_EQ(bitsOf(lattice.tileOriginX(tile)), bitsOf(static_cast<float>(exact)));
                EXPECT_EQ(tileOfLocal(lattice.tileOriginX(tile), lattice.tileSize()), tile) << "a corner belongs to the higher tile";
                EXPECT_EQ(tileOfLocal(lattice.tileOriginX(tile) + h * 0.5f, lattice.tileSize()), tile);
                EXPECT_EQ(TileCoord::fromKey(coord.key()), coord);
            }

            // every sample out to the limit is its exact position
            const int limitSamples = static_cast<int>(WORLD_EXTENT_LIMIT / h);
            for (int sample : {-limitSamples, -limitSamples + 1, -1, 0, 1, limitSamples - 1, limitSamples}) {
                EXPECT_EQ(bitsOf(lattice.localOfSample(sample)), bitsOf(static_cast<float>(static_cast<double>(sample) * h)));
            }
            EXPECT_EQ(bitsOf(lattice.localOfSample(limitSamples)), bitsOf(WORLD_EXTENT_LIMIT));
            // global texels are never negative and the lattice holds the whole addressable grid
            EXPECT_EQ(lattice.heightOrigin(), 256 * n);
            EXPECT_EQ(lattice.heightLatticeSide(), 512 * n + 1);
        }
    }
    std::string why;
    EXPECT_FALSE(validateLattice(smallLattice(32, 1.0f), &why));
    EXPECT_FALSE(validateLattice(smallLattice(2048, 1.0f), &why));
    EXPECT_FALSE(validateLattice(smallLattice(256, 0.3f), &why));
    EXPECT_FALSE(validateLattice(smallLattice(256, 16.0f), &why));
}

TEST(WorldLattice, HeightRoundTripsThroughRG8) {
    for (uint32_t value = 0; value <= 65535; value += 251) {
        uint8_t rg[2];
        encodeHeightRG8(static_cast<uint16_t>(value), rg);
        EXPECT_EQ(decodeHeightRG8(rg), value);
        EXPECT_EQ(heightOfSample(static_cast<uint16_t>(value), 1000.0f), static_cast<float>(value) / 65535.0f * 1000.0f);
    }
}

TEST(WorldLattice, TriangleSplitsAlongTheMeshDiagonal) {
    // on the diagonal both triangles agree, and each corner is its sample
    EXPECT_EQ(latticeTriangle(1.0f, 2.0f, 3.0f, 4.0f, 0.0f, 0.0f), 1.0f);
    EXPECT_EQ(latticeTriangle(1.0f, 2.0f, 3.0f, 4.0f, 1.0f, 0.0f), 2.0f);
    EXPECT_EQ(latticeTriangle(1.0f, 2.0f, 3.0f, 4.0f, 0.0f, 1.0f), 3.0f);
    EXPECT_EQ(latticeTriangle(1.0f, 2.0f, 3.0f, 4.0f, 1.0f, 1.0f), 4.0f);
    EXPECT_FLOAT_EQ(latticeTriangle(0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 0.5f), 0.5f) << "the (0,0)-(1,1) diagonal";
    EXPECT_FLOAT_EQ(latticeTriangle(0.0f, 1.0f, 1.0f, 0.0f, 0.5f, 0.5f), 0.0f) << "a bilerp would give 0.5";
    // off the diagonal, the low-x high-z corner belongs to the other triangle
    EXPECT_FLOAT_EQ(latticeTriangle(0.0f, 0.0f, 1.0f, 0.0f, 0.25f, 0.75f), 0.5f);
    EXPECT_FLOAT_EQ(latticeTriangle(0.0f, 0.0f, 1.0f, 0.0f, 0.75f, 0.25f), 0.0f);
}

TEST(WorldLattice, BorderHeightsBitIdentical) {
    FourTiles world;
    const int n = world.lattice.tileQuads;
    const auto a = world.access.tile(TileCoord(-1, 0)), b = world.access.tile(TileCoord(0, 0));
    const auto c = world.access.tile(TileCoord(-1, -1)), d = world.access.tile(TileCoord(-1, 0));
    ASSERT_TRUE(a && b && c && d);
    compareBorder(*a, *b, n, world.maps.maxHeight, world.lattice.spacing, true);
    compareBorder(*c, *d, n, world.maps.maxHeight, world.lattice.spacing, false);
    // each tile's shared samples are the fixture's, wherever they were written from
    for (const TileCoord tile : world.access.coords()) {
        const auto data = world.access.tile(tile);
        for (int j = 1; j <= n + 1; j++) {
            for (int i = 1; i <= n + 1; i++) ASSERT_EQ(data->sample(i, j), fixtureSample(tile.x * n + i - 1, tile.z * n + j - 1));
        }
    }
}

TEST(WorldLattice, BorderNormalsMatch) {
    FourTiles world;
    const int n = world.lattice.tileQuads;
    const float maxHeight = world.maps.maxHeight, h = world.lattice.spacing;
    const auto lowLeft = world.access.tile(TileCoord(-1, -1)), lowRight = world.access.tile(TileCoord(0, -1));
    const auto highLeft = world.access.tile(TileCoord(-1, 0)), highRight = world.access.tile(TileCoord(0, 0));
    EXPECT_EQ(compareBorder(*lowLeft, *lowRight, n, maxHeight, h, true), 0);
    EXPECT_EQ(compareBorder(*highLeft, *highRight, n, maxHeight, h, true), 0);
    EXPECT_EQ(compareBorder(*lowLeft, *highLeft, n, maxHeight, h, false), 0);
    EXPECT_EQ(compareBorder(*lowRight, *highRight, n, maxHeight, h, false), 0);

    // the control the spec names: without the gutter the two sides read different neighbours
    WorldTile noGutter = *highRight;
    for (int k = 0; k < n + 3; k++) noGutter.setSample(0, k, noGutter.sample(1, k));
    int differing = 0;
    for (int k = 0; k <= n; k++) {
        const LatticeNormal left = tileNormalAt(*highLeft, n + 1, k + 1, 1, maxHeight, h);
        const LatticeNormal right = tileNormalAt(noGutter, 1, k + 1, 1, maxHeight, h);
        if (!(left == right)) differing++;
    }
    EXPECT_GT(differing, n / 2) << "the comparison can see a missing gutter";
}

TEST(WorldLattice, AbsentNeighbourGutterRepeatsBorder) {
    WorldLattice lattice = smallLattice(64, 1.0f);
    WorldMaps maps;
    MemoryTileAccess access;
    TiledMapStore store(lattice, maps, access, TiledStoreOptions{true});
    ASSERT_TRUE(store.addTile(TileCoord(0, 0)));
    const int n = lattice.tileQuads;
    const int m = lattice.heightOrigin();
    // a ridge along the tile's high-x border and just past it, so the neighbour is made
    const TexelRect edge{m + n, m + 1, 2, n - 1};
    std::vector<float> values(static_cast<size_t>(edge.area()), 0.75f);
    ASSERT_TRUE(store.write(MapId{MapKind::Height, 0}, edge, values).ok);
    ASSERT_TRUE(access.present(TileCoord(1, 0)));
    EXPECT_EQ(access.count(), 2u) << "a change on a shared border alone makes no tile";
    ASSERT_TRUE(store.removeTile(TileCoord(1, 0)));
    const auto tile = access.tile(TileCoord(0, 0));
    for (int k = 0; k < n + 3; k++) EXPECT_EQ(tile->sample(n + 2, k), tile->sample(n + 1, std::min(std::max(k, 1), n + 1))) << k;
    EXPECT_EQ(tile->sample(n + 1, 10), static_cast<uint16_t>(quantizeChannel(0.75f, 16)));
}
