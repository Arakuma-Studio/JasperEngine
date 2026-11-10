// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// AC4 (docs/specs/S50-grid-tilemap.md R23-R28, D6). The blob goldens below are the cr31 indices of
// each cell, worked out by hand from the shapes; larger shapes are checked against
// cr31IndexFromPlane, a separate statement of cr31's index rule (it never calls the solver).
// The scoring cases restate Pixelorama's rules (+10 match, -2 mismatch, + specificity).

#include "tilemap/TerrainSolver.h"
#include "tilemap/TerrainTemplates.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>

using namespace jasperengine;

namespace {

    constexpr int FRAME_BY_MASK[16] = {-1, 15, 8, 9, 0, 11, 14, 7, 13, 4, 1, 10, 3, 2, 5, 6};

    TileGridSpec solverGrid(TileGridLayout layout = TileGridLayout::Rectangle, StaggerAxis axis = StaggerAxis::Y, StaggerIndex index = StaggerIndex::Odd){
        TileGridSpec spec;
        spec.layout = layout;
        spec.staggerAxis = axis;
        spec.staggerIndex = index;
        spec.cellWidth = 16;
        spec.cellHeight = 16;
        return spec;
    }

    bool isReducedBlob(int mask){
        const bool n = mask & 1, e = mask & 4, s = mask & 16, w = mask & 64;
        return !((mask & 2) && !(n && e)) && !((mask & 8) && !(e && s)) && !((mask & 32) && !(s && w)) && !((mask & 128) && !(w && n));
    }

    // the 47 reduced masks as tiles whose id equals the cr31 index
    TilesetData blobById(float probabilityOf255 = 1.0f){
        TilesetData d;
        TerrainSetDef set;
        set.name = "Ground";
        set.mode = TerrainMode::Blob;
        set.terrains = {{"Grass", "#00ff00"}};
        d.terrainSets.push_back(set);
        for (int mask = 0; mask < 256; mask++){
            if (!isReducedBlob(mask))
                continue;
            TileDef tile;
            tile.id = mask;
            tile.terrain.set = 0;
            tile.terrain.center = 0;
            for (int slot = 0; slot < 8; slot++)
                tile.terrain.peering[(size_t)slot] = (mask & (1 << slot)) ? (int16_t)0 : PEERING_NONE;
            tile.probability = mask == 255 ? probabilityOf255 : 1.0f;
            d.tiles.push_back(tile);
        }
        d.nextId = 256;
        return d;
    }

    TilesetData sidesById(){
        TilesetData d;
        TerrainSetDef set;
        set.name = "Road";
        set.mode = TerrainMode::Sides;
        set.terrains = {{"Road", "#404040"}};
        d.terrainSets.push_back(set);
        for (int mask = 0; mask < 16; mask++){
            TileDef tile;
            tile.id = mask;
            tile.terrain.set = 0;
            tile.terrain.center = 0;
            for (int bit = 0; bit < 4; bit++)
                tile.terrain.peering[(size_t)(bit * 2)] = (mask & (1 << bit)) ? (int16_t)0 : PEERING_NONE;
            d.tiles.push_back(tile);
        }
        d.nextId = 16;
        return d;
    }

    TilesetData dualByFrame(bool twoSurface){
        TilesetData d;
        TilesetSource source;
        d.sources.push_back(source);
        TerrainSetDef set;
        set.name = "Shore";
        set.mode = TerrainMode::DualGrid;
        set.twoSurface = twoSurface;
        set.terrains = {{"Sand", "#e3c27a"}, {"Water", "#3a6fd8"}};
        d.terrainSets.push_back(set);
        for (int y = 0; y < 4; y++){
            for (int x = 0; x < 4; x++)
                d.addTile(0, x, y);
        }
        std::vector<TilesetProblem> problems;
        EXPECT_TRUE(terrain::applyTemplate(d, 0, TerrainTemplate::DualGrid4x4, 0, TerrainTemplateOptions{}, problems));
        return d;
    }

    TerrainContext makeContext(const TilesetData& data, TileGridSpec grid = solverGrid(), bool dualGrid = false, uint32_t seed = 0){
        TerrainContext context;
        context.tileset = &data;
        context.grid = grid;
        context.dualGrid = dualGrid;
        context.seed = seed;
        return context;
    }

    std::vector<CellCoord> cellsOf(std::initializer_list<CellCoord> list){
        return std::vector<CellCoord>(list);
    }

    std::vector<CellCoord> boxOf(int x0, int y0, int x1, int y1){
        std::vector<CellCoord> out;
        for (int y = y0; y <= y1; y++){
            for (int x = x0; x <= x1; x++)
                out.push_back(CellCoord{x, y});
        }
        return out;
    }

    bool paintAll(TileCells& store, const TerrainContext& context, const std::vector<CellCoord>& cells, int terrainIndex, TerrainReport& report, TileCellPatch* patchOut = nullptr){
        TileCellPatch local;
        TileCellPatch& patch = patchOut ? *patchOut : local;
        std::string error;
        const bool ok = terrain::paint(store, context, std::span<const CellCoord>(cells.data(), cells.size()), terrainIndex, patch, report, error);
        EXPECT_TRUE(ok) << error;
        return ok;
    }

    // the grid as text, top row first: each cell's tile id, or . when empty
    std::string tilesText(const TileCells& store, int x0, int y0, int x1, int y1){
        std::string out;
        for (int y = y1; y >= y0; y--){
            for (int x = x0; x <= x1; x++){
                const int32_t tile = store.getTile(CellCoord{x, y});
                out += tile < 0 ? std::string(".") : std::to_string(tile);
                if (x < x1)
                    out += ' ';
            }
            out += '\n';
        }
        return out;
    }

    // cr31's rule, restated: sides count when the neighbour is filled; a corner when it is filled
    // and both sides beside it are. Independent of the solver's code path.
    int cr31IndexFromPlane(const std::set<std::pair<int, int>>& filled, int x, int y){
        const auto has = [&](int dx, int dy){ return filled.count({x + dx, y + dy}) > 0; };
        const bool n = has(0, 1), e = has(1, 0), s = has(0, -1), w = has(-1, 0);
        int mask = (n ? 1 : 0) | (e ? 4 : 0) | (s ? 16 : 0) | (w ? 64 : 0);
        if (n && e && has(1, 1)) mask |= 2;
        if (e && s && has(1, -1)) mask |= 8;
        if (s && w && has(-1, -1)) mask |= 32;
        if (w && n && has(-1, 1)) mask |= 128;
        return mask;
    }

}

TEST(TerrainSolver, ABlobSquareEqualsItsHandDerivedGrid){
    TileCells store;
    const TilesetData data = blobById();
    TerrainReport report;
    paintAll(store, makeContext(data), boxOf(0, 0, 2, 2), 0, report);
    EXPECT_EQ(tilesText(store, 0, 0, 2, 2),
              "28 124 112\n"
              "31 255 241\n"
              "7 199 193\n");
    EXPECT_TRUE(report.noTile.empty());
}

TEST(TerrainSolver, ABlobPlusEqualsItsHandDerivedGrid){
    TileCells store;
    const TilesetData data = blobById();
    TerrainReport report;
    paintAll(store, makeContext(data), cellsOf({{1, 1}, {1, 2}, {1, 0}, {0, 1}, {2, 1}}), 0, report);
    EXPECT_EQ(tilesText(store, 0, 0, 2, 2),
              ". 16 .\n"
              "4 85 64\n"
              ". 1 .\n");
}

TEST(TerrainSolver, ABlobLEqualsItsHandDerivedGrid){
    TileCells store;
    const TilesetData data = blobById();
    TerrainReport report;
    paintAll(store, makeContext(data), cellsOf({{0, 0}, {1, 0}, {0, 1}}), 0, report);
    EXPECT_EQ(tilesText(store, 0, 0, 1, 1),
              "16 .\n"
              "5 64\n");
}

TEST(TerrainSolver, ABlobDonutMatchesCr31Rule){
    std::set<std::pair<int, int>> filled;
    for (const CellCoord& c : boxOf(0, 0, 8, 8)){
        const bool hole = c.x >= 3 && c.x <= 5 && c.y >= 3 && c.y <= 5;
        const bool notch = c.x >= 7 && c.y >= 7;
        if (!hole && !notch)
            filled.insert({c.x, c.y});
    }
    TileCells store;
    const TilesetData data = blobById();
    std::vector<CellCoord> cells;
    for (const auto& [x, y] : filled)
        cells.push_back(CellCoord{x, y});
    TerrainReport report;
    paintAll(store, makeContext(data), cells, 0, report);
    for (const auto& [x, y] : filled)
        EXPECT_EQ(store.getTile(CellCoord{x, y}), cr31IndexFromPlane(filled, x, y)) << x << ", " << y;
}

TEST(TerrainSolver, TheCornerFilterClearsCornersWithoutBothSides){
    TileCells store;
    const TilesetData data = blobById();
    const TerrainContext context = makeContext(data);
    store.setTerrain(CellCoord{0, 0}, 0);
    store.setTerrain(CellCoord{1, 1}, 0);    // only the diagonal neighbour
    std::array<int16_t, MAX_PEERING_SLOTS> mask;
    terrain::mask(store, context, 0, CellCoord{0, 0}, mask);
    EXPECT_EQ(mask[1], PEERING_NONE) << "ne without n and e";
    store.setTerrain(CellCoord{0, 1}, 0);    // n only: still not both sides
    terrain::mask(store, context, 0, CellCoord{0, 0}, mask);
    EXPECT_EQ(mask[1], PEERING_NONE) << "ne with n only";
    store.setTerrain(CellCoord{1, 0}, 0);    // n and e: the corner counts
    terrain::mask(store, context, 0, CellCoord{0, 0}, mask);
    EXPECT_EQ(mask[1], 0);
}

TEST(TerrainSolver, ACornerNeedsBothSidesToHoldTheCentreTerrain){
    TilesetData data = blobById();
    data.terrainSets[0].terrains.push_back({"Dirt", "#884400"});
    TileCells store;
    const TerrainContext context = makeContext(data);
    // both sides hold dirt while the cell is grass: the corner requirement is cleared, as Pixelorama
    // clears it for any side value other than the centre terrain
    store.setTerrain(CellCoord{0, 0}, 0);
    store.setTerrain(CellCoord{0, 1}, 1);
    store.setTerrain(CellCoord{1, 0}, 1);
    store.setTerrain(CellCoord{1, 1}, 1);
    std::array<int16_t, MAX_PEERING_SLOTS> mask;
    terrain::mask(store, context, 0, CellCoord{0, 0}, mask);
    EXPECT_EQ(mask[0], 1);
    EXPECT_EQ(mask[2], 1);
    EXPECT_EQ(mask[1], PEERING_NONE);
}

TEST(TerrainSolver, MasksReadOnlyTheirSet){
    TilesetData data = blobById();
    TerrainSetDef second;
    second.name = "Water";
    second.mode = TerrainMode::Blob;
    second.terrains = {{"Water", "#0000ff"}};
    data.terrainSets.push_back(second);
    TileCells store;
    store.setTerrain(CellCoord{0, 0}, 0);
    store.setTerrain(CellCoord{0, 1}, 1);       // the other set's terrain is "none" here
    std::array<int16_t, MAX_PEERING_SLOTS> mask;
    terrain::mask(store, makeContext(data), 0, CellCoord{0, 0}, mask);
    EXPECT_EQ(mask[0], PEERING_NONE);
}

TEST(TerrainSolver, ScoringFollowsPixeloramasRules){
    TileTerrain tile;
    tile.peering[0] = 0;              // n wants terrain 0
    tile.peering[1] = PEERING_NONE;   // ne wants none
    tile.peering[2] = PEERING_ANY;    // e: no constraint
    tile.peering[3] = 1;              // se wants terrain 1
    const std::array<int16_t, MAX_PEERING_SLOTS> mask = [] {
        std::array<int16_t, MAX_PEERING_SLOTS> m;
        m.fill(PEERING_NONE);
        m[0] = 0;
        m[3] = 0;
        return m;
    }();
    const std::vector<int> slots = {0, 1, 2, 3};
    // n matches (+10), ne matches (+10), e skipped, se mismatches (-2), specificity +3
    EXPECT_EQ(terrain::score(tile, mask, slots), 10 + 10 - 2 + 3);
    TileTerrain unconstrained;
    EXPECT_EQ(terrain::score(unconstrained, mask, slots), 0);
}

TEST(TerrainSolver, TheMoreConstrainedTileWinsAnExactTie){
    TilesetData data;
    TerrainSetDef set;
    set.mode = TerrainMode::Sides;
    set.terrains = {{"A", "#000000"}};
    data.terrainSets.push_back(set);
    TileDef generic;
    generic.id = 0;
    generic.terrain.set = 0;
    generic.terrain.center = 0;
    TileDef specific = generic;
    specific.id = 1;
    specific.terrain.peering[0] = PEERING_NONE;
    data.tiles = {generic, specific};
    data.nextId = 2;
    TileCells store;
    store.setTerrain(CellCoord{0, 0}, 0);
    bool matched = false;
    const uint32_t chosen = terrain::chooseTile(store, makeContext(data), CellCoord{0, 0}, matched);
    EXPECT_TRUE(matched);
    EXPECT_EQ(cellValueTile(chosen), 1);
}

TEST(TerrainSolver, CandidatesMustHaveTheCellsCentreTerrain){
    TilesetData data = blobById();
    data.terrainSets[0].terrains.push_back({"Dirt", "#884400"});
    TileCells store;
    TerrainReport report;
    // every tile is centre 0, so a cell of terrain 1 has no candidate
    paintAll(store, makeContext(data), cellsOf({{0, 0}}), 1, report);
    EXPECT_EQ(report.noTile, cellsOf({{0, 0}}));
    EXPECT_EQ(store.get(CellCoord{0, 0}), 0u);
    EXPECT_EQ(store.getTerrain(CellCoord{0, 0}), 1) << "the cell keeps its terrain";
}

TEST(TerrainSolver, ATieChoosesByHashAndProbability){
    TilesetData data;
    TerrainSetDef set;
    set.mode = TerrainMode::Sides;
    set.terrains = {{"A", "#000000"}};
    data.terrainSets.push_back(set);
    for (int i = 0; i < 2; i++){
        TileDef tile;
        tile.id = i;
        tile.terrain.set = 0;
        tile.terrain.center = 0;
        for (int slot : {0, 2, 4, 6})
            tile.terrain.peering[(size_t)slot] = PEERING_NONE;
        tile.probability = i == 0 ? 1.0f : 0.0f;
        data.tiles.push_back(tile);
    }
    data.nextId = 2;
    for (int i = 0; i < 200; i++){
        TileCells store;
        store.setTerrain(CellCoord{i, -i}, 0);
        bool matched = false;
        EXPECT_EQ(cellValueTile(terrain::chooseTile(store, makeContext(data), CellCoord{i, -i}, matched)), 0);
    }
}

TEST(TerrainSolver, TheSameSeedGivesTheSameTiles){
    TilesetData data = blobById();
    TileDef variant;
    variant.id = 300;
    variant.terrain = data.findTile(255)->terrain;
    data.tiles.push_back(variant);
    data.nextId = 301;
    const auto paintWith = [&](uint32_t seed){
        TileCells store;
        TerrainReport report;
        paintAll(store, makeContext(data, solverGrid(), false, seed), boxOf(0, 0, 11, 11), 0, report);
        return tilesText(store, 0, 0, 11, 11);
    };
    EXPECT_EQ(paintWith(7), paintWith(7));
    EXPECT_NE(paintWith(7), paintWith(8));
    const std::string text = paintWith(7);
    EXPECT_NE(text.find("300"), std::string::npos);
    EXPECT_NE(text.find("255"), std::string::npos);
}

TEST(TerrainSolver, RefreshReChoosesAfterATilesetChange){
    TilesetData data = blobById();
    TileCells store;
    const TerrainContext context = makeContext(data);
    TerrainReport report;
    paintAll(store, context, boxOf(0, 0, 2, 2), 0, report);
    store.setTile(CellCoord{1, 1}, 7);
    TileCellPatch patch;
    TerrainReport refreshed;
    terrain::refresh(store, context, CellRect::of(CellCoord{-5, -5}, CellCoord{5, 5}), patch, refreshed);
    EXPECT_EQ(store.getTile(CellCoord{1, 1}), 255);
    EXPECT_EQ(refreshed.rechosen, 9u);
}

TEST(TerrainSolver, RefreshStopsAtTheRect){
    TilesetData data = blobById();
    TileCells store;
    const TerrainContext context = makeContext(data);
    TerrainReport report;
    paintAll(store, context, boxOf(0, 0, 4, 4), 0, report);
    store.setTile(CellCoord{0, 0}, 3);
    store.setTile(CellCoord{4, 4}, 3);
    TileCellPatch patch;
    TerrainReport refreshed;
    terrain::refresh(store, context, CellRect::of(CellCoord{0, 0}, CellCoord{0, 0}), patch, refreshed);
    EXPECT_EQ(store.getTile(CellCoord{0, 0}), 7);
    EXPECT_EQ(store.getTile(CellCoord{4, 4}), 3);
    EXPECT_EQ(refreshed.rechosen, 1u);
}

// R25's counts, from a large settled map: the erased cell + the neighbours that hold terrain
TEST(TerrainSolver, BlobRefreshTouchesTheEightNeighbours){
    TilesetData data = blobById();
    TileCells store;
    const TerrainContext context = makeContext(data);
    TerrainReport report;
    paintAll(store, context, boxOf(-20, -20, 20, 20), 0, report);
    report = TerrainReport();
    TileCellPatch patch;
    paintAll(store, context, cellsOf({{0, 0}}), NO_TERRAIN, report, &patch);
    EXPECT_EQ(report.rechosen, 9u);
    for (const TileCellChange& change : patch.changes()){
        EXPECT_LE(std::abs(change.cell.x), 1);
        EXPECT_LE(std::abs(change.cell.y), 1);
    }
}

TEST(TerrainSolver, SidesRefreshTouchesTheFourSides){
    TilesetData data = sidesById();
    TileCells store;
    const TerrainContext context = makeContext(data);
    TerrainReport report;
    paintAll(store, context, boxOf(-20, -20, 20, 20), 0, report);
    report = TerrainReport();
    TileCellPatch patch;
    paintAll(store, context, cellsOf({{0, 0}}), NO_TERRAIN, report, &patch);
    EXPECT_EQ(report.rechosen, 5u);
    for (const TileCellChange& change : patch.changes())
        EXPECT_LE(std::abs(change.cell.x) + std::abs(change.cell.y), 1);
}

TEST(TerrainSolver, RepaintingTheSameTerrainTouchesNothing){
    TilesetData data = blobById();
    TileCells store;
    const TerrainContext context = makeContext(data);
    TerrainReport report;
    paintAll(store, context, boxOf(0, 0, 4, 4), 0, report);
    report = TerrainReport();
    TileCellPatch patch;
    paintAll(store, context, cellsOf({{2, 2}}), 0, report, &patch);
    EXPECT_EQ(report.rechosen, 0u);
    EXPECT_TRUE(patch.empty());
}

TEST(TerrainSolver, ABoxTouchesItsAreaAndPerimeterOnly){
    TilesetData data = blobById();
    TileCells store;
    const TerrainContext context = makeContext(data);
    TerrainReport report;
    paintAll(store, context, boxOf(-30, -30, 30, 30), NO_TERRAIN, report);
    paintAll(store, context, boxOf(-30, -30, 30, 30), 0, report);
    report = TerrainReport();
    paintAll(store, context, boxOf(0, 0, 9, 9), NO_TERRAIN, report);
    // the erased box plus its ring of neighbours
    EXPECT_EQ(report.rechosen, 12u * 12u);
}

TEST(TerrainSolver, DualGridEqualsFrameByMaskForEveryMask){
    const TilesetData data = dualByFrame(false);
    const TerrainContext context = makeContext(data, solverGrid(), true);
    for (int mask = 0; mask < 16; mask++){
        TileCells store;
        // logical cells of display tile (5, 5): TL (4, 5) TR (5, 5) BL (4, 4) BR (5, 4)
        std::vector<CellCoord> cells;
        if (mask & 1) cells.push_back(CellCoord{4, 5});
        if (mask & 2) cells.push_back(CellCoord{5, 5});
        if (mask & 4) cells.push_back(CellCoord{4, 4});
        if (mask & 8) cells.push_back(CellCoord{5, 4});
        TerrainReport report;
        paintAll(store, context, cells, 0, report);
        const int32_t tile = store.getTile(CellCoord{5, 5});
        EXPECT_EQ(tile, FRAME_BY_MASK[mask]) << "mask " << mask;
    }
}

TEST(TerrainSolver, TwoSurfaceDualGridReachesFrame12){
    const TilesetData data = dualByFrame(true);
    const TerrainContext context = makeContext(data, solverGrid(), true);
    TileCells store;
    TerrainReport report;
    // a whole area of the second surface: every display cell inside is frame 12
    paintAll(store, context, boxOf(0, 0, 3, 3), data.terrainIndex(0, 1), report);
    EXPECT_EQ(store.getTile(CellCoord{2, 2}), 12);
    // and each of the 16 masks over the two surfaces
    for (int mask = 0; mask < 16; mask++){
        TileCells cells;
        TerrainReport r;
        for (const CellCoord& c : boxOf(4, 4, 5, 5)){
            const bool first = (c.x == 4 && c.y == 5 && (mask & 1)) || (c.x == 5 && c.y == 5 && (mask & 2)) || (c.x == 4 && c.y == 4 && (mask & 4)) || (c.x == 5 && c.y == 4 && (mask & 8));
            paintAll(cells, context, cellsOf({c}), first ? data.terrainIndex(0, 0) : data.terrainIndex(0, 1), r);
        }
        const int expected = mask == 0 ? 12 : FRAME_BY_MASK[mask];
        EXPECT_EQ(cells.getTile(CellCoord{5, 5}), expected) << "mask " << mask;
    }
}

TEST(TerrainSolver, DualGridRefreshTouchesTheFourDualCells){
    const TilesetData data = dualByFrame(false);
    const TerrainContext context = makeContext(data, solverGrid(), true);
    TileCells store;
    TerrainReport report;
    paintAll(store, context, boxOf(-10, -10, 10, 10), 0, report);
    report = TerrainReport();
    TileCellPatch patch;
    paintAll(store, context, cellsOf({{0, 0}}), NO_TERRAIN, report, &patch);
    EXPECT_EQ(report.rechosen, 4u);
    std::set<std::pair<int, int>> touched;
    for (const TileCellChange& change : patch.changes()){
        if (change.oldValue != change.newValue)
            touched.insert({change.cell.x, change.cell.y});
    }
    EXPECT_EQ(touched, (std::set<std::pair<int, int>>{{0, 0}, {1, 0}, {0, 1}, {1, 1}}));
}

TEST(TerrainSolver, ErasingEveryLogicalCellEmptiesTheDualTile){
    const TilesetData data = dualByFrame(false);
    const TerrainContext context = makeContext(data, solverGrid(), true);
    TileCells store;
    TerrainReport report;
    paintAll(store, context, cellsOf({{0, 0}}), 0, report);
    EXPECT_NE(store.get(CellCoord{1, 1}), 0u);
    paintAll(store, context, cellsOf({{0, 0}}), NO_TERRAIN, report);
    EXPECT_EQ(store.get(CellCoord{1, 1}), 0u);
    EXPECT_TRUE(store.empty());
}

TEST(TerrainSolver, IsometricDualGridUsesTheSameLogicalCorners){
    const TilesetData data = dualByFrame(false);
    const TerrainContext context = makeContext(data, solverGrid(TileGridLayout::Isometric), true);
    TileCells store;
    TerrainReport report;
    paintAll(store, context, cellsOf({{4, 4}}), 0, report);
    EXPECT_EQ(store.getTile(CellCoord{5, 5}), FRAME_BY_MASK[4]);   // the logical cell is BL of (5, 5)
}

TEST(TerrainSolver, HexSidesSelectTheTileOfTheirSixSides){
    for (StaggerAxis axis : {StaggerAxis::Y, StaggerAxis::X}){
        TilesetData data;
        data.shape = axis == StaggerAxis::Y ? TileShape::HexPointy : TileShape::HexFlat;
        TerrainSetDef set;
        set.mode = TerrainMode::Sides;
        set.terrains = {{"A", "#000000"}};
        data.terrainSets.push_back(set);
        for (int mask = 0; mask < 64; mask++){
            TileDef tile;
            tile.id = mask;
            tile.terrain.set = 0;
            tile.terrain.center = 0;
            for (int side = 0; side < 6; side++)
                tile.terrain.peering[(size_t)side] = (mask & (1 << side)) ? (int16_t)0 : PEERING_NONE;
            data.tiles.push_back(tile);
        }
        data.nextId = 64;
        const TileGridSpec grid = solverGrid(TileGridLayout::Hexagon, axis);
        const TerrainContext context = makeContext(data, grid);
        for (int mask = 0; mask < 64; mask++){
            for (const CellCoord& center : {CellCoord{4, 4}, CellCoord{4, 3}, CellCoord{-3, -2}}){
                TileCells store;
                TerrainReport report;
                paintAll(store, context, cellsOf({center}), 0, report);
                std::vector<CellCoord> neighbours;
                for (int side = 0; side < 6; side++){
                    if (mask & (1 << side))
                        neighbours.push_back(tilegrid::neighbour(grid, center, side));
                }
                paintAll(store, context, neighbours, 0, report);
                EXPECT_EQ(store.getTile(center), mask) << "axis " << (int)axis << " center " << center.x << "," << center.y;
            }
        }
    }
}

TEST(TerrainSolver, HexRefreshTouchesSixNeighbours){
    TilesetData data;
    data.shape = TileShape::HexPointy;
    TerrainSetDef set;
    set.mode = TerrainMode::Sides;
    set.terrains = {{"A", "#000000"}};
    data.terrainSets.push_back(set);
    TileDef tile;
    tile.id = 0;
    tile.terrain.set = 0;
    tile.terrain.center = 0;
    data.tiles.push_back(tile);
    data.nextId = 1;
    const TileGridSpec grid = solverGrid(TileGridLayout::Hexagon);
    const TerrainContext context = makeContext(data, grid);
    TileCells store;
    TerrainReport report;
    paintAll(store, context, boxOf(-10, -10, 10, 10), 0, report);
    report = TerrainReport();
    paintAll(store, context, cellsOf({{0, 0}}), NO_TERRAIN, report);
    EXPECT_EQ(report.rechosen, 7u);
}

TEST(TerrainSolver, HexBlobCornersSeeTheirSharedSides){
    TilesetData data;
    data.shape = TileShape::HexPointy;
    TerrainSetDef set;
    set.mode = TerrainMode::Blob;
    set.terrains = {{"A", "#000000"}};
    data.terrainSets.push_back(set);
    const TileGridSpec grid = solverGrid(TileGridLayout::Hexagon);
    const TerrainContext context = makeContext(data, grid);
    TileCells store;
    const CellCoord center{3, 3};
    store.setTerrain(center, 0);
    store.setTerrain(tilegrid::neighbour(grid, center, 0), 0);  // e
    store.setTerrain(tilegrid::neighbour(grid, center, 1), 0);  // ne
    std::array<int16_t, MAX_PEERING_SLOTS> mask;
    terrain::mask(store, context, 0, center, mask);
    EXPECT_EQ(mask[6], 0) << "c_ne between sides e and ne";
    EXPECT_EQ(mask[7], PEERING_NONE) << "c_n between ne and nw";
    EXPECT_EQ(mask[11], PEERING_NONE) << "c_se between se and e";
}

TEST(TerrainSolver, FlatHexBlobCornersFollowTheirKeys){
    TilesetData data;
    data.shape = TileShape::HexFlat;
    TerrainSetDef set;
    set.mode = TerrainMode::Blob;
    set.terrains = {{"A", "#000000"}};
    data.terrainSets.push_back(set);
    const TileGridSpec grid = solverGrid(TileGridLayout::Hexagon, StaggerAxis::X);
    const TerrainContext context = makeContext(data, grid);
    TileCells store;
    const CellCoord center{3, 3};
    store.setTerrain(center, 0);
    store.setTerrain(tilegrid::neighbour(grid, center, 5), 0);  // se
    store.setTerrain(tilegrid::neighbour(grid, center, 0), 0);  // ne
    std::array<int16_t, MAX_PEERING_SLOTS> mask;
    terrain::mask(store, context, 0, center, mask);
    EXPECT_EQ(mask[6], 0) << "c_e between sides se and ne";
    EXPECT_EQ(mask[7], PEERING_NONE) << "c_ne between ne and n";
}

TEST(TerrainSolver, LockedCellsAreNeverReChosen){
    TilesetData data = blobById();
    TileCells store;
    const TerrainContext context = makeContext(data);
    TerrainReport report;
    paintAll(store, context, boxOf(0, 0, 2, 2), 0, report);
    const uint32_t locked = makeCellValue(9, TILE_LOCK | TILE_FLIP_X);
    store.set(CellCoord{1, 1}, locked);
    report = TerrainReport();
    // a neighbour's change would re-choose (1, 1) if it were not locked
    paintAll(store, context, cellsOf({{1, 2}}), NO_TERRAIN, report);
    EXPECT_EQ(store.get(CellCoord{1, 1}), locked);
    // the erased cell and its four other terrain neighbours; (1, 1) is skipped
    EXPECT_EQ(report.rechosen, 1u + 4u);
    TileCellPatch patch;
    TerrainReport refreshed;
    terrain::refresh(store, context, CellRect::of(CellCoord{0, 0}, CellCoord{4, 4}), patch, refreshed);
    EXPECT_EQ(store.get(CellCoord{1, 1}), locked);
    for (const TileCellChange& change : patch.changes())
        EXPECT_FALSE(change.cell == (CellCoord{1, 1}));
}

TEST(TerrainSolver, ErasingALockedCellKeepsItsTileButClearsTheTerrain){
    TilesetData data = blobById();
    TileCells store;
    const TerrainContext context = makeContext(data);
    TerrainReport report;
    paintAll(store, context, cellsOf({{0, 0}}), 0, report);
    const uint32_t locked = makeCellValue(9, TILE_LOCK);
    store.set(CellCoord{0, 0}, locked);
    paintAll(store, context, cellsOf({{0, 0}}), NO_TERRAIN, report);
    EXPECT_EQ(store.get(CellCoord{0, 0}), locked);
    EXPECT_EQ(store.getTerrain(CellCoord{0, 0}), NO_TERRAIN);
}

TEST(TerrainSolver, ErasingTerrainClearsTheTile){
    TilesetData data = blobById();
    TileCells store;
    const TerrainContext context = makeContext(data);
    TerrainReport report;
    paintAll(store, context, boxOf(0, 0, 1, 1), 0, report);
    paintAll(store, context, cellsOf({{0, 0}}), NO_TERRAIN, report);
    EXPECT_EQ(store.get(CellCoord{0, 0}), 0u);
    EXPECT_EQ(store.getTerrain(CellCoord{0, 0}), NO_TERRAIN);
    EXPECT_EQ(store.getTile(CellCoord{1, 1}), cr31IndexFromPlane({{1, 0}, {0, 1}, {1, 1}}, 1, 1));
}

TEST(TerrainSolver, AManualTileNextToTerrainIsLeftAlone){
    TilesetData data = blobById();
    TileCells store;
    const TerrainContext context = makeContext(data);
    store.setTile(CellCoord{1, 0}, 42);
    TerrainReport report;
    paintAll(store, context, cellsOf({{0, 0}}), 0, report);
    EXPECT_EQ(store.getTile(CellCoord{1, 0}), 42);
    EXPECT_EQ(report.rechosen, 1u);
}

TEST(TerrainSolver, DualGridLayersRefuseOtherModes){
    const TilesetData data = blobById();
    TileCells store;
    TileCellPatch patch;
    TerrainReport report;
    std::string error;
    const std::vector<CellCoord> cells = {{0, 0}};
    EXPECT_FALSE(terrain::paint(store, makeContext(data, solverGrid(), true), cells, 0, patch, report, error));
    EXPECT_NE(error.find("dual_grid"), std::string::npos);
    EXPECT_TRUE(store.empty());
}

TEST(TerrainSolver, ADualGridSetNeedsADualGridLayer){
    const TilesetData data = dualByFrame(false);
    TileCells store;
    TileCellPatch patch;
    TerrainReport report;
    std::string error;
    const std::vector<CellCoord> cells = {{0, 0}};
    EXPECT_FALSE(terrain::paint(store, makeContext(data), cells, 0, patch, report, error));
    EXPECT_FALSE(error.empty());
}

TEST(TerrainSolver, DualGridNeedsARectangleOrIsometricGrid){
    const TilesetData data = dualByFrame(false);
    TileCells store;
    TileCellPatch patch;
    TerrainReport report;
    std::string error;
    const std::vector<CellCoord> cells = {{0, 0}};
    EXPECT_FALSE(terrain::paint(store, makeContext(data, solverGrid(TileGridLayout::IsometricStaggered), true), cells, 0, patch, report, error));
    EXPECT_FALSE(terrain::paint(store, makeContext(data, solverGrid(TileGridLayout::Hexagon), true), cells, 0, patch, report, error));
    EXPECT_TRUE(terrain::paint(store, makeContext(data, solverGrid(TileGridLayout::Isometric), true), cells, 0, patch, report, error));
}

TEST(TerrainSolver, UnknownTerrainsAreRefused){
    const TilesetData data = blobById();
    TileCells store;
    TileCellPatch patch;
    TerrainReport report;
    std::string error;
    const std::vector<CellCoord> cells = {{0, 0}};
    EXPECT_FALSE(terrain::paint(store, makeContext(data), cells, 9, patch, report, error));
    EXPECT_FALSE(error.empty());
}

TEST(TerrainSolver, TheSquareAndHexShapesMustMatchTheGrid){
    TilesetData data;
    data.shape = TileShape::HexPointy;
    TerrainSetDef set;
    set.mode = TerrainMode::Sides;
    set.terrains = {{"A", "#000000"}};
    data.terrainSets.push_back(set);
    TileCells store;
    TileCellPatch patch;
    TerrainReport report;
    std::string error;
    const std::vector<CellCoord> cells = {{0, 0}};
    EXPECT_FALSE(terrain::paint(store, makeContext(data), cells, 0, patch, report, error));
}

TEST(TerrainSolver, ATerrainMissingFromTheTilesetIsReportedNotDropped){
    TilesetData data = blobById();
    TileCells store;
    const TerrainContext context = makeContext(data);
    store.setTerrain(CellCoord{0, 0}, 40);
    TileCellPatch patch;
    TerrainReport report;
    terrain::refresh(store, context, CellRect::of(CellCoord{0, 0}, CellCoord{0, 0}), patch, report);
    EXPECT_EQ(report.noTile, cellsOf({{0, 0}}));
    EXPECT_EQ(store.getTerrain(CellCoord{0, 0}), 40);
}

TEST(TerrainSolver, AllowTransformsUsesFlippedVariants){
    TilesetData data = blobById();
    data.tiles.erase(std::remove_if(data.tiles.begin(), data.tiles.end(), [](const TileDef& t){ return t.id == 4; }), data.tiles.end());
    TileCells store;
    TerrainContext context = makeContext(data);
    store.setTerrain(CellCoord{0, 0}, 0);
    store.setTerrain(CellCoord{1, 0}, 0);
    bool matched = false;
    context.allowTransforms = true;
    const uint32_t chosen = terrain::chooseTile(store, context, CellCoord{0, 0}, matched);
    EXPECT_TRUE(matched);
    // tile 4 (east only) is gone; tiles 1, 16 and 64 each reach it through a transform and tie
    EXPECT_NE(chosen & TILE_TRANSFORM_MASK, 0u);
    const TileDef* tile = data.findTile(cellValueTile(chosen));
    ASSERT_NE(tile, nullptr);
    std::array<int16_t, MAX_PEERING_SLOTS> expected;
    expected.fill(PEERING_NONE);
    expected[2] = 0;
    for (size_t slot = MAX_PEERING_SLOTS - 4; slot < MAX_PEERING_SLOTS; slot++)
        expected[slot] = PEERING_ANY;
    const auto shown = terrain::transformPeering(tile->terrain.peering, chosen & TILE_TRANSFORM_MASK);
    EXPECT_EQ(shown, expected);
}

TEST(TerrainSolver, TransformsMapThePeeringAsTiledDoes){
    std::array<int16_t, MAX_PEERING_SLOTS> peering;
    peering.fill(PEERING_NONE);
    peering[0] = 0;   // n
    const auto flipped = terrain::transformPeering(peering, TILE_FLIP_Y);
    EXPECT_EQ(flipped[4], 0);
    EXPECT_EQ(flipped[0], PEERING_NONE);
    const auto transposed = terrain::transformPeering(peering, TILE_TRANSPOSE);
    EXPECT_EQ(transposed[6], 0);     // transposing the image swaps n and w
    // the transpose runs first: n -> w, then flip X sends w to e
    const auto both = terrain::transformPeering(peering, TILE_TRANSPOSE | TILE_FLIP_X);
    EXPECT_EQ(both[2], 0);
}

TEST(TerrainSolver, StaggeredGridsUseDiamondNeighbours){
    TilesetData data = blobById();
    data.shape = TileShape::Isometric;
    const TileGridSpec grid = solverGrid(TileGridLayout::IsometricStaggered);
    const TerrainContext context = makeContext(data, grid);
    TileCells store;
    const CellCoord center{3, 4};
    TerrainReport report;
    paintAll(store, context, cellsOf({center, tilegrid::neighbour(grid, center, 0)}), 0, report);
    EXPECT_EQ(store.getTile(center), 1);    // n only
}

TEST(TileCellPatch, RecordsFirstOldAndLastNewValues){
    TileCells store;
    store.setTile(CellCoord{0, 0}, 1);
    TileCellPatch patch;
    patch.setValue(store, CellCoord{0, 0}, makeCellValue(2, 0));
    patch.setValue(store, CellCoord{0, 0}, makeCellValue(3, 0));
    patch.setTerrainValue(store, CellCoord{1, 0}, 4);
    const std::vector<TileCellChange> changes = patch.changes();
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[0].cell, (CellCoord{0, 0}));
    EXPECT_EQ(changes[0].oldValue, makeCellValue(1, 0));
    EXPECT_EQ(changes[0].newValue, makeCellValue(3, 0));
    EXPECT_EQ(changes[1].newTerrain, 4);
    EXPECT_TRUE(patch.contains(CellCoord{1, 0}));
}

TEST(TileCellPatch, ACellPutBackIsNotAChange){
    TileCells store;
    store.setTile(CellCoord{0, 0}, 1);
    TileCellPatch patch;
    patch.setValue(store, CellCoord{0, 0}, makeCellValue(2, 0));
    patch.setValue(store, CellCoord{0, 0}, makeCellValue(1, 0));
    EXPECT_TRUE(patch.changes().empty());
    EXPECT_FALSE(patch.empty());
}

TEST(TileCellPatch, RevertRestoresEveryCell){
    TileCells store;
    store.setTile(CellCoord{5, 5}, 1);
    store.setTerrain(CellCoord{5, 5}, 2);
    const TileCells before = store;
    const TilesetData data = blobById();
    TileCellPatch patch;
    TerrainReport report;
    paintAll(store, makeContext(data), boxOf(0, 0, 9, 9), 0, report, &patch);
    EXPECT_NE(store, before);
    patch.revert(store);
    EXPECT_EQ(store, before);
    patch.clear();
    EXPECT_TRUE(patch.empty());
}

TEST(TileCellPatch, FollowsTheStoresNormalisationOfEmptyValues){
    TileCells store;
    TileCellPatch patch;
    patch.setValue(store, CellCoord{0, 0}, TILE_FLIP_X);
    EXPECT_TRUE(patch.changes().empty());
}
