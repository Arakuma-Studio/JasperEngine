// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The generation-stamped A* scratch (no per-solve clear), the octile and hex heuristics,
// reservations as a cost penalty (x3) and the leased solver pool of a parallel batch follow
// OpenEngine's AStarSolver, AStarSolverPool and PathSmoother (Engine/Modules/Pathfinding/Source/),
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef GRID_PATHFINDER_H
#define GRID_PATHFINDER_H

// A* on a NavGridMap (spec R16-R19): area costs, corner cutting off by default, line-of-sight
// smoothing (Free movement) or cell centres (CellCentres), Complete / Partial (the reachable cell
// closest to the goal by the heuristic) / Invalid, an O(1) reject of unreachable goals through the
// map's reachability index, time-windowed cell reservations keyed on fixed ticks, and a batch
// solve over leased solvers whose results never depend on the thread count.

#include "Export.h"
#include "navigation/NavGridMap.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace jasperengine::nav {

    enum class GridMovement {
        // smoothed by line of sight: straight segments between the path's turning cells
        Free,
        // from cell centre to cell centre (strategy and hex games)
        CellCentres
    };

    // Who holds which cells until which fixed tick (spec R18). Other agents pay the penalty on
    // a reserved cell; a reservation expires once its tick has passed.
    class JASPERENGINE_API GridReservations {
    public:
        void reset(int cellCount);
        // Reserves the cells for `agent` (non-zero) until untilTick, replacing what it held
        void reserve(uint32_t agent, const std::vector<int>& cells, uint32_t untilTick);
        void release(uint32_t agent);
        // whether another agent holds the cell at `tick`
        bool isReservedByOther(int cell, uint32_t agent, uint32_t tick) const;
        uint32_t holder(int cell, uint32_t tick) const;
        int getCellCount() const { return (int)holders.size(); }

    private:
        std::vector<uint32_t> holders;
        std::vector<uint32_t> until;
        // keyed by agent: moving agents replace their claims every fixed tick
        std::unordered_map<uint32_t, std::vector<int>> held;
    };

    struct GridQuery {
        uint16_t areaMask = ALL_AREAS;
        bool allowPartial = false;
        GridMovement movement = GridMovement::Free;
        // diagonal steps past a blocked corner
        bool cornerCutting = false;
        // the asking agent (0: none) and the reservations it pays for (null: none)
        uint32_t agent = 0;
        const GridReservations* reservations = nullptr;
        uint32_t tick = 0;
        float reservationPenalty = 3.0f;
    };

    struct GridPath {
        PathStatus status = PathStatus::Invalid;
        // every cell stepped through, start and end included
        std::vector<GridCell> cells;
        // the waypoints to move along (grid-local): smoothed corners (Free) or every cell centre
        std::vector<Vector2> points;
        // the summed step costs (cells x area cost x penalties)
        float cost = 0.0f;
        // nodes the search expanded (0 for an O(1) reject)
        uint32_t expanded = 0;
        // S75: None when Complete; an end outside the grid or on a Not Walkable cell is
        // *OffMesh, on a cell the mask leaves out *Filtered (also when allowPartial gives a
        // Partial path to it), else Disconnected
        PathFailure failure = PathFailure::NoSurface;
    };

    // One solver's scratch: 11 bytes per cell (g, parent, generation, state), reused across
    // solves without clearing. One thread at a time.
    class JASPERENGINE_API GridSolver {
    public:
        GridSolver();
        ~GridSolver();

        // areaCosts[16]: the cost of each area (>= 1)
        GridPath solve(const NavGridMap& map, GridCell start, GridCell goal, const GridQuery& query, const float* areaCosts);

        size_t scratchBytes() const;

    private:
        // solve without the failure classification (its partial searches recurse here)
        GridPath search(const NavGridMap& map, GridCell start, GridCell goal, const GridQuery& query, const float* areaCosts);
        void prepare(int cells);
        bool jpsApplies(const NavGridMap& map, const GridQuery& query, const float* areaCosts) const;
        GridPath solveJps(const NavGridMap& map, GridCell start, GridCell goal, const GridQuery& query, const float* areaCosts);
        // the passable cell of start's component closest to goal by the heuristic (-1: none)
        int nearestReachable(const NavGridMap& map, GridCell start, GridCell goal, const GridQuery& query);
        bool passable(const NavGridMap& map, int cell, const GridQuery& query) const;
        bool lineOfSight(const NavGridMap& map, GridCell a, GridCell b, const GridQuery& query, float maxCost,
                         const float* areaCosts) const;
        void smooth(const NavGridMap& map, GridPath& path, const GridQuery& query, const float* areaCosts) const;

        struct HeapNode {
            float f;
            float h;
            // the cell's g when pushed (f - h is not exactly g in floats)
            float g;
            int cell;
        };
        std::vector<float> g;
        std::vector<int32_t> parent;
        std::vector<uint16_t> generation;
        std::vector<uint8_t> state;
        std::vector<HeapNode> heap;
        uint16_t current = 0;
        // jump point search: the passable cells with a closed border ((width + 2) x (height + 2)),
        // kept while the map, its revision and the area mask stay the same
        std::vector<uint8_t> openCells;
        // per straight direction (+x, -x, +y, -y) and cell: the distance to the next jump point
        // (-1: a wall first) and to the last open cell of the run (JPS+ style, so a straight
        // scan is one lookup)
        std::vector<int16_t> jumpDistance[4];
        std::vector<int16_t> runDistance[4];
        const NavGridMap* openMap = nullptr;
        uint64_t openRevision = 0;
        uint32_t openMask = 0;
    };

    // Solvers leased for a batch; a solver goes back to the pool when the batch is done
    class JASPERENGINE_API GridSolverPool {
    public:
        std::unique_ptr<GridSolver> lease();
        void giveBack(std::unique_ptr<GridSolver> solver);
        size_t size() const;
        size_t scratchBytes() const;

    private:
        mutable std::mutex mutex;
        std::vector<std::unique_ptr<GridSolver>> free;
    };

    struct GridRequest {
        GridCell start;
        GridCell goal;
        GridQuery query;
    };

    // Solves every request (spec R19): jobs::parallelFor over leased solvers, results in request
    // order; a request's result depends only on the map, the request and the reservations, so
    // any thread count gives the same paths.
    JASPERENGINE_API void solveGridBatch(const NavGridMap& map, const std::vector<GridRequest>& requests, const float* areaCosts,
                                         GridSolverPool& pool, std::vector<GridPath>& results, size_t maxThreads = 0);

}

#endif // GRID_PATHFINDER_H
