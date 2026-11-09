// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TILEMAP_TILE_CELLS_H
#define TILEMAP_TILE_CELLS_H

// The cell store of a Tilemap Layer (docs/specs/S50-grid-tilemap.md R4-R6, D3, D4, R86, R59):
// sparse square chunks of chunkSize x chunkSize cells, unbounded, negative coordinates included.
// A chunk holds its cells (u32, D3), an optional terrain plane (u8, only once a terrain was painted
// in that chunk: R6) and its non-empty count; a chunk with no tile and no terrain is dropped.
//
// Encoding of one chunk (D4, R86): one header byte (bits 0-3 log2(chunkSize), bit 4 a terrain plane
// follows, bits 5-7 zero), then runs [varint count][u32 little-endian value] over the cells in row
// order (row 0, the bottom row, first; x ascending), then, with the terrain bit, runs [varint
// count][u8 value] over the terrain plane the same way; all base64 (Base64). Deterministic: the
// same cells give the same bytes. Values keep unknown high bits (28-31) as they are.
//
// Changes: every write that changes a value bumps the chunk's and the store's revision and records
// the chunk in the changed list (takeChanged), so the runtime rebuilds only those chunks (R33, R41).
// A dropped chunk is reported there too (chunk() then returns null).
//
// Not thread-safe for writes; concurrent reads (chunk builds on jobs) are safe while nothing writes.

#include "Export.h"
#include "tilemap/TileTypes.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace jasperengine{

    struct JASPERENGINE_API TileChunk{
        std::vector<uint32_t> cells;      // chunkSize^2, row-major from the bottom row
        std::vector<uint8_t> terrain;     // empty, or chunkSize^2
        uint32_t nonEmpty = 0;            // cells with a tile
        uint32_t terrainCount = 0;        // terrain values other than none
        uint64_t revision = 0;
        bool inChanged = false;           // already in the store's changed list
    };

    class JASPERENGINE_API TileCells{
    public:
        static constexpr int DEFAULT_CHUNK_SIZE = 32;

        explicit TileCells(int chunkSize = DEFAULT_CHUNK_SIZE);

        // 8, 16, 32 or 64; changing it moves every cell into chunks of the new size
        int getChunkSize() const { return chunkSize; }
        void setChunkSize(int size);
        static bool isValidChunkSize(int size);

        static ChunkCoord chunkOf(CellCoord cell, int chunkSize);
        ChunkCoord chunkOf(CellCoord cell) const { return chunkOf(cell, chunkSize); }
        // the chunk's first (bottom-left) cell
        CellCoord chunkOrigin(ChunkCoord chunk) const;
        CellRect chunkRect(ChunkCoord chunk) const;

        // the raw value (D3) of a cell; 0 when empty
        uint32_t get(CellCoord cell) const;
        // writes the raw value; returns the old one
        uint32_t set(CellCoord cell, uint32_t value);
        int32_t getTile(CellCoord cell, uint32_t* flags = nullptr) const;
        void setTile(CellCoord cell, int32_t tile, uint32_t flags = 0);
        // the terrain plane: 0 none, else terrain index + 1 (D3)
        uint8_t getTerrainValue(CellCoord cell) const;
        uint8_t setTerrainValue(CellCoord cell, uint8_t value);
        // the terrain index, NO_TERRAIN when none
        int getTerrain(CellCoord cell) const;
        void setTerrain(CellCoord cell, int terrain);
        // a chunk of this store has a terrain plane
        bool hasTerrain() const;

        void clear();
        bool empty() const { return chunkMap.empty(); }
        // the rectangle of the non-empty cells (tiles; terrain-only cells excluded)
        CellRect usedRect() const;
        // the rectangle of the cells with a tile or a terrain
        CellRect usedRectWithTerrain() const;
        // non-empty cells (with a tile)
        size_t cellCount() const;
        size_t chunkCount() const { return chunkMap.size(); }
        size_t memoryBytes() const;

        const TileChunk* chunk(ChunkCoord coord) const;
        const std::unordered_map<ChunkCoord, TileChunk, ChunkCoordHash>& chunks() const { return chunkMap; }
        // the chunk coordinates in the save order (cy, cx)
        std::vector<ChunkCoord> sortedChunks() const;

        // f(CellCoord, uint32_t value, uint8_t terrainValue) for every cell of the rectangle, empty
        // ones included, rows from the bottom, x ascending
        template<class F> void forEachCell(const CellRect& rect, F&& f) const;
        // f(CellCoord, uint32_t value, uint8_t terrainValue) for every cell with a tile or a
        // terrain, chunk by chunk in the save order
        template<class F> void forEachUsed(F&& f) const;

        std::string encodeChunk(ChunkCoord coord) const;
        // replaces the chunk with the decoded cells (an all-empty chunk is dropped); false with a
        // reason on a malformed string (the store is then unchanged). The chunk size of the data
        // must equal the store's, unless the store is empty (it then takes the data's size)
        bool decodeChunk(ChunkCoord coord, std::string_view base64, std::string& error);
        // the raw (not base64) bytes, for cooked scenes (R87)
        std::string encodeChunkBytes(ChunkCoord coord) const;
        bool decodeChunkBytes(ChunkCoord coord, std::string_view bytes, std::string& error);

        uint64_t getRevision() const { return revision; }
        // the chunks changed (or dropped) since the last call, each once
        std::vector<ChunkCoord> takeChanged();
        bool hasChanged() const { return !changedList.empty(); }
        // marks every chunk changed (a consumer that lost its state)
        void markAllChanged();

        bool operator==(const TileCells& other) const;
        bool operator!=(const TileCells& other) const { return !(*this == other); }

    private:
        int chunkSize = DEFAULT_CHUNK_SIZE;
        int chunkShift = 5;
        std::unordered_map<ChunkCoord, TileChunk, ChunkCoordHash> chunkMap;
        std::vector<ChunkCoord> changedList;
        uint64_t revision = 0;

        TileChunk* editChunk(ChunkCoord coord, bool create);
        void touched(ChunkCoord coord, TileChunk* chunk);
        void dropIfEmpty(ChunkCoord coord);
    };

    // The component's value: a store shared by copies (undo snapshots, Play-mode copies, the
    // clipboard) until one of them writes (copy on write). read() never allocates; edit() makes
    // the store, or a private copy of a shared one.
    class JASPERENGINE_API TileCellsRef{
    public:
        TileCellsRef() = default;
        explicit TileCellsRef(std::shared_ptr<TileCells> cells): store(std::move(cells)) {}

        const TileCells& read() const;
        TileCells& edit();
        bool isSet() const { return (bool)store; }
        const std::shared_ptr<TileCells>& shared() const { return store; }

        // equal contents (an unset ref equals an empty store)
        bool operator==(const TileCellsRef& other) const;
        bool operator!=(const TileCellsRef& other) const { return !(*this == other); }

    private:
        std::shared_ptr<TileCells> store;
    };

    template<class F>
    void TileCells::forEachCell(const CellRect& rect, F&& f) const{
        if (rect.isEmpty())
            return;
        for (int64_t y = rect.minY; y <= rect.maxY; y++){
            for (int64_t x = rect.minX; x <= rect.maxX; x++){
                const CellCoord c{(int32_t)x, (int32_t)y};
                f(c, get(c), getTerrainValue(c));
            }
        }
    }

    template<class F>
    void TileCells::forEachUsed(F&& f) const{
        const int n = chunkSize;
        for (const ChunkCoord& coord : sortedChunks()){
            const TileChunk& ch = chunkMap.at(coord);
            const CellCoord origin = chunkOrigin(coord);
            for (int i = 0; i < n * n; i++){
                const uint32_t value = ch.cells[(size_t)i];
                const uint8_t terrain = ch.terrain.empty() ? 0 : ch.terrain[(size_t)i];
                if (value == 0 && terrain == 0)
                    continue;
                f(CellCoord{origin.x + i % n, origin.y + i / n}, value, terrain);
            }
        }
    }

}

#endif // TILEMAP_TILE_CELLS_H
