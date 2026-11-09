// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/TileGridLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

using namespace jasperengine;

namespace {

    constexpr int GRID_SQUARE_DIRECTIONS[8][2] = {{0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, -1}, {-1, 0}, {-1, 1}};
    const char* const GRID_SQUARE_NAMES[8] = {"n", "ne", "e", "se", "s", "sw", "w", "nw"};
    const char* const GRID_HEX_POINTY_NAMES[6] = {"e", "ne", "nw", "w", "sw", "se"};
    const char* const GRID_HEX_FLAT_NAMES[6] = {"ne", "n", "nw", "sw", "s", "se"};

    double gridPositive(double value){
        return value > 0.0 && std::isfinite(value) ? value : 1.0;
    }

    int32_t gridClampCell(int64_t value){
        return (int32_t)std::clamp<int64_t>(value, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max());
    }

    // The n with n * pitch + offset <= value < (n + 1) * pitch + offset, judged by the same
    // products the anchors are made of: a pitch like 0.1 has no exact double, so a plain floor of
    // the quotient can land one cell off on a point that is exactly an anchor.
    int64_t gridFloorIndex(double value, double pitch, double offset){
        if (!std::isfinite(value))
            return 0;
        const double q = std::floor((value - offset) / pitch);
        const double limit = 4.0e18;
        int64_t n = (int64_t)std::clamp(q, -limit, limit);
        while ((double)(n + 1) * pitch + offset <= value)
            n++;
        while ((double)n * pitch + offset > value)
            n--;
        return n;
    }

    bool gridStaggeredIndex(StaggerIndex index, int64_t value){
        return (value & 1) == (index == StaggerIndex::Odd ? 1 : 0);
    }

    // the row spacing of a pointy-top layout / the column spacing of a flat-top one
    double gridHexPointyRow(const TileGridSpec& spec){
        return gridPositive((spec.cellHeight + tilegrid::hexSide(spec)) * 0.5 + spec.gapY);
    }

    double gridHexFlatColumn(const TileGridSpec& spec){
        return gridPositive((spec.cellWidth + tilegrid::hexSide(spec)) * 0.5 + spec.gapX);
    }

    // The diamond holding a point, half-open along both diamond axes: every diamond edge has a
    // direction with +v in it, so floors of the two axes are exactly "the cell above". A plain
    // floor can land a cell off for a pitch with no exact double (16.4), so the neighbours of the
    // floored cell are checked too, by the anchors' own arithmetic: an anchor is exactly (0, 0)
    // from itself, and otherwise the two axis tests leave only the half-open cell.
    void gridDiamondOf(double pitchW, double pitchH, double u, double v, int64_t& x, int64_t& y){
        if (!std::isfinite(u) || !std::isfinite(v)){
            x = 0;
            y = 0;
            return;
        }
        const double limit = 4.0e18;
        const int64_t floorX = (int64_t)std::clamp(std::floor(v / pitchH + u / pitchW), -limit, limit);
        const int64_t floorY = (int64_t)std::clamp(std::floor(v / pitchH - u / pitchW), -limit, limit);
        const auto axisA = [&](int64_t cx, int64_t cy){
            return (v - (double)(cx + cy) * pitchH * 0.5) / pitchH + (u - (double)(cx - cy) * pitchW * 0.5) / pitchW;
        };
        const auto axisB = [&](int64_t cx, int64_t cy){
            return (v - (double)(cx + cy) * pitchH * 0.5) / pitchH - (u - (double)(cx - cy) * pitchW * 0.5) / pitchW;
        };
        x = floorX;
        y = floorY;
        bool found = false;
        for (int64_t cy : {floorY, floorY - 1, floorY + 1}){
            for (int64_t cx : {floorX, floorX - 1, floorX + 1}){
                const double a = axisA(cx, cy), b = axisB(cx, cy);
                if (a == 0.0 && b == 0.0){
                    x = cx;
                    y = cy;
                    return;
                }
                if (!found && a >= 0.0 && b >= 0.0 && axisA(cx + 1, cy) < 0.0 && axisB(cx, cy + 1) < 0.0){
                    x = cx;
                    y = cy;
                    found = true;
                }
            }
        }
    }

    // Not NavGridMap's cube rounding: that needs a regular hexagon, and hexSideLength can be any
    // value. A row band is either a straight strip (one row) or a zigzag between two rows.
    CellCoord gridHexPointyAt(const TileGridSpec& spec, double u, double v){
        const double width = gridPositive(tilegrid::pitchX(spec));
        const double rowPitch = gridHexPointyRow(spec);
        const double slant = std::max(0.0, rowPitch - tilegrid::hexSide(spec));
        const int64_t row = gridFloorIndex(v, rowPitch, 0.0);
        const double t = v - (double)row * rowPitch;
        const bool staggered = gridStaggeredIndex(spec.staggerIndex, row);
        const double offset = staggered ? width * 0.5 : 0.0;
        const int64_t column = gridFloorIndex(u, width, offset - width * 0.5);
        if (t >= slant)
            return CellCoord{gridClampCell(column), gridClampCell(row)};
        const double d = u - ((double)column * width + offset);
        if (t * (width * 0.5) >= slant * std::abs(d))
            return CellCoord{gridClampCell(column), gridClampCell(row)};
        int64_t below = column;
        if (staggered){
            if (d >= 0.0)
                below = column + 1;
        }else if (d < 0.0){
            below = column - 1;
        }
        return CellCoord{gridClampCell(below), gridClampCell(row - 1)};
    }

    CellCoord gridHexFlatAt(const TileGridSpec& spec, double u, double v){
        const double height = gridPositive(tilegrid::pitchY(spec));
        const double columnPitch = gridHexFlatColumn(spec);
        const double side = tilegrid::hexSide(spec);
        const double slant = std::max(0.0, columnPitch - side);
        const int64_t column = gridFloorIndex(u, columnPitch, -side * 0.5);
        const double t = u - ((double)column * columnPitch - side * 0.5);
        const double offset = gridStaggeredIndex(spec.staggerIndex, column) ? height * 0.5 : 0.0;
        const int64_t row = gridFloorIndex(v, height, offset);
        if (t < side)
            return CellCoord{gridClampCell(column), gridClampCell(row)};
        const double e = t - side;
        const double w = v - ((double)row * height + offset);
        // the lower slant rises to the right, so "above" is the left column; the upper slant
        // falls to the right, so "above" is the right column
        const bool left = w < height * 0.5 ? e * (height * 0.5) <= slant * w : e * (height * 0.5) < slant * (height - w);
        if (left)
            return CellCoord{gridClampCell(column), gridClampCell(row)};
        const double nextOffset = gridStaggeredIndex(spec.staggerIndex, column + 1) ? height * 0.5 : 0.0;
        return CellCoord{gridClampCell(column + 1), gridClampCell(gridFloorIndex(v, height, nextOffset))};
    }

    // Axial coordinates of a hex cell, in which the six neighbours are the usual
    // (+-1, 0), (0, +-1), (+1, -1), (-1, +1). Even layouts differ from odd ones by a whole-lattice
    // shift of half a cell, which adjacency does not see.
    void gridHexAxial(const TileGridSpec& spec, CellCoord cell, int64_t& q, int64_t& r){
        const int64_t x = cell.x, y = cell.y;
        const bool odd = spec.staggerIndex == StaggerIndex::Odd;
        if (spec.staggerAxis == StaggerAxis::Y){
            r = y;
            q = x - (odd ? y - (y & 1) : y + (y & 1)) / 2;
        }else{
            q = x;
            r = y - (odd ? x - (x & 1) : x + (x & 1)) / 2;
        }
    }

    CellCoord gridHexFromAxial(const TileGridSpec& spec, int64_t q, int64_t r){
        const bool odd = spec.staggerIndex == StaggerIndex::Odd;
        if (spec.staggerAxis == StaggerAxis::Y)
            return CellCoord{gridClampCell(q + (odd ? r - (r & 1) : r + (r & 1)) / 2), gridClampCell(r)};
        return CellCoord{gridClampCell(q), gridClampCell(r + (odd ? q - (q & 1) : q + (q & 1)) / 2)};
    }

    int64_t gridHexDistance(const TileGridSpec& spec, CellCoord a, CellCoord b){
        int64_t aq, ar, bq, br;
        gridHexAxial(spec, a, aq, ar);
        gridHexAxial(spec, b, bq, br);
        const int64_t dq = aq - bq, dr = ar - br;
        return (std::llabs(dq) + std::llabs(dr) + std::llabs(dq + dr)) / 2;
    }

    void gridBresenham(CellCoord a, CellCoord b, std::vector<CellCoord>& out){
        int64_t x = a.x, y = a.y;
        const int64_t dx = std::llabs((int64_t)b.x - a.x), dy = -std::llabs((int64_t)b.y - a.y);
        const int64_t sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1;
        int64_t error = dx + dy;
        while (true){
            out.push_back(CellCoord{(int32_t)x, (int32_t)y});
            if (x == b.x && y == b.y)
                break;
            const int64_t e2 = 2 * error;
            if (e2 >= dy){
                error += dy;
                x += sx;
            }
            if (e2 <= dx){
                error += dx;
                y += sy;
            }
        }
    }

}

double tilegrid::pitchX(const TileGridSpec& spec){
    return spec.cellWidth + spec.gapX;
}

double tilegrid::pitchY(const TileGridSpec& spec){
    return spec.cellHeight + spec.gapY;
}

double tilegrid::hexSide(const TileGridSpec& spec){
    if (spec.hexSideLength > 0.0)
        return spec.hexSideLength;
    return spec.staggerAxis == StaggerAxis::Y ? spec.cellHeight * 0.5 : spec.cellWidth * 0.5;
}

bool tilegrid::isStaggered(const TileGridSpec& spec, int32_t index){
    if (spec.layout != TileGridLayout::IsometricStaggered && spec.layout != TileGridLayout::Hexagon)
        return false;
    return gridStaggeredIndex(spec.staggerIndex, index);
}

TilePoint tilegrid::cellToLocal(const TileGridSpec& spec, CellCoord cell){
    const double px = pitchX(spec), py = pitchY(spec);
    const double x = cell.x, y = cell.y;
    switch (spec.layout){
        case TileGridLayout::Rectangle:
            return TilePoint{x * px, y * py};
        case TileGridLayout::Isometric:
            return TilePoint{((double)((int64_t)cell.x - cell.y)) * px * 0.5, ((double)((int64_t)cell.x + cell.y)) * py * 0.5};
        case TileGridLayout::IsometricStaggered:
            if (spec.staggerAxis == StaggerAxis::Y)
                return TilePoint{x * px + (isStaggered(spec, cell.y) ? px * 0.5 : 0.0), y * py * 0.5};
            return TilePoint{x * px * 0.5, y * py + (isStaggered(spec, cell.x) ? py * 0.5 : 0.0)};
        case TileGridLayout::Hexagon:
            if (spec.staggerAxis == StaggerAxis::Y)
                return TilePoint{x * px + (isStaggered(spec, cell.y) ? px * 0.5 : 0.0), y * gridHexPointyRow(spec)};
            return TilePoint{x * gridHexFlatColumn(spec), y * py + (isStaggered(spec, cell.x) ? py * 0.5 : 0.0)};
    }
    return TilePoint{};
}

TilePoint tilegrid::cellCenter(const TileGridSpec& spec, CellCoord cell){
    const TilePoint anchor = cellToLocal(spec, cell);
    if (spec.layout == TileGridLayout::Rectangle)
        return TilePoint{anchor.x + spec.cellWidth * 0.5, anchor.y + spec.cellHeight * 0.5};
    return TilePoint{anchor.x, anchor.y + spec.cellHeight * 0.5};
}

CellCoord tilegrid::localToCell(const TileGridSpec& spec, TilePoint point){
    const double px = gridPositive(pitchX(spec)), py = gridPositive(pitchY(spec));
    switch (spec.layout){
        case TileGridLayout::Rectangle:
            return CellCoord{gridClampCell(gridFloorIndex(point.x, px, 0.0)), gridClampCell(gridFloorIndex(point.y, py, 0.0))};
        case TileGridLayout::Isometric:{
            int64_t x, y;
            gridDiamondOf(px, py, point.x, point.y, x, y);
            return CellCoord{gridClampCell(x), gridClampCell(y)};
        }
        case TileGridLayout::IsometricStaggered:{
            // an Even layout is the diamond lattice moved by half a cell along the stagger
            double u = point.x, v = point.y;
            if (spec.staggerIndex == StaggerIndex::Even){
                if (spec.staggerAxis == StaggerAxis::Y)
                    u -= px * 0.5;
                else
                    v -= py * 0.5;
            }
            int64_t x, y;
            gridDiamondOf(px, py, u, v, x, y);
            return fromDiamond(spec, CellCoord{gridClampCell(x), gridClampCell(y)});
        }
        case TileGridLayout::Hexagon:
            return spec.staggerAxis == StaggerAxis::Y ? gridHexPointyAt(spec, point.x, point.y) : gridHexFlatAt(spec, point.x, point.y);
    }
    return CellCoord{};
}

TileCellPolygon tilegrid::cellPolygon(const TileGridSpec& spec, CellCoord cell){
    const TilePoint a = cellToLocal(spec, cell);
    const double w = spec.cellWidth, h = spec.cellHeight;
    TileCellPolygon polygon;
    switch (spec.layout){
        case TileGridLayout::Rectangle:
            polygon.count = 4;
            polygon.points[0] = TilePoint{a.x, a.y};
            polygon.points[1] = TilePoint{a.x + w, a.y};
            polygon.points[2] = TilePoint{a.x + w, a.y + h};
            polygon.points[3] = TilePoint{a.x, a.y + h};
            break;
        case TileGridLayout::Isometric:
        case TileGridLayout::IsometricStaggered:
            polygon.count = 4;
            polygon.points[0] = TilePoint{a.x, a.y};
            polygon.points[1] = TilePoint{a.x + w * 0.5, a.y + h * 0.5};
            polygon.points[2] = TilePoint{a.x, a.y + h};
            polygon.points[3] = TilePoint{a.x - w * 0.5, a.y + h * 0.5};
            break;
        case TileGridLayout::Hexagon:{
            const double side = hexSide(spec);
            polygon.count = 6;
            if (spec.staggerAxis == StaggerAxis::Y){
                const double low = (h - side) * 0.5, high = (h + side) * 0.5;
                polygon.points[0] = TilePoint{a.x, a.y};
                polygon.points[1] = TilePoint{a.x + w * 0.5, a.y + low};
                polygon.points[2] = TilePoint{a.x + w * 0.5, a.y + high};
                polygon.points[3] = TilePoint{a.x, a.y + h};
                polygon.points[4] = TilePoint{a.x - w * 0.5, a.y + high};
                polygon.points[5] = TilePoint{a.x - w * 0.5, a.y + low};
            }else{
                polygon.points[0] = TilePoint{a.x - side * 0.5, a.y};
                polygon.points[1] = TilePoint{a.x + side * 0.5, a.y};
                polygon.points[2] = TilePoint{a.x + w * 0.5, a.y + h * 0.5};
                polygon.points[3] = TilePoint{a.x + side * 0.5, a.y + h};
                polygon.points[4] = TilePoint{a.x - side * 0.5, a.y + h};
                polygon.points[5] = TilePoint{a.x - w * 0.5, a.y + h * 0.5};
            }
            break;
        }
    }
    return polygon;
}

void tilegrid::cellBounds(const TileGridSpec& spec, CellCoord cell, TilePoint& min, TilePoint& max){
    const TileCellPolygon polygon = cellPolygon(spec, cell);
    min = polygon.points[0];
    max = polygon.points[0];
    for (int i = 1; i < polygon.count; i++){
        min.x = std::min(min.x, polygon.points[(size_t)i].x);
        min.y = std::min(min.y, polygon.points[(size_t)i].y);
        max.x = std::max(max.x, polygon.points[(size_t)i].x);
        max.y = std::max(max.y, polygon.points[(size_t)i].y);
    }
}

int tilegrid::directionCount(const TileGridSpec& spec){
    return spec.layout == TileGridLayout::Hexagon ? 6 : 8;
}

const char* tilegrid::directionName(const TileGridSpec& spec, int direction){
    if (direction < 0 || direction >= directionCount(spec))
        return "";
    if (spec.layout != TileGridLayout::Hexagon)
        return GRID_SQUARE_NAMES[direction];
    return spec.staggerAxis == StaggerAxis::Y ? GRID_HEX_POINTY_NAMES[direction] : GRID_HEX_FLAT_NAMES[direction];
}

CellCoord tilegrid::neighbour(const TileGridSpec& spec, CellCoord cell, int direction){
    if (direction < 0 || direction >= directionCount(spec))
        return cell;
    const int64_t x = cell.x, y = cell.y;
    if (spec.layout == TileGridLayout::Hexagon){
        // y up: the row above a plain row is shifted right of it, the row above a staggered one
        // left of it; likewise the columns beside a plain flat-top column sit half a cell higher
        int64_t nx = x, ny = y;
        if (spec.staggerAxis == StaggerAxis::Y){
            const int64_t right = isStaggered(spec, cell.y) ? 1 : 0;
            switch (direction){
                case 0: nx = x + 1; break;
                case 1: nx = x + right; ny = y + 1; break;
                case 2: nx = x + right - 1; ny = y + 1; break;
                case 3: nx = x - 1; break;
                case 4: nx = x + right - 1; ny = y - 1; break;
                default: nx = x + right; ny = y - 1; break;
            }
        }else{
            const int64_t up = isStaggered(spec, cell.x) ? 1 : 0;
            switch (direction){
                case 0: nx = x + 1; ny = y + up; break;
                case 1: ny = y + 1; break;
                case 2: nx = x - 1; ny = y + up; break;
                case 3: nx = x - 1; ny = y + up - 1; break;
                case 4: ny = y - 1; break;
                default: nx = x + 1; ny = y + up - 1; break;
            }
        }
        return CellCoord{gridClampCell(nx), gridClampCell(ny)};
    }
    const CellCoord base = toDiamond(spec, cell);
    const CellCoord moved{gridClampCell((int64_t)base.x + GRID_SQUARE_DIRECTIONS[direction][0]),
                          gridClampCell((int64_t)base.y + GRID_SQUARE_DIRECTIONS[direction][1])};
    return fromDiamond(spec, moved);
}

void tilegrid::sideNeighbours(const TileGridSpec& spec, CellCoord cell, std::vector<CellCoord>& out){
    out.clear();
    if (spec.layout == TileGridLayout::Hexagon){
        for (int direction = 0; direction < 6; direction++)
            out.push_back(neighbour(spec, cell, direction));
    }else{
        for (int direction = 0; direction < 8; direction += 2)
            out.push_back(neighbour(spec, cell, direction));
    }
}

CellCoord tilegrid::toDiamond(const TileGridSpec& spec, CellCoord cell){
    if (spec.layout != TileGridLayout::IsometricStaggered)
        return cell;
    const int64_t x = cell.x, y = cell.y;
    const bool odd = spec.staggerIndex == StaggerIndex::Odd;
    int64_t sum, difference;
    if (spec.staggerAxis == StaggerAxis::Y){
        sum = y;
        difference = 2 * x + (odd ? (y & 1) : -(y & 1));
    }else{
        difference = x;
        sum = 2 * y + (odd ? (x & 1) : -(x & 1));
    }
    return CellCoord{gridClampCell((sum + difference) / 2), gridClampCell((sum - difference) / 2)};
}

CellCoord tilegrid::fromDiamond(const TileGridSpec& spec, CellCoord diamond){
    if (spec.layout != TileGridLayout::IsometricStaggered)
        return diamond;
    const int64_t sum = (int64_t)diamond.x + diamond.y;
    const int64_t difference = (int64_t)diamond.x - diamond.y;
    const bool odd = spec.staggerIndex == StaggerIndex::Odd;
    if (spec.staggerAxis == StaggerAxis::Y)
        return CellCoord{gridClampCell((difference - (odd ? (sum & 1) : -(sum & 1))) / 2), gridClampCell(sum)};
    return CellCoord{gridClampCell(difference), gridClampCell((sum - (odd ? (difference & 1) : -(difference & 1))) / 2)};
}

std::vector<CellCoord> tilegrid::line(const TileGridSpec& spec, CellCoord a, CellCoord b){
    std::vector<CellCoord> cells;
    if (spec.layout == TileGridLayout::Hexagon){
        int64_t aq, ar, bq, br;
        gridHexAxial(spec, a, aq, ar);
        gridHexAxial(spec, b, bq, br);
        const int64_t steps = gridHexDistance(spec, a, b);
        cells.reserve((size_t)steps + 1);
        for (int64_t i = 0; i <= steps; i++){
            const double t = steps == 0 ? 0.0 : (double)i / (double)steps;
            // the nudge sends a point exactly between two cells to the same side every time
            const double fq = (double)aq + (double)(bq - aq) * t + 1e-7;
            const double fr = (double)ar + (double)(br - ar) * t + 2e-7;
            const double fs = -(double)aq - (double)ar + (double)(aq + ar - bq - br) * t - 3e-7;
            int64_t q = (int64_t)std::llround(fq), r = (int64_t)std::llround(fr);
            const int64_t s = (int64_t)std::llround(fs);
            const double dq = std::abs((double)q - fq), dr = std::abs((double)r - fr), ds = std::abs((double)s - fs);
            if (dq > dr && dq > ds)
                q = -r - s;
            else if (dr > ds)
                r = -q - s;
            const CellCoord cell = gridHexFromAxial(spec, q, r);
            if (cells.empty() || cells.back() != cell)
                cells.push_back(cell);
        }
        return cells;
    }
    if (spec.layout == TileGridLayout::IsometricStaggered){
        std::vector<CellCoord> diamonds;
        gridBresenham(toDiamond(spec, a), toDiamond(spec, b), diamonds);
        cells.reserve(diamonds.size());
        for (CellCoord diamond : diamonds)
            cells.push_back(fromDiamond(spec, diamond));
        return cells;
    }
    gridBresenham(a, b, cells);
    return cells;
}

int64_t tilegrid::distance(const TileGridSpec& spec, CellCoord a, CellCoord b){
    if (spec.layout == TileGridLayout::Hexagon)
        return gridHexDistance(spec, a, b);
    const CellCoord da = toDiamond(spec, a), db = toDiamond(spec, b);
    return std::max(std::llabs((int64_t)da.x - db.x), std::llabs((int64_t)da.y - db.y));
}

bool tilegrid::painterLess(const TileGridSpec& spec, CellCoord a, CellCoord b){
    int64_t va = a.y, vb = b.y;
    if (spec.layout == TileGridLayout::Isometric){
        va = (int64_t)a.x + a.y;
        vb = (int64_t)b.x + b.y;
    }else if ((spec.layout == TileGridLayout::IsometricStaggered || spec.layout == TileGridLayout::Hexagon) && spec.staggerAxis == StaggerAxis::X){
        va = 2 * (int64_t)a.y + (isStaggered(spec, a.x) ? 1 : 0);
        vb = 2 * (int64_t)b.y + (isStaggered(spec, b.x) ? 1 : 0);
    }
    if (va != vb)
        return va > vb;
    if (a.x != b.x)
        return a.x < b.x;
    return a.y < b.y;
}

bool tilegrid::layoutOverlaps(const TileGridSpec& spec){
    return spec.layout != TileGridLayout::Rectangle;
}

void tilegrid::planeToLocal(const TileGridSpec& spec, TilePoint p, float out[3]){
    out[0] = (float)p.x;
    if (spec.plane == TileGridPlane::XY){
        out[1] = (float)p.y;
        out[2] = 0.0f;
    }else{
        out[1] = 0.0f;
        out[2] = (float)-p.y;
    }
}

TilePoint tilegrid::localToPlane(const TileGridSpec& spec, const float local[3]){
    if (spec.plane == TileGridPlane::XY)
        return TilePoint{local[0], local[1]};
    return TilePoint{local[0], -local[2]};
}
