// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TILEMAP_TILE_TYPES_H
#define TILEMAP_TILE_TYPES_H

// The grid tilemap's shared vocabulary (docs/specs/S50-grid-tilemap.md D3, D5): cell and chunk
// coordinates, the cell value's bits and the layout enums. Cells are addressed by signed 32-bit
// (x, y) with y up: row 0 is the bottom row, negative coordinates included.

#include <cstddef>
#include <cstdint>
#include <functional>

namespace jasperengine{

    // TileGridLayout, not GridLayout: nav::GridLayout (S19's NavGrid) already exists
    // (int-sized: the editor's enum fields read and write their values as int)
    enum class TileGridLayout { Rectangle, Isometric, IsometricStaggered, Hexagon };
    // the Grid's plane: XY (2D, default: local (u, v) is (x, y)) or XZ (3D boards: local (u, v) is
    // (x, z), as S19's NavGrid lays its cells along local X and Z)
    enum class TileGridPlane { XY, XZ };
    enum class StaggerAxis { X, Y };
    enum class StaggerIndex { Odd, Even };
    enum class TilemapRenderMode { Chunk, Individual };
    // a chunk's GPU buffer: built when first visible and released after 2 s unseen (Visible), or
    // kept from its first build on (Always)
    enum class TilemapGpuResidency { Visible, Always };

    struct CellCoord{
        int32_t x = 0;
        int32_t y = 0;
        bool operator==(const CellCoord& o) const { return x == o.x && y == o.y; }
        bool operator!=(const CellCoord& o) const { return !(*this == o); }
        // row-major with y first: (y, x)
        bool operator<(const CellCoord& o) const { return y != o.y ? y < o.y : x < o.x; }
    };

    struct ChunkCoord{
        int32_t x = 0;
        int32_t y = 0;
        bool operator==(const ChunkCoord& o) const { return x == o.x && y == o.y; }
        bool operator!=(const ChunkCoord& o) const { return !(*this == o); }
        // the save order (R86): (cy, cx)
        bool operator<(const ChunkCoord& o) const { return y != o.y ? y < o.y : x < o.x; }
    };

    struct CellCoordHash{
        size_t operator()(const CellCoord& c) const {
            return std::hash<uint64_t>()(((uint64_t)(uint32_t)c.x << 32) | (uint32_t)c.y);
        }
    };
    struct ChunkCoordHash{
        size_t operator()(const ChunkCoord& c) const {
            return std::hash<uint64_t>()(((uint64_t)(uint32_t)c.x << 32) | (uint32_t)c.y);
        }
    };

    // an inclusive rectangle of cells; empty while max < min
    struct CellRect{
        int32_t minX = 0;
        int32_t minY = 0;
        int32_t maxX = -1;
        int32_t maxY = -1;
        bool isEmpty() const { return maxX < minX || maxY < minY; }
        int64_t width() const { return isEmpty() ? 0 : (int64_t)maxX - minX + 1; }
        int64_t height() const { return isEmpty() ? 0 : (int64_t)maxY - minY + 1; }
        int64_t area() const { return width() * height(); }
        bool contains(CellCoord c) const { return c.x >= minX && c.x <= maxX && c.y >= minY && c.y <= maxY; }
        void merge(CellCoord c){
            if (isEmpty()){ minX = maxX = c.x; minY = maxY = c.y; return; }
            if (c.x < minX) minX = c.x;
            if (c.x > maxX) maxX = c.x;
            if (c.y < minY) minY = c.y;
            if (c.y > maxY) maxY = c.y;
        }
        static CellRect of(CellCoord a, CellCoord b){
            CellRect r;
            r.minX = a.x < b.x ? a.x : b.x; r.maxX = a.x < b.x ? b.x : a.x;
            r.minY = a.y < b.y ? a.y : b.y; r.maxY = a.y < b.y ? b.y : a.y;
            return r;
        }
        bool operator==(const CellRect& o) const {
            return (isEmpty() && o.isEmpty()) || (minX == o.minX && minY == o.minY && maxX == o.maxX && maxY == o.maxY);
        }
    };

    // ---- the cell value (D3): bits 0-23 tile id + 1 (0 = empty), 24 flip X, 25 flip Y, 26 transpose
    // (Tiled's H, V, D: transpose applies first), 27 lock (manual tile over terrain, R26), 28-31
    // reserved (kept as loaded, warned about)
    constexpr uint32_t TILE_ID_MASK = 0x00FFFFFFu;
    constexpr uint32_t TILE_FLIP_X = 1u << 24;
    constexpr uint32_t TILE_FLIP_Y = 1u << 25;
    constexpr uint32_t TILE_TRANSPOSE = 1u << 26;
    constexpr uint32_t TILE_LOCK = 1u << 27;
    constexpr uint32_t TILE_TRANSFORM_MASK = TILE_FLIP_X | TILE_FLIP_Y | TILE_TRANSPOSE;
    constexpr uint32_t TILE_RESERVED_MASK = 0xF0000000u;
    constexpr int32_t NO_TILE = -1;
    // the largest tile id a cell stores
    constexpr int32_t MAX_TILE_ID = (int32_t)TILE_ID_MASK - 1;

    // a cell's value from a tile id (NO_TILE: empty) and its flag bits (24-31)
    constexpr uint32_t makeCellValue(int32_t tile, uint32_t flags){
        return tile < 0 ? 0u : ((((uint32_t)tile + 1u) & TILE_ID_MASK) | (flags & ~TILE_ID_MASK));
    }
    constexpr int32_t cellValueTile(uint32_t value){
        return (int32_t)(value & TILE_ID_MASK) - 1;
    }
    constexpr uint32_t cellValueFlags(uint32_t value){
        return value & ~TILE_ID_MASK;
    }
    constexpr bool cellValueEmpty(uint32_t value){
        return (value & TILE_ID_MASK) == 0;
    }

    // ---- the terrain plane (D3): 0 none, 1-254 the tileset's terrain index + 1, 255 reserved
    constexpr uint8_t TERRAIN_NONE_VALUE = 0;
    constexpr int MAX_TILESET_TERRAINS = 254;
    constexpr int NO_TERRAIN = -1;

    // flood fills and region reads stop at this many cells (R21, R79 region_too_large)
    constexpr int64_t MAX_TILE_REGION_CELLS = 1000000;

}

#endif // TILEMAP_TILE_TYPES_H
