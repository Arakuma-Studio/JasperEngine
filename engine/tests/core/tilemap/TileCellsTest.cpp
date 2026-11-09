// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// AC1 (docs/specs/S50-grid-tilemap.md R4-R6, D3, D4, R86): the sparse cell store and its encoding.
// The byte-level expectations below are written out from D4's format, not produced by the encoder.

#include "tilemap/TileCells.h"

#include "util/Base64.h"

#include <gtest/gtest.h>

#include <random>
#include <string>

using namespace jasperengine;

namespace {

    std::string cellsRawBase64(const std::string& bytes){
        return Base64::encode(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    }

    std::string cellsBytes(std::initializer_list<int> values){
        std::string out;
        for (int v : values)
            out.push_back((char)(unsigned char)v);
        return out;
    }

}

TEST(TileCells, SetsGetsAndErasesAtFarCoordinates){
    TileCells cells;
    const int32_t far = 1 << 30;
    for (CellCoord c : {CellCoord{far, far}, CellCoord{-far, far}, CellCoord{far, -far}, CellCoord{-far, -far}, CellCoord{INT32_MAX, INT32_MIN}}){
        cells.setTile(c, 7, TILE_FLIP_X);
        uint32_t flags = 0;
        EXPECT_EQ(cells.getTile(c, &flags), 7);
        EXPECT_EQ(flags, TILE_FLIP_X);
        cells.setTile(c, NO_TILE);
        EXPECT_EQ(cells.get(c), 0u);
    }
    EXPECT_TRUE(cells.empty());
}

TEST(TileCells, ChunkOfFloorsNegativeCoordinates){
    EXPECT_EQ(TileCells::chunkOf(CellCoord{-1, -32}, 32), (ChunkCoord{-1, -1}));
    EXPECT_EQ(TileCells::chunkOf(CellCoord{-33, 31}, 32), (ChunkCoord{-2, 0}));
    EXPECT_EQ(TileCells::chunkOf(CellCoord{32, 0}, 32), (ChunkCoord{1, 0}));
    TileCells cells;
    EXPECT_EQ(cells.chunkOrigin(ChunkCoord{-2, 1}), (CellCoord{-64, 32}));
    const CellRect r = cells.chunkRect(ChunkCoord{-1, 0});
    EXPECT_EQ(r.minX, -32);
    EXPECT_EQ(r.maxX, -1);
    EXPECT_EQ(r.minY, 0);
    EXPECT_EQ(r.maxY, 31);
}

TEST(TileCells, AValueWithNoTileIsStoredAsZero){
    TileCells cells;
    cells.set(CellCoord{1, 1}, TILE_FLIP_X | TILE_LOCK);
    EXPECT_EQ(cells.get(CellCoord{1, 1}), 0u);
    EXPECT_TRUE(cells.empty());
}

TEST(TileCells, EmptyChunksAreDroppedAndReported){
    TileCells cells;
    cells.setTile(CellCoord{40, 40}, 3);
    ASSERT_EQ(cells.chunkCount(), 1u);
    EXPECT_EQ(cells.takeChanged(), std::vector<ChunkCoord>{(ChunkCoord{1, 1})});
    cells.setTile(CellCoord{40, 40}, NO_TILE);
    EXPECT_EQ(cells.chunkCount(), 0u);
    EXPECT_EQ(cells.chunk(ChunkCoord{1, 1}), nullptr);
    EXPECT_EQ(cells.takeChanged(), std::vector<ChunkCoord>{(ChunkCoord{1, 1})});
}

TEST(TileCells, ChangedChunksAreReportedOnceAndSorted){
    TileCells cells;
    cells.setTile(CellCoord{0, 40}, 1);
    cells.setTile(CellCoord{40, 0}, 2);
    cells.setTile(CellCoord{41, 0}, 2);
    cells.setTile(CellCoord{0, 40}, NO_TILE);
    cells.setTile(CellCoord{0, 40}, 5);
    const std::vector<ChunkCoord> changed = cells.takeChanged();
    ASSERT_EQ(changed.size(), 2u);
    EXPECT_EQ(changed[0], (ChunkCoord{1, 0}));
    EXPECT_EQ(changed[1], (ChunkCoord{0, 1}));
    EXPECT_FALSE(cells.hasChanged());
}

TEST(TileCells, RevisionMovesOnlyWhenAValueChanges){
    TileCells cells;
    cells.setTile(CellCoord{0, 0}, 1);
    const uint64_t revision = cells.getRevision();
    cells.setTile(CellCoord{0, 0}, 1);
    EXPECT_EQ(cells.getRevision(), revision);
    cells.setTile(CellCoord{0, 0}, 2);
    EXPECT_GT(cells.getRevision(), revision);
}

TEST(TileCells, TerrainPlaneExistsOnlyWhilePainted){
    TileCells cells;
    cells.setTile(CellCoord{2, 2}, 1);
    EXPECT_FALSE(cells.hasTerrain());
    cells.setTerrain(CellCoord{2, 2}, 4);
    EXPECT_TRUE(cells.hasTerrain());
    EXPECT_EQ(cells.getTerrain(CellCoord{2, 2}), 4);
    EXPECT_EQ(cells.getTerrainValue(CellCoord{2, 2}), 5);
    cells.setTerrain(CellCoord{2, 2}, NO_TERRAIN);
    EXPECT_FALSE(cells.hasTerrain());
    EXPECT_TRUE(cells.chunk(ChunkCoord{0, 0})->terrain.empty());
    EXPECT_EQ(cells.getTerrain(CellCoord{2, 2}), NO_TERRAIN);
}

TEST(TileCells, ATerrainOnlyChunkIsKept){
    TileCells cells;
    cells.setTerrain(CellCoord{5, 5}, 0);
    EXPECT_EQ(cells.chunkCount(), 1u);
    EXPECT_EQ(cells.cellCount(), 0u);
    EXPECT_TRUE(cells.usedRect().isEmpty());
    const CellRect withTerrain = cells.usedRectWithTerrain();
    EXPECT_EQ(withTerrain.minX, 5);
    EXPECT_EQ(withTerrain.maxY, 5);
    cells.setTerrain(CellCoord{5, 5}, NO_TERRAIN);
    EXPECT_EQ(cells.chunkCount(), 0u);
}

TEST(TileCells, UsedRectCoversNegativeCells){
    TileCells cells;
    cells.setTile(CellCoord{-70, 5}, 1);
    cells.setTile(CellCoord{12, -9}, 1);
    const CellRect r = cells.usedRect();
    EXPECT_EQ(r.minX, -70);
    EXPECT_EQ(r.maxX, 12);
    EXPECT_EQ(r.minY, -9);
    EXPECT_EQ(r.maxY, 5);
    EXPECT_EQ(cells.cellCount(), 2u);
}

TEST(TileCells, SetChunkSizeRebucketsEveryCell){
    TileCells cells(32);
    cells.setTile(CellCoord{-1, -1}, 1);
    cells.setTile(CellCoord{33, 2}, 2, TILE_FLIP_Y);
    cells.setTerrain(CellCoord{33, 2}, 3);
    cells.takeChanged();
    cells.setChunkSize(8);
    EXPECT_EQ(cells.getChunkSize(), 8);
    EXPECT_EQ(cells.getTile(CellCoord{-1, -1}), 1);
    EXPECT_EQ(cells.get(CellCoord{33, 2}), makeCellValue(2, TILE_FLIP_Y));
    EXPECT_EQ(cells.getTerrain(CellCoord{33, 2}), 3);
    EXPECT_NE(cells.chunk(ChunkCoord{4, 0}), nullptr);
    EXPECT_EQ(cells.cellCount(), 2u);
    // the coordinates of both sizes come back: stale builds of the old chunks must go too
    const std::vector<ChunkCoord> changed = cells.takeChanged();
    EXPECT_NE(std::find(changed.begin(), changed.end(), (ChunkCoord{4, 0})), changed.end());
    EXPECT_NE(std::find(changed.begin(), changed.end(), (ChunkCoord{1, 0})), changed.end());
}

TEST(TileCells, InvalidChunkSizesAreIgnored){
    TileCells cells(16);
    cells.setChunkSize(10);
    EXPECT_EQ(cells.getChunkSize(), 16);
    TileCells fallback(7);
    EXPECT_EQ(fallback.getChunkSize(), TileCells::DEFAULT_CHUNK_SIZE);
}

TEST(TileCells, EncodesExactlyTheDocumentedBytes){
    TileCells cells(8);
    cells.setTile(CellCoord{0, 0}, 0);
    cells.setTile(CellCoord{1, 0}, 299);
    // log2(8) = 3; [1][id 0 + 1]; [1][id 299 + 1 = 0x12C]; [62][empty]
    EXPECT_EQ(cells.encodeChunkBytes(ChunkCoord{0, 0}),
              cellsBytes({0x03, 1, 1, 0, 0, 0, 1, 0x2C, 1, 0, 0, 62, 0, 0, 0, 0}));
}

TEST(TileCells, EncodesTheTerrainPlaneAfterTheCells){
    TileCells cells(8);
    cells.setTerrain(CellCoord{0, 1}, 2);
    // bit 4 = terrain; cells [64][0]; terrain: 8 of row 0 none, value 3, then 55 of none
    EXPECT_EQ(cells.encodeChunkBytes(ChunkCoord{0, 0}),
              cellsBytes({0x13, 64, 0, 0, 0, 0, 8, 0, 1, 3, 55, 0}));
}

TEST(TileCells, ARunLengthAbove127IsAVarint){
    TileCells cells(16);
    cells.setTile(CellCoord{0, 0}, 0);
    // 256 cells: [1][1] and [255][0], 255 = 0xFF 0x01 as a varint
    EXPECT_EQ(cells.encodeChunkBytes(ChunkCoord{0, 0}),
              cellsBytes({0x04, 1, 1, 0, 0, 0, 0xFF, 0x01, 0, 0, 0, 0}));
}

TEST(TileCells, RoundTripsRandomChunks){
    std::mt19937 random(12345);
    for (int round = 0; round < 50; round++){
        TileCells source(32);
        const int writes = 1 + (int)(random() % 600);
        for (int i = 0; i < writes; i++){
            const CellCoord cell{(int32_t)(random() % 32), (int32_t)(random() % 32)};
            source.setTile(cell, (int32_t)(random() % 8), (random() & 7u) << 24);
            if (random() % 3 == 0)
                source.setTerrain(cell, (int)(random() % 4));
        }
        TileCells target(32);
        std::string error;
        ASSERT_TRUE(target.decodeChunk(ChunkCoord{0, 0}, source.encodeChunk(ChunkCoord{0, 0}), error)) << error;
        EXPECT_EQ(source, target);
        EXPECT_EQ(target.encodeChunk(ChunkCoord{0, 0}), source.encodeChunk(ChunkCoord{0, 0}));
    }
}

TEST(TileCells, RoundTripsTheWorstCaseWithinTheBudget){
    TileCells source(32);
    for (int32_t i = 0; i < 32 * 32; i++)
        source.setTile(CellCoord{i % 32, i / 32}, i * 7 + 1);
    const std::string text = source.encodeChunk(ChunkCoord{0, 0});
    // D4: every cell its own run, 5 KB of runs, at most 6.9 KB of base64
    EXPECT_LE(text.size(), 6900u);
    TileCells target(32);
    std::string error;
    ASSERT_TRUE(target.decodeChunk(ChunkCoord{0, 0}, text, error)) << error;
    EXPECT_EQ(source, target);
}

TEST(TileCells, EncodingIsDeterministicForAnyWriteOrder){
    TileCells a(32), b(32);
    a.setTile(CellCoord{1, 1}, 1);
    a.setTile(CellCoord{2, 2}, 2);
    b.setTile(CellCoord{2, 2}, 2);
    b.setTile(CellCoord{1, 1}, 1);
    EXPECT_EQ(a.encodeChunk(ChunkCoord{0, 0}), b.encodeChunk(ChunkCoord{0, 0}));
    EXPECT_EQ(a.sortedChunks(), b.sortedChunks());
}

TEST(TileCells, KeepsUnknownHighBits){
    TileCells source(8);
    const uint32_t value = makeCellValue(9, TILE_FLIP_X) | 0xA0000000u;
    source.set(CellCoord{3, 3}, value);
    TileCells target(8);
    std::string error;
    ASSERT_TRUE(target.decodeChunk(ChunkCoord{0, 0}, source.encodeChunk(ChunkCoord{0, 0}), error)) << error;
    EXPECT_EQ(target.get(CellCoord{3, 3}), value);
    EXPECT_EQ(target.encodeChunk(ChunkCoord{0, 0}), source.encodeChunk(ChunkCoord{0, 0}));
}

TEST(TileCells, DecodingDropsFlagsOfEmptyCells){
    TileCells cells(8);
    std::string error;
    // 64 cells of value 0x01000000: tile id 0 + 1 is absent, only a flip bit
    ASSERT_TRUE(cells.decodeChunkBytes(ChunkCoord{0, 0}, cellsBytes({0x03, 64, 0, 0, 0, 1}), error)) << error;
    EXPECT_TRUE(cells.empty());
}

TEST(TileCells, ADecodedEmptyChunkReplacesAndDropsTheOldOne){
    TileCells cells(8);
    cells.setTile(CellCoord{0, 0}, 3);
    cells.takeChanged();
    std::string error;
    ASSERT_TRUE(cells.decodeChunk(ChunkCoord{0, 0}, cellsRawBase64(cellsBytes({0x03, 64, 0, 0, 0, 0})), error)) << error;
    EXPECT_TRUE(cells.empty());
    EXPECT_EQ(cells.takeChanged(), std::vector<ChunkCoord>{(ChunkCoord{0, 0})});
}

TEST(TileCells, AMissingChunkEncodesAsAValidEmptyOne){
    TileCells cells(8);
    TileCells target(8);
    std::string error;
    ASSERT_TRUE(target.decodeChunk(ChunkCoord{3, 3}, cells.encodeChunk(ChunkCoord{3, 3}), error)) << error;
    EXPECT_TRUE(target.empty());
}

TEST(TileCells, AnEmptyStoreTakesTheDataChunkSize){
    TileCells source(64);
    source.setTile(CellCoord{1, 1}, 1);
    TileCells target(32);
    std::string error;
    ASSERT_TRUE(target.decodeChunk(ChunkCoord{0, 0}, source.encodeChunk(ChunkCoord{0, 0}), error)) << error;
    EXPECT_EQ(target.getChunkSize(), 64);
    EXPECT_EQ(target.getTile(CellCoord{1, 1}), 1);
}

TEST(TileCells, RefusesAChunkOfAnotherSizeOnAFilledStore){
    TileCells source(64);
    source.setTile(CellCoord{1, 1}, 1);
    TileCells target(32);
    target.setTile(CellCoord{0, 0}, 5);
    const TileCells before = target;
    std::string error;
    EXPECT_FALSE(target.decodeChunk(ChunkCoord{0, 0}, source.encodeChunk(ChunkCoord{0, 0}), error));
    EXPECT_NE(error.find("chunk size"), std::string::npos);
    EXPECT_EQ(target, before);
}

TEST(TileCells, MalformedDataIsRefusedAndLeavesTheStoreUnchanged){
    struct Case{ const char* name; std::string bytes; };
    const std::vector<Case> cases = {
        {"no header", cellsBytes({})},
        {"reserved header bits", cellsBytes({0x23, 64, 0, 0, 0, 0})},
        {"bad chunk size", cellsBytes({0x02, 64, 0, 0, 0, 0})},
        {"huge chunk size", cellsBytes({0x0F, 64, 0, 0, 0, 0})},
        {"run overflows the chunk", cellsBytes({0x03, 65, 0, 0, 0, 0})},
        {"second run overflows", cellsBytes({0x03, 60, 0, 0, 0, 0, 5, 1, 0, 0, 0})},
        {"zero-length run", cellsBytes({0x03, 0, 0, 0, 0, 0})},
        {"truncated value", cellsBytes({0x03, 64, 0, 0})},
        {"missing runs", cellsBytes({0x03, 10, 1, 0, 0, 0})},
        {"varint never ends", cellsBytes({0x03, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80})},
        {"varint overflows u32", cellsBytes({0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0, 0, 0, 0})},
        {"trailing bytes", cellsBytes({0x03, 64, 0, 0, 0, 0, 9})},
        {"terrain plane cut off", cellsBytes({0x13, 64, 0, 0, 0, 0, 10, 1})},
        {"terrain run overflows", cellsBytes({0x13, 64, 0, 0, 0, 0, 65, 1})},
    };
    TileCells cells(8);
    cells.setTile(CellCoord{2, 2}, 4);
    const TileCells before = cells;
    for (const Case& c : cases){
        std::string error;
        EXPECT_FALSE(cells.decodeChunkBytes(ChunkCoord{0, 0}, c.bytes, error)) << c.name;
        EXPECT_FALSE(error.empty()) << c.name;
        EXPECT_EQ(cells, before) << c.name;
    }
}

TEST(TileCells, RandomBytesNeverBreakTheStore){
    std::mt19937 random(777);
    for (int round = 0; round < 2000; round++){
        std::string bytes(1 + random() % 300, '\0');
        for (char& c : bytes)
            c = (char)(unsigned char)(random() & 0xFF);
        // a plausible header half the time, so the run parser is reached
        if (round % 2 == 0)
            bytes[0] = (char)(unsigned char)(3 + random() % 4 + (random() % 2 ? 0x10 : 0));
        TileCells cells(8);
        cells.setTile(CellCoord{1, 1}, 2);
        const TileCells before = cells;
        std::string error;
        if (!cells.decodeChunkBytes(ChunkCoord{0, 0}, bytes, error)){
            EXPECT_FALSE(error.empty());
            EXPECT_EQ(cells, before);
        }
    }
}

TEST(TileCells, MalformedBase64IsRefused){
    TileCells cells(8);
    std::string error;
    EXPECT_FALSE(cells.decodeChunk(ChunkCoord{0, 0}, "", error));
    EXPECT_FALSE(cells.decodeChunk(ChunkCoord{0, 0}, "abc", error));
    EXPECT_FALSE(cells.decodeChunk(ChunkCoord{0, 0}, "ab$d", error));
    EXPECT_NE(error.find("invalid character"), std::string::npos);
    // a damaged tail must not pass as a shorter valid chunk
    std::string text = cellsRawBase64(cellsBytes({0x03, 64, 0, 0, 0, 0}));
    text[2] = '*';
    EXPECT_FALSE(cells.decodeChunk(ChunkCoord{0, 0}, text, error));
}

TEST(TileCellsRef, CopiesShareUntilOneWrites){
    TileCellsRef a;
    EXPECT_FALSE(a.isSet());
    EXPECT_TRUE(a.read().empty());
    a.edit().setTile(CellCoord{1, 1}, 1);
    TileCellsRef b = a;
    EXPECT_EQ(a.shared().get(), b.shared().get());
    b.edit().setTile(CellCoord{1, 1}, 2);
    EXPECT_NE(a.shared().get(), b.shared().get());
    EXPECT_EQ(a.read().getTile(CellCoord{1, 1}), 1);
    EXPECT_EQ(b.read().getTile(CellCoord{1, 1}), 2);
    EXPECT_NE(a, b);
}

TEST(TileCellsRef, AnUnsetRefEqualsAnEmptyStore){
    TileCellsRef unset;
    TileCellsRef empty;
    empty.edit();
    EXPECT_EQ(unset, empty);
    empty.edit().setTile(CellCoord{0, 0}, 1);
    EXPECT_NE(unset, empty);
}
