// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/TileColliderBuilder.h"

#include "clipper2/clipper.h"
#include "math/Vector2.h"
#include "physics/PolygonDecomposition2D.h"
#include "tilemap/TilesetPool.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

using namespace jasperengine;

namespace {

    using Clipper2Lib::Clipper64;
    using Clipper2Lib::ClipType;
    using Clipper2Lib::FillRule;
    using Clipper2Lib::Path64;
    using Clipper2Lib::Paths64;
    using Clipper2Lib::Point64;
    using Clipper2Lib::PolyPath64;
    using Clipper2Lib::PolyTree64;

    // the integer lattice of the union (R39: every vertex on the pixel grid is on it)
    constexpr int64_t SUBDIVISIONS = 16;
    // Box2D's linear slop at 64 px/m is 0.32 px (5.12 lattice units): points must be further apart
    constexpr int64_t MIN_POINT_SPACING = 6;
    // how far inside the solid a run's owner is looked up (well under a lattice unit)
    constexpr double OWNER_NUDGE = 1.0 / 64.0;
    // how far past a cell's edge the walk steps to find the next cell
    constexpr double CELL_STEP = 1e-5;

    int64_t toLattice(double value){
        return std::llround(value * (double)SUBDIVISIONS);
    }

    double fromLattice(int64_t value){
        return (double)value / (double)SUBDIVISIONS;
    }

    Point64 toLattice(const TilePoint& point){
        return Point64(toLattice(point.x), toLattice(point.y));
    }

    int64_t turn(const Point64& a, const Point64& b, const Point64& c){
        return (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
    }

    bool isCollinear(const Point64& a, const Point64& b, const Point64& c){
        return turn(a, b, c) == 0 && ((b.x - a.x) * (c.x - b.x) + (b.y - a.y) * (c.y - b.y)) > 0;
    }

    double distanceSq(const Point64& a, const Point64& b){
        const double dx = (double)(b.x - a.x);
        const double dy = (double)(b.y - a.y);
        return dx * dx + dy * dy;
    }

    bool pointsTooClose(const Point64& a, const Point64& b){
        return distanceSq(a, b) < (double)(MIN_POINT_SPACING * MIN_POINT_SPACING);
    }

    // Clipper2 needs every input with the same winding: under non-zero fill, a mirrored tile
    // (clockwise once flipped) and its neighbour would cancel where they overlap
    Path64 counterClockwise(Path64 path){
        if (Clipper2Lib::Area(path) < 0.0){
            std::reverse(path.begin(), path.end());
        }
        return path;
    }

    class CellReader{
    public:
        explicit CellReader(const TileCells& store): cells(store), size(store.getChunkSize()) {}

        uint32_t value(CellCoord cell){
            const TileChunk* chunk = locate(cell);
            return chunk ? chunk->cells[index(cell)] : 0;
        }

        uint8_t terrain(CellCoord cell){
            const TileChunk* chunk = locate(cell);
            return chunk && !chunk->terrain.empty() ? chunk->terrain[index(cell)] : (uint8_t)0;
        }

    private:
        const TileCells& cells;
        const int size;
        bool cached = false;
        ChunkCoord cachedCoord;
        const TileChunk* cachedChunk = nullptr;

        const TileChunk* locate(CellCoord cell){
            const ChunkCoord coord = TileCells::chunkOf(cell, size);
            if (!cached || coord != cachedCoord){
                cachedChunk = cells.chunk(coord);
                cachedCoord = coord;
                cached = true;
            }
            return cachedChunk;
        }

        size_t index(CellCoord cell) const {
            const int64_t x = (int64_t)cell.x - (int64_t)cachedCoord.x * size;
            const int64_t y = (int64_t)cell.y - (int64_t)cachedCoord.y * size;
            return (size_t)(y * size + x);
        }
    };

    // ---- the outline's walk through the grid's cells

    // the point of an outline where it enters a new cell or has a vertex; `owner` is the chunk of
    // the segment that starts here
    struct WalkEntry{
        Point64 point;
        ChunkCoord owner;
        bool real = true;        // a vertex of the outline, not a cell-boundary crossing
    };

    // the first point at or after tMin where the segment leaves the convex cell, as a fraction of
    // the segment; 1 when it ends inside. A rectangle cell is its pitch box, so a gap between
    // cells belongs to a cell (localToCell's half-open rule).
    double cellExit(const TileGridSpec& grid, CellCoord cell, TilePoint start, double dx, double dy, double tMin){
        std::array<TilePoint, 6> corners;
        int count = 0;
        if (grid.layout == TileGridLayout::Rectangle){
            const TilePoint origin = tilegrid::cellToLocal(grid, cell);
            const double px = tilegrid::pitchX(grid);
            const double py = tilegrid::pitchY(grid);
            corners[0] = origin;
            corners[1] = {origin.x + px, origin.y};
            corners[2] = {origin.x + px, origin.y + py};
            corners[3] = {origin.x, origin.y + py};
            count = 4;
        }else{
            const TileCellPolygon polygon = tilegrid::cellPolygon(grid, cell);
            corners = polygon.points;
            count = polygon.count;
        }
        double best = 1.0;
        for (int i = 0; i < count; i++){
            const TilePoint a = corners[(size_t)i];
            const TilePoint b = corners[(size_t)((i + 1) % count)];
            const double ex = b.x - a.x;
            const double ey = b.y - a.y;
            const double denom = dx * ey - dy * ex;
            // outward crossings only (a CCW polygon: its outside is to the right of the edge)
            if (denom <= 1e-12){
                continue;
            }
            const double t = ((a.x - start.x) * ey - (a.y - start.y) * ex) / denom;
            if (t > tMin + 1e-12 && t < best){
                best = t;
            }
        }
        return best;
    }

    // appends the edge's start and every cell it crosses, the crossings on the lattice
    void walkEdge(const TileGridSpec& grid, int chunkSize, const Point64& a, const Point64& b, std::vector<WalkEntry>& out){
        const double ax = fromLattice(a.x);
        const double ay = fromLattice(a.y);
        const double dx = fromLattice(b.x) - ax;
        const double dy = fromLattice(b.y) - ay;
        const double length = std::sqrt(dx * dx + dy * dy);
        // the solid is on the left of the outline, either winding: its CCW solids and CW holes
        const TilePoint start = {ax - dy / length * OWNER_NUDGE, ay + dx / length * OWNER_NUDGE};
        // the cell the edge goes into: a vertex on a cell's edge belongs to the cell above or to the
        // right (localToCell), which an edge heading left or down never enters
        const double stepFraction = CELL_STEP / length;
        CellCoord cell = tilegrid::localToCell(grid, TilePoint{start.x + dx * stepFraction, start.y + dy * stepFraction});
        out.push_back(WalkEntry{a, TileCells::chunkOf(cell, chunkSize), true});

        double t = 0.0;
        while (true){
            const double next = cellExit(grid, cell, start, dx, dy, t);
            if (next >= 1.0){
                break;
            }
            const TilePoint past = {start.x + dx * (next + stepFraction), start.y + dy * (next + stepFraction)};
            const CellCoord nextCell = tilegrid::localToCell(grid, past);
            t = next;
            if (nextCell == cell){
                continue;
            }
            cell = nextCell;
            const ChunkCoord owner = TileCells::chunkOf(cell, chunkSize);
            const Point64 crossing(std::llround((double)a.x + (double)(b.x - a.x) * next),
                                   std::llround((double)a.y + (double)(b.y - a.y) * next));
            if (crossing == b){
                // on the edge's end: the next edge's own walk decides
                break;
            }
            if (crossing == out.back().point){
                out.back().owner = owner;
                continue;
            }
            out.push_back(WalkEntry{crossing, owner, false});
        }
    }

    // the walk of a whole path; a closed one wraps, an open one ends with its last point
    std::vector<WalkEntry> walkPath(const TileGridSpec& grid, int chunkSize, const Path64& path, bool closed){
        std::vector<WalkEntry> entries;
        const size_t count = path.size();
        const size_t edges = closed ? count : count - 1;
        entries.reserve(count * 2);
        for (size_t i = 0; i < edges; i++){
            walkEdge(grid, chunkSize, path[i], path[(i + 1) % count], entries);
        }
        if (!closed){
            entries.push_back(WalkEntry{path.back(), ChunkCoord{}, true});
        }
        return entries;
    }

    TilePoint toPlane(const Point64& point, const TilePoint& offset){
        return {fromLattice(point.x) + offset.x, fromLattice(point.y) + offset.y};
    }

    // the chain's points without those closer than Box2D's slop to the one before; the ends stay
    std::vector<Point64> weldChain(const std::vector<Point64>& points){
        std::vector<Point64> welded;
        welded.reserve(points.size());
        for (size_t i = 0; i < points.size(); i++){
            if (i == 0 || i + 1 == points.size() || !pointsTooClose(welded.back(), points[i])){
                welded.push_back(points[i]);
            }
        }
        if (welded.size() >= 3 && pointsTooClose(welded[welded.size() - 2], welded.back())){
            welded.erase(welded.end() - 2);
        }
        return welded;
    }

    // a loop's points, closing gap welded too, and from 4 points (Box2D's chain minimum: a
    // triangle tile gets a point on its longest side)
    std::vector<Point64> finishLoop(std::vector<Point64> points){
        std::vector<Point64> welded;
        for (const Point64& point : points){
            if (welded.empty() || !pointsTooClose(welded.back(), point)){
                welded.push_back(point);
            }
        }
        while (welded.size() >= 2 && pointsTooClose(welded.back(), welded.front())){
            welded.pop_back();
        }
        if (welded.size() == 3){
            size_t longest = 0;
            for (size_t i = 1; i < 3; i++){
                if (distanceSq(welded[i], welded[(i + 1) % 3]) > distanceSq(welded[longest], welded[(longest + 1) % 3])){
                    longest = i;
                }
            }
            const Point64& a = welded[longest];
            const Point64& b = welded[(longest + 1) % 3];
            const Point64 middle((a.x + b.x) / 2, (a.y + b.y) / 2);
            welded.insert(welded.begin() + (std::ptrdiff_t)longest + 1, middle);
        }
        // a canonical start: the outputs of two builds compare equal
        std::rotate(welded.begin(), std::min_element(welded.begin(), welded.end(), [](const Point64& p, const Point64& q){
            return p.y != q.y ? p.y < q.y : p.x < q.x;
        }), welded.end());
        return welded;
    }

    // the points Box2D ghosts past an open chain's end when no neighbouring run continues it
    Point64 extrapolate(const Point64& end, const Point64& inner){
        const double dx = (double)(end.x - inner.x);
        const double dy = (double)(end.y - inner.y);
        const double length = std::sqrt(dx * dx + dy * dy);
        return Point64(end.x + std::llround(dx / length * (double)SUBDIVISIONS), end.y + std::llround(dy / length * (double)SUBDIVISIONS));
    }

    // the runs of the walk owned by this chunk, as chains: the crossings and the entries beside
    // them kept (the three shared points), and one more point past each end
    void emitRuns(const std::vector<WalkEntry>& entries, bool closed, ChunkCoord chunk, const TilePoint& offset,
                  std::vector<TileColliderChain>* loops, std::vector<TileColliderChain>& chains){
        const size_t count = entries.size();
        const size_t segments = closed ? count : count - 1;
        if (segments == 0){
            return;
        }
        auto owned = [&](size_t segment){ return entries[segment % segments].owner == chunk; };

        size_t first = segments;
        bool allOwned = true;
        for (size_t i = 0; i < segments; i++){
            if (owned(i)){
                if (first == segments && (i > 0 ? !owned(i - 1) : (!closed || !owned(segments - 1)))){
                    first = i;
                }
            }else{
                allOwned = false;
            }
        }

        if (closed && allOwned){
            if (!loops){
                return;
            }
            std::vector<Point64> points;
            for (const WalkEntry& entry : entries){
                if (entry.real){
                    points.push_back(entry.point);
                }
            }
            points = finishLoop(std::move(points));
            if (points.size() >= 4){
                TileColliderChain chain;
                chain.loop = true;
                chain.points.reserve(points.size());
                for (const Point64& point : points){
                    chain.points.push_back(toPlane(point, offset));
                }
                loops->push_back(std::move(chain));
            }
            return;
        }
        if (first == segments){
            return;
        }

        auto ghostBefore = [&](size_t index) -> Point64 {
            if (!closed && index == 0){
                return extrapolate(entries[0].point, entries[1].point);
            }
            const Point64& end = entries[index].point;
            size_t i = index;
            for (size_t guard = 0; guard < count; guard++){
                if (closed){
                    i = (i + count - 1) % count;
                }else if (i == 0){
                    return extrapolate(entries[0].point, entries[1].point);
                }else{
                    i--;
                }
                if (!pointsTooClose(entries[i].point, end)){
                    return entries[i].point;
                }
            }
            return entries[(index + count - 1) % count].point;
        };
        auto ghostAfter = [&](size_t index) -> Point64 {
            if (!closed && index + 1 == count){
                return extrapolate(entries[count - 1].point, entries[count - 2].point);
            }
            const Point64& end = entries[index].point;
            size_t i = index;
            for (size_t guard = 0; guard < count; guard++){
                if (closed){
                    i = (i + 1) % count;
                }else if (i + 1 == count){
                    return extrapolate(entries[count - 1].point, entries[count - 2].point);
                }else{
                    i++;
                }
                if (!pointsTooClose(entries[i].point, end)){
                    return entries[i].point;
                }
            }
            return entries[(index + 1) % count].point;
        };

        const size_t limit = first + segments;
        size_t segment = first;
        while (segment < limit){
            if (!owned(segment)){
                segment++;
                continue;
            }
            const size_t start = segment;
            while (segment + 1 < limit + 1 && owned(segment + 1) && segment + 1 < start + segments){
                segment++;
            }
            const size_t lastSegment = segment;
            const size_t end = lastSegment + 1;

            auto at = [&](size_t i){ return entries[closed ? i % count : i]; };
            std::vector<Point64> body;
            for (size_t i = start; i <= end; i++){
                const bool border = i == start || i == start + 1 || i == lastSegment || i == end;
                if (border || at(i).real){
                    body.push_back(at(i).point);
                }
            }
            body = weldChain(body);

            const size_t startIndex = closed ? start % count : start;
            const size_t endIndex = closed ? end % count : end;
            if (body.size() >= 2 && !pointsTooClose(body.front(), body.back())){
                std::vector<Point64> points;
                points.reserve(body.size() + 2);
                points.push_back(ghostBefore(startIndex));
                points.insert(points.end(), body.begin(), body.end());
                points.push_back(ghostAfter(endIndex));

                TileColliderChain chain;
                chain.points.reserve(points.size());
                for (const Point64& point : points){
                    chain.points.push_back(toPlane(point, offset));
                }
                chains.push_back(std::move(chain));
            }
            segment++;
        }
    }

    // ---- the tile shapes in plane coordinates

    Path64 toPath(const TilePoint* points, int count){
        Path64 path;
        path.reserve((size_t)count);
        for (int i = 0; i < count; i++){
            path.push_back(toLattice(points[i]));
        }
        return path;
    }

    void applyFlip(int& w, int& h, uint32_t flags, double& x, double& y){
        if (flags & TILE_TRANSPOSE){
            const double nx = (double)h - y;
            const double ny = (double)w - x;
            x = nx;
            y = ny;
            std::swap(w, h);
        }
        if (flags & TILE_FLIP_X){
            x = (double)w - x;
        }
        if (flags & TILE_FLIP_Y){
            y = (double)h - y;
        }
    }

    TilePoint flippedDirection(TilePoint direction, uint32_t flags){
        if (flags & TILE_TRANSPOSE){
            const double x = -direction.y;
            direction.y = -direction.x;
            direction.x = x;
        }
        if (flags & TILE_FLIP_X){
            direction.x = -direction.x;
        }
        if (flags & TILE_FLIP_Y){
            direction.y = -direction.y;
        }
        return direction;
    }

    TilePoint oneWayVector(TileOneWayDirection direction){
        switch (direction){
            case TileOneWayDirection::Down: return {0.0, -1.0};
            case TileOneWayDirection::Left: return {-1.0, 0.0};
            case TileOneWayDirection::Right: return {1.0, 0.0};
            case TileOneWayDirection::Up: break;
        }
        return {0.0, 1.0};
    }

    // ---- convex pieces

    void appendConvexPieces(const Path64& ring, const TilePoint& offset, std::vector<TileColliderPolygon>& out){
        std::vector<Vector2> points;
        points.reserve(ring.size());
        for (const Point64& point : ring){
            points.emplace_back((float)fromLattice(point.x), (float)fromLattice(point.y));
        }
        const Decomposition2D decomposition = decomposePolygon2D(points, 1.0f / 32.0f, 8);
        for (const std::vector<Vector2>& piece : decomposition.pieces){
            TileColliderPolygon polygon;
            polygon.points.reserve(piece.size());
            for (const Vector2& point : piece){
                polygon.points.push_back({(double)point.x + offset.x, (double)point.y + offset.y});
            }
            out.push_back(std::move(polygon));
        }
    }

    Path64 boundsRect(int64_t minX, int64_t minY, int64_t maxX, int64_t maxY){
        return Path64{Point64(minX, minY), Point64(maxX, minY), Point64(maxX, maxY), Point64(minX, maxY)};
    }

    void collectGroup(const PolyPath64& outer, std::vector<Paths64>& groups);

    void collectChildren(const PolyPath64& parent, std::vector<Paths64>& groups){
        for (size_t i = 0; i < parent.Count(); i++){
            collectGroup(*parent.Child(i), groups);
        }
    }

    // an outer polygon and its holes as one group; the islands inside its holes as more groups
    void collectGroup(const PolyPath64& outer, std::vector<Paths64>& groups){
        Paths64 group;
        group.push_back(outer.Polygon());
        for (size_t i = 0; i < outer.Count(); i++){
            const PolyPath64& hole = *outer.Child(i);
            group.push_back(hole.Polygon());
            collectChildren(hole, groups);
        }
        groups.push_back(std::move(group));
    }

    // a group of an outer ring and its holes as convex pieces: decomposePolygon2D takes one
    // simple ring, so each hole is cut through (halves of a smaller hole count) before it
    void appendConvexGroup(const Paths64& group, const TilePoint& offset, std::vector<TileColliderPolygon>& out){
        if (group.size() == 1){
            appendConvexPieces(group[0], offset, out);
            return;
        }
        const Path64& hole = group[1];
        int64_t holeMinX = hole[0].x, holeMaxX = hole[0].x, holeMinY = hole[0].y, holeMaxY = hole[0].y;
        for (const Point64& point : hole){
            holeMinX = std::min(holeMinX, point.x);
            holeMaxX = std::max(holeMaxX, point.x);
            holeMinY = std::min(holeMinY, point.y);
            holeMaxY = std::max(holeMaxY, point.y);
        }
        int64_t minX = holeMinX, maxX = holeMaxX, minY = holeMinY, maxY = holeMaxY;
        for (const Point64& point : group[0]){
            minX = std::min(minX, point.x);
            maxX = std::max(maxX, point.x);
            minY = std::min(minY, point.y);
            maxY = std::max(maxY, point.y);
        }
        const bool cutX = holeMaxX - holeMinX >= holeMaxY - holeMinY;
        const int64_t holeMin = cutX ? holeMinX : holeMinY;
        const int64_t holeMax = cutX ? holeMaxX : holeMinY + (holeMaxY - holeMinY);
        const int64_t cut = holeMin + (holeMax - holeMin) / 2;
        if (cut == holeMin){
            // a hole under a lattice unit across: nothing Box2D could tell from solid
            Paths64 withoutHole(group);
            withoutHole.erase(withoutHole.begin() + 1);
            appendConvexGroup(withoutHole, offset, out);
            return;
        }
        for (int side = 0; side < 2; side++){
            const Path64 half = cutX ? (side == 0 ? boundsRect(minX, minY, cut, maxY) : boundsRect(cut, minY, maxX, maxY))
                                     : (side == 0 ? boundsRect(minX, minY, maxX, cut) : boundsRect(minX, cut, maxX, maxY));
            Clipper64 clipper;
            clipper.AddSubject(group);
            clipper.AddClip(Paths64{half});
            PolyTree64 tree;
            clipper.Execute(ClipType::Intersection, FillRule::NonZero, tree);
            std::vector<Paths64> groups;
            collectChildren(tree, groups);
            for (const Paths64& piece : groups){
                appendConvexGroup(piece, offset, out);
            }
        }
    }

    void appendConvexOfTree(const PolyTree64& tree, const TilePoint& offset, std::vector<TileColliderPolygon>& out){
        std::vector<Paths64> groups;
        collectChildren(tree, groups);
        for (const Paths64& group : groups){
            appendConvexGroup(group, offset, out);
        }
    }

    // ---- one-way chains

    struct DirectedEdge{
        Point64 from;
        Point64 to;
    };

    bool pointLess(const Point64& a, const Point64& b){
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    }

    // the polygon's edges that face `direction` (CCW polygon: outward is to the right of an edge)
    void appendFacingEdges(const Path64& polygon, const TilePoint& direction, std::vector<DirectedEdge>& out){
        const Path64 path = counterClockwise(polygon);
        for (size_t i = 0; i < path.size(); i++){
            const Point64& a = path[i];
            const Point64& b = path[(i + 1) % path.size()];
            const double nx = (double)(b.y - a.y);
            const double ny = -(double)(b.x - a.x);
            if (nx * direction.x + ny * direction.y > 1e-9){
                out.push_back(DirectedEdge{a, b});
            }
        }
    }

    // links the facing edges into open paths (the edges of a row of platforms chain up)
    std::vector<Path64> linkEdges(std::vector<DirectedEdge> edges){
        std::sort(edges.begin(), edges.end(), [](const DirectedEdge& a, const DirectedEdge& b){
            if (a.from != b.from) return pointLess(a.from, b.from);
            return pointLess(a.to, b.to);
        });
        edges.erase(std::unique(edges.begin(), edges.end(), [](const DirectedEdge& a, const DirectedEdge& b){
            return a.from == b.from && a.to == b.to;
        }), edges.end());

        auto pointCmp = [](const Point64& a, const Point64& b){ return pointLess(a, b); };
        std::map<Point64, size_t, decltype(pointCmp)> byStart(pointCmp);
        std::set<Point64, decltype(pointCmp)> ends(pointCmp);
        for (size_t i = 0; i < edges.size(); i++){
            byStart.emplace(edges[i].from, i);
            ends.insert(edges[i].to);
        }
        std::vector<bool> used(edges.size(), false);
        std::vector<Path64> paths;
        auto walkFrom = [&](size_t start){
            Path64 path;
            path.push_back(edges[start].from);
            size_t index = start;
            while (true){
                used[index] = true;
                path.push_back(edges[index].to);
                auto next = byStart.find(edges[index].to);
                if (next == byStart.end() || used[next->second]){
                    break;
                }
                index = next->second;
            }
            // collinear runs of cells are one edge, as an outline's are
            Path64 merged;
            for (size_t i = 0; i < path.size(); i++){
                if (i > 0 && i + 1 < path.size() && isCollinear(path[i - 1], path[i], path[i + 1])){
                    continue;
                }
                merged.push_back(path[i]);
            }
            paths.push_back(std::move(merged));
        };
        // starts first; what is left can only be a closed loop of facing edges
        for (size_t i = 0; i < edges.size(); i++){
            if (!used[i] && ends.find(edges[i].from) == ends.end()){
                walkFrom(i);
            }
        }
        for (size_t i = 0; i < edges.size(); i++){
            if (!used[i]){
                walkFrom(i);
            }
        }
        return paths;
    }

    bool pointListLess(const std::vector<TilePoint>& a, const std::vector<TilePoint>& b){
        const size_t count = std::min(a.size(), b.size());
        for (size_t i = 0; i < count; i++){
            if (a[i].y != b[i].y) return a[i].y < b[i].y;
            if (a[i].x != b[i].x) return a[i].x < b[i].x;
        }
        return a.size() < b.size();
    }

    bool pointsEqual(const std::vector<TilePoint>& a, const std::vector<TilePoint>& b){
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++){
            if (a[i].x != b[i].x || a[i].y != b[i].y) return false;
        }
        return true;
    }

    TilePoint cellBasis(const TileGridSpec& grid, CellCoord step){
        const TilePoint origin = tilegrid::cellToLocal(grid, CellCoord{0, 0});
        const TilePoint target = tilegrid::cellToLocal(grid, step);
        return {target.x - origin.x, target.y - origin.y};
    }

}

bool TileColliderChunk::operator==(const TileColliderChunk& other) const {
    auto chainsEqual = [](const std::vector<TileColliderChain>& a, const std::vector<TileColliderChain>& b){
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++){
            if (a[i].loop != b[i].loop || !pointsEqual(a[i].points, b[i].points)) return false;
        }
        return true;
    };
    auto polygonsEqual = [](const std::vector<TileColliderPolygon>& a, const std::vector<TileColliderPolygon>& b){
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++){
            if (!pointsEqual(a[i].points, b[i].points)) return false;
        }
        return true;
    };
    return chainsEqual(loops, other.loops) && chainsEqual(chains, other.chains) && chainsEqual(oneWay, other.oneWay) &&
           polygonsEqual(polygons, other.polygons) && polygonsEqual(sensors, other.sensors);
}

TileColliderBuilder::TileColliderBuilder(TileColliderSettings input): settings(std::move(input)){
    const TileGridSpec& grid = settings.grid;
    // the layout's cells also touch the second ring (staggered rows half a pitch apart, hexes)
    int baseMargin = (grid.layout == TileGridLayout::Rectangle || grid.layout == TileGridLayout::Isometric) ? 1 : 2;

    if (settings.source == TileColliderSource::Terrain){
        solidTerrain.assign(256, 0);
        for (int terrain : settings.solidTerrains){
            if (terrain >= 0 && terrain < 254){
                solidTerrain[(size_t)terrain + 1] = 1;
            }
        }
        margin = baseMargin;
        return;
    }

    const TilePoint basisX = cellBasis(grid, CellCoord{1, 0});
    const TilePoint basisY = cellBasis(grid, CellCoord{0, 1});
    tileShift = {settings.tileAnchor.x * basisX.x + settings.tileAnchor.y * basisY.x,
                 settings.tileAnchor.x * basisX.y + settings.tileAnchor.y * basisY.y};
    if (settings.dualGrid){
        tileShift.x -= (basisX.x + basisY.x) * 0.5;
        tileShift.y -= (basisX.y + basisY.y) * 0.5;
    }

    double overhang = std::max(std::fabs(tileShift.x), std::fabs(tileShift.y));
    const double cellSpan = std::min(tilegrid::pitchX(grid), tilegrid::pitchY(grid));
    const double stepSize = std::max(cellSpan * 0.5, 1.0);

    if (settings.tileset){
        const TilesetData& tileset = *settings.tileset;
        tiles.resize((size_t)std::max(tileset.nextId, 0));
        for (const TileDef& tile : tileset.tiles){
            if (tile.id < 0 || (size_t)tile.id >= tiles.size() || tile.collision.mode == TileCollisionMode::None){
                continue;
            }
            TileInfo info;
            info.collision = &tile.collision;
            int width = tileset.tileWidth;
            int height = tileset.tileHeight;
            if (const TilesetSource* source = tileset.findSource(tile.source)){
                width = source->tileWidth > 0 ? source->tileWidth : width;
                height = source->tileHeight > 0 ? source->tileHeight : height;
            }
            if (settings.runtime){
                if (const TileRenderInfo* render = settings.runtime->tile(tile.id)){
                    if (render->width > 0 && render->height > 0){
                        width = render->width;
                        height = render->height;
                    }
                }
            }
            info.width = width;
            info.height = height;
            info.offsetX = tile.offsetX;
            info.offsetY = tile.offsetY;
            tiles[(size_t)tile.id] = info;

            // how far its shapes can reach beyond its cell, whatever its flips
            double reach = std::max(0.0, (double)std::max(width, height) - cellSpan);
            reach += (double)std::max(std::abs(tile.offsetX), std::abs(tile.offsetY));
            for (const TilePolygon& polygon : tile.collision.polygons){
                for (const auto& point : polygon.points){
                    reach = std::max(reach, (double)-point[0]);
                    reach = std::max(reach, (double)-point[1]);
                    reach = std::max(reach, (double)point[0] - width);
                    reach = std::max(reach, (double)point[1] - height);
                }
            }
            overhang = std::max(overhang, reach);
        }
    }
    margin = baseMargin + (int)std::ceil(overhang / stepSize);
}

int TileColliderBuilder::chunkReach() const {
    const int chunkSize = settings.cells ? settings.cells->getChunkSize() : TileCells::DEFAULT_CHUNK_SIZE;
    return (margin + chunkSize - 1) / chunkSize;
}

TileColliderChunk TileColliderBuilder::build(ChunkCoord chunk) const {
    TileColliderChunk out;
    if (!settings.cells){
        return out;
    }
    const TileCells& store = *settings.cells;
    const TileGridSpec& grid = settings.grid;
    const int chunkSize = store.getChunkSize();
    const CellRect own = store.chunkRect(chunk);
    const CellRect area{own.minX - margin, own.minY - margin, own.maxX + margin, own.maxY + margin};
    const int64_t areaWidth = area.width();
    const int64_t areaHeight = area.height();

    Paths64 solids;
    std::vector<DirectedEdge> facing;
    // full rectangle cells merge into spans before the union: a chunk of solid cells is then a
    // handful of rectangles, not a thousand (R58)
    const bool coalesce = settings.merge && grid.layout == TileGridLayout::Rectangle && grid.gapX == 0.0 && grid.gapY == 0.0;
    std::vector<uint8_t> fullCells;
    if (coalesce){
        fullCells.assign((size_t)(areaWidth * areaHeight), 0);
    }
    auto addSolid = [&](Path64 path, std::vector<Path64>* perCell){
        if (perCell){
            perCell->push_back(counterClockwise(std::move(path)));
        }else{
            solids.push_back(counterClockwise(std::move(path)));
        }
    };

    CellReader reader(store);
    const int solidCellCapacity = 8;
    std::vector<Path64> cellSolids;
    cellSolids.reserve((size_t)solidCellCapacity);
    std::vector<DirectedEdge> cellFacing;

    // a cell's shapes in plane coordinates, through addSolid and the sensors
    auto addCellPolygon = [&](CellCoord cell, const TilePoint& shift, std::vector<Path64>* perCell){
        const TileCellPolygon polygon = tilegrid::cellPolygon(grid, cell);
        TilePoint moved[6];
        for (int i = 0; i < polygon.count; i++){
            moved[i] = {polygon.points[(size_t)i].x + shift.x, polygon.points[(size_t)i].y + shift.y};
        }
        addSolid(toPath(moved, polygon.count), perCell);
    };

    auto addShapes = [&](CellCoord cell, uint32_t value, const CellRect& ownRect, bool merged){
        if (cellValueEmpty(value)){
            return;
        }
        const int32_t tile = cellValueTile(value);
        if (tile < 0 || (size_t)tile >= tiles.size() || !tiles[(size_t)tile].collision){
            return;
        }
        const TileInfo& info = tiles[(size_t)tile];
        const TileCollision& collision = *info.collision;
        const uint32_t flags = cellValueFlags(value);
        const bool isOwn = ownRect.contains(cell);

        auto placePolygon = [&](const TilePolygon& polygon, Path64& path){
            path.clear();
            path.reserve(polygon.points.size());
            const TilePoint anchor = tilegrid::cellToLocal(grid, cell);
            const bool centred = grid.layout != TileGridLayout::Rectangle;
            for (const auto& point : polygon.points){
                double x = (double)point[0];
                double y = (double)point[1];
                int width = info.width;
                int height = info.height;
                applyFlip(width, height, flags, x, y);
                const double left = centred ? (double)width * 0.5 : 0.0;
                path.push_back(toLattice(TilePoint{anchor.x - left + x + tileShift.x + (double)info.offsetX,
                                                    anchor.y + y + tileShift.y + (double)info.offsetY}));
            }
        };

        std::vector<Path64>* perCell = merged ? nullptr : &cellSolids;
        Path64 scratch;
        if (collision.mode == TileCollisionMode::Full){
            if (collision.kind == TileCollisionKind::Solid){
                if (coalesce && settings.merge){
                    fullCells[(size_t)(((int64_t)cell.y - area.minY) * areaWidth + ((int64_t)cell.x - area.minX))] = 1;
                }else{
                    addCellPolygon(cell, tileShift, perCell);
                }
            }else if (collision.kind == TileCollisionKind::Sensor){
                if (isOwn){
                    const TileCellPolygon polygon = tilegrid::cellPolygon(grid, cell);
                    Path64 path;
                    for (int i = 0; i < polygon.count; i++){
                        path.push_back(toLattice(TilePoint{polygon.points[(size_t)i].x + tileShift.x, polygon.points[(size_t)i].y + tileShift.y}));
                    }
                    appendConvexPieces(path, settings.offset, out.sensors);
                }
            }else if (merged || isOwn){
                const TileCellPolygon polygon = tilegrid::cellPolygon(grid, cell);
                TilePoint moved[6];
                for (int i = 0; i < polygon.count; i++){
                    moved[i] = {polygon.points[(size_t)i].x + tileShift.x, polygon.points[(size_t)i].y + tileShift.y};
                }
                appendFacingEdges(toPath(moved, polygon.count), flippedDirection(oneWayVector(collision.oneWay), flags), merged ? facing : cellFacing);
            }
            return;
        }
        for (const TilePolygon& polygon : collision.polygons){
            if (polygon.points.size() < 3){
                continue;
            }
            placePolygon(polygon, scratch);
            if (collision.kind == TileCollisionKind::Solid){
                addSolid(scratch, perCell);
            }else if (collision.kind == TileCollisionKind::Sensor){
                if (isOwn){
                    appendConvexPieces(counterClockwise(scratch), settings.offset, out.sensors);
                }
            }else if (merged || isOwn){
                appendFacingEdges(scratch, flippedDirection(oneWayVector(collision.oneWay), flags), merged ? facing : cellFacing);
            }
        }
    };

    if (!settings.merge){
        // one shape per cell: nothing crosses a chunk, so only the chunk's own cells count
        for (int64_t y = own.minY; y <= own.maxY; y++){
            for (int64_t x = own.minX; x <= own.maxX; x++){
                const CellCoord cell{(int32_t)x, (int32_t)y};
                cellSolids.clear();
                cellFacing.clear();
                if (settings.source == TileColliderSource::Terrain){
                    const uint8_t terrain = reader.terrain(cell);
                    if (terrain != TERRAIN_NONE_VALUE && solidTerrain[terrain]){
                        addCellPolygon(cell, TilePoint{}, &cellSolids);
                    }
                }else{
                    addShapes(cell, reader.value(cell), own, false);
                }
                if (!cellSolids.empty()){
                    if (settings.geometry == TileColliderGeometry::Polygons){
                        Clipper64 clipper;
                        clipper.AddSubject(cellSolids);
                        PolyTree64 tree;
                        clipper.Execute(ClipType::Union, FillRule::NonZero, tree);
                        appendConvexOfTree(tree, settings.offset, out.polygons);
                    }else{
                        const Paths64 outlines = Clipper2Lib::Union(cellSolids, FillRule::NonZero);
                        for (const Path64& outline : outlines){
                            std::vector<Point64> points(outline.begin(), outline.end());
                            points = finishLoop(std::move(points));
                            if (points.size() >= 4){
                                TileColliderChain chain;
                                chain.loop = true;
                                for (const Point64& point : points){
                                    chain.points.push_back(toPlane(point, settings.offset));
                                }
                                out.loops.push_back(std::move(chain));
                            }
                        }
                    }
                }
                if (!cellFacing.empty()){
                    for (const Path64& path : linkEdges(cellFacing)){
                        emitRuns(walkPath(grid, chunkSize, path, false), false, chunk, settings.offset, nullptr, out.oneWay);
                    }
                }
            }
        }
    }else{
        for (int64_t y = area.minY; y <= area.maxY; y++){
            for (int64_t x = area.minX; x <= area.maxX; x++){
                const CellCoord cell{(int32_t)x, (int32_t)y};
                if (settings.source == TileColliderSource::Terrain){
                    const uint8_t terrain = reader.terrain(cell);
                    if (terrain != TERRAIN_NONE_VALUE && solidTerrain[terrain]){
                        if (coalesce){
                            fullCells[(size_t)((y - area.minY) * areaWidth + (x - area.minX))] = 1;
                        }else{
                            addCellPolygon(cell, TilePoint{}, nullptr);
                        }
                    }
                }else{
                    addShapes(cell, reader.value(cell), own, true);
                }
            }
        }

        if (coalesce){
            // spans of full cells per row, then rows with the same span stacked
            const double pitchX = tilegrid::pitchX(grid);
            const double pitchY = tilegrid::pitchY(grid);
            const TilePoint shift = settings.source == TileColliderSource::Terrain ? TilePoint{} : tileShift;
            std::map<std::pair<int64_t, int64_t>, int64_t> open;    // (x0, x1) -> first row
            auto emitRect = [&](int64_t x0, int64_t x1, int64_t y0, int64_t y1){
                const Point64 minCorner = toLattice(TilePoint{(double)x0 * pitchX + shift.x, (double)y0 * pitchY + shift.y});
                const Point64 maxCorner = toLattice(TilePoint{(double)x1 * pitchX + shift.x, (double)y1 * pitchY + shift.y});
                addSolid(boundsRect(minCorner.x, minCorner.y, maxCorner.x, maxCorner.y), nullptr);
            };
            for (int64_t row = 0; row <= areaHeight; row++){
                std::map<std::pair<int64_t, int64_t>, int64_t> spans;
                if (row < areaHeight){
                    int64_t runStart = -1;
                    for (int64_t column = 0; column <= areaWidth; column++){
                        const bool full = column < areaWidth && fullCells[(size_t)(row * areaWidth + column)];
                        if (full && runStart < 0){
                            runStart = column;
                        }else if (!full && runStart >= 0){
                            const auto key = std::make_pair(runStart, column);
                            auto found = open.find(key);
                            spans[key] = found != open.end() ? found->second : row;
                            runStart = -1;
                        }
                    }
                }
                for (const auto& [span, start] : open){
                    if (spans.find(span) == spans.end()){
                        emitRect(area.minX + span.first, area.minX + span.second, area.minY + start, area.minY + row);
                    }
                }
                open = std::move(spans);
            }
        }

        if (!solids.empty()){
            const Paths64 outlines = Clipper2Lib::Union(solids, FillRule::NonZero);
            if (settings.geometry == TileColliderGeometry::Polygons){
                // the chunk's region: the cells' area (half-open between chunks, as the polygons
                // only need to meet exactly)
                Paths64 region;
                if (grid.layout == TileGridLayout::Rectangle){
                    const double pitchX = tilegrid::pitchX(grid);
                    const double pitchY = tilegrid::pitchY(grid);
                    const Point64 minCorner = toLattice(TilePoint{(double)own.minX * pitchX, (double)own.minY * pitchY});
                    const Point64 maxCorner = toLattice(TilePoint{((double)own.maxX + 1.0) * pitchX, ((double)own.maxY + 1.0) * pitchY});
                    region.push_back(boundsRect(minCorner.x, minCorner.y, maxCorner.x, maxCorner.y));
                }else{
                    Paths64 cellPolygons;
                    for (int64_t y = own.minY; y <= own.maxY; y++){
                        for (int64_t x = own.minX; x <= own.maxX; x++){
                            const TileCellPolygon polygon = tilegrid::cellPolygon(grid, CellCoord{(int32_t)x, (int32_t)y});
                            cellPolygons.push_back(counterClockwise(toPath(polygon.points.data(), polygon.count)));
                        }
                    }
                    region = Clipper2Lib::Union(cellPolygons, FillRule::NonZero);
                }
                Clipper64 clipper;
                clipper.AddSubject(outlines);
                clipper.AddClip(region);
                PolyTree64 tree;
                clipper.Execute(ClipType::Intersection, FillRule::NonZero, tree);
                appendConvexOfTree(tree, settings.offset, out.polygons);
            }else{
                for (const Path64& outline : outlines){
                    const std::vector<WalkEntry> entries = walkPath(grid, chunkSize, outline, true);
                    emitRuns(entries, true, chunk, settings.offset, &out.loops, out.chains);
                }
            }
        }

        if (!facing.empty()){
            for (const Path64& path : linkEdges(std::move(facing))){
                emitRuns(walkPath(grid, chunkSize, path, false), false, chunk, settings.offset, nullptr, out.oneWay);
            }
        }
    }

    auto byPoints = [](const auto& a, const auto& b){ return pointListLess(a.points, b.points); };
    std::sort(out.loops.begin(), out.loops.end(), byPoints);
    std::sort(out.chains.begin(), out.chains.end(), byPoints);
    std::sort(out.oneWay.begin(), out.oneWay.end(), byPoints);
    std::sort(out.polygons.begin(), out.polygons.end(), byPoints);
    std::sort(out.sensors.begin(), out.sensors.end(), byPoints);
    return out;
}
