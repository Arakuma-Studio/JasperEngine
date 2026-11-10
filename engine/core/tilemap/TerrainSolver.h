// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TILEMAP_TERRAIN_SOLVER_H
#define TILEMAP_TERRAIN_SOLVER_H

// Terrain autotiling (docs/specs/S50-grid-tilemap.md R23-R28, D6). The terrain plane is the truth
// and the tiles are derived from it: painting writes the plane, then re-chooses the tiles of the
// display cells whose mask changed (R25: a cell and its 8 neighbours for blob and isometric, 4 + 1
// for sides, the 4 dual cells around a logical cell, 6 + 1 on hexagons), all recorded in one patch.
//
// Mask: per peering slot of the set's mode (Tileset.h), the terrain the slot sees, read from the
// plane only (never from neighbour tiles, so nothing propagates further): blob reads the 8
// neighbours, then clears a corner unless both sides next to it hold the cell's own terrain
// (Pixelorama's corner filter); sides the 4 (6 on hexagons) sides; dual grid the 4 logical cells
// TL = (x - 1, y), TR = (x, y), BL = (x - 1, y - 1), BR = (x, y - 1) of display tile (x, y) (R27);
// hexagon blob also 6 corners, a corner seeing the terrain both sides beside it share (else none).
// A neighbour's terrain from another set reads as none.
//
// Best match (R24, Pixelorama CelTileMap.gd:437-476): candidates are the set's tiles whose centre
// is the cell's terrain (dual grid: every tile of the set); score = +10 per constrained peering
// value that matches, -2 per one that does not, + the number of constrained values; the highest
// wins, ties by probability with tileset::cellHash(seed, 0, x, y). allowTransforms (R20) also
// tries the flipped and transposed variants of rectangle tiles. No candidate: the cell keeps its
// terrain, shows no tile, and is reported (no_tile_for_terrain). Locked cells (TILE_LOCK, R26) are
// never re-chosen.

#include "Export.h"
#include "tilemap/TileCells.h"
#include "tilemap/TileGridLayout.h"
#include "tilemap/Tileset.h"

#include <array>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine{

    // one changed cell: what undo restores and redo reapplies (TilemapCellsCmd, D10)
    struct TileCellChange{
        CellCoord cell;
        uint32_t oldValue = 0;
        uint32_t newValue = 0;
        uint8_t oldTerrain = 0;
        uint8_t newTerrain = 0;
    };

    // Records writes to a store: the first old value and the last new value of each cell, in the
    // order cells were first written. Writes go to the store at once (a live stroke).
    class JASPERENGINE_API TileCellPatch{
    public:
        void setValue(TileCells& cells, CellCoord cell, uint32_t value);
        void setTerrainValue(TileCells& cells, CellCoord cell, uint8_t terrain);
        // the cells whose value or terrain really changed
        std::vector<TileCellChange> changes() const;
        bool empty() const { return order.empty(); }
        size_t size() const { return order.size(); }
        // puts every recorded cell back to its first old value
        void revert(TileCells& cells) const;
        void clear();
        // whether this patch already wrote the cell (and its first old values)
        bool contains(CellCoord cell) const;

    private:
        struct Entry{ uint32_t oldValue; uint32_t newValue; uint8_t oldTerrain; uint8_t newTerrain; };
        std::unordered_map<CellCoord, Entry, CellCoordHash> entries;
        std::vector<CellCoord> order;
        Entry& entry(const TileCells& cells, CellCoord cell);
    };

    struct TerrainReport{
        // cells left without a tile because no tile matches their terrain
        std::vector<CellCoord> noTile;
        // display cells whose tile was re-chosen (R25's count)
        size_t rechosen = 0;
    };

    struct TerrainContext{
        const TilesetData* tileset = nullptr;
        TileGridSpec grid;
        bool dualGrid = false;
        uint32_t seed = 0;               // the layer's seed
        bool allowTransforms = false;
    };

    namespace terrain{

        // sets the terrain of `cells` (logical cells on a dual-grid layer) to `terrainIndex`
        // (tileset-global; NO_TERRAIN erases) and re-chooses the affected tiles. Painting a set of
        // another mode than the layer accepts (R27: a dual-grid layer takes dual_grid sets only,
        // and dual grid only on rectangle and isometric Grids) returns false with `error`.
        JASPERENGINE_API bool paint(TileCells& store, const TerrainContext& context, std::span<const CellCoord> cells, int terrainIndex,
                                    TileCellPatch& patch, TerrainReport& report, std::string& error);
        // re-chooses every terrain cell of the rectangle (display cells on a dual-grid layer)
        JASPERENGINE_API void refresh(TileCells& store, const TerrainContext& context, const CellRect& rect, TileCellPatch& patch,
                                      TerrainReport& report);
        // the tile value (id + transform bits) best matching a display cell's mask; 0 when none
        JASPERENGINE_API uint32_t chooseTile(const TileCells& store, const TerrainContext& context, CellCoord displayCell, bool& matched);
        // the score of one tile against a mask (exposed for the tests: R24's rules)
        JASPERENGINE_API int score(const TileTerrain& tile, std::span<const int16_t> mask, std::span<const int> slots);
        // the mask a display cell sees (MAX_PEERING_SLOTS values: set-local terrain or PEERING_NONE)
        JASPERENGINE_API void mask(const TileCells& store, const TerrainContext& context, int set, CellCoord displayCell,
                                   std::array<int16_t, MAX_PEERING_SLOTS>& out);
        // the peering values of a square tile as drawn with these transform bits (R20: slots 0-7;
        // the transpose applies first, then the flips)
        JASPERENGINE_API std::array<int16_t, MAX_PEERING_SLOTS> transformPeering(const std::array<int16_t, MAX_PEERING_SLOTS>& peering, uint32_t flags);
    }

}

#endif // TILEMAP_TERRAIN_SOLVER_H
