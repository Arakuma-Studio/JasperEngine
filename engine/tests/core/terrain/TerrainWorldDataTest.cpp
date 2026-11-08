// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A Terrain World's manifest, overview and collision samples (T4 R9, R20, R22, R23, R48, R52, R53)

#include "TerrainWorldTestSupport.h"
#include "terrain/world/TerrainWorldData.h"
#include "terrain/world/WorldChunkSamples.h"

#include <gtest/gtest.h>

using namespace terrainworldtest;

TEST(TerrainWorldData, WorldBinRoundTrip) {
    WorldManifest manifest;
    manifest.guid = "0d6f3c2a-6b2e-4c55-9a39-2f1f8a1b9e01";
    manifest.lattice = smallLattice(256, 2.0f);
    manifest.maps.maxHeight = 750.0f;
    manifest.maps.baseHeight = 1234;
    manifest.maps.blendMaps = 2;
    manifest.maps.blendFill[1][2] = 0.5f;
    manifest.maps.hasHoles = true;
    manifest.maps.densityMaps = 3;
    manifest.layerHash = 0x1122334455667788ull;
    manifest.skirtDepth = 0.01f;
    manifest.tiles = {{TileCoord(-3, 4), 10, 2000}, {TileCoord(255, -256), 0, 65535}};
    std::vector<uint8_t> bytes;
    encodeWorldBin(manifest, bytes);
    WorldManifest back;
    std::string error;
    ASSERT_TRUE(decodeWorldBin(bytes, back, &error)) << error;
    EXPECT_EQ(back.guid, manifest.guid);
    EXPECT_EQ(back.lattice, manifest.lattice);
    EXPECT_EQ(back.maps.maxHeight, 750.0f);
    EXPECT_EQ(back.maps.baseHeight, 1234);
    EXPECT_EQ(back.maps.blendMaps, 2);
    EXPECT_EQ(back.maps.blendFill[1][2], 0.5f);
    EXPECT_TRUE(back.maps.hasHoles);
    EXPECT_EQ(back.maps.densityMaps, 3);
    EXPECT_EQ(back.layerHash, manifest.layerHash);
    ASSERT_EQ(back.tiles.size(), 2u);
    EXPECT_EQ(back.tiles[1].coord, TileCoord(255, -256));
    EXPECT_EQ(back.tiles[0].maxHeight, 2000);
    // the same manifest, the same bytes
    std::vector<uint8_t> again;
    encodeWorldBin(back, again);
    EXPECT_EQ(again, bytes);
    // any flipped byte is refused, and so is a cut
    for (size_t i = 0; i < bytes.size(); i += 7) {
        std::vector<uint8_t> bad = bytes;
        bad[i] ^= 0x5A;
        EXPECT_FALSE(decodeWorldBin(bad, back, &error)) << i;
    }
    EXPECT_FALSE(decodeWorldBin(std::span<const uint8_t>(bytes).first(bytes.size() - 3), back, &error));
}

TEST(TerrainWorldData, OverviewHoldsExactSamples) {
    const WorldLattice lattice = smallLattice(64, 1.0f);
    WorldOverview overview;
    initOverview(overview, lattice, TileCoord(-1, -1), TileCoord(1, 0), 0);
    EXPECT_EQ(overview.width(), 3 * 16 + 1);
    EXPECT_EQ(overview.height(), 2 * 16 + 1);
    for (int z = -1; z <= 0; z++) {
        for (int x = -1; x <= 1; x++) {
            auto tile = fixtureTile(lattice, TileCoord(x, z));
            const float colours[] = {0.25f, 0.5f, 0.75f, 1.0f};
            buildOverviewColour(*tile, lattice, colours, 0);
            ASSERT_TRUE(setOverviewPatch(overview, *tile));
        }
    }
    EXPECT_FALSE(setOverviewPatch(overview, *fixtureTile(lattice, TileCoord(5, 5)))) << "outside the box";
    // at every overview sample the query is the tile's own sample there (R9: exact at its stride)
    const int step = lattice.overviewStep();
    for (int j = 0; j < overview.height(); j++) {
        for (int i = 0; i < overview.width(); i++) {
            const int sx = (-1 * 16 + i) * step, sz = (-1 * 16 + j) * step;
            const SurfaceSample sample = sampleOverview(overview, lattice, 100.0f, static_cast<float>(sx), static_cast<float>(sz));
            EXPECT_FALSE(sample.exact);
            EXPECT_TRUE(sample.present);
            EXPECT_EQ(sample.height, heightOfSample(fixtureSample(sx, sz), 100.0f)) << i << ", " << j;
        }
    }
    // round trip through the container
    std::vector<uint8_t> bytes;
    std::string error;
    ASSERT_TRUE(encodeOverview(overview, lattice, zstdCodec(), 9, bytes, &error)) << error;
    WorldOverview back;
    ASSERT_EQ(decodeOverview(bytes, zstdCodec(), back), TileError::None);
    EXPECT_EQ(back.low, overview.low);
    EXPECT_EQ(back.tilesX, 3);
    EXPECT_EQ(back.tilesZ, 2);
    EXPECT_EQ(back.heights, overview.heights);
    EXPECT_EQ(back.colour, overview.colour);
    EXPECT_EQ(back.skirtDepth, overview.skirtDepth);
    // a tile file is not an overview
    std::vector<uint8_t> tileBytes;
    ASSERT_TRUE(encodeTile(*fixtureTile(lattice, TileCoord(0, 0)), zstdCodec(), TileEncodeOptions{}, tileBytes));
    EXPECT_NE(decodeOverview(tileBytes, zstdCodec(), back), TileError::None);
}

TEST(TerrainWorldData, ChunkSamplesAreTheTileSamples) {
    const WorldLattice lattice = smallLattice(128, 0.5f);
    auto tile = fixtureTile(lattice, TileCoord(2, -1));
    TileNeighbourhood near;
    near.tiles[1][1] = tile.get();
    std::vector<float> samples;
    EXPECT_EQ(worldChunkSamples(near, lattice, 300.0f, 64, 1, 0, samples), 0);
    ASSERT_EQ(samples.size(), 65u * 65u);
    for (int j = 0; j <= 64; j++) {
        for (int i = 0; i <= 64; i++) {
            const int sx = 2 * 128 + 64 + i, sz = -128 + j;
            ASSERT_EQ(samples[j * 65 + i], heightOfSample(fixtureSample(sx, sz), 300.0f));
        }
    }
}

TEST(TerrainWorldData, ChunkHolesFollowT3Rim) {
    const WorldLattice lattice = smallLattice(64, 1.0f);
    auto left = fixtureTile(lattice, TileCoord(0, 0));
    auto right = fixtureTile(lattice, TileCoord(1, 0));
    // a hole over the last 8 columns of the left tile and the first 8 of the right one
    left->holes.allocate(lattice.holeSide(), 1, 8, 255);
    right->holes.allocate(lattice.holeSide(), 1, 8, 255);
    for (int z = 20; z < 40; z++) {
        for (int x = 56; x < 64; x++) left->holes.texel(x, z)[0] = 0;
        for (int x = 0; x < 8; x++) right->holes.texel(x, z)[0] = 0;
    }
    TileNeighbourhood near;
    near.tiles[1][1] = left.get();
    near.tiles[1][2] = right.get();
    std::vector<float> samples;
    const int open = worldChunkSamples(near, lattice, 50.0f, 32, 1, 0, samples);
    EXPECT_GT(open, 0);
    // the border sample x = 64 inside the hole is open: its squares in both tiles are holes
    EXPECT_EQ(samples[30 * 33 + 32], WORLD_NO_COLLISION);
    // the rim stays: the hole's first sample column has a solid square on its left
    EXPECT_NE(samples[30 * 33 + (56 - 32)], WORLD_NO_COLLISION);
    EXPECT_EQ(samples[30 * 33 + (57 - 32)], WORLD_NO_COLLISION);
    // without the right tile in memory its squares count as solid: the border stays closed
    near.tiles[1][2] = nullptr;
    worldChunkSamples(near, lattice, 50.0f, 32, 1, 0, samples);
    EXPECT_NE(samples[30 * 33 + 32], WORLD_NO_COLLISION);
    EXPECT_EQ(samples[30 * 33 + 31], WORLD_NO_COLLISION);
}

TEST(TerrainWorldData, CoarseSamplesFromOverview) {
    const WorldLattice lattice = smallLattice(64, 1.0f);
    WorldOverview overview;
    initOverview(overview, lattice, TileCoord(0, 0), TileCoord(1, 1), 0);
    auto tile = fixtureTile(lattice, TileCoord(1, 0));
    ASSERT_TRUE(setOverviewPatch(overview, *tile));
    std::vector<float> coarse;
    ASSERT_TRUE(worldCoarseSamples(overview, 80.0f, TileCoord(1, 0), coarse));
    ASSERT_EQ(coarse.size(), 17u * 17u);
    EXPECT_EQ(coarse[0], heightOfSample(fixtureSample(64, 0), 80.0f));
    EXPECT_EQ(coarse[16 * 17 + 16], heightOfSample(fixtureSample(128, 64), 80.0f));
    EXPECT_FALSE(worldCoarseSamples(overview, 80.0f, TileCoord(3, 3), coarse));
}

TEST(TerrainWorldData, PlatformBudgets) {
    const WorldPlatformBudget desktop = worldPlatformBudget(WorldPlatform::Desktop);
    const WorldPlatformBudget mobile = worldPlatformBudget(WorldPlatform::Mobile);
    const WorldPlatformBudget web = worldPlatformBudget(WorldPlatform::WebSingleThread);
    EXPECT_EQ(desktop.poolSize, 64);
    EXPECT_EQ(desktop.maxTileQuads, 1024);
    EXPECT_EQ(mobile.poolSize, 20);
    EXPECT_EQ(mobile.maxTileQuads, 512);
    EXPECT_EQ(web.poolSize, 16);
    EXPECT_EQ(web.maxTileQuads, 256);
    EXPECT_EQ(web.inFlight, 1);
    EXPECT_EQ(web.overviewMaxSide, 2049);
    EXPECT_EQ(mobile.collisionRadius, 96.0f);
}
