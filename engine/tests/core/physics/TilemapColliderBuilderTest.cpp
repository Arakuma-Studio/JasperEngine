// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The tilemap collider builder (docs/specs/S50-grid-tilemap.md R38-R43, D8, AC7): pure geometry,
// no Box2D. Chains stitched across chunks prove the three-point overlap on every layout.

#include <gtest/gtest.h>

#include "tilemap/TileColliderBuilder.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <vector>

using namespace jasperengine;

namespace {

    enum TestTile : int32_t { FULL = 0, TRIANGLE = 1, ONE_WAY = 2, SENSOR = 3, L_SHAPE = 4 };

    TilesetData testTileset() {
        TilesetData data;
        data.tileWidth = 16;
        data.tileHeight = 16;
        TilesetSource source;
        source.id = 0;
        data.sources.push_back(source);
        auto add = [&](TileCollisionMode mode, TileCollisionKind kind, std::vector<TilePolygon> polygons) {
            TileDef& tile = data.addTile(0, (int)data.tiles.size(), 0);
            tile.collision.mode = mode;
            tile.collision.kind = kind;
            tile.collision.polygons = std::move(polygons);
        };
        add(TileCollisionMode::Full, TileCollisionKind::Solid, {});
        add(TileCollisionMode::Custom, TileCollisionKind::Solid, {TilePolygon{{{{0, 0}}, {{8, 0}}, {{0, 8}}}}});
        add(TileCollisionMode::Full, TileCollisionKind::OneWay, {});
        add(TileCollisionMode::Full, TileCollisionKind::Sensor, {});
        add(TileCollisionMode::Custom, TileCollisionKind::Solid,
            {TilePolygon{{{{0, 0}}, {{16, 0}}, {{16, 8}}, {{8, 8}}, {{8, 16}}, {{0, 16}}}}});
        return data;
    }

    TileColliderSettings settingsFor(const TileCells& cells, const TilesetData& tileset) {
        TileColliderSettings settings;
        settings.cells = &cells;
        settings.tileset = &tileset;
        return settings;
    }

    double signedArea(const std::vector<TilePoint>& points) {
        double area = 0.0;
        for (size_t i = 0; i < points.size(); i++) {
            const TilePoint& a = points[i];
            const TilePoint& b = points[(i + 1) % points.size()];
            area += a.x * b.y - b.x * a.y;
        }
        return area * 0.5;
    }

    // the winding number of `point` around all the rings: non-zero is inside the solid
    int windingAt(const std::vector<std::vector<TilePoint>>& rings, TilePoint point) {
        int winding = 0;
        for (const std::vector<TilePoint>& ring : rings) {
            for (size_t i = 0; i < ring.size(); i++) {
                const TilePoint& a = ring[i];
                const TilePoint& b = ring[(i + 1) % ring.size()];
                const double side = (b.x - a.x) * (point.y - a.y) - (b.y - a.y) * (point.x - a.x);
                if (a.y <= point.y && b.y > point.y && side > 0.0) {
                    winding++;
                } else if (a.y > point.y && b.y <= point.y && side < 0.0) {
                    winding--;
                }
            }
        }
        return winding;
    }

    bool samePoint(const TilePoint& a, const TilePoint& b) {
        return a.x == b.x && a.y == b.y;
    }

    std::string describe(const TilePoint& p) {
        return "(" + std::to_string(p.x) + ", " + std::to_string(p.y) + ")";
    }

    void expectSpaced(const std::vector<TilePoint>& points, bool loop) {
        const size_t count = points.size();
        for (size_t i = 0; i + (loop ? 0 : 1) < count; i++) {
            const TilePoint& a = points[i];
            const TilePoint& b = points[(i + 1) % count];
            EXPECT_GT(std::hypot(b.x - a.x, b.y - a.y), 0.32) << describe(a) << " to " << describe(b);
        }
    }

    // every chunk's colliders of the rectangle of chunks
    std::vector<TileColliderChunk> buildAll(const TileColliderSettings& settings, int minX, int minY, int maxX, int maxY) {
        std::vector<TileColliderChunk> result;
        TileColliderBuilder builder(settings);
        for (int y = minY; y <= maxY; y++) {
            for (int x = minX; x <= maxX; x++) {
                result.push_back(builder.build(ChunkCoord{x, y}));
            }
        }
        return result;
    }

    // the chains stitched back into closed rings, every junction checked: the last three points
    // of a chain are the first three of the next
    std::vector<std::vector<TilePoint>> stitchRings(const std::vector<TileColliderChunk>& chunks) {
        std::vector<std::vector<TilePoint>> rings;
        std::vector<const TileColliderChain*> open;
        for (const TileColliderChunk& chunk : chunks) {
            for (const TileColliderChain& chain : chunk.loops) {
                EXPECT_TRUE(chain.loop);
                EXPECT_GE(chain.points.size(), 4u);
                expectSpaced(chain.points, true);
                rings.push_back(chain.points);
            }
            for (const TileColliderChain& chain : chunk.chains) {
                EXPECT_FALSE(chain.loop);
                EXPECT_GE(chain.points.size(), 4u);
                expectSpaced(chain.points, false);
                open.push_back(&chain);
            }
        }
        std::map<std::pair<double, double>, const TileColliderChain*> byStart;
        for (const TileColliderChain* chain : open) {
            const TilePoint& start = chain->points[1];
            const bool fresh = byStart.emplace(std::make_pair(start.x, start.y), chain).second;
            EXPECT_TRUE(fresh) << "two chains start at " << describe(start);
        }
        std::set<const TileColliderChain*> used;
        for (const TileColliderChain* first : open) {
            if (used.count(first)) {
                continue;
            }
            std::vector<TilePoint> ring;
            const TileColliderChain* chain = first;
            while (used.insert(chain).second) {
                const size_t count = chain->points.size();
                ring.insert(ring.end(), chain->points.begin() + 1, chain->points.end() - 2);
                const TilePoint& end = chain->points[count - 2];
                auto next = byStart.find(std::make_pair(end.x, end.y));
                if (next == byStart.end()) {
                    ADD_FAILURE() << "no chain continues at " << describe(end);
                    break;
                }
                const std::vector<TilePoint>& nextPoints = next->second->points;
                EXPECT_TRUE(samePoint(chain->points[count - 3], nextPoints[0]))
                    << describe(chain->points[count - 3]) << " != " << describe(nextPoints[0]);
                EXPECT_TRUE(samePoint(chain->points[count - 1], nextPoints[2]))
                    << describe(chain->points[count - 1]) << " != " << describe(nextPoints[2]);
                chain = next->second;
            }
            rings.push_back(std::move(ring));
        }
        return rings;
    }

    std::vector<CellCoord> patternCells() {
        // a block with a hole, a bridge over a chunk border, a loose cell
        std::vector<CellCoord> cells;
        for (int y = 5; y < 22; y++) {
            for (int x = 4; x < 28; x++) {
                const bool hole = x >= 12 && x < 20 && y >= 11 && y < 16;
                if (!hole) {
                    cells.push_back(CellCoord{x, y});
                }
            }
        }
        cells.push_back(CellCoord{31, 31});
        cells.push_back(CellCoord{-3, -2});
        cells.push_back(CellCoord{-2, -2});
        return cells;
    }

    void expectMatchesCells(const TileGridSpec& grid, int chunkSize) {
        const TilesetData tileset = testTileset();
        TileCells cells(chunkSize);
        std::set<std::pair<int, int>> solid;
        for (const CellCoord& c : patternCells()) {
            cells.setTile(c, FULL);
            solid.insert({c.x, c.y});
        }
        TileColliderSettings settings = settingsFor(cells, tileset);
        settings.grid = grid;
        const std::vector<TileColliderChunk> chunks = buildAll(settings, -2, -2, 5, 5);
        const std::vector<std::vector<TilePoint>> rings = stitchRings(chunks);
        EXPECT_FALSE(rings.empty());

        int inside = 0;
        for (int y = -6; y < 36; y++) {
            for (int x = -6; x < 36; x++) {
                const TilePoint center = tilegrid::cellCenter(grid, CellCoord{x, y});
                const bool expected = solid.count({x, y}) > 0;
                ASSERT_EQ(windingAt(rings, center) != 0, expected) << "cell " << x << ", " << y;
                inside += expected ? 1 : 0;
            }
        }
        EXPECT_GT(inside, 300);
    }

}

TEST(TilemapColliderBuilder, SingleFullCellIsCounterClockwiseLoop) {
    const TilesetData tileset = testTileset();
    TileCells cells(8);
    cells.setTile(CellCoord{2, 3}, FULL);
    const TileColliderChunk chunk = TileColliderBuilder(settingsFor(cells, tileset)).build(ChunkCoord{0, 0});
    ASSERT_EQ(chunk.loops.size(), 1u);
    EXPECT_TRUE(chunk.chains.empty());
    const std::vector<TilePoint>& points = chunk.loops[0].points;
    ASSERT_EQ(points.size(), 4u);
    EXPECT_GT(signedArea(points), 0.0);
    EXPECT_DOUBLE_EQ(signedArea(points), 256.0);
    EXPECT_TRUE(samePoint(points[0], TilePoint{32.0, 48.0})) << describe(points[0]);
}

TEST(TilemapColliderBuilder, HolesAreClockwiseAndSolidsCounterClockwise) {
    const TilesetData tileset = testTileset();
    TileCells cells(8);
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 3; x++) {
            if (x != 1 || y != 1) {
                cells.setTile(CellCoord{x, y}, FULL);
            }
        }
    }
    const TileColliderChunk chunk = TileColliderBuilder(settingsFor(cells, tileset)).build(ChunkCoord{0, 0});
    ASSERT_EQ(chunk.loops.size(), 2u);
    std::vector<double> areas = {signedArea(chunk.loops[0].points), signedArea(chunk.loops[1].points)};
    std::sort(areas.begin(), areas.end());
    EXPECT_DOUBLE_EQ(areas[0], -256.0);
    EXPECT_DOUBLE_EQ(areas[1], 48.0 * 48.0);
}

TEST(TilemapColliderBuilder, TriangleLoopGetsAFourthPoint) {
    const TilesetData tileset = testTileset();
    TileCells cells(8);
    cells.setTile(CellCoord{0, 0}, TRIANGLE);
    const TileColliderChunk chunk = TileColliderBuilder(settingsFor(cells, tileset)).build(ChunkCoord{0, 0});
    ASSERT_EQ(chunk.loops.size(), 1u);
    EXPECT_EQ(chunk.loops[0].points.size(), 4u);
    EXPECT_DOUBLE_EQ(signedArea(chunk.loops[0].points), 32.0);
}

TEST(TilemapColliderBuilder, OutlinesMatchTheCellsRectangle) {
    TileGridSpec grid;
    expectMatchesCells(grid, 8);
    expectMatchesCells(grid, 32);
}

TEST(TilemapColliderBuilder, OutlinesMatchTheCellsIsometric) {
    TileGridSpec grid;
    grid.layout = TileGridLayout::Isometric;
    grid.cellWidth = 32.0;
    grid.cellHeight = 16.0;
    expectMatchesCells(grid, 8);
}

TEST(TilemapColliderBuilder, OutlinesMatchTheCellsStaggered) {
    TileGridSpec grid;
    grid.layout = TileGridLayout::IsometricStaggered;
    grid.cellWidth = 32.0;
    grid.cellHeight = 16.0;
    expectMatchesCells(grid, 8);
}

TEST(TilemapColliderBuilder, OutlinesMatchTheCellsHexagonPointy) {
    TileGridSpec grid;
    grid.layout = TileGridLayout::Hexagon;
    grid.cellWidth = 16.0;
    grid.cellHeight = 16.0;
    expectMatchesCells(grid, 8);
}

TEST(TilemapColliderBuilder, OutlinesMatchTheCellsHexagonFlat) {
    TileGridSpec grid;
    grid.layout = TileGridLayout::Hexagon;
    grid.staggerAxis = StaggerAxis::X;
    grid.cellWidth = 16.0;
    grid.cellHeight = 16.0;
    expectMatchesCells(grid, 8);
}

TEST(TilemapColliderBuilder, ThreePointOverlapOnManyChunkFloor) {
    const TilesetData tileset = testTileset();
    TileCells cells(8);
    for (int x = 0; x < 64; x++) {
        cells.setTile(CellCoord{x, 0}, FULL);
        cells.setTile(CellCoord{x, 1}, FULL);
    }
    const std::vector<TileColliderChunk> chunks = buildAll(settingsFor(cells, tileset), 0, 0, 7, 0);
    // the middle chunks: one chain along the top, one along the bottom
    for (int c = 1; c < 7; c++) {
        ASSERT_EQ(chunks[(size_t)c].chains.size(), 2u) << "chunk " << c;
        EXPECT_TRUE(chunks[(size_t)c].loops.empty());
    }

    // the top surface runs right to left: its chain in chunk c starts at c + 1's border
    auto topChain = [&](int c) -> const std::vector<TilePoint>& {
        for (const TileColliderChain& chain : chunks[(size_t)c].chains) {
            if (chain.points[0].y == 32.0) {
                return chain.points;
            }
        }
        static const std::vector<TilePoint> none;
        return none;
    };
    for (int c = 1; c < 6; c++) {
        const std::vector<TilePoint>& right = topChain(c + 1);
        const std::vector<TilePoint>& left = topChain(c);
        ASSERT_GE(right.size(), 4u);
        ASSERT_GE(left.size(), 4u);
        const size_t n = right.size();
        EXPECT_TRUE(samePoint(right[n - 3], left[0])) << describe(right[n - 3]) << " " << describe(left[0]);
        EXPECT_TRUE(samePoint(right[n - 2], left[1]));
        EXPECT_TRUE(samePoint(right[n - 1], left[2]));
        // the crossing is at the border, the ghost one cell beyond it
        EXPECT_DOUBLE_EQ(left[1].x, (c + 1) * 128.0);
        EXPECT_DOUBLE_EQ(left[0].x, (c + 1) * 128.0 + 16.0);
        EXPECT_DOUBLE_EQ(right[n - 1].x, (c + 1) * 128.0 - 16.0);
    }
    const std::vector<std::vector<TilePoint>> rings = stitchRings(chunks);
    ASSERT_EQ(rings.size(), 1u);
    EXPECT_DOUBLE_EQ(signedArea(rings[0]), 64.0 * 16.0 * 32.0);
}

TEST(TilemapColliderBuilder, OneWayChainsFollowOnlyFacingEdges) {
    const TilesetData tileset = testTileset();
    TileCells cells(8);
    for (int x = 2; x < 14; x++) {
        cells.setTile(CellCoord{x, 3}, ONE_WAY);
    }
    const std::vector<TileColliderChunk> chunks = buildAll(settingsFor(cells, tileset), 0, 0, 1, 0);
    size_t chains = 0;
    for (const TileColliderChunk& chunk : chunks) {
        EXPECT_TRUE(chunk.loops.empty());
        EXPECT_TRUE(chunk.chains.empty());
        chains += chunk.oneWay.size();
        for (const TileColliderChain& chain : chunk.oneWay) {
            EXPECT_FALSE(chain.loop);
            ASSERT_GE(chain.points.size(), 4u);
            // right to left: the collision normal (to the right of the direction) points up
            for (size_t i = 1; i + 2 < chain.points.size(); i++) {
                EXPECT_DOUBLE_EQ(chain.points[i].y, 64.0);
                EXPECT_LT(chain.points[i + 1].x, chain.points[i].x);
            }
        }
    }
    EXPECT_EQ(chains, 2u);

    // the row's facing edges are one chain cut at the border, with the overlap
    ASSERT_EQ(chunks[0].oneWay.size(), 1u);
    ASSERT_EQ(chunks[1].oneWay.size(), 1u);
    const std::vector<TilePoint>& right = chunks[1].oneWay[0].points;
    const std::vector<TilePoint>& left = chunks[0].oneWay[0].points;
    const size_t n = right.size();
    EXPECT_DOUBLE_EQ(right[n - 2].x, 128.0);
    EXPECT_TRUE(samePoint(right[n - 3], left[0]));
    EXPECT_TRUE(samePoint(right[n - 2], left[1]));
    EXPECT_TRUE(samePoint(right[n - 1], left[2]));
}

TEST(TilemapColliderBuilder, OneWayDirectionFollowsFlipsAndRotates) {
    const TilesetData tileset = testTileset();
    TileCells cells(8);
    cells.setTile(CellCoord{4, 4}, ONE_WAY, TILE_FLIP_Y);
    TileColliderSettings settings = settingsFor(cells, tileset);
    const TileColliderChunk chunk = TileColliderBuilder(settings).build(ChunkCoord{0, 0});
    ASSERT_EQ(chunk.oneWay.size(), 1u);
    const std::vector<TilePoint>& points = chunk.oneWay[0].points;
    // a flipped platform faces down: its bottom edge, left to right
    ASSERT_GE(points.size(), 4u);
    EXPECT_DOUBLE_EQ(points[1].y, 64.0);
    EXPECT_GT(points[2].x, points[1].x);
}

TEST(TilemapColliderBuilder, SensorsArePolygonsNeverChains) {
    const TilesetData tileset = testTileset();
    TileCells cells(8);
    cells.setTile(CellCoord{1, 1}, SENSOR);
    const TileColliderChunk chunk = TileColliderBuilder(settingsFor(cells, tileset)).build(ChunkCoord{0, 0});
    EXPECT_TRUE(chunk.loops.empty());
    EXPECT_TRUE(chunk.chains.empty());
    EXPECT_TRUE(chunk.oneWay.empty());
    ASSERT_EQ(chunk.sensors.size(), 1u);
    EXPECT_EQ(chunk.sensors[0].points.size(), 4u);
    EXPECT_DOUBLE_EQ(signedArea(chunk.sensors[0].points), 256.0);
}

TEST(TilemapColliderBuilder, FlipsAndTransposeApplyToShapes) {
    const TilesetData tileset = testTileset();
    // the triangle sits in the corner (0, 0), (8, 0), (0, 8) of the 16 px tile
    auto vertices = [&](uint32_t flags) {
        TileCells cells(8);
        cells.setTile(CellCoord{0, 0}, TRIANGLE, flags);
        const TileColliderChunk chunk = TileColliderBuilder(settingsFor(cells, tileset)).build(ChunkCoord{0, 0});
        std::set<std::pair<double, double>> points;
        if (!chunk.loops.empty()) {
            for (const TilePoint& p : chunk.loops[0].points) {
                points.insert({p.x, p.y});
            }
        }
        return points;
    };
    using Set = std::set<std::pair<double, double>>;
    // each set includes the point the fourth vertex adds on the hypotenuse
    EXPECT_EQ(vertices(0), (Set{{0, 0}, {8, 0}, {0, 8}, {4, 4}}));
    EXPECT_EQ(vertices(TILE_FLIP_X), (Set{{16, 0}, {8, 0}, {16, 8}, {12, 4}}));
    EXPECT_EQ(vertices(TILE_FLIP_Y), (Set{{0, 16}, {8, 16}, {0, 8}, {4, 12}}));
    EXPECT_EQ(vertices(TILE_FLIP_X | TILE_FLIP_Y), (Set{{16, 16}, {8, 16}, {16, 8}, {12, 12}}));
    // Tiled's diagonal flip, in y-up tile pixels: (x, y) -> (16 - y, 16 - x)
    EXPECT_EQ(vertices(TILE_TRANSPOSE), (Set{{16, 16}, {16, 8}, {8, 16}, {12, 12}}));
}

TEST(TilemapColliderBuilder, TerrainSourceUsesLogicalCellsOnDualGrid) {
    TilesetData tileset = testTileset();
    TerrainSetDef terrainSet;
    terrainSet.name = "ground";
    terrainSet.mode = TerrainMode::DualGrid;
    terrainSet.terrains.push_back(TerrainDef{"grass", "#00ff00"});
    terrainSet.terrains.push_back(TerrainDef{"water", "#0000ff"});
    tileset.terrainSets.push_back(terrainSet);

    TileCells cells(8);
    cells.setTerrain(CellCoord{2, 2}, 0);
    cells.setTerrain(CellCoord{3, 2}, 0);
    cells.setTerrain(CellCoord{4, 2}, 1);
    // a tile that would collide if the tiles were the source
    cells.setTile(CellCoord{6, 6}, FULL);

    TileColliderSettings settings = settingsFor(cells, tileset);
    settings.source = TileColliderSource::Terrain;
    settings.solidTerrains = {0};
    settings.dualGrid = true;
    const TileColliderChunk chunk = TileColliderBuilder(settings).build(ChunkCoord{0, 0});
    ASSERT_EQ(chunk.loops.size(), 1u);
    EXPECT_DOUBLE_EQ(signedArea(chunk.loops[0].points), 2.0 * 256.0);
    // the logical cells 2 and 3, not moved half a cell like the drawn tiles
    double minX = 1e9, minY = 1e9;
    for (const TilePoint& p : chunk.loops[0].points) {
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
    }
    EXPECT_DOUBLE_EQ(minX, 32.0);
    EXPECT_DOUBLE_EQ(minY, 32.0);
}

TEST(TilemapColliderBuilder, TilesSourceOnDualGridIsDrawnPosition) {
    const TilesetData tileset = testTileset();
    TileCells cells(8);
    cells.setTile(CellCoord{3, 3}, FULL);
    TileColliderSettings settings = settingsFor(cells, tileset);
    settings.dualGrid = true;
    const TileColliderChunk chunk = TileColliderBuilder(settings).build(ChunkCoord{0, 0});
    ASSERT_EQ(chunk.loops.size(), 1u);
    EXPECT_TRUE(samePoint(chunk.loops[0].points[0], TilePoint{40.0, 40.0})) << describe(chunk.loops[0].points[0]);
}

TEST(TilemapColliderBuilder, OffsetAndTileAnchorMoveShapes) {
    const TilesetData tileset = testTileset();
    TileCells cells(8);
    cells.setTile(CellCoord{1, 1}, FULL);
    TileColliderSettings settings = settingsFor(cells, tileset);
    settings.offset = TilePoint{3.0, -2.0};
    settings.tileAnchor = TilePoint{1.0, 0.0};
    const TileColliderChunk chunk = TileColliderBuilder(settings).build(ChunkCoord{0, 0});
    ASSERT_EQ(chunk.loops.size(), 1u);
    EXPECT_TRUE(samePoint(chunk.loops[0].points[0], TilePoint{35.0, 14.0})) << describe(chunk.loops[0].points[0]);
}

TEST(TilemapColliderBuilder, PolygonsGeometryIsConvexAndCoversTheUnion) {
    const TilesetData tileset = testTileset();
    TileCells cells(8);
    // a ring around a hole, an L-shaped custom tile and a triangle tile
    for (int y = 0; y < 5; y++) {
        for (int x = 0; x < 5; x++) {
            if (x < 2 || x > 2 || y < 2 || y > 2) {
                cells.setTile(CellCoord{x, y}, FULL);
            }
        }
    }
    cells.setTile(CellCoord{6, 6}, L_SHAPE);
    cells.setTile(CellCoord{7, 0}, TRIANGLE);
    TileColliderSettings settings = settingsFor(cells, tileset);
    settings.geometry = TileColliderGeometry::Polygons;
    const TileColliderChunk chunk = TileColliderBuilder(settings).build(ChunkCoord{0, 0});
    EXPECT_TRUE(chunk.loops.empty());
    EXPECT_TRUE(chunk.chains.empty());
    ASSERT_FALSE(chunk.polygons.empty());
    double area = 0.0;
    for (const TileColliderPolygon& polygon : chunk.polygons) {
        ASSERT_GE(polygon.points.size(), 3u);
        EXPECT_LE(polygon.points.size(), 8u);
        EXPECT_GT(signedArea(polygon.points), 0.0);
        const size_t n = polygon.points.size();
        for (size_t i = 0; i < n; i++) {
            const TilePoint& a = polygon.points[i];
            const TilePoint& b = polygon.points[(i + 1) % n];
            const TilePoint& c = polygon.points[(i + 2) % n];
            EXPECT_GE((b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x), -1e-3) << "reflex vertex at " << describe(b);
        }
        area += signedArea(polygon.points);
    }
    // the ring's 24 cells, the L (3 quarters of a cell) and the triangle
    EXPECT_NEAR(area, 24.0 * 256.0 + 192.0 + 32.0, 1e-3);
}

TEST(TilemapColliderBuilder, PolygonsGeometryIsClippedToTheChunk) {
    const TilesetData tileset = testTileset();
    TileCells cells(8);
    for (int x = 4; x < 12; x++) {
        cells.setTile(CellCoord{x, 2}, FULL);
    }
    TileColliderSettings settings = settingsFor(cells, tileset);
    settings.geometry = TileColliderGeometry::Polygons;
    const std::vector<TileColliderChunk> chunks = buildAll(settings, 0, 0, 1, 0);
    double areas[2] = {0.0, 0.0};
    for (int c = 0; c < 2; c++) {
        for (const TileColliderPolygon& polygon : chunks[(size_t)c].polygons) {
            areas[c] += signedArea(polygon.points);
            for (const TilePoint& p : polygon.points) {
                EXPECT_GE(p.x, c * 128.0);
                EXPECT_LE(p.x, (c + 1) * 128.0);
            }
        }
    }
    EXPECT_DOUBLE_EQ(areas[0], 4.0 * 256.0);
    EXPECT_DOUBLE_EQ(areas[1], 4.0 * 256.0);
}

TEST(TilemapColliderBuilder, MergeOffGivesOneLoopPerCell) {
    const TilesetData tileset = testTileset();
    TileCells cells(8);
    cells.setTile(CellCoord{1, 1}, FULL);
    cells.setTile(CellCoord{2, 1}, FULL);
    TileColliderSettings settings = settingsFor(cells, tileset);
    settings.merge = false;
    const TileColliderChunk chunk = TileColliderBuilder(settings).build(ChunkCoord{0, 0});
    EXPECT_EQ(chunk.loops.size(), 2u);
    EXPECT_TRUE(chunk.chains.empty());
}

TEST(TilemapColliderBuilder, OutputIsDeterministic) {
    const TilesetData tileset = testTileset();
    TileCells cells(8);
    for (const CellCoord& c : patternCells()) {
        cells.setTile(c, FULL);
    }
    const TileColliderBuilder builder(settingsFor(cells, tileset));
    for (int y = -1; y <= 3; y++) {
        for (int x = -1; x <= 3; x++) {
            EXPECT_TRUE(builder.build(ChunkCoord{x, y}) == builder.build(ChunkCoord{x, y}));
        }
    }
}

TEST(TilemapColliderBuilder, AnEditChangesOnlyTheTouchedAndBorderSharingChunks) {
    const TilesetData tileset = testTileset();
    TileCells cells(8);
    for (int y = -8; y < 16; y++) {
        for (int x = -8; x < 24; x++) {
            cells.setTile(CellCoord{x, y}, FULL);
        }
    }
    const TileColliderSettings settings = settingsFor(cells, tileset);
    const TileColliderBuilder builder(settings);
    EXPECT_EQ(builder.chunkReach(), 1);
    auto snapshot = [&]() {
        std::map<std::pair<int, int>, TileColliderChunk> all;
        for (int y = -2; y <= 2; y++) {
            for (int x = -2; x <= 3; x++) {
                all[{x, y}] = builder.build(ChunkCoord{x, y});
            }
        }
        return all;
    };
    const auto before = snapshot();

    // an inner cell of chunk (0, 0): its outline gets a hole, only that chunk changes
    cells.setTile(CellCoord{3, 3}, NO_TILE);
    auto after = snapshot();
    for (const auto& [coord, chunk] : before) {
        const bool touched = coord == std::make_pair(0, 0);
        EXPECT_EQ(after.at(coord) == chunk, !touched) << coord.first << ", " << coord.second;
    }
    cells.setTile(CellCoord{3, 3}, FULL);

    // a hole across the border between chunks (0, 0) and (1, 0): both change, nothing else
    cells.setTile(CellCoord{7, 3}, NO_TILE);
    cells.setTile(CellCoord{8, 3}, NO_TILE);
    after = snapshot();
    for (const auto& [coord, chunk] : before) {
        const bool touched = coord == std::make_pair(0, 0) || coord == std::make_pair(1, 0);
        EXPECT_EQ(after.at(coord) == chunk, !touched) << coord.first << ", " << coord.second;
    }
}
