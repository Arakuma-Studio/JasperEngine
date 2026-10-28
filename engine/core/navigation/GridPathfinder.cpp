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
#include <cmath>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::nav;

namespace {

    constexpr uint8_t STATE_OPEN = 1;
    constexpr uint8_t STATE_CLOSED = 2;

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
    for (auto it = held.begin(); it != held.end(); ++it) {
        if (it->first != agent)
            continue;
        for (int cell : it->second) {
            if (cell >= 0 && (size_t)cell < holders.size() && holders[(size_t)cell] == agent) {
                holders[(size_t)cell] = 0;
                until[(size_t)cell] = 0;
            }
        }
        held.erase(it);
        return;
    }
}

void GridReservations::reserve(uint32_t agent, const std::vector<int>& cells, uint32_t untilTick) {
    if (agent == 0)
        return;
    release(agent);
    std::vector<int> taken;
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
    held.emplace_back(agent, std::move(taken));
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
           state.capacity() * sizeof(uint8_t) + heap.capacity() * sizeof(HeapNode);
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

    prepare(map.getCellCount());
    auto visited = [&](int cell) { return generation[(size_t)cell] == current; };

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
    const bool diagonalRule = map.getLayout() == GridLayout::Square && map.getNeighbourMode() == 8 && !query.cornerCutting;

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
        const int count = map.neighbours(cell, next, steps);
        for (int n = 0; n < count; n++) {
            const int to = map.index(next[n]);
            if (!passable(map, to, query))
                continue;
            if (diagonalRule && steps[n] > 1.0f) {
                // no corner cutting: both orthogonal cells of the diagonal must be open
                const int sideA = map.index(GridCell{next[n].x, cell.y});
                const int sideB = map.index(GridCell{cell.x, next[n].y});
                if (!passable(map, sideA, query) || !passable(map, sideB, query))
                    continue;
            }
            if (!map.stepAllowed(node.cell, to, steps[n]))
                continue;
            if (visited(to) && state[(size_t)to] == STATE_CLOSED)
                continue;
            float cost = steps[n] * areaCosts[map.getAreaAt(to)];
            if (query.reservations && query.reservations->isReservedByOther(to, query.agent, query.tick))
                cost *= query.reservationPenalty;
            const float candidate = g[(size_t)node.cell] + cost;
            if (visited(to) && candidate >= g[(size_t)to])
                continue;
            generation[(size_t)to] = current;
            g[(size_t)to] = candidate;
            parent[(size_t)to] = node.cell;
            state[(size_t)to] = STATE_OPEN;
            const float h = map.heuristic(next[n], goal);
            heap.push_back(HeapNode{candidate + h, h, candidate, to});
            std::push_heap(heap.begin(), heap.end(), HeapGreater());
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
    path.points.push_back(map.cellCenter(path.cells.front()));
    size_t anchor = 0;
    float maxCost = areaCosts[map.getArea(path.cells[0])];
    for (size_t k = 1; k < path.cells.size(); k++) {
        maxCost = std::max(maxCost, areaCosts[map.getArea(path.cells[k])]);
        if (k + 1 < path.cells.size()) {
            const float nextCost = std::max(maxCost, areaCosts[map.getArea(path.cells[k + 1])]);
            if (lineOfSight(map, path.cells[anchor], path.cells[k + 1], query, nextCost, areaCosts))
                continue;
            path.points.push_back(map.cellCenter(path.cells[k]));
            anchor = k;
            maxCost = areaCosts[map.getArea(path.cells[k])];
        }
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
    const size_t perChunk = (requests.size() + chunks - 1) / chunks;
    jobs::ParallelForOptions options;
    options.maxHelpers = chunks > 0 ? chunks - 1 : 0;
    jobs::parallelFor(chunks, [&](size_t chunk) {
        const size_t begin = chunk * perChunk;
        const size_t end = std::min(requests.size(), begin + perChunk);
        for (size_t i = begin; i < end; i++)
            results[i] = solvers[chunk]->solve(map, requests[i].start, requests[i].goal, requests[i].query, areaCosts);
    }, options);
    for (auto& solver : solvers)
        pool.giveBack(std::move(solver));
}
