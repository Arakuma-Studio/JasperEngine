// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The brush operations (docs/specs/S50-grid-tilemap.md R21, R22, R26-R28, AC6) against golden ASCII
// grids written by hand from the rules, on every layout; the top line is the highest row. One undo
// of TilemapCellsCmd restores the cells exactly (D10), and two runs give identical cells (R28).

#include "tilemap/TilemapBrush.h"
#include "tilemap/TilemapCellsCmd.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "component/TilemapLayerComponent.h"
#include "support/EditorTestSupport.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::tilemap;

namespace {

    TileGridSpec layoutSpec(TileGridLayout layout) {
        TileGridSpec spec;
        spec.layout = layout;
        return spec;
    }

    const TileGridLayout kLayouts[] = {TileGridLayout::Rectangle, TileGridLayout::Isometric, TileGridLayout::IsometricStaggered, TileGridLayout::Hexagon};

    // tiles 0, 1 and 2, no pixels: the brush only needs their ids and probabilities
    TilesetData makeTileset() {
        TilesetData data;
        for (int i = 0; i < 3; i++) data.addTile(0, i, 0);
        return data;
    }

    // one row per line, the highest row first, '.' empty, the tile id's digit otherwise
    std::string render(const TileCells& cells, const CellRect& rect) {
        std::string out;
        for (int64_t y = rect.maxY; y >= rect.minY; y--) {
            if (y != rect.maxY) out += '\n';
            for (int64_t x = rect.minX; x <= rect.maxX; x++) {
                const uint32_t value = cells.get(CellCoord{(int32_t)x, (int32_t)y});
                out += cellValueEmpty(value) ? '.' : (char)('0' + cellValueTile(value));
            }
        }
        return out;
    }

    CellRect rectOf(int minX, int minY, int maxX, int maxY) {
        return CellRect::of(CellCoord{minX, minY}, CellCoord{maxX, maxY});
    }

    // a terrain set with one tile of the terrain and every peering "any": it fits every cell of
    // the terrain, so a test reads the terrain plane's effect without a template's tiles
    TilesetData makeTerrainTileset(TileShape shape, TerrainMode mode) {
        TilesetData data;
        data.shape = shape;
        TerrainSetDef set;
        set.name = "Ground";
        set.mode = mode;
        set.terrains.push_back(TerrainDef{"Grass", "#00ff00"});
        data.terrainSets.push_back(set);
        data.addTile(0, 0, 0);
        data.tiles[0].terrain.set = 0;
        data.tiles[0].terrain.center = mode == TerrainMode::DualGrid ? -1 : 0;
        return data;
    }

}

TEST(TilemapBrush, StampKeepsOffsetsFromTheAnchorOnEveryLayout) {
    for (TileGridLayout layout : kLayouts) {
        TileCells cells;
        const TilesetData tileset = makeTileset();
        BrushTarget target{cells, &tileset, layoutSpec(layout)};
        BrushEdit edit;
        BrushStamp stamp;
        stamp.cells = {{0, 0, 1, 0}, {1, 0, 2, 0}, {0, 1, 0, 0}};
        ASSERT_TRUE(paintStamp(target, stamp, CellCoord{1, 1}, PaintOptions{}, edit));
        EXPECT_EQ(render(cells, rectOf(0, 0, 3, 3)), "....\n.0..\n.12.\n....") << (int)layout;
        EXPECT_EQ(edit.patch.changes().size(), 3u);
    }
}

TEST(TilemapBrush, UnknownTileIsRefusedAndWritesNothing) {
    TileCells cells;
    const TilesetData tileset = makeTileset();
    BrushTarget target{cells, &tileset, layoutSpec(TileGridLayout::Rectangle)};
    BrushEdit edit;
    EXPECT_FALSE(paintStamp(target, BrushStamp::single(9), CellCoord{0, 0}, PaintOptions{}, edit));
    EXPECT_EQ(edit.error.kind, "unknown_tile");
    EXPECT_TRUE(cells.empty());
    EXPECT_FALSE(edit.error.fix.empty());
}

TEST(TilemapBrush, TransposeIsRefusedOnHexagonsOnly) {
    TileCells cells;
    const TilesetData tileset = makeTileset();
    const BrushStamp stamp = BrushStamp::single(0, TILE_TRANSPOSE);
    {
        BrushTarget target{cells, &tileset, layoutSpec(TileGridLayout::Hexagon)};
        BrushEdit edit;
        EXPECT_FALSE(paintStamp(target, stamp, CellCoord{0, 0}, PaintOptions{}, edit));
        EXPECT_EQ(edit.error.kind, "layout_mismatch");
    }
    for (TileGridLayout layout : {TileGridLayout::Rectangle, TileGridLayout::Isometric, TileGridLayout::IsometricStaggered}) {
        BrushTarget target{cells, &tileset, layoutSpec(layout)};
        BrushEdit edit;
        EXPECT_TRUE(paintStamp(target, stamp, CellCoord{0, 0}, PaintOptions{}, edit)) << (int)layout;
    }
    EXPECT_EQ(cells.get(CellCoord{0, 0}) & TILE_TRANSFORM_MASK, TILE_TRANSPOSE);
}

TEST(TilemapBrush, NoTilesetIsATypedError) {
    TileCells cells;
    BrushTarget target{cells, nullptr, layoutSpec(TileGridLayout::Rectangle)};
    BrushEdit edit;
    EXPECT_FALSE(paintStamp(target, BrushStamp::single(0), CellCoord{0, 0}, PaintOptions{}, edit));
    EXPECT_EQ(edit.error.kind, "tileset_missing");
}

TEST(TilemapBrush, BoxFillRepeatsTheStampFromTheBottomLeftCell) {
    for (TileGridLayout layout : kLayouts) {
        TileCells cells;
        const TilesetData tileset = makeTileset();
        BrushTarget target{cells, &tileset, layoutSpec(layout)};
        BrushEdit edit;
        BrushStamp stamp;
        stamp.cells = {{0, 0, 1, 0}, {1, 0, 2, 0}};
        ASSERT_TRUE(boxFill(target, stamp, rectOf(1, 1, 4, 2), PaintOptions{}, edit));
        EXPECT_EQ(render(cells, rectOf(0, 0, 5, 3)), "......\n.1212.\n.1212.\n......") << (int)layout;
    }
}

TEST(TilemapBrush, BoxOverTheRegionLimitIsTyped) {
    TileCells cells;
    const TilesetData tileset = makeTileset();
    BrushTarget target{cells, &tileset, layoutSpec(TileGridLayout::Rectangle)};
    BrushEdit edit;
    EXPECT_FALSE(boxFill(target, BrushStamp::single(0), rectOf(0, 0, 2000, 1000), PaintOptions{}, edit));
    EXPECT_EQ(edit.error.kind, "region_too_large");
    EXPECT_TRUE(cells.empty());
}

// a box over the whole int32 range has 2^64 cells: its count must not wrap past the limit
TEST(TilemapBrush, BoxesAtTheInt32ExtremesAreTyped) {
    TileCells cells;
    const TilesetData tileset = makeTileset();
    BrushTarget target{cells, &tileset, layoutSpec(TileGridLayout::Rectangle)};
    const CellRect all = rectOf(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX);
    BrushEdit edit;
    EXPECT_FALSE(boxFill(target, BrushStamp::single(0), all, PaintOptions{}, edit));
    EXPECT_EQ(edit.error.kind, "region_too_large");
    BrushEdit erase;
    EXPECT_FALSE(eraseBox(target, all, erase));
    EXPECT_EQ(erase.error.kind, "region_too_large");
    BrushEdit row;
    EXPECT_FALSE(boxFill(target, BrushStamp::single(0), rectOf(INT32_MIN, 0, INT32_MAX, 0), PaintOptions{}, row));
    EXPECT_EQ(row.error.kind, "region_too_large");
    EXPECT_TRUE(cells.empty());
}

TEST(TilemapBrush, LineFollowsEachLayout) {
    struct Case {
        TileGridLayout layout;
        CellCoord to;
        CellRect region;
        const char* golden;
    };
    const Case cases[] = {
        // Bresenham: (0,0) (1,1) (2,1) (3,2) (4,2)
        {TileGridLayout::Rectangle, {4, 2}, rectOf(0, 0, 4, 2), "...11\n.11..\n1...."},
        {TileGridLayout::Isometric, {4, 2}, rectOf(0, 0, 4, 2), "...11\n.11..\n1...."},
        // three diamond steps east: (0,0) (0,1) (1,2) (1,3), the staggered rows' own shift
        {TileGridLayout::IsometricStaggered, {1, 3}, rectOf(0, 0, 1, 3), ".1\n.1\n1.\n1."},
        // a straight NE ray of hexes (no rounding): the same four cells
        {TileGridLayout::Hexagon, {1, 3}, rectOf(0, 0, 1, 3), ".1\n.1\n1.\n1."},
    };
    for (const Case& c : cases) {
        TileCells cells;
        const TilesetData tileset = makeTileset();
        BrushTarget target{cells, &tileset, layoutSpec(c.layout)};
        BrushEdit edit;
        ASSERT_TRUE(paintLine(target, BrushStamp::single(1), CellCoord{0, 0}, c.to, PaintOptions{}, edit));
        EXPECT_EQ(render(cells, c.region), c.golden) << (int)c.layout;
    }
}

TEST(TilemapBrush, EraseFollowsEachLayoutLine) {
    const TileGridSpec hex = layoutSpec(TileGridLayout::Hexagon);
    TileCells cells;
    const TilesetData tileset = makeTileset();
    BrushTarget target{cells, &tileset, hex};
    BrushEdit edit;
    ASSERT_TRUE(boxFill(target, BrushStamp::single(1), rectOf(0, 0, 1, 3), PaintOptions{}, edit));
    ASSERT_TRUE(eraseLine(target, CellCoord{0, 0}, CellCoord{1, 3}, edit));
    EXPECT_EQ(render(cells, rectOf(0, 0, 1, 3)), "1.\n1.\n.1\n.1");
}

TEST(TilemapBrush, FloodConnectsThroughEachLayoutsSides) {
    // A cells at (0,0), (-1,-1) and (1,1): the (-1,-1) cell is a side of (0,0) on a hexagon and on
    // a staggered diamond, a corner on the square layouts; (1,1) is connected on none of them
    const char* const squareGolden = "..0\n.1.\n0..";
    const char* const sidesGolden = "..0\n.1.\n1..";
    struct Case {
        TileGridLayout layout;
        const char* golden;
    };
    const Case cases[] = {
        {TileGridLayout::Rectangle, squareGolden},
        {TileGridLayout::Isometric, squareGolden},
        {TileGridLayout::IsometricStaggered, sidesGolden},
        {TileGridLayout::Hexagon, sidesGolden},
    };
    for (const Case& c : cases) {
        TileCells cells;
        const TilesetData tileset = makeTileset();
        BrushTarget target{cells, &tileset, layoutSpec(c.layout)};
        cells.setTile(CellCoord{0, 0}, 0);
        cells.setTile(CellCoord{-1, -1}, 0);
        cells.setTile(CellCoord{1, 1}, 0);
        BrushEdit edit;
        ASSERT_TRUE(floodFill(target, BrushStamp::single(1), CellCoord{0, 0}, FloodScope::Contiguous, PaintOptions{}, edit));
        EXPECT_EQ(render(cells, rectOf(-1, -1, 1, 1)), c.golden) << (int)c.layout;
    }
}

TEST(TilemapBrush, FloodEveryEqualCoversTheUsedRect) {
    TileCells cells;
    const TilesetData tileset = makeTileset();
    BrushTarget target{cells, &tileset, layoutSpec(TileGridLayout::Rectangle)};
    cells.setTile(CellCoord{0, 0}, 0);
    cells.setTile(CellCoord{3, 2}, 0);
    cells.setTile(CellCoord{2, 2}, 2);
    BrushEdit edit;
    ASSERT_TRUE(floodFill(target, BrushStamp::single(1), CellCoord{0, 0}, FloodScope::EveryEqual, PaintOptions{}, edit));
    EXPECT_EQ(render(cells, rectOf(0, 0, 3, 2)), "..21\n....\n1...");
}

TEST(TilemapBrush, FloodComparesTransformBits) {
    TileCells cells;
    const TilesetData tileset = makeTileset();
    BrushTarget target{cells, &tileset, layoutSpec(TileGridLayout::Rectangle)};
    cells.setTile(CellCoord{0, 0}, 0);
    cells.setTile(CellCoord{1, 0}, 0, TILE_FLIP_X);
    BrushEdit edit;
    ASSERT_TRUE(floodFill(target, BrushStamp::single(2), CellCoord{0, 0}, FloodScope::Contiguous, PaintOptions{}, edit));
    EXPECT_EQ(render(cells, rectOf(0, 0, 1, 0)), "20");
}

TEST(TilemapBrush, FloodEveryEqualOverTheLimitIsTyped) {
    TileCells cells;
    const TilesetData tileset = makeTileset();
    BrushTarget target{cells, &tileset, layoutSpec(TileGridLayout::Rectangle)};
    cells.setTile(CellCoord{0, 0}, 0);
    cells.setTile(CellCoord{2000, 1000}, 0);
    BrushEdit edit;
    EXPECT_FALSE(floodFill(target, BrushStamp::single(1), CellCoord{0, 0}, FloodScope::EveryEqual, PaintOptions{}, edit));
    EXPECT_EQ(edit.error.kind, "region_too_large");
}

TEST(TilemapBrush, ContiguousFloodStopsAtTheCellLimit) {
    // an empty store: the contiguous region is unbounded, so the bound is the only way out
    TileCells cells;
    const TilesetData tileset = makeTileset();
    BrushTarget target{cells, &tileset, layoutSpec(TileGridLayout::Rectangle)};
    BrushEdit edit;
    EXPECT_FALSE(floodFill(target, BrushStamp::single(1), CellCoord{0, 0}, FloodScope::Contiguous, PaintOptions{}, edit));
    EXPECT_EQ(edit.error.kind, "region_too_large");
    EXPECT_TRUE(cells.empty());
}

TEST(TilemapBrush, EraseVariantsEmptyTheirCells) {
    TileCells cells;
    const TilesetData tileset = makeTileset();
    BrushTarget target{cells, &tileset, layoutSpec(TileGridLayout::Rectangle)};
    BrushEdit edit;
    ASSERT_TRUE(boxFill(target, BrushStamp::single(0), rectOf(0, 0, 4, 2), PaintOptions{}, edit));
    ASSERT_TRUE(eraseCell(target, CellCoord{0, 0}, edit));
    ASSERT_TRUE(eraseBox(target, rectOf(3, 0, 4, 1), edit));
    ASSERT_TRUE(eraseFlood(target, CellCoord{0, 2}, FloodScope::Contiguous, edit));
    EXPECT_EQ(render(cells, rectOf(0, 0, 4, 2)), ".....\n.....\n.....");
}

TEST(TilemapBrush, PickGivesAOneCellStampWithoutTheLock) {
    TileCells cells;
    BrushTarget target{cells, nullptr, layoutSpec(TileGridLayout::Rectangle)};
    cells.setTile(CellCoord{2, 3}, 2, TILE_FLIP_Y | TILE_LOCK);
    const BrushStamp stamp = pick(target, CellCoord{2, 3});
    ASSERT_EQ(stamp.cells.size(), 1u);
    EXPECT_EQ(stamp.cells[0].x, 0);
    EXPECT_EQ(stamp.cells[0].y, 0);
    EXPECT_EQ(stamp.cells[0].tile, 2);
    EXPECT_EQ(stamp.cells[0].flags, TILE_FLIP_Y);
    EXPECT_TRUE(pick(target, CellCoord{0, 0}).empty());
}

TEST(TilemapBrush, RandomChoosesByTheHashOfSeedsAndCell) {
    TileCells cells, again, otherStroke;
    TilesetData tileset = makeTileset();
    tileset.tiles[2].probability = 0.0f;   // never chosen by the default weights
    BrushTarget target{cells, &tileset, layoutSpec(TileGridLayout::Rectangle), false, 7u};
    BrushTarget targetAgain{again, &tileset, layoutSpec(TileGridLayout::Rectangle), false, 7u};
    BrushTarget targetOther{otherStroke, &tileset, layoutSpec(TileGridLayout::Rectangle), false, 7u};
    std::vector<CellCoord> region;
    PixelProblem boxError;
    ASSERT_TRUE(boxCells(rectOf(0, 0, 7, 7), region, boxError));
    const std::vector<RandomEntry> entries = {{0}, {1}, {2}};
    BrushEdit edit, editAgain, editOther;
    ASSERT_TRUE(paintRandom(target, entries, 11u, region, PaintOptions{}, edit));
    ASSERT_TRUE(paintRandom(targetAgain, entries, 11u, region, PaintOptions{}, editAgain));
    ASSERT_TRUE(paintRandom(targetOther, entries, 12u, region, PaintOptions{}, editOther));
    EXPECT_EQ(cells, again);
    EXPECT_NE(cells, otherStroke) << "another stroke seed must change the choices";
    for (const CellCoord& cell : region) {
        const float weights[3] = {tileset.tiles[0].probability, tileset.tiles[1].probability, tileset.tiles[2].probability};
        const int picked = tileset::weightedPick(weights, 3, tileset::cellHash(7u, 11u, cell.x, cell.y));
        EXPECT_EQ(cellValueTile(cells.get(cell)), picked) << cell.x << "," << cell.y;
        EXPECT_NE(cellValueTile(cells.get(cell)), 2) << "the tile with probability 0 is never drawn";
    }
}

TEST(TilemapBrush, TerrainPaintSelectsTilesOnEveryLayout) {
    for (TileGridLayout layout : kLayouts) {
        const TileShape shape = layout == TileGridLayout::Hexagon ? TileShape::HexPointy : TileShape::Square;
        const TilesetData tileset = makeTerrainTileset(shape, TerrainMode::Sides);
        TileCells cells;
        BrushTarget target{cells, &tileset, layoutSpec(layout)};
        BrushEdit edit;
        const CellCoord painted[2] = {{0, 0}, {1, 0}};
        ASSERT_TRUE(paintTerrain(target, painted, 0, edit)) << edit.error.message;
        EXPECT_EQ(render(cells, rectOf(0, 0, 1, 0)), "00") << (int)layout;
        EXPECT_EQ(cells.getTerrain(CellCoord{1, 0}), 0);
    }
}

TEST(TilemapBrush, BadTerrainIndexIsATypedError) {
    const TilesetData tileset = makeTerrainTileset(TileShape::Square, TerrainMode::Sides);
    TileCells cells;
    BrushTarget target{cells, &tileset, layoutSpec(TileGridLayout::Rectangle)};
    BrushEdit edit;
    const CellCoord cell{0, 0};
    EXPECT_FALSE(paintTerrain(target, std::span<const CellCoord>(&cell, 1), 5, edit));
    EXPECT_EQ(edit.error.kind, "unknown_terrain");
}

TEST(TilemapBrush, DualGridTakesLogicalCellsForTerrainAndDisplayCellsForTiles) {
    const TilesetData tileset = makeTerrainTileset(TileShape::Square, TerrainMode::DualGrid);
    TileCells cells;
    BrushTarget target{cells, &tileset, layoutSpec(TileGridLayout::Rectangle), true};
    BrushEdit edit;
    const CellCoord logical{0, 0};
    ASSERT_TRUE(paintTerrain(target, std::span<const CellCoord>(&logical, 1), 0, edit)) << edit.error.message;
    // the plane holds the logical cell; the display tiles fall on the four cells around it
    EXPECT_EQ(cells.getTerrain(logical), 0);
    EXPECT_EQ(render(cells, rectOf(0, 0, 1, 1)), "00\n00");
    // a manual tile addresses a display cell and leaves the logical plane alone
    BrushEdit manual;
    ASSERT_TRUE(paintStamp(target, BrushStamp::single(0, TILE_FLIP_X), CellCoord{5, 5}, PaintOptions{}, manual));
    EXPECT_EQ(cells.getTerrain(CellCoord{5, 5}), NO_TERRAIN);
    EXPECT_EQ(cells.get(CellCoord{5, 5}) & TILE_TRANSFORM_MASK, TILE_FLIP_X);
}

// R26: a tile over a terrain cell clears its terrain; keep_terrain keeps it and locks the cell; the
// solver leaves a locked cell alone on a refresh; erasing the tile clears the lock.
TEST(TilemapBrush, ManualTilesOverTerrainFollowR26) {
    const TilesetData tileset = makeTerrainTileset(TileShape::Square, TerrainMode::Sides);
    TileCells cells;
    BrushTarget target{cells, &tileset, layoutSpec(TileGridLayout::Rectangle)};
    BrushEdit terrainEdit;
    const CellCoord painted[3] = {{0, 0}, {1, 0}, {2, 0}};
    ASSERT_TRUE(paintTerrain(target, painted, 0, terrainEdit));
    ASSERT_EQ(render(cells, rectOf(0, 0, 2, 0)), "000");

    BrushEdit clearing;
    ASSERT_TRUE(paintStamp(target, BrushStamp::single(0, TILE_FLIP_Y), CellCoord{1, 0}, PaintOptions{}, clearing));
    EXPECT_EQ(cells.getTerrain(CellCoord{1, 0}), NO_TERRAIN) << "painted without Keep Terrain: the terrain goes";
    EXPECT_EQ(cells.get(CellCoord{1, 0}) & TILE_LOCK, 0u);

    PaintOptions keep;
    keep.keepTerrain = true;
    BrushEdit keeping;
    ASSERT_TRUE(paintStamp(target, BrushStamp::single(0, TILE_FLIP_Y), CellCoord{2, 0}, keep, keeping));
    EXPECT_EQ(cells.getTerrain(CellCoord{2, 0}), 0) << "Keep Terrain keeps the terrain";
    EXPECT_NE(cells.get(CellCoord{2, 0}) & TILE_LOCK, 0u) << "and locks the cell";

    // a refresh re-chooses terrain cells (flips cleared) but not the locked one
    BrushEdit refresh;
    ASSERT_TRUE(refreshTerrain(target, rectOf(0, 0, 2, 0), refresh));
    EXPECT_EQ(cells.get(CellCoord{2, 0}) & TILE_TRANSFORM_MASK, TILE_FLIP_Y);

    BrushEdit erase;
    ASSERT_TRUE(eraseCell(target, CellCoord{2, 0}, erase));
    EXPECT_EQ(cells.get(CellCoord{2, 0}), 0u) << "erasing clears the tile and the lock";
    EXPECT_EQ(cells.getTerrain(CellCoord{2, 0}), 0) << "the terrain stays (R26)";
}

TEST(TilemapBrush, CopyDeleteAndMoveASelection) {
    TileCells cells;
    const TilesetData tileset = makeTileset();
    BrushTarget target{cells, &tileset, layoutSpec(TileGridLayout::Rectangle)};
    cells.setTile(CellCoord{1, 1}, 1);
    cells.setTile(CellCoord{2, 1}, 2, TILE_FLIP_X);
    cells.setTile(CellCoord{2, 2}, 0);

    BrushStamp copied;
    PixelProblem copyError;
    ASSERT_TRUE(copyRect(target, rectOf(1, 1, 2, 2), copied, copyError));
    EXPECT_EQ(copied.width, 2);
    EXPECT_EQ(copied.height, 2);
    ASSERT_EQ(copied.cells.size(), 3u);
    EXPECT_EQ(copied.cells[0].x, 0);
    EXPECT_EQ(copied.cells[0].y, 0);
    EXPECT_EQ(copied.cells[0].tile, 1);
    EXPECT_EQ(copied.cells[1].flags, TILE_FLIP_X);

    // the copy pastes at a new anchor with its hole left alone
    BrushEdit paste;
    ASSERT_TRUE(paintStamp(target, copied, CellCoord{4, 0}, PaintOptions{}, paste));
    EXPECT_EQ(render(cells, rectOf(0, 0, 5, 2)), "..0...\n.12..0\n....12");

    BrushEdit move;
    ASSERT_TRUE(moveRect(target, rectOf(1, 1, 2, 2), CellCoord{0, 1}, move));
    EXPECT_EQ(render(cells, rectOf(0, 0, 5, 3)), "..0...\n.12...\n.....0\n....12");

    BrushEdit erased;
    ASSERT_TRUE(deleteRect(target, rectOf(1, 2, 2, 3), erased));
    EXPECT_EQ(render(cells, rectOf(0, 0, 5, 3)), "......\n......\n.....0\n....12");
}

TEST(TilemapBrush, MoveCarriesTheTerrainPlaneAndRefreshesAround) {
    const TilesetData tileset = makeTerrainTileset(TileShape::Square, TerrainMode::Sides);
    TileCells cells;
    BrushTarget target{cells, &tileset, layoutSpec(TileGridLayout::Rectangle)};
    BrushEdit painted;
    const CellCoord cell{0, 0};
    ASSERT_TRUE(paintTerrain(target, std::span<const CellCoord>(&cell, 1), 0, painted));
    BrushEdit move;
    ASSERT_TRUE(moveRect(target, rectOf(0, 0, 0, 0), CellCoord{3, 0}, move));
    EXPECT_EQ(cells.getTerrain(CellCoord{3, 0}), 0);
    EXPECT_EQ(cells.getTerrain(CellCoord{0, 0}), NO_TERRAIN);
    EXPECT_EQ(render(cells, rectOf(0, 0, 3, 0)), "...0");
}

TEST(TilemapBrush, LockedLinkedErrorNamesTheFix) {
    const PixelProblem error = lockedLinkedError();
    EXPECT_EQ(error.kind, "locked_linked");
    EXPECT_EQ(error.fix, "unlink the Grid or edit the source map");
}

TEST(TilemapBrush, SameOperationGivesIdenticalCellsTwice) {
    const auto run = [] {
        TileCells cells;
        const TilesetData tileset = makeTileset();
        BrushTarget target{cells, &tileset, layoutSpec(TileGridLayout::Hexagon), false, 3u};
        BrushEdit edit;
        boxFill(target, BrushStamp::single(0), rectOf(0, 0, 9, 9), PaintOptions{}, edit);
        const std::vector<RandomEntry> entries = {{1, 2.0f}, {2, 1.0f}};
        std::vector<CellCoord> region;
        PixelProblem boxError;
        boxCells(rectOf(2, 2, 6, 6), region, boxError);
        paintRandom(target, entries, 42u, region, PaintOptions{}, edit);
        floodFill(target, BrushStamp::single(2), CellCoord{0, 0}, FloodScope::Contiguous, PaintOptions{}, edit);
        return std::make_pair(cells, edit.patch.changes());
    };
    const auto first = run();
    const auto second = run();
    EXPECT_EQ(first.first, second.first);
    ASSERT_EQ(first.second.size(), second.second.size());
    for (size_t i = 0; i < first.second.size(); i++) {
        EXPECT_EQ(first.second[i].cell, second.second[i].cell) << "patches agree in order too: " << i;
        EXPECT_EQ(first.second[i].newValue, second.second[i].newValue) << i;
    }
}

namespace {

    class TilemapBrushProjectTest : public ::testing::Test {
    protected:
        jasperengine::test::TestTempDir dir{"tilemap-brush"};
        std::unique_ptr<Project> project;
        SceneProject* scene = nullptr;
        uint32_t sceneId = 0;
        Entity layerEntity = NULL_ENTITY;

        void SetUp() override {
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(), fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            project = std::make_unique<Project>();
            project->setSkipProjectLock(true);
            ASSERT_TRUE(project->loadProject(dir.path(), false));
            for (SceneProject& candidate : project->getScenes()) {
                if (!candidate.filepath.empty() && !candidate.scene) project->loadScene(candidate.filepath, false, false, true);
            }
            for (SceneProject& candidate : project->getScenes()) {
                if (candidate.scene && candidate.name == "Main Scene") scene = &candidate;
            }
            ASSERT_NE(scene, nullptr);
            sceneId = scene->id;
            project->setSelectedSceneId(sceneId);
            layerEntity = scene->scene->createEntity();
            scene->scene->addComponent<TilemapLayerComponent>(layerEntity, TilemapLayerComponent{});
            scene->isModified = false;
        }
        void TearDown() override {
            if (project) jasperengine::editor::test::closeProject(*project);
        }
    };

}

TEST_F(TilemapBrushProjectTest, OneUndoRestoresEveryCellExactly) {
    TilemapLayerComponent& layer = scene->scene->getComponent<TilemapLayerComponent>(layerEntity);
    layer.cells.edit().setTile(CellCoord{5, 5}, 2, TILE_FLIP_X | TILE_LOCK);
    const TileCells before = layer.cells.read();

    TilesetData tileset = makeTerrainTileset(TileShape::Square, TerrainMode::Sides);
    tileset.addTile(0, 1, 0);
    BrushTarget target{layer.cells.edit(), &tileset, layoutSpec(TileGridLayout::Rectangle)};
    BrushEdit edit;
    ASSERT_TRUE(boxFill(target, BrushStamp::single(1), rectOf(-2, -2, 7, 7), PaintOptions{}, edit));
    const CellCoord terrainCell{0, 0};
    ASSERT_TRUE(paintTerrain(target, std::span<const CellCoord>(&terrainCell, 1), 0, edit));
    ASSERT_NE(layer.cells.read(), before);
    const TileCells after = layer.cells.read();

    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t position = history->position();
    ASSERT_TRUE(history->addCommand(new TilemapCellsCmd(project.get(), sceneId, layerEntity, edit.patch.changes(), "Paint Tiles")));
    EXPECT_TRUE(scene->isModified);
    EXPECT_EQ(history->position(), position + 1);

    ASSERT_TRUE(history->undo());
    EXPECT_EQ(layer.cells.read(), before);
    EXPECT_EQ(layer.cells.read().get(CellCoord{5, 5}), makeCellValue(2, TILE_FLIP_X | TILE_LOCK));
    ASSERT_TRUE(history->redo());
    EXPECT_EQ(layer.cells.read(), after);
}

TEST_F(TilemapBrushProjectTest, EmptyPatchIsNoOp) {
    TilemapCellsCmd command(project.get(), sceneId, layerEntity, {}, "Paint Tiles");
    EXPECT_TRUE(command.isNoOp());
    EXPECT_FALSE(command.affectsStructure());
    EXPECT_EQ(command.describe(), "Paint Tiles");
    EXPECT_TRUE(command.execute());
}

TEST_F(TilemapBrushProjectTest, HundredThousandCellFloodPatchIsCounted) {
    TilemapLayerComponent& layer = scene->scene->getComponent<TilemapLayerComponent>(layerEntity);
    TileCells& cells = layer.cells.edit();
    for (int y = 0; y < 1000; y++) {
        for (int x = 0; x < 100; x++) cells.setTile(CellCoord{x, y}, 0);
    }
    TilesetData tileset = makeTileset();
    BrushTarget target{cells, &tileset, layoutSpec(TileGridLayout::Rectangle)};
    BrushEdit edit;
    const auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(floodFill(target, BrushStamp::single(1), CellCoord{0, 0}, FloodScope::Contiguous, PaintOptions{}, edit));
    const std::vector<TileCellChange> changes = edit.patch.changes();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ(changes.size(), 100000u);
    TilemapCellsCmd command(project.get(), sceneId, layerEntity, changes, "Flood Fill");
    const size_t bytes = command.memoryBytes();
    EXPECT_GE(bytes, 100000u * sizeof(TileCellChange));
    std::printf("[bench] TilemapBrush flood 100000 cells: %.1f ms, undo patch %zu bytes\n", ms, bytes);
    // R61: 100,000-cell flood with undo within 20 ms on the benchmark server only
    const double slack = jasperengine::editor::test::perfSlack();
    if (slack > 0.0) {
        EXPECT_LT(ms, 20.0 * slack);
    }
}
