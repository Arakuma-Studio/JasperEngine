// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The generation-stamped scratch, the heuristics, the reservation penalty and the leased solver
// pool follow OpenEngine's AStarSolver.cpp, AStarSolverPool.cpp and PathSmoother.cpp
// (Engine/Modules/Pathfinding/Source/), Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors, MIT License. See NOTICE.md.

#include "GridPathfinder.h"

#include "jobs/JobSystem.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::nav;

namespace {

    constexpr uint8_t STATE_OPEN = 1;
    constexpr uint8_t STATE_CLOSED = 2;

    // a diagonal step's length, as NavGridMap::neighbours gives it
    constexpr float SQRT2_STEP = 1.4142135623730951f;

    struct HeapGreater {
        template <class Node>
        bool operator()(const Node& a, const Node& b) const {
            if (a.f != b.f) return a.f > b.f;
            if (a.h != b.h) return a.h > b.h;
            return a.cell > b.cell;
        }
    };

}

// ---------------------------------------------------------------- reservations

void GridReservations::reset(int cellCount) {
    holders.assign((size_t)std::max(0, cellCount), 0);
    until.assign((size_t)std::max(0, cellCount), 0);
    held.clear();
}

void GridReservations::release(uint32_t agent) {
    auto it = held.find(agent);
    if (it == held.end())
        return;
    for (int cell : it->second) {
        if (cell >= 0 && (size_t)cell < holders.size() && holders[(size_t)cell] == agent) {
            holders[(size_t)cell] = 0;
            until[(size_t)cell] = 0;
        }
    }
    held.erase(it);
}

void GridReservations::reserve(uint32_t agent, const std::vector<int>& cells, uint32_t untilTick) {
    if (agent == 0)
        return;
    std::vector<int>& taken = held[agent];
    // Keep the agent's path storage while replacing its claims. A transferred cell belongs
    // to its new holder even if it is still in this agent's previous path.
    for (int cell : taken) {
        if (cell >= 0 && (size_t)cell < holders.size() && holders[(size_t)cell] == agent) {
            holders[(size_t)cell] = 0;
            until[(size_t)cell] = 0;
        }
    }
    taken.clear();
    taken.reserve(cells.size());
    for (int cell : cells) {
        if (cell < 0 || (size_t)cell >= holders.size())
            continue;
        // a cell another agent holds for longer stays theirs
        if (holders[(size_t)cell] != 0 && holders[(size_t)cell] != agent && until[(size_t)cell] >= untilTick)
            continue;
        holders[(size_t)cell] = agent;
        until[(size_t)cell] = untilTick;
        taken.push_back(cell);
    }
}

bool GridReservations::isReservedByOther(int cell, uint32_t agent, uint32_t tick) const {
    if (cell < 0 || (size_t)cell >= holders.size())
        return false;
    const uint32_t who = holders[(size_t)cell];
    return who != 0 && who != agent && until[(size_t)cell] >= tick;
}

uint32_t GridReservations::holder(int cell, uint32_t tick) const {
    if (cell < 0 || (size_t)cell >= holders.size() || until[(size_t)cell] < tick)
        return 0;
    return holders[(size_t)cell];
}

// ---------------------------------------------------------------- solver

GridSolver::GridSolver() = default;
GridSolver::~GridSolver() = default;

size_t GridSolver::scratchBytes() const {
    return g.capacity() * sizeof(float) + parent.capacity() * sizeof(int32_t) + generation.capacity() * sizeof(uint16_t) +
           state.capacity() * sizeof(uint8_t) + heap.capacity() * sizeof(HeapNode) + openCells.capacity() +
           4 * (jumpDistance[0].capacity() + runDistance[0].capacity()) * sizeof(int16_t);
}

void GridSolver::prepare(int cells) {
    if ((int)g.size() != cells) {
        g.assign((size_t)cells, 0.0f);
        parent.assign((size_t)cells, -1);
        generation.assign((size_t)cells, 0);
        state.assign((size_t)cells, 0);
        current = 0;
    }
    current++;
    if (current == 0) {
        // the stamps wrapped: one clear every 65535 solves
        std::fill(generation.begin(), generation.end(), 0);
        current = 1;
    }
    heap.clear();
}

bool GridSolver::passable(const NavGridMap& map, int cell, const GridQuery& query) const {
    const uint8_t area = map.getAreaAt(cell);
    return area != AREA_NOT_WALKABLE && (query.areaMask & (1u << area)) != 0;
}

GridPath GridSolver::solve(const NavGridMap& map, GridCell start, GridCell goal, const GridQuery& query, const float* areaCosts) {
    GridPath path;
    if (!map.contains(start) || !map.contains(goal))
        return path;
    const int startIndex = map.index(start);
    const int goalIndex = map.index(goal);
    if (!passable(map, startIndex, query))
        return path;
    const bool goalOpen = passable(map, goalIndex, query) && map.isReachable(start, goal);
    // the reachability index rejects without a search (spec R17)
    if (!goalOpen && !query.allowPartial)
        return path;
    // An unreachable goal with allowPartial: the reachable cell closest to it (by the heuristic)
    // is found from the goal outward, then searched for like any goal, instead of searching all
    // of the start's component
    if (!goalOpen) {
        const int target = nearestReachable(map, start, goal, query);
        if (target == startIndex) {
            path.status = PathStatus::Partial;
            path.cells.push_back(start);
            smooth(map, path, query, areaCosts);
            return path;
        }
        if (target >= 0) {
            GridQuery exact = query;
            exact.allowPartial = false;
            GridPath partial = solve(map, start, map.cellOf(target), exact, areaCosts);
            if (partial.status == PathStatus::Complete) {
                partial.status = PathStatus::Partial;
                return partial;
            }
        }
        // the area mask splits the component further than the index knows: the full search
    }

    // a uniform-cost square grid (8 neighbours, no corner cutting, no reservations or heights):
    // jump point search finds a path of the same cost expanding only its jump points
    if (goalOpen && jpsApplies(map, query, areaCosts))
        return solveJps(map, start, goal, query, areaCosts);

    prepare(map.getCellCount());

    g[(size_t)startIndex] = 0.0f;
    parent[(size_t)startIndex] = -1;
    generation[(size_t)startIndex] = current;
    state[(size_t)startIndex] = STATE_OPEN;
    const float startH = map.heuristic(start, goal);
    heap.push_back(HeapNode{startH, startH, 0.0f, startIndex});

    int best = startIndex;
    float bestH = startH;
    float bestG = 0.0f;
    bool found = false;
    GridCell next[NavGridMap::MAX_NEIGHBOURS];
    float steps[NavGridMap::MAX_NEIGHBOURS];
    const uint8_t* areas = map.areaData();
    // the areas a step may enter: the query's mask without Not Walkable
    const uint32_t passMask = (uint32_t)query.areaMask & ~(1u << AREA_NOT_WALKABLE);
    const bool square = map.getLayout() == GridLayout::Square;
    const bool eight = square && map.getNeighbourMode() == 8;
    const bool cornerCutting = query.cornerCutting;
    const bool checkHeights = map.hasHeights();
    const int width = map.getWidth(), height = map.getHeight();

    while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), HeapGreater());
        const HeapNode node = heap.back();
        heap.pop_back();
        // a stale entry (the cell was reached cheaper since): its g is not the cell's
        if (state[(size_t)node.cell] == STATE_CLOSED || node.g != g[(size_t)node.cell])
            continue;
        state[(size_t)node.cell] = STATE_CLOSED;
        path.expanded++;
        if (node.cell == goalIndex) {
            found = true;
            break;
        }
        if (node.h < bestH || (node.h == bestH && g[(size_t)node.cell] < bestG)) {
            best = node.cell;
            bestH = node.h;
            bestG = g[(size_t)node.cell];
        }
        const GridCell cell = map.cellOf(node.cell);
        const float base = g[(size_t)node.cell];
        // one step to `to` (its step length): the rules, the cost, the open list
        auto relax = [&](int to, GridCell nextCell, float step) {
            if (!((passMask >> areas[to]) & 1u))
                return;
            if (checkHeights && !map.stepAllowed(node.cell, to, step))
                return;
            const bool seen = generation[(size_t)to] == current;
            if (seen && state[(size_t)to] == STATE_CLOSED)
                return;
            float cost = step * areaCosts[areas[to]];
            if (query.reservations && query.reservations->isReservedByOther(to, query.agent, query.tick))
                cost *= query.reservationPenalty;
            const float candidate = base + cost;
            if (seen && candidate >= g[(size_t)to])
                return;
            generation[(size_t)to] = current;
            g[(size_t)to] = candidate;
            parent[(size_t)to] = node.cell;
            state[(size_t)to] = STATE_OPEN;
            const float h = map.heuristic(nextCell, goal);
            heap.push_back(HeapNode{candidate + h, h, candidate, to});
            std::push_heap(heap.begin(), heap.end(), HeapGreater());
        };
        if (square) {
            // the square layout by index: right, up, left, down, then the diagonals
            const int x = cell.x, y = cell.y;
            const bool right = x + 1 < width, left = x > 0, up = y + 1 < height, down = y > 0;
            const int i = node.cell;
            if (right) relax(i + 1, GridCell{x + 1, y}, 1.0f);
            if (up) relax(i + width, GridCell{x, y + 1}, 1.0f);
            if (left) relax(i - 1, GridCell{x - 1, y}, 1.0f);
            if (down) relax(i - width, GridCell{x, y - 1}, 1.0f);
            if (eight) {
                auto open = [&](int index) { return ((passMask >> areas[index]) & 1u) != 0; };
                // no corner cutting: both orthogonal cells of a diagonal must be open
                if (right && up && (cornerCutting || (open(i + 1) && open(i + width)))) relax(i + 1 + width, GridCell{x + 1, y + 1}, SQRT2_STEP);
                if (left && up && (cornerCutting || (open(i - 1) && open(i + width)))) relax(i - 1 + width, GridCell{x - 1, y + 1}, SQRT2_STEP);
                if (left && down && (cornerCutting || (open(i - 1) && open(i - width)))) relax(i - 1 - width, GridCell{x - 1, y - 1}, SQRT2_STEP);
                if (right && down && (cornerCutting || (open(i + 1) && open(i - width)))) relax(i + 1 - width, GridCell{x + 1, y - 1}, SQRT2_STEP);
            }
        } else {
            const int count = map.neighbours(cell, next, steps);
            for (int n = 0; n < count; n++)
                relax(map.index(next[n]), next[n], steps[n]);
        }
    }

    int end = goalIndex;
    if (!found) {
        if (!query.allowPartial)
            return GridPath{PathStatus::Invalid, {}, {}, 0.0f, path.expanded};
        end = best;
    }
    for (int cell = end; cell >= 0; cell = parent[(size_t)cell])
        path.cells.push_back(map.cellOf(cell));
    std::reverse(path.cells.begin(), path.cells.end());
    path.cost = g[(size_t)end];
    path.status = found ? PathStatus::Complete : PathStatus::Partial;
    smooth(map, path, query, areaCosts);
    return path;
}

bool GridSolver::jpsApplies(const NavGridMap& map, const GridQuery& query, const float* areaCosts) const {
    if (map.getLayout() != GridLayout::Square || map.getNeighbourMode() != 8 || query.cornerCutting || map.hasHeights() ||
        query.reservations)
        return false;
    const uint32_t passMask = (uint32_t)query.areaMask & ~(1u << AREA_NOT_WALKABLE);
    float cost = -1.0f;
    for (int a = 0; a < AREA_COUNT; a++) {
        if (!((passMask >> a) & 1u))
            continue;
        if (cost < 0.0f)
            cost = areaCosts[a];
        else if (areaCosts[a] != cost)
            return false;
    }
    return cost >= 0.0f;
}

// Jump point search without corner cutting (a diagonal needs both orthogonal cells open), the
// pruning rules of PathFinding.js's JPFMoveDiagonallyIfNoObstacles: straight runs and diagonals
// are scanned without the open list, which only holds jump points. Every passable area costs
// the same, so a path's cost is its octile length times that cost, as the plain A* gives it.
GridPath GridSolver::solveJps(const NavGridMap& map, GridCell start, GridCell goal, const GridQuery& query, const float* areaCosts) {
    GridPath path;
    const uint8_t* areas = map.areaData();
    const uint32_t passMask = (uint32_t)query.areaMask & ~(1u << AREA_NOT_WALKABLE);
    const int width = map.getWidth(), height = map.getHeight();
    const float unit = areaCosts[areas[map.index(start)]];
    const int stride = width + 2;
    if (openMap != &map || openRevision != map.getRevision() || openMask != passMask || openCells.size() != (size_t)(stride * (height + 2))) {
        openCells.assign((size_t)(stride * (height + 2)), 0);
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
                openCells[(size_t)((y + 1) * stride + x + 1)] = (uint8_t)((passMask >> areas[y * width + x]) & 1u);
        openMap = &map;
        openRevision = map.getRevision();
        openMask = passMask;
        const uint8_t* at = openCells.data() + stride + 1;
        auto isOpen = [at, stride](int x, int y) { return at[y * stride + x] != 0; };
        const int steps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int d = 0; d < 4; d++) {
            const int dx = steps[d][0], dy = steps[d][1];
            jumpDistance[d].assign((size_t)width * (size_t)height, -1);
            runDistance[d].assign((size_t)width * (size_t)height, 0);
            // from the far end of each run backwards: what lies ahead is known
            const int x0 = dx > 0 ? width - 1 : 0, x1 = dx > 0 ? -1 : width, sx = dx > 0 ? -1 : 1;
            const int y0 = dy > 0 ? height - 1 : 0, y1 = dy > 0 ? -1 : height, sy = dy > 0 ? -1 : 1;
            for (int y = y0; y != y1; y += sy) {
                for (int x = x0; x != x1; x += sx) {
                    const size_t c = (size_t)(y * width + x);
                    if (!isOpen(x, y))
                        continue;
                    bool forced;
                    if (dx != 0)
                        forced = (isOpen(x, y - 1) && !isOpen(x - dx, y - 1)) || (isOpen(x, y + 1) && !isOpen(x - dx, y + 1));
                    else
                        forced = (isOpen(x - 1, y) && !isOpen(x - 1, y - dy)) || (isOpen(x + 1, y) && !isOpen(x + 1, y - dy));
                    const int nx = x + dx, ny = y + dy;
                    const bool aheadOpen = isOpen(nx, ny);
                    const size_t ahead = aheadOpen ? (size_t)(ny * width + nx) : 0;
                    runDistance[d][c] = aheadOpen ? (int16_t)(runDistance[d][ahead] + 1) : 0;
                    if (forced)
                        jumpDistance[d][c] = 0;
                    else if (aheadOpen && jumpDistance[d][ahead] >= 0)
                        jumpDistance[d][c] = (int16_t)(jumpDistance[d][ahead] + 1);
                }
            }
        }
    }
    const uint8_t* openAt = openCells.data() + stride + 1;
    // x and y may be one cell outside the grid: the border is closed
    auto open = [openAt, stride](int x, int y) { return openAt[y * stride + x] != 0; };
    const int goalIndex = map.index(goal);
    // the next jump point from (x, y) moving straight (one of dx, dy zero), -1 when none
    auto jumpStraight = [&](int x, int y, int dx, int dy) -> int {
        if (!open(x, y))
            return -1;
        const int d = dx > 0 ? 0 : dx < 0 ? 1 : dy > 0 ? 2 : 3;
        const size_t c = (size_t)(y * width + x);
        const int jumpAt = jumpDistance[d][c];
        const int reach = jumpAt >= 0 ? jumpAt : runDistance[d][c];
        // the goal on the run before the jump point (or the wall)
        const int k = dx != 0 ? (goal.y == y ? (goal.x - x) * dx : -1) : (goal.x == x ? (goal.y - y) * dy : -1);
        if (k >= 0 && k <= reach)
            return goalIndex;
        return jumpAt >= 0 ? (y + dy * jumpAt) * width + (x + dx * jumpAt) : -1;
    };
    // moving diagonally: a cell is a jump point when a straight scan from it finds one
    auto jump = [&](int x, int y, int dx, int dy) -> int {
        if (dx == 0 || dy == 0)
            return jumpStraight(x, y, dx, dy);
        while (true) {
            if (!open(x, y))
                return -1;
            const int index = y * width + x;
            if (index == goalIndex)
                return index;
            if (jumpStraight(x + dx, y, dx, 0) >= 0 || jumpStraight(x, y + dy, 0, dy) >= 0)
                return index;
            if (!(open(x + dx, y) && open(x, y + dy)))
                return -1;
            x += dx;
            y += dy;
        }
    };
    auto octile = [](int ax, int ay, int bx, int by) {
        const int dx = std::abs(ax - bx), dy = std::abs(ay - by);
        return (float)(dx + dy) + (SQRT2_STEP - 2.0f) * (float)std::min(dx, dy);
    };

    prepare(map.getCellCount());
    const int startIndex = map.index(start);
    g[(size_t)startIndex] = 0.0f;
    parent[(size_t)startIndex] = -1;
    generation[(size_t)startIndex] = current;
    state[(size_t)startIndex] = STATE_OPEN;
    const float startH = octile(start.x, start.y, goal.x, goal.y) * unit;
    heap.push_back(HeapNode{startH, startH, 0.0f, startIndex});
    bool found = false;
    int directions[8][2];
    while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), HeapGreater());
        const HeapNode node = heap.back();
        heap.pop_back();
        if (state[(size_t)node.cell] == STATE_CLOSED || node.g != g[(size_t)node.cell])
            continue;
        state[(size_t)node.cell] = STATE_CLOSED;
        path.expanded++;
        if (node.cell == goalIndex) {
            found = true;
            break;
        }
        const int x = node.cell % width, y = node.cell / width;
        // the pruned directions (all of them at the start)
        int count = 0;
        const int from = parent[(size_t)node.cell];
        if (from < 0) {
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if ((dx || dy) && open(x + dx, y + dy) && (!(dx && dy) || (open(x + dx, y) && open(x, y + dy)))) {
                        directions[count][0] = dx;
                        directions[count][1] = dy;
                        count++;
                    }
        } else {
            const int px = from % width, py = from / width;
            const int dx = (x > px) - (x < px), dy = (y > py) - (y < py);
            auto add = [&](int ddx, int ddy) {
                directions[count][0] = ddx;
                directions[count][1] = ddy;
                count++;
            };
            if (dx && dy) {
                if (open(x, y + dy)) add(0, dy);
                if (open(x + dx, y)) add(dx, 0);
                if (open(x, y + dy) && open(x + dx, y)) add(dx, dy);
            } else if (dx) {
                const bool next = open(x + dx, y), up = open(x, y + 1), down = open(x, y - 1);
                if (next) {
                    add(dx, 0);
                    if (up) add(dx, 1);
                    if (down) add(dx, -1);
                }
                if (up) add(0, 1);
                if (down) add(0, -1);
            } else {
                const bool next = open(x, y + dy), right = open(x + 1, y), left = open(x - 1, y);
                if (next) {
                    add(0, dy);
                    if (right) add(1, dy);
                    if (left) add(-1, dy);
                }
                if (right) add(1, 0);
                if (left) add(-1, 0);
            }
        }
        for (int d = 0; d < count; d++) {
            const int dx = directions[d][0], dy = directions[d][1];
            // a diagonal entry needs its two orthogonal cells (checked above for the pruned set)
            const int to = jump(x + dx, y + dy, dx, dy);
            if (to < 0 || (state[(size_t)to] == STATE_CLOSED && generation[(size_t)to] == current))
                continue;
            const int tx = to % width, ty = to / width;
            const float candidate = node.g + octile(x, y, tx, ty) * unit;
            if (generation[(size_t)to] == current && candidate >= g[(size_t)to])
                continue;
            generation[(size_t)to] = current;
            g[(size_t)to] = candidate;
            parent[(size_t)to] = node.cell;
            state[(size_t)to] = STATE_OPEN;
            const float h = octile(tx, ty, goal.x, goal.y) * unit;
            heap.push_back(HeapNode{candidate + h, h, candidate, to});
            std::push_heap(heap.begin(), heap.end(), HeapGreater());
        }
    }
    if (!found)
        return GridPath{PathStatus::Invalid, {}, {}, 0.0f, path.expanded};
    // the jump points, then every cell between them (straight or diagonal runs)
    std::vector<int> points;
    for (int cell = goalIndex; cell >= 0; cell = parent[(size_t)cell])
        points.push_back(cell);
    std::reverse(points.begin(), points.end());
    path.cells.push_back(start);
    for (size_t i = 1; i < points.size(); i++) {
        int x = points[i - 1] % width, y = points[i - 1] / width;
        const int tx = points[i] % width, ty = points[i] / width;
        const int dx = (tx > x) - (tx < x), dy = (ty > y) - (ty < y);
        while (x != tx || y != ty) {
            // a jump is purely straight or purely diagonal
            const int sx = (x != tx) ? dx : 0, sy = (y != ty) ? dy : 0;
            x += sx;
            y += sy;
            path.cells.push_back(GridCell{x, y});
        }
    }
    path.cost = g[(size_t)goalIndex];
    path.status = PathStatus::Complete;
    smooth(map, path, query, areaCosts);
    return path;
}

int GridSolver::nearestReachable(const NavGridMap& map, GridCell start, GridCell goal, const GridQuery& query) {
    const uint32_t component = map.getComponent(start);
    if (component == UINT32_MAX)
        return -1;
    prepare(map.getCellCount());
    const int goalIndex = map.index(goal);
    generation[(size_t)goalIndex] = current;
    const float goalH = 0.0f;
    heap.push_back(HeapNode{goalH, goalH, 0.0f, goalIndex});
    GridCell next[NavGridMap::MAX_NEIGHBOURS];
    float steps[NavGridMap::MAX_NEIGHBOURS];
    while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), HeapGreater());
        const HeapNode node = heap.back();
        heap.pop_back();
        const GridCell cell = map.cellOf(node.cell);
        if (passable(map, node.cell, query) && map.getComponent(cell) == component)
            return node.cell;
        // every neighbour, walkable or not: the search crosses walls to reach the component
        const int count = map.neighbours(cell, next, steps);
        for (int n = 0; n < count; n++) {
            const int to = map.index(next[n]);
            if (generation[(size_t)to] == current)
                continue;
            generation[(size_t)to] = current;
            const float h = map.heuristic(next[n], goal);
            heap.push_back(HeapNode{h, h, 0.0f, to});
            std::push_heap(heap.begin(), heap.end(), HeapGreater());
        }
    }
    return -1;
}

bool GridSolver::lineOfSight(const NavGridMap& map, GridCell a, GridCell b, const GridQuery& query, float maxCost,
                             const float* areaCosts) const {
    auto ok = [&](GridCell cell, int previous) {
        if (!map.contains(cell))
            return false;
        const int i = map.index(cell);
        if (!passable(map, i, query) || areaCosts[map.getAreaAt(i)] > maxCost)
            return false;
        return previous < 0 || map.stepAllowed(previous, i, 1.0f);
    };
    if (map.getLayout() == GridLayout::Hex) {
        // cube interpolation, sampled a hair to both sides so a line along an edge sees both cells
        const int steps = map.hexDistance(a, b);
        const Vector2 pa = map.cellCenter(a), pb = map.cellCenter(b);
        const Vector2 normal = Vector2(-(pb.y - pa.y), pb.x - pa.x).normalized() * (map.getCellSize() * 1e-3f);
        int previous = map.index(a);
        for (int i = 1; i <= steps; i++) {
            const float t = (float)i / (float)steps;
            const Vector2 p = pa + (pb - pa) * t;
            for (int side = -1; side <= 1; side += 2) {
                const GridCell cell = map.cellAt(p + normal * (float)side);
                if (!ok(cell, previous))
                    return false;
            }
            previous = map.index(map.cellAt(p));
        }
        return true;
    }
    // Amanatides-Woo traversal between the centres; through a corner both side cells must be open
    const float size = map.getCellSize();
    const Vector2 pa = map.cellCenter(a), pb = map.cellCenter(b);
    const float dx = pb.x - pa.x, dy = pb.y - pa.y;
    int x = a.x, y = a.y;
    const int stepX = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
    const int stepY = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
    const float inf = std::numeric_limits<float>::infinity();
    const float tDeltaX = stepX != 0 ? size / std::fabs(dx) : inf;
    const float tDeltaY = stepY != 0 ? size / std::fabs(dy) : inf;
    float tMaxX = stepX != 0 ? 0.5f * tDeltaX : inf;
    float tMaxY = stepY != 0 ? 0.5f * tDeltaY : inf;
    int previous = map.index(a);
    while (x != b.x || y != b.y) {
        if (std::fabs(tMaxX - tMaxY) < 1e-6f) {
            if (!ok(GridCell{x + stepX, y}, previous) || !ok(GridCell{x, y + stepY}, previous))
                return false;
            x += stepX;
            y += stepY;
            tMaxX += tDeltaX;
            tMaxY += tDeltaY;
        } else if (tMaxX < tMaxY) {
            x += stepX;
            tMaxX += tDeltaX;
        } else {
            y += stepY;
            tMaxY += tDeltaY;
        }
        if (!ok(GridCell{x, y}, previous))
            return false;
        previous = map.index(GridCell{x, y});
        if (tMaxX > 1.0f + 1e-6f && tMaxY > 1.0f + 1e-6f && (x != b.x || y != b.y))
            return false;
    }
    return true;
}

void GridSolver::smooth(const NavGridMap& map, GridPath& path, const GridQuery& query, const float* areaCosts) const {
    path.points.clear();
    if (path.cells.empty())
        return;
    if (query.movement == GridMovement::CellCentres || path.cells.size() <= 2) {
        // cell by cell: what strategy and hex games step through
        for (const GridCell& cell : path.cells)
            path.points.push_back(map.cellCenter(cell));
        return;
    }
    // Only the cells where the path turns are tested: a straight run between two of them is
    // its own line of sight, so a long corridor costs one test, not one per cell
    static thread_local std::vector<size_t> turns;
    turns.clear();
    turns.push_back(0);
    const size_t last = path.cells.size() - 1;
    for (size_t k = 1; k < last; k++) {
        const GridCell& a = path.cells[k - 1];
        const GridCell& b = path.cells[k];
        const GridCell& c = path.cells[k + 1];
        if (b.x - a.x != c.x - b.x || b.y - a.y != c.y - b.y)
            turns.push_back(k);
    }
    turns.push_back(last);

    auto costOf = [&](size_t k) { return areaCosts[map.getArea(path.cells[k])]; };
    path.points.push_back(map.cellCenter(path.cells.front()));
    size_t anchor = 0;
    // the costliest cell of the path from the anchor up to `scanned`: a shortcut may not cross a
    // costlier one
    float maxCost = costOf(0);
    size_t scanned = 0;
    for (size_t t = 1; t + 1 < turns.size(); t++) {
        float candidate = maxCost;
        for (size_t k = scanned + 1; k <= turns[t + 1]; k++)
            candidate = std::max(candidate, costOf(k));
        if (lineOfSight(map, path.cells[anchor], path.cells[turns[t + 1]], query, candidate, areaCosts)) {
            maxCost = candidate;
            scanned = turns[t + 1];
            continue;
        }
        anchor = turns[t];
        path.points.push_back(map.cellCenter(path.cells[anchor]));
        maxCost = costOf(anchor);
        scanned = anchor;
    }
    path.points.push_back(map.cellCenter(path.cells.back()));
}

// ---------------------------------------------------------------- pool and batch

std::unique_ptr<GridSolver> GridSolverPool::lease() {
    std::lock_guard<std::mutex> lock(mutex);
    if (free.empty())
        return std::make_unique<GridSolver>();
    std::unique_ptr<GridSolver> solver = std::move(free.back());
    free.pop_back();
    return solver;
}

void GridSolverPool::giveBack(std::unique_ptr<GridSolver> solver) {
    if (!solver)
        return;
    std::lock_guard<std::mutex> lock(mutex);
    free.push_back(std::move(solver));
}

size_t GridSolverPool::size() const {
    std::lock_guard<std::mutex> lock(mutex);
    return free.size();
}

size_t GridSolverPool::scratchBytes() const {
    std::lock_guard<std::mutex> lock(mutex);
    size_t bytes = 0;
    for (const auto& solver : free)
        bytes += solver->scratchBytes();
    return bytes;
}

void nav::solveGridBatch(const NavGridMap& map, const std::vector<GridRequest>& requests, const float* areaCosts,
                         GridSolverPool& pool, std::vector<GridPath>& results, size_t maxThreads) {
    results.assign(requests.size(), GridPath());
    if (requests.empty())
        return;
    // the reachability index is built once, before the threads read it
    map.ensureComponents();
    const size_t threads = jobs::system().getWorkerCount() + 1u;
    const size_t chunks = std::min(requests.size(), maxThreads > 0 ? std::min(maxThreads, threads) : threads);
    std::vector<std::unique_ptr<GridSolver>> solvers(chunks);
    for (size_t i = 0; i < chunks; i++)
        solvers[i] = pool.lease();
    // each worker takes the next request until none is left (paths differ a lot in cost, so a
    // static split would wait on its slowest share); a result depends only on its request
    std::atomic<size_t> next{0};
    jobs::ParallelForOptions options;
    options.maxHelpers = chunks > 0 ? chunks - 1 : 0;
    jobs::parallelFor(chunks, [&](size_t chunk) {
        for (size_t i = next.fetch_add(1, std::memory_order_relaxed); i < requests.size(); i = next.fetch_add(1, std::memory_order_relaxed))
            results[i] = solvers[chunk]->solve(map, requests[i].start, requests[i].goal, requests[i].query, areaCosts);
    }, options);
    for (auto& solver : solvers)
        pool.giveBack(std::move(solver));
}
