// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// AC2 (docs/specs/S50-grid-tilemap.md R1, R7, D5). Each layout's positions are compared with Tiled's
// own formulas (src/libtiled/*renderer.cpp, restated below), not with the code under test.

#include "tilemap/TileGridLayout.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

using namespace jasperengine;

namespace {

    TileGridSpec layoutSpec(TileGridLayout layout, StaggerAxis axis = StaggerAxis::Y, StaggerIndex index = StaggerIndex::Odd, double w = 32, double h = 16, double side = 0){
        TileGridSpec spec;
        spec.layout = layout;
        spec.cellWidth = w;
        spec.cellHeight = h;
        spec.staggerAxis = axis;
        spec.staggerIndex = index;
        spec.hexSideLength = side;
        return spec;
    }

    struct NamedSpec{
        std::string name;
        TileGridSpec spec;
    };

    std::vector<NamedSpec> allLayouts(){
        std::vector<NamedSpec> out;
        out.push_back({"rectangle", layoutSpec(TileGridLayout::Rectangle)});
        out.push_back({"isometric", layoutSpec(TileGridLayout::Isometric)});
        for (StaggerAxis axis : {StaggerAxis::Y, StaggerAxis::X}){
            for (StaggerIndex index : {StaggerIndex::Odd, StaggerIndex::Even}){
                const std::string suffix = std::string(axis == StaggerAxis::Y ? "_y" : "_x") + (index == StaggerIndex::Odd ? "_odd" : "_even");
                out.push_back({"staggered" + suffix, layoutSpec(TileGridLayout::IsometricStaggered, axis, index)});
                out.push_back({"hex" + suffix, layoutSpec(TileGridLayout::Hexagon, axis, index, 28, 32)});
                out.push_back({"hex_side8" + suffix, layoutSpec(TileGridLayout::Hexagon, axis, index, 28, 32, 8)});
            }
        }
        NamedSpec gapped{"rectangle_gap_fraction", layoutSpec(TileGridLayout::Rectangle, StaggerAxis::Y, StaggerIndex::Odd, 16.3, 7.7)};
        gapped.spec.gapX = 0.1;
        gapped.spec.gapY = 0.2;
        out.push_back(gapped);
        NamedSpec isoGap{"isometric_fraction", layoutSpec(TileGridLayout::Isometric, StaggerAxis::Y, StaggerIndex::Odd, 16.3, 7.7)};
        isoGap.spec.gapX = 0.1;
        out.push_back(isoGap);
        NamedSpec hexFrac{"hex_fraction", layoutSpec(TileGridLayout::Hexagon, StaggerAxis::Y, StaggerIndex::Even, 13.7, 15.1, 7.3)};
        hexFrac.spec.gapX = 0.3;
        hexFrac.spec.gapY = 0.2;
        out.push_back(hexFrac);
        return out;
    }

    bool sameCell(CellCoord a, CellCoord b){
        return a == b;
    }

    std::string cellText(CellCoord c){
        return "(" + std::to_string(c.x) + ", " + std::to_string(c.y) + ")";
    }

    const double NUDGE_UP = 1e-5;
    const double NUDGE_RIGHT = 1e-9;

}

TEST(TileGridLayout, LocalToCellInvertsCellToLocalOverALargeRange){
    for (const NamedSpec& named : allLayouts()){
        for (int y = -100; y < 100; y++){
            for (int x = -100; x < 100; x++){
                const CellCoord c{x, y};
                ASSERT_TRUE(sameCell(tilegrid::localToCell(named.spec, tilegrid::cellToLocal(named.spec, c)), c)) << named.name << " " << cellText(c);
            }
        }
    }
}

TEST(TileGridLayout, TheCentreOfEveryCellBelongsToIt){
    for (const NamedSpec& named : allLayouts()){
        for (int y = -30; y < 30; y++){
            for (int x = -30; x < 30; x++){
                const CellCoord c{x, y};
                EXPECT_TRUE(sameCell(tilegrid::localToCell(named.spec, tilegrid::cellCenter(named.spec, c)), c)) << named.name << " " << cellText(c);
            }
        }
    }
}

TEST(TileGridLayout, CoordinatesFarFromTheOriginRoundTrip){
    const int32_t far = 1 << 28;
    for (const NamedSpec& named : allLayouts()){
        for (CellCoord c : {CellCoord{far, far}, CellCoord{-far, far}, CellCoord{far - 1, -far}, CellCoord{-far + 3, -far - 1}})
            EXPECT_TRUE(sameCell(tilegrid::localToCell(named.spec, tilegrid::cellToLocal(named.spec, c)), c)) << named.name << " " << cellText(c);
    }
}

// The rule is "up, then right". The nudged point is off every boundary, so where it lands does not
// depend on how a boundary is decided.
TEST(TileGridLayout, PointsOnEdgesFollowTheHalfOpenRule){
    for (const NamedSpec& named : allLayouts()){
        // cell sizes that are exact in binary keep the probe points truly on the boundaries
        if (named.name.find("fraction") != std::string::npos)
            continue;
        for (int y = -6; y < 6; y++){
            for (int x = -6; x < 6; x++){
                const TileCellPolygon polygon = tilegrid::cellPolygon(named.spec, CellCoord{x, y});
                for (int i = 0; i < polygon.count; i++){
                    const TilePoint a = polygon.points[(size_t)i];
                    const TilePoint b = polygon.points[(size_t)((i + 1) % polygon.count)];
                    for (double t : {0.0, 0.25, 0.5}){
                        const TilePoint p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                        const TilePoint nudged{p.x + NUDGE_RIGHT, p.y + NUDGE_UP};
                        const CellCoord expected = tilegrid::localToCell(named.spec, nudged);
                        EXPECT_TRUE(sameCell(tilegrid::localToCell(named.spec, p), expected))
                            << named.name << " cell " << cellText(CellCoord{x, y}) << " edge " << i << " t " << t;
                    }
                }
            }
        }
    }
}

TEST(TileGridLayout, ASharedEdgeBelongsToTheCellAboveOrToTheRight){
    const TileGridSpec rect = layoutSpec(TileGridLayout::Rectangle, StaggerAxis::Y, StaggerIndex::Odd, 16, 16);
    EXPECT_EQ(tilegrid::localToCell(rect, TilePoint{16, 8}), (CellCoord{1, 0}));
    EXPECT_EQ(tilegrid::localToCell(rect, TilePoint{8, 16}), (CellCoord{0, 1}));
    EXPECT_EQ(tilegrid::localToCell(rect, TilePoint{16, 16}), (CellCoord{1, 1}));
    EXPECT_EQ(tilegrid::localToCell(rect, TilePoint{0, 0}), (CellCoord{0, 0}));
    EXPECT_EQ(tilegrid::localToCell(rect, TilePoint{-0.5, -0.5}), (CellCoord{-1, -1}));

    const TileGridSpec iso = layoutSpec(TileGridLayout::Isometric);
    // the top vertex of (0, 0) is the bottom vertex of (1, 1), the cell above
    EXPECT_EQ(tilegrid::localToCell(iso, TilePoint{0, 16}), (CellCoord{1, 1}));
    // the right vertex of (0, 0) goes to (1, 0), the left vertex to (0, 1)
    EXPECT_EQ(tilegrid::localToCell(iso, TilePoint{16, 8}), (CellCoord{1, 0}));
    EXPECT_EQ(tilegrid::localToCell(iso, TilePoint{-16, 8}), (CellCoord{0, 1}));
    // the middle of the upper-right edge of (0, 0): the cell above it, (1, 0)
    EXPECT_EQ(tilegrid::localToCell(iso, TilePoint{8, 12}), (CellCoord{1, 0}));
    // the middle of the upper-left edge: (0, 1)
    EXPECT_EQ(tilegrid::localToCell(iso, TilePoint{-8, 12}), (CellCoord{0, 1}));

    const TileGridSpec hex = layoutSpec(TileGridLayout::Hexagon, StaggerAxis::Y, StaggerIndex::Odd, 28, 32);
    // the vertical edge between (0, 0) and (1, 0), at u = 14: to the right
    EXPECT_EQ(tilegrid::localToCell(hex, TilePoint{14, 16}), (CellCoord{1, 0}));
    // row 1 of odd-r is shifted right by half a cell: its bottom vertex sits on row 0's top slants
    EXPECT_EQ(tilegrid::localToCell(hex, TilePoint{14, 24}), (CellCoord{0, 1}));
}

TEST(TileGridLayout, IsometricAnchorsAreTheDiamondBottomVertex){
    const TileGridSpec spec = layoutSpec(TileGridLayout::Isometric);
    const TilePoint a = tilegrid::cellToLocal(spec, CellCoord{3, 1});
    EXPECT_DOUBLE_EQ(a.x, (3 - 1) * 32 / 2.0);
    EXPECT_DOUBLE_EQ(a.y, (3 + 1) * 16 / 2.0);
    const TileCellPolygon p = tilegrid::cellPolygon(spec, CellCoord{3, 1});
    ASSERT_EQ(p.count, 4);
    EXPECT_DOUBLE_EQ(p.points[2].y, a.y + 16);
}

TEST(TileGridLayout, PolygonsAreCounterClockwise){
    for (const NamedSpec& named : allLayouts()){
        const TileCellPolygon p = tilegrid::cellPolygon(named.spec, CellCoord{3, -2});
        double area = 0.0;
        for (int i = 0; i < p.count; i++){
            const TilePoint a = p.points[(size_t)i], b = p.points[(size_t)((i + 1) % p.count)];
            area += a.x * b.y - b.x * a.y;
        }
        EXPECT_GT(area, 0.0) << named.name;
    }
}

TEST(TileGridLayout, BoundsContainThePolygon){
    const TileGridSpec spec = layoutSpec(TileGridLayout::Hexagon, StaggerAxis::X, StaggerIndex::Even, 28, 32);
    TilePoint min, max;
    tilegrid::cellBounds(spec, CellCoord{1, 1}, min, max);
    const TilePoint a = tilegrid::cellToLocal(spec, CellCoord{1, 1});
    EXPECT_DOUBLE_EQ(min.x, a.x - 14);
    EXPECT_DOUBLE_EQ(max.x, a.x + 14);
    EXPECT_DOUBLE_EQ(min.y, a.y);
    EXPECT_DOUBLE_EQ(max.y, a.y + 32);
}

TEST(TileGridLayout, NeighboursGoBothWays){
    for (const NamedSpec& named : allLayouts()){
        const int directions = tilegrid::directionCount(named.spec);
        for (int y = -5; y < 5; y++){
            for (int x = -5; x < 5; x++){
                const CellCoord c{x, y};
                std::set<std::pair<int, int>> distinct;
                for (int d = 0; d < directions; d++){
                    const CellCoord n = tilegrid::neighbour(named.spec, c, d);
                    distinct.insert({n.x, n.y});
                    bool back = false;
                    for (int e = 0; e < directions; e++)
                        back = back || tilegrid::neighbour(named.spec, n, e) == c;
                    EXPECT_TRUE(back) << named.name << " " << cellText(c) << " dir " << d;
                }
                EXPECT_EQ(distinct.size(), (size_t)directions) << named.name;
            }
        }
    }
}

TEST(TileGridLayout, HexNeighboursLieInSixDirections){
    for (StaggerAxis axis : {StaggerAxis::Y, StaggerAxis::X}){
        for (StaggerIndex index : {StaggerIndex::Odd, StaggerIndex::Even}){
            const TileGridSpec spec = layoutSpec(TileGridLayout::Hexagon, axis, index, 28, 32);
            for (int y = -4; y < 4; y++){
                for (int x = -4; x < 4; x++){
                    const CellCoord c{x, y};
                    const TilePoint center = tilegrid::cellCenter(spec, c);
                    std::set<int> seen;
                    for (int d = 0; d < 6; d++){
                        const CellCoord n = tilegrid::neighbour(spec, c, d);
                        const TilePoint other = tilegrid::cellCenter(spec, n);
                        // direction d's angle: 60 degrees apart, starting at 0 (pointy) or 30 (flat)
                        const double du = (other.x - center.x) / 28.0;
                        const double dv = (other.y - center.y) / 32.0;
                        const double angle = std::atan2(dv, du) * 180.0 / 3.14159265358979323846;
                        seen.insert((int)std::lround((angle < -1 ? angle + 360 : angle) / 30.0));
                    }
                    EXPECT_EQ(seen.size(), 6u) << "axis " << (int)axis << " cell " << cellText(c);
                }
            }
        }
    }
}

TEST(TileGridLayout, DirectionNamesFollowThePeeringKeys){
    const TileGridSpec square = layoutSpec(TileGridLayout::Rectangle);
    EXPECT_STREQ(tilegrid::directionName(square, 0), "n");
    EXPECT_STREQ(tilegrid::directionName(square, 7), "nw");
    EXPECT_STREQ(tilegrid::directionName(square, 8), "");
    EXPECT_EQ(tilegrid::neighbour(square, CellCoord{0, 0}, 1), (CellCoord{1, 1}));
    const TileGridSpec pointy = layoutSpec(TileGridLayout::Hexagon);
    EXPECT_STREQ(tilegrid::directionName(pointy, 0), "e");
    EXPECT_STREQ(tilegrid::directionName(pointy, 5), "se");
    const TileGridSpec flat = layoutSpec(TileGridLayout::Hexagon, StaggerAxis::X);
    EXPECT_STREQ(tilegrid::directionName(flat, 0), "ne");
    EXPECT_STREQ(tilegrid::directionName(flat, 4), "s");
}

TEST(TileGridLayout, HexNeighboursMatchTheStaggeredRows){
    // odd-r, y up: from an even row, north-east is the same column of the staggered row above
    const TileGridSpec spec = layoutSpec(TileGridLayout::Hexagon, StaggerAxis::Y, StaggerIndex::Odd, 28, 32);
    EXPECT_EQ(tilegrid::neighbour(spec, CellCoord{0, 0}, 1), (CellCoord{0, 1}));
    EXPECT_EQ(tilegrid::neighbour(spec, CellCoord{0, 0}, 2), (CellCoord{-1, 1}));
    // from a staggered row, north-east moves one column right
    EXPECT_EQ(tilegrid::neighbour(spec, CellCoord{0, 1}, 1), (CellCoord{1, 2}));
    EXPECT_EQ(tilegrid::neighbour(spec, CellCoord{0, 1}, 2), (CellCoord{0, 2}));
    // parity of negative rows
    EXPECT_EQ(tilegrid::neighbour(spec, CellCoord{0, -1}, 1), (CellCoord{1, 0}));
}

TEST(TileGridLayout, DiamondConversionIsABijection){
    for (StaggerAxis axis : {StaggerAxis::Y, StaggerAxis::X}){
        for (StaggerIndex index : {StaggerIndex::Odd, StaggerIndex::Even}){
            const TileGridSpec spec = layoutSpec(TileGridLayout::IsometricStaggered, axis, index);
            std::set<std::pair<int, int>> seen;
            for (int y = -20; y < 20; y++){
                for (int x = -20; x < 20; x++){
                    const CellCoord c{x, y};
                    const CellCoord diamond = tilegrid::toDiamond(spec, c);
                    EXPECT_EQ(tilegrid::fromDiamond(spec, diamond), c);
                    EXPECT_TRUE(seen.insert({diamond.x, diamond.y}).second);
                }
            }
        }
    }
}

TEST(TileGridLayout, StaggeredAnchorsEqualTheDiamondAnchorsOfTheOddLayout){
    const TileGridSpec staggered = layoutSpec(TileGridLayout::IsometricStaggered, StaggerAxis::Y, StaggerIndex::Odd);
    const TileGridSpec diamond = layoutSpec(TileGridLayout::Isometric);
    for (int y = -8; y < 8; y++){
        for (int x = -8; x < 8; x++){
            const TilePoint a = tilegrid::cellToLocal(staggered, CellCoord{x, y});
            const TilePoint b = tilegrid::cellToLocal(diamond, tilegrid::toDiamond(staggered, CellCoord{x, y}));
            EXPECT_DOUBLE_EQ(a.x, b.x);
            EXPECT_DOUBLE_EQ(a.y, b.y);
        }
    }
}

TEST(TileGridLayout, StaggeredNeighboursAreTheDiamondNeighbours){
    const TileGridSpec staggered = layoutSpec(TileGridLayout::IsometricStaggered, StaggerAxis::Y, StaggerIndex::Even);
    const TileGridSpec diamond = layoutSpec(TileGridLayout::Isometric);
    const CellCoord c{3, -4};
    for (int d = 0; d < 8; d++){
        const CellCoord viaDiamond = tilegrid::neighbour(diamond, tilegrid::toDiamond(staggered, c), d);
        EXPECT_EQ(tilegrid::neighbour(staggered, c, d), tilegrid::fromDiamond(staggered, viaDiamond));
    }
}

TEST(TileGridLayout, SideNeighboursAreFourOrSix){
    std::vector<CellCoord> out;
    tilegrid::sideNeighbours(layoutSpec(TileGridLayout::Rectangle), CellCoord{0, 0}, out);
    EXPECT_EQ(out, (std::vector<CellCoord>{{0, 1}, {1, 0}, {0, -1}, {-1, 0}}));
    tilegrid::sideNeighbours(layoutSpec(TileGridLayout::Hexagon), CellCoord{0, 0}, out);
    EXPECT_EQ(out.size(), 6u);
}

TEST(TileGridLayout, ALineHasNoRepeatsAndConnects){
    for (const NamedSpec& named : allLayouts()){
        for (CellCoord b : {CellCoord{9, 4}, CellCoord{-7, 12}, CellCoord{13, -13}, CellCoord{0, 0}}){
            const std::vector<CellCoord> cells = tilegrid::line(named.spec, CellCoord{0, 0}, b);
            ASSERT_FALSE(cells.empty()) << named.name;
            EXPECT_EQ(cells.front(), (CellCoord{0, 0})) << named.name;
            EXPECT_EQ(cells.back(), b) << named.name;
            std::set<std::pair<int, int>> distinct;
            for (size_t i = 0; i < cells.size(); i++){
                EXPECT_TRUE(distinct.insert({cells[i].x, cells[i].y}).second) << named.name;
                if (i > 0){
                    bool adjacent = false;
                    for (int d = 0; d < tilegrid::directionCount(named.spec); d++)
                        adjacent = adjacent || tilegrid::neighbour(named.spec, cells[i - 1], d) == cells[i];
                    EXPECT_TRUE(adjacent) << named.name << " step " << i;
                }
            }
            EXPECT_EQ((int64_t)cells.size() - 1, tilegrid::distance(named.spec, CellCoord{0, 0}, b)) << named.name;
        }
    }
}

TEST(TileGridLayout, HexDistanceIsTheNumberOfSteps){
    const TileGridSpec spec = layoutSpec(TileGridLayout::Hexagon, StaggerAxis::Y, StaggerIndex::Odd, 28, 32);
    for (int d = 0; d < 6; d++)
        EXPECT_EQ(tilegrid::distance(spec, CellCoord{2, 3}, tilegrid::neighbour(spec, CellCoord{2, 3}, d)), 1);
    EXPECT_EQ(tilegrid::distance(spec, CellCoord{0, 0}, CellCoord{0, 6}), 6);
    EXPECT_EQ(tilegrid::distance(spec, CellCoord{0, 0}, CellCoord{5, 0}), 5);
}

TEST(TileGridLayout, ChebyshevDistanceOnSquares){
    EXPECT_EQ(tilegrid::distance(layoutSpec(TileGridLayout::Rectangle), CellCoord{0, 0}, CellCoord{3, -8}), 8);
}

TEST(TileGridLayout, PainterOrderRectangleRowsFromTheTop){
    const TileGridSpec spec = layoutSpec(TileGridLayout::Rectangle);
    EXPECT_TRUE(tilegrid::painterLess(spec, CellCoord{5, 3}, CellCoord{0, 2}));
    EXPECT_TRUE(tilegrid::painterLess(spec, CellCoord{0, 3}, CellCoord{1, 3}));
    EXPECT_FALSE(tilegrid::painterLess(spec, CellCoord{1, 3}, CellCoord{1, 3}));
}

TEST(TileGridLayout, PainterOrderIsometricBySumDescending){
    const TileGridSpec spec = layoutSpec(TileGridLayout::Isometric);
    EXPECT_TRUE(tilegrid::painterLess(spec, CellCoord{2, 2}, CellCoord{1, 2}));
    EXPECT_TRUE(tilegrid::painterLess(spec, CellCoord{1, 3}, CellCoord{2, 2}));
}

TEST(TileGridLayout, PainterOrderMatchesAnchorsOnEveryLayout){
    for (const NamedSpec& named : allLayouts()){
        for (int ay = -5; ay < 5; ay++){
            for (int ax = -5; ax < 5; ax++){
                const CellCoord a{ax, ay};
                const TilePoint pa = tilegrid::cellToLocal(named.spec, a);
                for (int by = -5; by < 5; by++){
                    for (int bx = -5; bx < 5; bx++){
                        const CellCoord b{bx, by};
                        const TilePoint pb = tilegrid::cellToLocal(named.spec, b);
                        const bool expected = pa.y != pb.y ? pa.y > pb.y : (pa.x != pb.x ? pa.x < pb.x : (a.y != b.y ? a.y < b.y : a.x < b.x));
                        EXPECT_EQ(tilegrid::painterLess(named.spec, a, b), expected) << named.name << " " << cellText(a) << " " << cellText(b);
                    }
                }
            }
        }
    }
}

TEST(TileGridLayout, PlaneMappingFollowsTheAxes){
    TileGridSpec spec = layoutSpec(TileGridLayout::Rectangle);
    float out[3];
    tilegrid::planeToLocal(spec, TilePoint{3, 4}, out);
    EXPECT_FLOAT_EQ(out[0], 3);
    EXPECT_FLOAT_EQ(out[1], 4);
    EXPECT_FLOAT_EQ(out[2], 0);
    TilePoint back = tilegrid::localToPlane(spec, out);
    EXPECT_DOUBLE_EQ(back.y, 4);
    spec.plane = TileGridPlane::XZ;
    tilegrid::planeToLocal(spec, TilePoint{3, 4}, out);
    EXPECT_FLOAT_EQ(out[1], 0);
    EXPECT_FLOAT_EQ(out[2], -4);
    back = tilegrid::localToPlane(spec, out);
    EXPECT_DOUBLE_EQ(back.y, 4);
}

// Tiled's map coordinates: y grows downwards, a tile's position is its bounding box's top-left
// corner. Tiled formulas, restated from src/libtiled/isometricrenderer.cpp (tileToScreenCoords)
// and hexagonalrenderer.cpp / staggeredrenderer.cpp (RenderParams, tileToScreenCoords).
namespace {

    struct TiledFixture{
        const char* name;
        TileGridSpec spec;
        CellCoord (*toOurs)(int tx, int ty);
        void (*tiledPixel)(int tx, int ty, double& x, double& y);
        double width, height;
    };

    constexpr int TILED_W = 64, TILED_H = 32, TILED_HEX_W = 28, TILED_HEX_H = 32;
    constexpr int TILED_SIDE = 16;
    constexpr int TILED_MAP_HEIGHT = 8;

    CellCoord oursSame(int tx, int ty){ return CellCoord{tx, -ty}; }
    CellCoord oursDiamond(int tx, int ty){ return CellCoord{-ty, -tx}; }

    void tiledOrthogonal(int tx, int ty, double& x, double& y){
        x = tx * TILED_W;
        y = ty * TILED_H;
    }

    // originX = mapHeight * tileWidth / 2; screen = ((tx - ty) * w / 2 + originX, (tx + ty) * h / 2)
    void tiledIsometric(int tx, int ty, double& x, double& y){
        const double originX = TILED_MAP_HEIGHT * TILED_W / 2.0;
        x = (tx - ty) * TILED_W / 2.0 + originX;
        y = (tx + ty) * TILED_H / 2.0;
    }

    // staggered maps are hexagonal RenderParams with side length 0 on both axes:
    // sideOffset = tile / 2, rowHeight = sideOffsetY + sideLengthY, columnWidth = sideOffsetX + sideLengthX
    void tiledStaggeredY(int tx, int ty, double& x, double& y){
        x = tx * TILED_W;
        if (ty & 1)
            x += TILED_W / 2.0;
        y = ty * (TILED_H / 2.0);
    }

    void tiledStaggeredX(int tx, int ty, double& x, double& y){
        y = ty * TILED_H;
        if (tx & 1)
            y += TILED_H / 2.0;
        x = tx * (TILED_W / 2.0);
    }

    void tiledHexY(int tx, int ty, double& x, double& y){
        x = tx * TILED_HEX_W;
        if (ty & 1)
            x += TILED_HEX_W / 2.0;
        y = ty * ((TILED_HEX_H - TILED_SIDE) / 2.0 + TILED_SIDE);
    }

    void tiledHexX(int tx, int ty, double& x, double& y){
        y = ty * TILED_HEX_H;
        if (tx & 1)
            y += TILED_HEX_H / 2.0;
        x = tx * ((TILED_HEX_W - TILED_SIDE) / 2.0 + TILED_SIDE);
    }

}

TEST(TileGridLayout, PositionsEqualTiledsFormulas){
    std::vector<TiledFixture> fixtures;
    fixtures.push_back({"orthogonal", layoutSpec(TileGridLayout::Rectangle, StaggerAxis::Y, StaggerIndex::Odd, TILED_W, TILED_H), oursSame, tiledOrthogonal, (double)TILED_W, (double)TILED_H});
    fixtures.push_back({"isometric", layoutSpec(TileGridLayout::Isometric, StaggerAxis::Y, StaggerIndex::Odd, TILED_W, TILED_H), oursDiamond, tiledIsometric, (double)TILED_W, (double)TILED_H});
    fixtures.push_back({"staggered_y_odd", layoutSpec(TileGridLayout::IsometricStaggered, StaggerAxis::Y, StaggerIndex::Odd, TILED_W, TILED_H), oursSame, tiledStaggeredY, (double)TILED_W, (double)TILED_H});
    // y flipped, Tiled's odd columns down are our even columns up (same relative stagger)
    fixtures.push_back({"staggered_x_odd", layoutSpec(TileGridLayout::IsometricStaggered, StaggerAxis::X, StaggerIndex::Even, TILED_W, TILED_H), oursSame, tiledStaggeredX, (double)TILED_W, (double)TILED_H});
    fixtures.push_back({"hex_y_odd", layoutSpec(TileGridLayout::Hexagon, StaggerAxis::Y, StaggerIndex::Odd, TILED_HEX_W, TILED_HEX_H, TILED_SIDE), oursSame, tiledHexY, (double)TILED_HEX_W, (double)TILED_HEX_H});
    fixtures.push_back({"hex_x_odd", layoutSpec(TileGridLayout::Hexagon, StaggerAxis::X, StaggerIndex::Even, TILED_HEX_W, TILED_HEX_H, TILED_SIDE), oursSame, tiledHexX, (double)TILED_HEX_W, (double)TILED_HEX_H});

    for (const TiledFixture& fixture : fixtures){
        double offsetX = 0, offsetY = 0;
        bool first = true;
        for (int ty = 0; ty < TILED_MAP_HEIGHT; ty++){
            for (int tx = 0; tx < 8; tx++){
                double px, py;
                fixture.tiledPixel(tx, ty, px, py);
                const TilePoint ours = tilegrid::cellToLocal(fixture.spec, fixture.toOurs(tx, ty));
                const double candidateX = px - ours.x;
                const double candidateY = py + ours.y;
                if (first){
                    offsetX = candidateX;
                    offsetY = candidateY;
                    first = false;
                }
                EXPECT_DOUBLE_EQ(candidateX, offsetX) << fixture.name << " tile (" << tx << ", " << ty << ")";
                EXPECT_DOUBLE_EQ(candidateY, offsetY) << fixture.name << " tile (" << tx << ", " << ty << ")";
            }
        }
    }
}

TEST(TileGridLayout, NegativeRowsAndColumnsFollowTheParityRule){
    const TileGridSpec spec = layoutSpec(TileGridLayout::Hexagon, StaggerAxis::Y, StaggerIndex::Odd, 28, 32);
    EXPECT_TRUE(tilegrid::isStaggered(spec, -1));
    EXPECT_FALSE(tilegrid::isStaggered(spec, -2));
    EXPECT_TRUE(tilegrid::isStaggered(spec, -3));
    const TileGridSpec rect = layoutSpec(TileGridLayout::Rectangle);
    EXPECT_FALSE(tilegrid::isStaggered(rect, 1));
}

TEST(TileGridLayout, OverlapsAreRectangleFree){
    EXPECT_FALSE(tilegrid::layoutOverlaps(layoutSpec(TileGridLayout::Rectangle)));
    EXPECT_TRUE(tilegrid::layoutOverlaps(layoutSpec(TileGridLayout::Isometric)));
    EXPECT_TRUE(tilegrid::layoutOverlaps(layoutSpec(TileGridLayout::IsometricStaggered)));
    EXPECT_TRUE(tilegrid::layoutOverlaps(layoutSpec(TileGridLayout::Hexagon)));
}

TEST(TileGridLayout, ANonRegularHexSideChangesRowSpacing){
    const TileGridSpec spec = layoutSpec(TileGridLayout::Hexagon, StaggerAxis::Y, StaggerIndex::Odd, 28, 32, 8);
    EXPECT_DOUBLE_EQ(tilegrid::cellToLocal(spec, CellCoord{0, 1}).y, (32 + 8) / 2.0);
    // above the row band's slant, a point just left of the vertical edge is in the left column
    EXPECT_EQ(tilegrid::localToCell(spec, TilePoint{13.99, 14}), (CellCoord{0, 0}));
    EXPECT_EQ(tilegrid::localToCell(spec, TilePoint{14, 14}), (CellCoord{1, 0}));
}

// R8, R27, R2 tileAnchor: where a layer draws its tiles against their anchors, written out per
// layout (cells 32 x 16)
TEST(TileGridLayout, TileShiftOfAnchorsAndDualGrids){
    const TileGridSpec rect = layoutSpec(TileGridLayout::Rectangle);
    const TileGridSpec iso = layoutSpec(TileGridLayout::Isometric);
    auto expectPoint = [](TilePoint got, double x, double y){
        EXPECT_DOUBLE_EQ(got.x, x);
        EXPECT_DOUBLE_EQ(got.y, y);
    };
    expectPoint(tilegrid::dualGridShift(rect), -16, -8);
    // the diamond (1, 1) step is straight up: half of it down
    expectPoint(tilegrid::dualGridShift(iso), 0, -8);
    expectPoint(tilegrid::tileAnchorShift(iso, 0.5, -0.25), 16, -4);
    expectPoint(tilegrid::tileShift(rect, 1, 1, true), 16, 8);
    expectPoint(tilegrid::tileShift(iso, 0.5, 0.25, true), 16, -4);
    expectPoint(tilegrid::tileShift(iso, 0, 0, false), 0, 0);
    // a shown tile's centre names its cell; a point just under its bottom does not
    for (const TileGridSpec& spec : {rect, iso}){
        const TilePoint shift = tilegrid::tileShift(spec, 0.25, 0.5, true);
        for (CellCoord cell : {CellCoord{0, 0}, CellCoord{5, -3}, CellCoord{-7, 2}}){
            const TilePoint centre = tilegrid::cellCenter(spec, cell);
            EXPECT_EQ(tilegrid::shownCellAt(spec, TilePoint{centre.x + shift.x, centre.y + shift.y}, shift), cell);
        }
    }
}
