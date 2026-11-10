// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A heightmap imported into a Terrain World's tiles by bands of rows (T4 R32; AC29's slicing half): a
// RAW file read a row at a time reassembles exactly, memory never holds the image, and the tiles it
// writes are those T4 R28's store leaves when the same samples go through it.

#include "TerrainWorldTestSupport.h"
#include "support/AllocationCounter.h"
#include "support/TestTempDir.h"
#include "terrain/world/WorldImport.h"

#include <gtest/gtest.h>

#include <fstream>
#include <map>

using namespace terrainworldtest;

namespace {

    // A 16-bit little-endian RAW of the fixture's samples at image (x, z)
    void writeFixtureRaw(const std::filesystem::path& path, int width, int height) {
        std::ofstream file(path, std::ios::binary);
        std::vector<uint8_t> row(static_cast<size_t>(width) * 2);
        for (int z = 0; z < height; z++) {
            for (int x = 0; x < width; x++) {
                const uint16_t value = fixtureSample(x, z);
                row[static_cast<size_t>(x) * 2] = static_cast<uint8_t>(value & 0xFF);
                row[static_cast<size_t>(x) * 2 + 1] = static_cast<uint8_t>(value >> 8);
            }
            file.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
        }
    }

    // An image in memory: (x, z) -> its sample
    HeightmapRows imageRows(int width, int height, std::function<uint16_t(int, int)> sample, int* rowsRead = nullptr) {
        HeightmapRows rows;
        rows.width = width;
        rows.height = height;
        rows.read = [width, sample, rowsRead](int row, std::vector<uint16_t>& out) {
            if (rowsRead) (*rowsRead)++;
            for (int x = 0; x < width; x++) out[static_cast<size_t>(x)] = sample(x, row);
            return true;
        };
        return rows;
    }

    ImportTarget memoryTarget(const MemoryTileAccess& world, std::map<uint32_t, WorldTile>& written) {
        ImportTarget target;
        target.present = [&world](TileCoord tile) { return world.present(tile); };
        target.read = [&world](TileCoord tile) -> std::shared_ptr<WorldTile> {
            std::shared_ptr<const WorldTile> data = world.tile(tile);
            return data ? std::make_shared<WorldTile>(*data) : nullptr;
        };
        target.write = [&written](const WorldTile& tile) {
            written[tile.coord.key()] = tile;
            return true;
        };
        return target;
    }

}

TEST(TerrainWorldImport, SlicesExactly) {
    jasperengine::test::TestTempDir dir("terrain-world-import");
    const std::filesystem::path path = dir.path() / "island.r16";
    const int side = 2049;
    writeFixtureRaw(path, side, side);

    RawHeightmapScan scan;
    std::string error;
    ASSERT_TRUE(scanRawHeightmap(path.string(), side, side, nullptr, scan, error)) << error;
    EXPECT_FALSE(scan.bigEndian()) << "the smooth byte order is little endian";
    uint16_t low = 65535, high = 0;
    for (int z = 0; z < side; z++) {
        for (int x = 0; x < side; x++) {
            low = std::min(low, fixtureSample(x, z));
            high = std::max(high, fixtureSample(x, z));
        }
    }
    EXPECT_EQ(scan.minLittle, low);
    EXPECT_EQ(scan.maxLittle, high);

    // the file's own values, unscaled, so every sample can be compared bit for bit
    RawHeightmapFile raw;
    raw.path = path.string();
    raw.width = side;
    raw.height = side;
    HeightmapRows file = rawHeightmapRows(raw);
    int rowsRead = 0;
    HeightmapRows rows;
    rows.width = file.width;
    rows.height = file.height;
    rows.read = [&](int row, std::vector<uint16_t>& out) {
        rowsRead++;
        return file.read(row, out);
    };
    HeightmapImport plan;
    plan.lattice = smallLattice(256, 1.0f);
    plan.origin = TileCoord(-4, -4);
    MemoryTileAccess world;
    ImportTarget target;
    target.present = [&world](TileCoord tile) { return world.present(tile); };
    target.read = [](TileCoord) { return std::shared_ptr<WorldTile>(); };
    // each tile checked as it comes and dropped, so what the import holds is all that is held
    int tiles = 0;
    target.write = [&](const WorldTile& tile) {
        tiles++;
        const int tx = tile.coord.x - plan.origin.x, tz = tile.coord.z - plan.origin.z;
        for (int j = 0; j <= 258; j++) {
            for (int i = 0; i <= 258; i++) {
                // a gutter sample past the image repeats the tile's own border (no neighbour there), as
                // TiledMapStore::syncGutters leaves it
                const int gx = tx * 256 + i - 1, gz = tz * 256 + j - 1;
                const bool inside = gx >= 0 && gx < side && gz >= 0 && gz < side;
                const int x = inside ? gx : tx * 256 + std::min(std::max(i, 1), 257) - 1;
                const int z = inside ? gz : tz * 256 + std::min(std::max(j, 1), 257) - 1;
                if (tile.sample(i, j) != fixtureSample(x, z)) {
                    ADD_FAILURE() << "tile " << tile.coord.x << "," << tile.coord.z << " texel " << i << "," << j;
                    return false;
                }
            }
        }
        return true;
    };
    testsupport::resetLargestAllocation();
    ASSERT_TRUE(importHeightmap(rows, plan, target, nullptr, nullptr, error)) << error;
    EXPECT_EQ(tiles, 8 * 8);
    // bands: each image row read about once (a tile row's band overlaps the next by three rows)
    EXPECT_LE(rowsRead, side + 3 * 8);
    // RAM: rows and tiles, never the image (8 MiB here) nor two tile rows of it (2 MiB)
    if (testsupport::countsAllocations()) {
        const uint64_t image = static_cast<uint64_t>(side) * side * 2;
        EXPECT_LE(testsupport::largestAllocation(), image / 32) << "of an image of " << image << " bytes";
    }
}

TEST(TerrainWorldImport, MatchesStorePath) {
    // a 2 x 2 import (the second row of tiles partial) next to tiles that are there: one covered,
    // neighbours along an edge and a corner, and one that does not touch it
    const WorldLattice lattice = smallLattice(64, 1.0f);
    WorldMaps maps;
    maps.maxHeight = 50.0f;
    maps.baseHeight = 1000;
    const std::vector<TileCoord> existing = {TileCoord(0, 0), TileCoord(-1, 0), TileCoord(2, 1), TileCoord(1, 2), TileCoord(-1, -1), TileCoord(4, 0)};
    MemoryTileAccess viaStore, viaImport;
    for (const TileCoord& tile : existing) {
        viaStore.add(tile, fixtureTile(lattice, tile));
        viaImport.add(tile, fixtureTile(lattice, tile));
    }
    const int width = 129, height = 97;
    auto image = [](int x, int z) { return static_cast<uint16_t>(65535 - fixtureSample(x * 3 + 7, z * 2 + 11)); };

    // the store: tiles added, each covered tile's own samples written, gutters settled
    TiledMapStore store(lattice, maps, viaStore);
    const MapId height16{MapKind::Height, 0};
    HeightmapImport plan;
    plan.lattice = lattice;
    for (const TileCoord& tile : importCoveredTiles(plan, width, height)) {
        ASSERT_TRUE(store.addTile(tile));
        const TexelRect rect = store.tileRect(height16, tile);
        std::vector<uint8_t> bytes(static_cast<size_t>(rect.area()) * 2);
        for (int j = 0; j < rect.h; j++) {
            for (int i = 0; i < rect.w; i++) {
                const int sx = tile.x * 64 + i, sz = tile.z * 64 + j;
                const uint16_t value = image(std::min(sx, width - 1), std::min(sz, height - 1));
                bytes[(static_cast<size_t>(j) * rect.w + i) * 2] = static_cast<uint8_t>(value & 0xFF);
                bytes[(static_cast<size_t>(j) * rect.w + i) * 2 + 1] = static_cast<uint8_t>(value >> 8);
            }
        }
        ASSERT_TRUE(store.writeBytes(height16, rect, bytes).ok);
    }
    for (const TileCoord& tile : viaStore.coords()) store.syncGutters(tile);

    std::map<uint32_t, WorldTile> written;
    ImportTarget target = memoryTarget(viaImport, written);
    target.baseHeight = maps.baseHeight;
    std::string error;
    int last = 0, total = 0;
    ASSERT_TRUE(importHeightmap(imageRows(width, height, image), plan, target, nullptr, [&](int done, int all) {
        last = done;
        total = all;
    }, error)) << error;
    const std::vector<TileCoord> changed = importChangedTiles(plan, width, height, [&](TileCoord tile) { return viaImport.present(tile); });
    EXPECT_EQ(last, total);
    EXPECT_EQ(static_cast<size_t>(total), changed.size());
    // the four covered tiles and the four that were there round them; (4, 0) is untouched
    EXPECT_EQ(written.size(), 8u);
    EXPECT_EQ(written.count(TileCoord(4, 0).key()), 0u);
    for (const auto& [key, tile] : written) {
        std::shared_ptr<const WorldTile> expected = viaStore.tile(TileCoord::fromKey(key));
        ASSERT_TRUE(expected) << tile.coord.x << "," << tile.coord.z;
        EXPECT_EQ(tile.height.bytes, expected->height.bytes) << "tile " << tile.coord.x << "," << tile.coord.z;
        EXPECT_FALSE(tile.minMax.empty());
    }
}

TEST(TerrainWorldImport, CancelStopsAndLimitsRefuse) {
    HeightmapImport plan;
    plan.lattice = smallLattice(128, 1.0f);
    HeightmapRows source = imageRows(513, 513, [](int x, int z) { return fixtureSample(x, z); });
    MemoryTileAccess world;
    std::map<uint32_t, WorldTile> written;
    ImportTarget target = memoryTarget(world, written);
    std::string error;
    EXPECT_FALSE(importHeightmap(source, plan, target, [&] { return written.size() >= 3; }, nullptr, error));
    EXPECT_EQ(written.size(), 3u);
    EXPECT_EQ(error, "cancelled");
    // past +-16384 m
    plan.origin = TileCoord(127, 0);
    EXPECT_FALSE(importHeightmap(source, plan, target, nullptr, nullptr, error));
    EXPECT_NE(error.find("16384"), std::string::npos);
    EXPECT_EQ(importTilesAcross(1025, 256), 4);
    EXPECT_EQ(importTilesAcross(1026, 256), 5);
    // flipped, the first row is the highest z
    plan.origin = TileCoord(0, 0);
    plan.flipZ = true;
    written.clear();
    ASSERT_TRUE(importHeightmap(source, plan, target, nullptr, nullptr, error)) << error;
    EXPECT_EQ(written[TileCoord(0, 0).key()].sample(1, 1), fixtureSample(0, 512));
}
