// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The brush operations of a Tilemap Layer (docs/specs/S50-grid-tilemap.md R21, R22, R26-R28): the
// one implementation the Scene view tools and the MCP `edit_tilemap` tool share, so the same
// operation gives the same cells whoever runs it (R28). Pure over the cell store: no Project, no
// UI. An operation writes through a BrushEdit, which holds the TileCellPatch a stroke accumulates
// (first old value, last new value of each cell): apply TilemapCellsCmd with patch.changes() when
// the stroke ends, or patch.revert() to cancel it.
//
// Every operation either applies or, returning false with edit.error set, writes nothing, so a
// failing op leaves the store as it was. Errors are R79's typed kinds (PixelProblem): unknown_tile,
// unknown_terrain, layout_mismatch, tileset_missing, region_too_large, locked_linked.
//
// Coordinates: cells (x, y up, negatives included). A stamp's offsets are relative to its anchor.
// Dual-grid layers (R27): terrain operations take logical cells; manual tiles are stored on the
// display cells, so a stamp, erase, select or move on a dual-grid layer addresses display cells.
//
// Manual tiles over terrain (R26): painting a tile onto a cell that has a terrain clears that
// cell's terrain (the neighbours are re-chosen) unless PaintOptions::keepTerrain, which keeps the
// terrain and sets the cell's lock bit so refreshes leave it alone. On a dual-grid layer a display
// cell has no terrain of its own: the terrain under it is left as it is, and keepTerrain locks a
// display cell that has terrain on a corner. Erasing a tile clears the lock and keeps the terrain.

#pragma once

#include "pixelart/PixelProblem.h"
#include "tilemap/TerrainSolver.h"

#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine::editor::tilemap {

    using pixelart::PixelProblem;

    // The layer a brush edits. `tileset` may be null for the operations that do not place tiles
    // (erase, pick, select, delete, move); painting needs it to validate the tiles.
    struct BrushTarget {
        TileCells& cells;
        const TilesetData* tileset = nullptr;
        TileGridSpec grid;
        bool dualGrid = false;
        uint32_t seed = 0;               // the layer's random seed (R22)
    };

    struct BrushCell {
        int32_t x = 0;                   // the offset from the anchor
        int32_t y = 0;
        int32_t tile = 0;
        uint32_t flags = 0;              // TILE_FLIP_X | TILE_FLIP_Y | TILE_TRANSPOSE (other bits ignored)
    };

    // One or more tiles with their transform bits. Width and height are the period box fills, lines
    // and floods repeat it with (0: the cells' bounding box); a stamp copied from a selection keeps
    // the selection's size, so its offsets start at (0, 0) and empty cells stay holes.
    struct BrushStamp {
        std::vector<BrushCell> cells;
        int width = 0;
        int height = 0;

        bool empty() const { return cells.empty(); }
        static BrushStamp single(int32_t tile, uint32_t flags = 0);
    };

    struct PaintOptions {
        bool keepTerrain = false;        // R26: keep the terrain under a manual tile and lock the cell
    };

    enum class FloodScope {
        Contiguous,                      // connected through the layout's sides (4, or 6 on hexagons)
        EveryEqual                       // every equal cell of the used rect
    };

    // one entry of a random brush; a negative weight means the tile's own probability (R22)
    struct RandomEntry {
        int32_t tile = 0;
        float weight = -1.0f;
        uint32_t flags = 0;
    };

    // What one stroke (or one MCP call) accumulates across operations.
    struct BrushEdit {
        TileCellPatch patch;
        TerrainReport terrain;           // accumulated across terrain ops (no_tile_for_terrain cells)
        PixelProblem error;              // set when an op returns false; cleared at each op's start
    };

    // the locked_linked error of a layer whose Grid is linked to an imported map (R30): the callers
    // refuse with it before editing
    PixelProblem lockedLinkedError();

    // ---- tiles
    bool paintStamp(const BrushTarget& target, const BrushStamp& stamp, CellCoord anchor, const PaintOptions& options, BrushEdit& edit);
    // the stamp repeats from the rect's bottom-left cell
    bool boxFill(const BrushTarget& target, const BrushStamp& stamp, const CellRect& rect, const PaintOptions& options, BrushEdit& edit);
    // the cells of tilegrid::line, the stamp repeating from `from`
    bool paintLine(const BrushTarget& target, const BrushStamp& stamp, CellCoord from, CellCoord to, const PaintOptions& options, BrushEdit& edit);
    // replaces the cells equal to `start` (same tile and transform bits; the stamp repeats from it).
    // region_too_large past MAX_TILE_REGION_CELLS
    bool floodFill(const BrushTarget& target, const BrushStamp& stamp, CellCoord start, FloodScope scope, const PaintOptions& options, BrushEdit& edit);

    // R22: each cell of `cells` chooses by tileset::cellHash(layer seed, strokeSeed, x, y)
    bool paintRandom(const BrushTarget& target, std::span<const RandomEntry> entries, uint32_t strokeSeed, std::span<const CellCoord> cells,
                     const PaintOptions& options, BrushEdit& edit);

    // ---- erase: empties the tile cells and their lock bits (a terrain stays until a terrain op)
    bool eraseCell(const BrushTarget& target, CellCoord cell, BrushEdit& edit);
    bool eraseBox(const BrushTarget& target, const CellRect& rect, BrushEdit& edit);
    bool eraseLine(const BrushTarget& target, CellCoord from, CellCoord to, BrushEdit& edit);
    bool eraseFlood(const BrushTarget& target, CellCoord start, FloodScope scope, BrushEdit& edit);

    // ---- terrain (R23-R25, D6): logical cells on a dual-grid layer; terrainIndex is
    // tileset-global, NO_TERRAIN erases the terrain of those cells
    bool paintTerrain(const BrushTarget& target, std::span<const CellCoord> cells, int terrainIndex, BrushEdit& edit);
    // re-chooses every terrain cell of the rectangle (R25)
    bool refreshTerrain(const BrushTarget& target, const CellRect& rect, BrushEdit& edit);

    // ---- pick and selection
    // the cell's tile as a one-cell stamp (no lock bit); empty for an empty cell
    BrushStamp pick(const BrushTarget& target, CellCoord cell);
    // the rect's tiles as a stamp with offsets from its bottom-left cell and the rect's size
    // (region_too_large past MAX_TILE_REGION_CELLS)
    bool copyRect(const BrushTarget& target, const CellRect& rect, BrushStamp& out, PixelProblem& error);
    // empties the rect (tiles and, on layers with a terrain plane under them, terrain, re-choosing
    // the neighbours); a selection is what the user sees, so its deletion removes the terrain too
    bool deleteRect(const BrushTarget& target, const CellRect& rect, BrushEdit& edit);
    // moves the rect's non-empty cells by `delta`; the destination cells they land on are replaced,
    // the others keep their tile. A terrain plane moves with its tile (not on dual-grid layers,
    // whose terrain is the logical plane); neighbours are re-chosen when terrain moved or was lost
    bool moveRect(const BrushTarget& target, const CellRect& rect, CellCoord delta, BrushEdit& edit);

    // ---- what the tools preview (and the MCP tool reports) without touching the store
    bool floodCells(const BrushTarget& target, CellCoord start, FloodScope scope, std::vector<CellCoord>& out, PixelProblem& error);
    // the rect's cells row by row; region_too_large past MAX_TILE_REGION_CELLS
    bool boxCells(const CellRect& rect, std::vector<CellCoord>& out, PixelProblem& error);

}
