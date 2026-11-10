// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TILEMAP_TILE_GRID_LAYOUT_H
#define TILEMAP_TILE_GRID_LAYOUT_H

// Cell layout maths of a Grid (docs/specs/S50-grid-tilemap.md R1, R7, R8, D5): cell <-> local
// point, cell polygons, neighbours, lines and the painter's order, for the rectangle, isometric
// (diamond), staggered isometric and hexagon layouts. Pure and thread-safe.
//
// Space: the Grid's plane coordinates (u right, v up), in world units of the Grid's GameObject
// (pixels in 2D). planeToLocal/localToPlane map them to the GameObject's 3D local space (XY:
// (u, v, 0); XZ: (u, 0, -v)).
//
// Pitch: a cell is cellWidth x cellHeight; the pitch between cells adds the gap (Unity's Cell Gap).
//
// Anchors (cellToLocal, R8): rectangle: the cell's bottom-left corner; isometric: the diamond's
// bottom vertex; staggered and hexagon: the bottom-centre of the cell's bounding box.
//   Rectangle:  anchor = (x * pitchX, y * pitchY)
//   Isometric:  anchor = ((x - y) * pitchX / 2, (x + y) * pitchY / 2) (Unity/Tiled's diamond, y up)
//   Staggered (axis Y): rows are pitchY / 2 apart; the staggered rows (staggerIndex Odd: odd y,
//     Even: even y, y up, parity of y & 1 for negatives too) move right by pitchX / 2.
//     Axis X: columns pitchX / 2 apart, staggered columns move up by pitchY / 2.
//   Hexagon (axis Y, pointy-top, the default; odd-r with staggerIndex Odd): columns pitchX apart,
//     rows (cellHeight + side) / 2 + gapY apart, staggered rows move right by pitchX / 2.
//     Axis X (flat-top): rows pitchY apart, columns (cellWidth + side) / 2 + gapX apart,
//     staggered columns move up by pitchY / 2.
//   side = hexSideLength, or when 0 the regular default: cellHeight / 2 (axis Y), cellWidth / 2 (axis X).
//
// localToCell is exact and half-open: a point on a shared edge or vertex belongs to the cell
// reached by moving an infinitesimal step up (+v), then right (+u): "a shared edge belongs to the
// cell above or to the right". So localToCell(cellToLocal(c)) == c for every layout. Maths in
// double, so it stays exact far beyond +-2^24 cells.
//
// Directions: rectangle and isometric use 8 directions in cell space, in the peering-key order
// n ne e se s sw w nw (n = (0, +1), e = (+1, 0)); staggered isometric uses the same 8 directions of
// the equivalent diamond (toDiamond), so terrains and templates work alike on both. Hexagon has 6
// sides, counter-clockwise from east (pointy-top: e ne nw w sw se; flat-top: ne n nw sw s se).

#include "Export.h"
#include "tilemap/TileTypes.h"

#include <array>
#include <vector>

namespace jasperengine{

    struct TileGridSpec{
        TileGridLayout layout = TileGridLayout::Rectangle;
        double cellWidth = 16.0;
        double cellHeight = 16.0;
        double gapX = 0.0;
        double gapY = 0.0;
        StaggerAxis staggerAxis = StaggerAxis::Y;
        StaggerIndex staggerIndex = StaggerIndex::Odd;
        double hexSideLength = 0.0;    // 0: the regular default (see above)
        TileGridPlane plane = TileGridPlane::XY;
    };

    struct TilePoint{
        double x = 0.0;
        double y = 0.0;
    };

    // a cell's outline: 4 points (rectangle, diamonds) or 6 (hexagon), counter-clockwise
    struct TileCellPolygon{
        std::array<TilePoint, 6> points;
        int count = 0;
    };

    namespace tilegrid{

        JASPERENGINE_API double pitchX(const TileGridSpec& spec);
        JASPERENGINE_API double pitchY(const TileGridSpec& spec);
        JASPERENGINE_API double hexSide(const TileGridSpec& spec);
        // whether the layout's row (axis Y) or column (axis X) at `index` is shifted
        JASPERENGINE_API bool isStaggered(const TileGridSpec& spec, int32_t index);

        // the anchor (R8) and the centre of a cell, in plane coordinates
        JASPERENGINE_API TilePoint cellToLocal(const TileGridSpec& spec, CellCoord cell);
        JASPERENGINE_API TilePoint cellCenter(const TileGridSpec& spec, CellCoord cell);
        // the cell holding a point (half-open, see above)
        JASPERENGINE_API CellCoord localToCell(const TileGridSpec& spec, TilePoint point);
        JASPERENGINE_API TileCellPolygon cellPolygon(const TileGridSpec& spec, CellCoord cell);
        // the bounding box of the cell's polygon
        JASPERENGINE_API void cellBounds(const TileGridSpec& spec, CellCoord cell, TilePoint& min, TilePoint& max);

        // 8 (rectangle, isometric, staggered) or 6 (hexagon)
        JASPERENGINE_API int directionCount(const TileGridSpec& spec);
        // "n", "ne", ... (the peering keys of the layout's sides and, for 8, corners)
        JASPERENGINE_API const char* directionName(const TileGridSpec& spec, int direction);
        JASPERENGINE_API CellCoord neighbour(const TileGridSpec& spec, CellCoord cell, int direction);
        // the neighbours flood fills and outlines connect through: the 4 sides (n e s w) of
        // rectangle, isometric and staggered cells, the 6 sides of hexagons
        JASPERENGINE_API void sideNeighbours(const TileGridSpec& spec, CellCoord cell, std::vector<CellCoord>& out);

        // staggered isometric <-> the equivalent diamond (isometric) cell coordinates; identity
        // for the other layouts
        JASPERENGINE_API CellCoord toDiamond(const TileGridSpec& spec, CellCoord cell);
        JASPERENGINE_API CellCoord fromDiamond(const TileGridSpec& spec, CellCoord diamond);

        // the cells of a line from a to b, both included, without repeats: Bresenham on rectangle
        // and isometric cells (and on the diamonds of staggered ones), a cube line on hexagons
        JASPERENGINE_API std::vector<CellCoord> line(const TileGridSpec& spec, CellCoord a, CellCoord b);
        // cube distance on hexagons, Chebyshev distance on the others
        JASPERENGINE_API int64_t distance(const TileGridSpec& spec, CellCoord a, CellCoord b);

        // the painter's order (D7), back to front: true when a is drawn before b. Anchors with a
        // larger v first, then a smaller u; ties (none in a valid layout) by (y, x). So rectangle:
        // rows from the top, then x ascending; isometric: (x + y) descending, then x ascending;
        // staggered and hexagon: rows from the top with the stagger. Exact (integer comparisons).
        JASPERENGINE_API bool painterLess(const TileGridSpec& spec, CellCoord a, CellCoord b);
        // true when tiles drawn at their cells may overlap their neighbours (isometric, staggered,
        // hexagon), so a chunk keeps the painter's order instead of grouping by page (R32)
        JASPERENGINE_API bool layoutOverlaps(const TileGridSpec& spec);

        // ---- where a layer draws its tiles (R8, R27): the one rule the chunk builder, the
        // colliders, picking, the NavGrid and the editor's previews share. A tile drawn for cell c
        // sits at cellToLocal(c) + tileShift(...), in plane units.
        //
        // the layer's tileAnchor, in cells: (ax, ay) moves every tile by ax pitches along u and ay
        // pitches along v, whatever the layout
        JASPERENGINE_API TilePoint tileAnchorShift(const TileGridSpec& spec, double anchorX, double anchorY);
        // a dual-grid tile against its cell's anchor: half a cell down-left in cell space, so it
        // covers the shared corner of the four logical cells (x - 1, y - 1)..(x, y): (-pitchX / 2,
        // -pitchY / 2) on rectangles, (0, -pitchY / 2) on isometric diamonds (the layouts R27 allows;
        // the others take the rectangle's)
        JASPERENGINE_API TilePoint dualGridShift(const TileGridSpec& spec);
        // both of them: a layer's tiles against their cells' anchors
        JASPERENGINE_API TilePoint tileShift(const TileGridSpec& spec, double anchorX, double anchorY, bool dualGrid);
        // the cell whose drawn tile's cell polygon holds a point, given the layer's tileShift
        JASPERENGINE_API CellCoord shownCellAt(const TileGridSpec& spec, TilePoint point, TilePoint shift);

        // the plane's (u, v) in the Grid GameObject's local 3D space, and back (drops the third axis)
        JASPERENGINE_API void planeToLocal(const TileGridSpec& spec, TilePoint p, float out[3]);
        JASPERENGINE_API TilePoint localToPlane(const TileGridSpec& spec, const float local[3]);
    }

}

#endif // TILEMAP_TILE_GRID_LAYOUT_H
