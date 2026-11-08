// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A heightmap sliced into a Terrain World's tiles by bands of rows (T4 R32; AC29's slicing half)

#include "TerrainWorldTestSupport.h"
#include "terrain/world/WorldImport.h"

#include <gtest/gtest.h>

#include <map>

using namespace terrainworldtest;

TEST(TerrainWorldImport, SlicesExactly) {
    HeightmapRows source;
    source.width = 1025;
    source.height = 769;
    int rowsRead = 0;
    source.read = [&](int row, std::vector<uint16_t>& out) {
        rowsRead++;
        for (int x = 0; x < source.width; x++) out[static_cast<size_t>(x)] = fixtureSample(x, row);
        return true;
    };
    HeightmapImport plan;
    plan.lattice = smallLattice(256, 1.0f);
    plan.origin = TileCoord(-2, 1);
    std::map<uint32_t, WorldTile> tiles;
    std::string error;
    ASSERT_TRUE(sliceHeightmap(source, plan, [&](WorldTile& tile) {
        tiles[tile.coord.key()] = tile;
        return true;
    }, nullptr, error)) << error;
    EXPECT_EQ(tiles.size(), 4u * 3u);
    // every shared sample is the image's, and a border is the same in both tiles holding it
    for (const auto& [key, tile] : tiles) {
        const int tx = tile.coord.x - plan.origin.x, tz = tile.coord.z - plan.origin.z;
        for (int j = 1; j <= 257; j++) {
            for (int i = 1; i <= 257; i++) {
                const int x = std::min(tx * 256 + i - 1, source.width - 1), z = std::min(tz * 256 + j - 1, source.height - 1);
                ASSERT_EQ(tile.sample(i, j), fixtureSample(x, z)) << tile.coord.x << "," << tile.coord.z;
            }
        }
    }
    // bands: each image row read about once (a tile row's band overlaps the next by three rows)
    EXPECT_LT(rowsRead, source.height + 3 * 4);
    // flipped, the first row is the highest z
    plan.flipZ = true;
    tiles.clear();
    ASSERT_TRUE(sliceHeightmap(source, plan, [&](WorldTile& tile) {
        tiles[tile.coord.key()] = tile;
        return true;
    }, nullptr, error));
    EXPECT_EQ(tiles[TileCoord(-2, 1).key()].sample(1, 1), fixtureSample(0, source.height - 1));
}

TEST(TerrainWorldImport, CancelStopsAndLimitsRefuse) {
    HeightmapRows source;
    source.width = 513;
    source.height = 513;
    source.read = [&](int row, std::vector<uint16_t>& out) {
        for (int x = 0; x < source.width; x++) out[static_cast<size_t>(x)] = fixtureSample(x, row);
        return true;
    };
    HeightmapImport plan;
    plan.lattice = smallLattice(128, 1.0f);
    int emitted = 0;
    std::string error;
    EXPECT_FALSE(sliceHeightmap(source, plan, [&](WorldTile&) { return ++emitted > 0; }, [&] { return emitted >= 3; }, error));
    EXPECT_EQ(emitted, 3);
    EXPECT_EQ(error, "cancelled");
    // past +-16384 m
    plan.origin = TileCoord(127, 0);
    EXPECT_FALSE(sliceHeightmap(source, plan, [&](WorldTile&) { return true; }, nullptr, error));
    EXPECT_NE(error.find("16384"), std::string::npos);
    EXPECT_EQ(importTilesAcross(1025, 256), 4);
    EXPECT_EQ(importTilesAcross(1026, 256), 5);
}
