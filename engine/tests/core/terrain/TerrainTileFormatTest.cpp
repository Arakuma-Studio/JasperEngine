// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The .jtile format (T4 R3, R49, R52, D7; AC30, AC31)

#include "TerrainWorldTestSupport.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <random>

using namespace terrainworldtest;

namespace {

    // A tile with every kind of section, from the fixture
    std::shared_ptr<TileData> fullTile(const WorldLattice& lattice, TileCoord coord) {
        auto tile = fixtureTile(lattice, coord);
        tile->blend.resize(2);
        for (int i = 0; i < 2; i++) {
            tile->blend[i].allocate(lattice.blendSide(), 4, 8, 0);
            for (size_t b = 0; b < tile->blend[i].bytes.size(); b++) tile->blend[i].bytes[b] = static_cast<uint8_t>((b * (7 + i)) % 251);
        }
        tile->holes.allocate(lattice.holeSide(), 1, 8, 255);
        tile->holes.bytes[17] = 0;
        tile->density.resize(3);
        tile->density[2].allocate(lattice.densitySide(), 1, 8, 9);
        const float colours[] = {0.5f, 0.4f, 0.3f, 1.0f, 0.1f, 0.6f, 0.2f, 1.0f, 0.9f, 0.9f, 0.9f, 1.0f};
        buildOverviewColour(*tile, lattice, colours, 2);
        return tile;
    }

    void expectSameTile(const TileData& a, const TileData& b) {
        EXPECT_EQ(a.coord, b.coord);
        EXPECT_EQ(a.tileQuads, b.tileQuads);
        EXPECT_EQ(a.spacing, b.spacing);
        EXPECT_EQ(a.height.bytes, b.height.bytes);
        ASSERT_EQ(a.blend.size(), b.blend.size());
        for (size_t i = 0; i < a.blend.size(); i++) EXPECT_EQ(a.blend[i].bytes, b.blend[i].bytes);
        EXPECT_EQ(a.holes.bytes, b.holes.bytes);
        ASSERT_EQ(a.density.size(), b.density.size());
        for (size_t i = 0; i < a.density.size(); i++) EXPECT_EQ(a.density[i].bytes, b.density[i].bytes);
        EXPECT_EQ(a.overviewHeight, b.overviewHeight);
        EXPECT_EQ(a.overviewColour, b.overviewColour);
        EXPECT_EQ(a.minMax, b.minMax);
        EXPECT_EQ(a.skirtDepth, b.skirtDepth);
    }

}

TEST(TerrainTileFormat, RoundTrip) {
    const WorldLattice lattice = smallLattice(128, 0.5f);
    const auto tile = fullTile(lattice, TileCoord(-3, 7));
    for (bool compressed : {true, false}) {
        TileCodec codec = compressed ? zstdCodec() : TileCodec{};
        std::vector<uint8_t> bytes;
        std::string error;
        ASSERT_TRUE(encodeTile(*tile, codec, TileEncodeOptions{}, bytes, &error)) << error;
        TileData back;
        ASSERT_EQ(decodeTile(bytes, zstdCodec(), back), TileError::None);
        expectSameTile(*tile, back);
    }
    // D8: the overview alone, without decoding the samples
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(encodeTile(*tile, zstdCodec(), TileEncodeOptions{}, bytes));
    TileData overviewOnly;
    const uint32_t only[] = {TILE_SECTION_OVERVIEW_HEIGHT, TILE_SECTION_OVERVIEW_COLOUR};
    ASSERT_EQ(decodeTile(bytes, zstdCodec(), overviewOnly, only), TileError::None);
    EXPECT_TRUE(overviewOnly.height.empty());
    EXPECT_EQ(overviewOnly.overviewHeight, tile->overviewHeight);
    EXPECT_EQ(overviewOnly.overviewColour, tile->overviewColour);
}

TEST(TerrainTileFormat, DeterministicBytes) {
    const WorldLattice lattice = smallLattice(256, 1.0f);
    std::vector<uint8_t> first, second;
    ASSERT_TRUE(encodeTile(*fullTile(lattice, TileCoord(4, -2)), zstdCodec(), TileEncodeOptions{}, first));
    ASSERT_TRUE(encodeTile(*fullTile(lattice, TileCoord(4, -2)), zstdCodec(), TileEncodeOptions{}, second));
    EXPECT_EQ(first, second);
    // decode and encode again: the same file
    TileData back;
    ASSERT_EQ(decodeTile(first, zstdCodec(), back), TileError::None);
    std::vector<uint8_t> third;
    ASSERT_TRUE(encodeTile(back, zstdCodec(), TileEncodeOptions{}, third));
    EXPECT_EQ(first, third);
}

TEST(TerrainTileFormat, PreservesUnknownSections) {
    const WorldLattice lattice = smallLattice(64, 1.0f);
    auto tile = fixtureTile(lattice, TileCoord(0, 0));
    TileRawSection future;
    future.id = tileSectionId('F', 'U', 'T', 'R');
    future.index = 3;
    future.codec = static_cast<uint32_t>(TileCodecId::Raw);
    future.stored = {1, 2, 3, 4, 5};
    future.rawBytes = 5;
    future.checksum = 0x1234;
    tile->unknown.push_back(future);
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(encodeTile(*tile, zstdCodec(), TileEncodeOptions{}, bytes));
    TileData back;
    ASSERT_EQ(decodeTile(bytes, zstdCodec(), back), TileError::None);
    ASSERT_EQ(back.unknown.size(), 1u);
    EXPECT_EQ(back.unknown[0].id, future.id);
    EXPECT_EQ(back.unknown[0].index, 3u);
    EXPECT_EQ(back.unknown[0].stored, future.stored);
    EXPECT_EQ(back.unknown[0].checksum, 0x1234u);
    // rewritten, it is still there; the cook drops it
    std::vector<uint8_t> again;
    ASSERT_TRUE(encodeTile(back, zstdCodec(), TileEncodeOptions{}, again));
    EXPECT_EQ(again, bytes);
    TileEncodeOptions cook;
    cook.level = 19;
    cook.keepUnknown = false;
    ASSERT_TRUE(encodeTile(back, zstdCodec(), cook, again));
    TileData cooked;
    ASSERT_EQ(decodeTile(again, zstdCodec(), cooked), TileError::None);
    EXPECT_TRUE(cooked.unknown.empty());
}

TEST(TerrainTileFormat, RefusesNewerMajor) {
    const WorldLattice lattice = smallLattice(64, 1.0f);
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(encodeTile(*fixtureTile(lattice, TileCoord(1, 1)), zstdCodec(), TileEncodeOptions{}, bytes));
    bytes[8] = static_cast<uint8_t>(TILE_FORMAT_VERSION + 1);
    TileData back;
    EXPECT_EQ(decodeTile(bytes, zstdCodec(), back), TileError::NewerVersion);
    TileHeader header;
    EXPECT_EQ(parseTileHeader(bytes, header), TileError::NewerVersion);
}

TEST(TerrainTileFormat, RejectsCorrupt) {
    const WorldLattice lattice = smallLattice(64, 1.0f);
    std::vector<uint8_t> good;
    ASSERT_TRUE(encodeTile(*fullTile(lattice, TileCoord(2, -5)), zstdCodec(), TileEncodeOptions{}, good));
    const TileCodec codec = zstdCodec();
    std::mt19937 random(1234);
    int rejected = 0;
    for (int trial = 0; trial < 1000; trial++) {
        std::vector<uint8_t> bad = good;
        switch (trial % 4) {
            case 0: bad.resize(random() % bad.size()); break;     // truncated
            case 1: bad[random() % bad.size()] ^= static_cast<uint8_t>(1 + random() % 255); break;
            case 2:
                for (int k = 0; k < 8; k++) bad[random() % bad.size()] = static_cast<uint8_t>(random());
                break;
            default: {
                // a header or table field
                const size_t at = 8 + random() % std::min<size_t>(bad.size() - 8, 36 + 48 * 4);
                bad[at] = static_cast<uint8_t>(random());
                break;
            }
        }
        TileData out;
        const TileError error = decodeTile(bad, codec, out);
        if (error != TileError::None) rejected++;
    }
    // a flip inside padding changes nothing; everything else is refused, and nothing crashed
    EXPECT_GT(rejected, 900);
    TileData out;
    EXPECT_EQ(decodeTile(std::span<const uint8_t>(), codec, out), TileError::Truncated);
    const uint8_t notATile[64] = {'P', 'N', 'G'};
    EXPECT_EQ(decodeTile(notATile, codec, out), TileError::BadMagic);
}

TEST(TerrainTileFormat, MigratesThroughTheTable) {
    // version 1 is the only one: the table is empty, and a synthetic step shows how it runs
    EXPECT_TRUE(tileMigrations().empty());
    TileData data;
    data.skirtDepth = 0.25f;
    const TileMigration steps[] = {{1, [](TileData& tile, std::string*) {
                                        tile.skirtDepth *= 2.0f;
                                        return true;
                                    }},
                                   {2, [](TileData& tile, std::string*) {
                                        tile.flags |= 4u;
                                        return true;
                                    }}};
    std::string error;
    ASSERT_TRUE(migrateTile(data, 1, 3, steps, &error)) << error;
    EXPECT_EQ(data.skirtDepth, 0.5f);
    EXPECT_EQ(data.flags, 4u);
    EXPECT_FALSE(migrateTile(data, 3, 5, steps, &error));
    EXPECT_NE(error.find("3"), std::string::npos);
}

TEST(TerrainTileFormat, PredictorRoundTrips) {
    std::vector<uint8_t> samples(37 * 19 * 2);
    std::mt19937 random(7);
    for (uint8_t& byte : samples) byte = static_cast<uint8_t>(random());
    std::vector<uint8_t> filtered, back;
    predictorEncode16(samples, 37, 19, filtered);
    ASSERT_TRUE(predictorDecode16(filtered, 37, 19, back));
    EXPECT_EQ(back, samples);
}

TEST(TerrainTileFormat, DerivedSections) {
    const WorldLattice lattice = smallLattice(128, 1.0f);
    const auto tile = fixtureTile(lattice, TileCoord(0, 0));
    // the overview holds the exact samples at its stride
    const int step = lattice.overviewStep();
    for (int j = 0; j < lattice.overviewSide(); j++) {
        for (int i = 0; i < lattice.overviewSide(); i++) {
            EXPECT_EQ(tile->overviewHeight[j * lattice.overviewSide() + i], fixtureSample(i * step, j * step));
        }
    }
    // min and max: the whole tile at T, a cell's own samples at 0
    const TileMinMax whole = tileNodeMinMax(*tile, lattice, lattice.tileLevel(), 0, 0);
    uint16_t low = 0xFFFF, high = 0;
    for (int z = 0; z <= 128; z++) {
        for (int x = 0; x <= 128; x++) {
            low = std::min(low, fixtureSample(x, z));
            high = std::max(high, fixtureSample(x, z));
        }
    }
    EXPECT_EQ(whole.min, low);
    EXPECT_EQ(whole.max, high);
    const TileMinMax cell = tileNodeMinMax(*tile, lattice, 0, 3, 1);
    low = 0xFFFF;
    high = 0;
    for (int z = 32; z <= 64; z++) {
        for (int x = 96; x <= 128; x++) {
            low = std::min(low, fixtureSample(x, z));
            high = std::max(high, fixtureSample(x, z));
        }
    }
    EXPECT_EQ(cell.min, low);
    EXPECT_EQ(cell.max, high);
    // a flat tile needs no skirt; a bumpy one does
    TileData flat;
    initTile(flat, lattice, TileCoord(0, 0), 1000);
    EXPECT_EQ(flat.skirtDepth, 0.0f);
    EXPECT_GT(tile->skirtDepth, 0.0f);
    // the macro colour of a tile with no paint is the base colour
    const float colours[] = {0.2f, 0.4f, 0.6f, 1.0f};
    buildOverviewColour(flat, lattice, colours, 0);
    EXPECT_EQ(flat.overviewColour[0], 51);
    EXPECT_EQ(flat.overviewColour[1], 102);
    EXPECT_EQ(flat.overviewColour[2], 153);
}

TEST(TerrainTileFormat, CompressedSizeBudget) {
    // AC30: the fixture's tile at N = 256 with a painted blend map, as the cook writes it
    const WorldLattice lattice = smallLattice(256, 1.0f);
    auto tile = fixtureTile(lattice, TileCoord(1, 2));
    tile->blend.resize(1);
    tile->blend[0].allocate(lattice.blendSide(), 4, 8, 0);
    for (int z = 0; z < lattice.blendSide(); z++) {
        for (int x = 0; x < lattice.blendSide(); x++) {
            uint8_t* texel = tile->blend[0].texel(x, z);
            const uint16_t height = fixtureSample(x * 2, z * 2);
            texel[0] = static_cast<uint8_t>(height >> 8);
            texel[1] = static_cast<uint8_t>(255 - texel[0]);
            texel[2] = 0;
            texel[3] = 255;
        }
    }
    const float colours[] = {0.5f, 0.5f, 0.5f, 1.0f, 0.3f, 0.6f, 0.2f, 1.0f, 0.6f, 0.5f, 0.4f, 1.0f};
    buildOverviewColour(*tile, lattice, colours, 2);
    TileEncodeOptions cook;
    cook.level = 19;
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(encodeTile(*tile, zstdCodec(), cook, bytes));
    std::printf("[bench] terrain_tile_compressed_bytes N=256: %zu\n", bytes.size());
    EXPECT_LE(bytes.size(), 120u * 1024u);
}
