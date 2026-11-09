// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Navigation grids (docs/specs/S19-navigation.md R15-R20, R29; AC16-AC18): A* against a reference
// Dijkstra on square and hex layouts, exact hex point lookup, the reachability index, reservations
// spreading agents, deterministic batches, runtime edits and the .navgrid format.

#include "NavTestUtil.h"

#include "component/NavGridComponent.h"
#include "component/TerrainComponent.h"
#include "navigation/GridPathfinder.h"
#include "navigation/NavGridMap.h"
#include "object/navigation/NavGrid.h"
#include "object/navigation/NavMeshAgent.h"
#include "terrain/TerrainHoles.h"
#include "texture/TextureData.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>
#include <random>
#include <set>

using namespace jasperengine;
using namespace jasperengine::navtest;
using nav::GridCell;
using nav::NavGridMap;

namespace {

    class NavGridTest : public NavSceneTest {};

    float costs[nav::AREA_COUNT];

    void defaultCosts() {
        for (int i = 0; i < nav::AREA_COUNT; i++)
            costs[i] = 1.0f;
        costs[3] = 2.5f;
        costs[4] = 5.0f;
    }

    // a w x h map with ~blocked share Not Walkable and some costly areas, seeded
    NavGridMap randomMap(int width, int height, nav::GridLayout layout, int neighbours, uint32_t seed, float blocked = 0.2f) {
        NavGridMap map;
        map.reset(width, height);
        map.setLayout(layout, neighbours);
        std::mt19937 random(seed);
        std::uniform_real_distribution<float> roll(0.0f, 1.0f);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                const float r = roll(random);
                uint8_t area = nav::AREA_WALKABLE;
                if (r < blocked) area = nav::AREA_NOT_WALKABLE;
                else if (r < blocked + 0.1f) area = 3;
                else if (r < blocked + 0.15f) area = 4;
                map.setArea(GridCell{x, y}, area);
            }
        }
        return map;
    }

    // the reference: Dijkstra over the same steps and costs (no heuristic, no corner cutting)
    float dijkstra(const NavGridMap& map, GridCell start, GridCell goal) {
        std::vector<float> distance((size_t)map.getCellCount(), std::numeric_limits<float>::infinity());
        using Item = std::pair<float, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
        distance[(size_t)map.index(start)] = 0.0f;
        open.push({0.0f, map.index(start)});
        GridCell next[8];
        float steps[8];
        while (!open.empty()) {
            const auto [d, i] = open.top();
            open.pop();
            if (d > distance[(size_t)i])
                continue;
            if (i == map.index(goal))
                return d;
            const GridCell cell = map.cellOf(i);
            const int count = map.neighbours(cell, next, steps);
            for (int n = 0; n < count; n++) {
                const int j = map.index(next[n]);
                if (!map.isWalkable(j))
                    continue;
                if (map.getLayout() == nav::GridLayout::Square && steps[n] > 1.0f &&
                    (!map.isWalkable(map.index(GridCell{next[n].x, cell.y})) || !map.isWalkable(map.index(GridCell{cell.x, next[n].y}))))
                    continue;
                const float candidate = d + steps[n] * costs[map.getAreaAt(j)];
                if (candidate < distance[(size_t)j]) {
                    distance[(size_t)j] = candidate;
                    open.push({candidate, j});
                }
            }
        }
        return std::numeric_limits<float>::infinity();
    }

    void comparesWithDijkstra(nav::GridLayout layout, int neighbours, uint32_t seed) {
        defaultCosts();
        NavGridMap map = randomMap(48, 40, layout, neighbours, seed);
        nav::GridSolver solver;
        std::mt19937 random(seed + 1);
        std::uniform_int_distribution<int> px(0, map.getWidth() - 1), py(0, map.getHeight() - 1);
        int compared = 0;
        for (int trial = 0; trial < 400 && compared < 120; trial++) {
            const GridCell a{px(random), py(random)}, b{px(random), py(random)};
            if (!map.isWalkable(map.index(a)) || !map.isWalkable(map.index(b)))
                continue;
            const float reference = dijkstra(map, a, b);
            const nav::GridPath path = solver.solve(map, a, b, nav::GridQuery(), costs);
            if (std::isinf(reference)) {
                EXPECT_EQ(path.status, nav::PathStatus::Invalid);
                continue;
            }
            ASSERT_EQ(path.status, nav::PathStatus::Complete);
            EXPECT_NEAR(path.cost, reference, 1e-4f * std::max(1.0f, reference));
            ASSERT_FALSE(path.cells.empty());
            EXPECT_EQ(path.cells.front(), a);
            EXPECT_EQ(path.cells.back(), b);
            compared++;
        }
        EXPECT_GE(compared, 60);
    }

}

TEST_F(NavGridTest, SquareFourMatchesDijkstra){
    comparesWithDijkstra(nav::GridLayout::Square, 4, 11);
}

TEST_F(NavGridTest, SquareEightMatchesDijkstra){
    comparesWithDijkstra(nav::GridLayout::Square, 8, 12);
}

TEST_F(NavGridTest, HexMatchesDijkstra){
    comparesWithDijkstra(nav::GridLayout::Hex, 6, 13);
}

TEST_F(NavGridTest, HexWorldToCellExact){
    NavGridMap map;
    map.reset(40, 30);
    map.setLayout(nav::GridLayout::Hex, 6);
    map.setCellSize(1.7f);
    std::mt19937 random(5);
    const Vector2 size = map.getSize();
    std::uniform_real_distribution<float> rx(0.0f, size.x), ry(0.0f, size.y);
    const float rowStep = 1.5f * map.getCellSize() / std::sqrt(3.0f);
    // the brute force: the nearest centre among every cell within three rows and columns of the
    // point (any farther centre is farther than all of those)
    auto nearest = [&](const Vector2& p) {
        GridCell best{-1, -1};
        float bestDistance = std::numeric_limits<float>::max();
        const int row = (int)std::floor(p.y / rowStep), column = (int)std::floor(p.x / map.getCellSize());
        for (int y = row - 3; y <= row + 3; y++) {
            for (int x = column - 3; x <= column + 3; x++) {
                const float d = (map.cellCenter(GridCell{x, y}) - p).length();
                if (d < bestDistance) {
                    bestDistance = d;
                    best = GridCell{x, y};
                }
            }
        }
        return best;
    };
    int mismatches = 0;
    for (int i = 0; i < 100000; i++) {
        const Vector2 p(rx(random), ry(random));
        // the nearest centre is the cell holding the point (a hexagon is its Voronoi cell)
        if (map.cellAt(p) != nearest(p))
            mismatches++;
    }
    EXPECT_EQ(mismatches, 0);
    // exactly at the edges: a hair to each side of the midpoint of two neighbours
    for (int y = 1; y < map.getHeight() - 1; y++) {
        for (int x = 1; x < map.getWidth() - 1; x++) {
            const GridCell cell{x, y};
            GridCell around[8];
            float steps[8];
            const int count = map.neighbours(cell, around, steps);
            for (int n = 0; n < count; n++) {
                const Vector2 a = map.cellCenter(cell), b = map.cellCenter(around[n]);
                const Vector2 mid = (a + b) * 0.5f;
                const Vector2 towardsA = (a - b).normalized() * 1e-3f;
                EXPECT_EQ(map.cellAt(mid + towardsA), cell);
                EXPECT_EQ(map.cellAt(mid - towardsA), around[n]);
            }
        }
    }
}

TEST_F(NavGridTest, UnreachableRejectedWithoutExpansion){
    defaultCosts();
    NavGridMap map;
    map.reset(64, 64);
    map.setLayout(nav::GridLayout::Square, 8);
    // a full wall at x = 32
    map.fillRect(GridCell{32, 0}, GridCell{32, 63}, nav::AREA_NOT_WALKABLE);
    nav::GridSolver solver;
    const nav::GridPath rejected = solver.solve(map, GridCell{2, 2}, GridCell{60, 60}, nav::GridQuery(), costs);
    EXPECT_EQ(rejected.status, nav::PathStatus::Invalid);
    EXPECT_EQ(rejected.expanded, 0u);
    // a blocked goal too
    const nav::GridPath blockedGoal = solver.solve(map, GridCell{2, 2}, GridCell{32, 10}, nav::GridQuery(), costs);
    EXPECT_EQ(blockedGoal.status, nav::PathStatus::Invalid);
    EXPECT_EQ(blockedGoal.expanded, 0u);
    // with allowPartial: the reachable cell nearest the goal
    nav::GridQuery partial;
    partial.allowPartial = true;
    const nav::GridPath closest = solver.solve(map, GridCell{2, 2}, GridCell{60, 60}, partial, costs);
    EXPECT_EQ(closest.status, nav::PathStatus::Partial);
    ASSERT_FALSE(closest.cells.empty());
    EXPECT_EQ(closest.cells.back().x, 31);
    EXPECT_EQ(closest.cells.back().y, 60);
    // the index follows edits: a door in the wall
    map.setArea(GridCell{32, 40}, nav::AREA_WALKABLE);
    EXPECT_TRUE(map.componentsDirty());
    EXPECT_TRUE(map.isReachable(GridCell{2, 2}, GridCell{60, 60}));
    EXPECT_EQ(solver.solve(map, GridCell{2, 2}, GridCell{60, 60}, nav::GridQuery(), costs).status, nav::PathStatus::Complete);
}

TEST_F(NavGridTest, CornerCuttingOffByDefault){
    defaultCosts();
    NavGridMap map;
    map.reset(3, 3);
    map.setLayout(nav::GridLayout::Square, 8);
    map.setArea(GridCell{1, 0}, nav::AREA_NOT_WALKABLE);
    nav::GridSolver solver;
    // (0,0) -> (1,1) diagonally passes the blocked (1,0): not allowed, two orthogonal steps
    const nav::GridPath path = solver.solve(map, GridCell{0, 0}, GridCell{1, 1}, nav::GridQuery(), costs);
    ASSERT_EQ(path.status, nav::PathStatus::Complete);
    EXPECT_NEAR(path.cost, 2.0f, 1e-5f);
    nav::GridQuery cutting;
    cutting.cornerCutting = true;
    EXPECT_NEAR(solver.solve(map, GridCell{0, 0}, GridCell{1, 1}, cutting, costs).cost, 1.41421356f, 1e-5f);
}

TEST_F(NavGridTest, SmoothingAndCellCentres){
    defaultCosts();
    NavGridMap map;
    map.reset(20, 20);
    map.setLayout(nav::GridLayout::Square, 8);
    nav::GridSolver solver;
    nav::GridQuery free;
    free.movement = nav::GridMovement::Free;
    const nav::GridPath smoothed = solver.solve(map, GridCell{0, 0}, GridCell{19, 7}, free, costs);
    ASSERT_EQ(smoothed.status, nav::PathStatus::Complete);
    // an open field: line of sight from start to goal, two points
    EXPECT_EQ(smoothed.points.size(), 2u);
    nav::GridQuery centres;
    centres.movement = nav::GridMovement::CellCentres;
    const nav::GridPath stepped = solver.solve(map, GridCell{0, 0}, GridCell{19, 7}, centres, costs);
    EXPECT_EQ(stepped.points.size(), stepped.cells.size());
}

TEST_F(NavGridTest, BatchDeterministic){
    defaultCosts();
    NavGridMap map = randomMap(128, 128, nav::GridLayout::Square, 8, 21, 0.15f);
    std::mt19937 random(22);
    std::uniform_int_distribution<int> p(0, 127);
    std::vector<nav::GridRequest> requests;
    while (requests.size() < 200) {
        nav::GridRequest request;
        request.start = GridCell{p(random), p(random)};
        request.goal = GridCell{p(random), p(random)};
        request.query.allowPartial = true;
        if (map.isWalkable(map.index(request.start)))
            requests.push_back(request);
    }
    nav::GridSolverPool pool;
    std::vector<nav::GridPath> one, eight;
    nav::solveGridBatch(map, requests, costs, pool, one, 1);
    nav::solveGridBatch(map, requests, costs, pool, eight, 8);
    ASSERT_EQ(one.size(), eight.size());
    for (size_t i = 0; i < one.size(); i++) {
        EXPECT_EQ(one[i].status, eight[i].status) << i;
        EXPECT_EQ(one[i].cost, eight[i].cost) << i;
        ASSERT_EQ(one[i].cells.size(), eight[i].cells.size()) << i;
        for (size_t c = 0; c < one[i].cells.size(); c++)
            EXPECT_EQ(one[i].cells[c], eight[i].cells[c]) << i;
    }
}

TEST_F(NavGridTest, MemoryPerCell){
    // R29: 1 byte a cell (+4 with heights), 11 bytes of solver scratch a cell
    NavGridMap map;
    map.reset(256, 256);
    const size_t cells = 256u * 256u;
    EXPECT_LE(map.memoryBytes(), cells + cells / 8 + 64);
    map.setHasHeights(true);
    EXPECT_LE(map.memoryBytes(), cells * 5 + cells / 8 + 64);
    defaultCosts();
    nav::GridSolver solver;
    solver.solve(map, GridCell{0, 0}, GridCell{255, 255}, nav::GridQuery(), costs);
    // the scratch arrays, without the open list (which grows with the frontier, not the map)
    const size_t perCell = sizeof(float) + sizeof(int32_t) + sizeof(uint16_t) + sizeof(uint8_t);
    EXPECT_EQ(perCell, 11u);
    EXPECT_GE(solver.scratchBytes(), cells * perCell);
}

TEST_F(NavGridTest, FileRoundTripAndNewerVersionRefused){
    NavGridMap map = randomMap(33, 17, nav::GridLayout::Hex, 6, 31);
    map.setOverride(GridCell{4, 5}, true);
    map.setHasHeights(true);
    map.setHeight(GridCell{2, 3}, 1.25f);
    const std::vector<uint8_t> bytes = map.write();
    NavGridMap read;
    std::string error;
    ASSERT_TRUE(read.read(bytes.data(), bytes.size(), error)) << error;
    EXPECT_EQ(read.getWidth(), 33);
    EXPECT_EQ(read.getHeight(), 17);
    EXPECT_EQ(read.getLayout(), nav::GridLayout::Hex);
    EXPECT_TRUE(read.getOverride(GridCell{4, 5}));
    EXPECT_FLOAT_EQ(read.getHeight(GridCell{2, 3}), 1.25f);
    for (int i = 0; i < map.getCellCount(); i++)
        EXPECT_EQ(read.getAreaAt(i), map.getAreaAt(i));
    EXPECT_EQ(read.write(), bytes);

    std::vector<uint8_t> newer = bytes;
    newer[4] = (uint8_t)(NavGridMap::FILE_VERSION + 1);
    EXPECT_FALSE(read.read(newer.data(), newer.size(), error));
    EXPECT_NE(error.find("newer"), std::string::npos) << error;
    std::vector<uint8_t> damaged = bytes;
    damaged[40] ^= 0x5A;
    EXPECT_FALSE(read.read(damaged.data(), damaged.size(), error));
    EXPECT_FALSE(read.read(bytes.data(), bytes.size() - 3, error));
}

// 8 agents through a 3-wide corridor: they use at least two lanes and almost never share a cell
TEST_F(NavGridTest, ReservationsSpread){
    const Entity gridEntity = transformEntity(*scene, Vector3(), NULL_ENTITY, "Grid");
    NavGridComponent grid;
    grid.width = 30;
    grid.height = 9;
    grid.cellSize = 1.0f;
    grid.neighbours = 8;
    scene->addComponent<NavGridComponent>(gridEntity, grid);
    NavGrid handle(scene.get(), gridEntity);
    // walls leave rows 3-5 open between x = 10 and x = 19
    handle.fillRect(GridCell{10, 0}, GridCell{19, 2}, nav::AREA_NOT_WALKABLE);
    handle.fillRect(GridCell{10, 6}, GridCell{19, 8}, nav::AREA_NOT_WALKABLE);
    std::vector<Entity> agents;
    for (int i = 0; i < 8; i++) {
        const GridCell cell{1 + (i % 2) * 2, 1 + i};
        agents.push_back(agent(*scene, handle.cellToWorld(cell), 2.0f));
        scene->getComponent<NavMeshAgentComponent>(agents.back()).surface = gridEntity;
    }
    step();
    for (int i = 0; i < 8; i++)
        NavMeshAgent(scene.get(), agents[(size_t)i]).setDestination(handle.cellToWorld(GridCell{28, 1 + i}));
    std::set<int> lanes;
    int ticks = 0, shared = 0, arrived = 0;
    for (; ticks < 60 * 40 && arrived < 8; ticks++) {
        step();
        std::set<std::pair<int, int>> occupied;
        bool clash = false;
        arrived = 0;
        for (Entity entity : agents) {
            const GridCell cell = handle.worldToCell(position(entity));
            if (cell.x >= 10 && cell.x <= 19)
                lanes.insert(cell.y);
            if (!occupied.insert({cell.x, cell.y}).second)
                clash = true;
            if (cell.x == 28)
                arrived++;
        }
        shared += clash ? 1 : 0;
    }
    EXPECT_EQ(arrived, 8);
    EXPECT_GE(lanes.size(), 2u);
    EXPECT_LT((double)shared / (double)ticks, 0.01) << shared << " of " << ticks;
}

// an edit on an agent's path makes it plan again (R20)
TEST_F(NavGridTest, RuntimeEditsRepath){
    const Entity gridEntity = transformEntity(*scene, Vector3(), NULL_ENTITY, "Grid");
    NavGridComponent grid;
    grid.width = 20;
    grid.height = 10;
    scene->addComponent<NavGridComponent>(gridEntity, grid);
    NavGrid handle(scene.get(), gridEntity);
    const Entity walker = agent(*scene, handle.cellToWorld(GridCell{1, 5}), 2.0f);
    scene->getComponent<NavMeshAgentComponent>(walker).surface = gridEntity;
    step();
    NavMeshAgent mover(scene.get(), walker);
    ASSERT_TRUE(mover.setDestination(handle.cellToWorld(GridCell{18, 5})));
    steps(3);
    ASSERT_FALSE(mover.isPathPending());
    // a wall across the straight line, with a gap at the top
    handle.fillRect(GridCell{10, 0}, GridCell{10, 8}, nav::AREA_NOT_WALKABLE);
    int reached = 0;
    mover.getOnDestinationReached() = std::function<void(NavMeshAgent)>([&](NavMeshAgent) { reached++; });
    bool crossedWall = false;
    for (int tick = 0; tick < 60 * 30 && reached == 0; tick++) {
        step();
        const GridCell cell = handle.worldToCell(position(walker));
        if (cell.x == 10)
            crossedWall = crossedWall || cell.y != 9;
    }
    EXPECT_EQ(reached, 1);
    EXPECT_FALSE(crossedWall);
    EXPECT_EQ(handle.worldToCell(position(walker)), (GridCell{18, 5}));
}

// Uniform costs take jump point search: the same cost as the plain A* (forced here by an empty
// reservation table), a contiguous path of open cells, never cutting a corner
TEST_F(NavGridTest, JumpPointSearchMatchesAStar){
    float uniform[nav::AREA_COUNT];
    for (float& cost : uniform)
        cost = 1.0f;
    for (uint32_t seed : {3u, 17u, 41u}) {
        NavGridMap map = randomMap(64, 48, nav::GridLayout::Square, 8, seed, 0.25f);
        nav::GridReservations none;
        none.reset(map.getCellCount());
        nav::GridQuery plain;
        plain.reservations = &none;
        nav::GridSolver solver;
        std::mt19937 random(seed + 7);
        std::uniform_int_distribution<int> px(0, map.getWidth() - 1), py(0, map.getHeight() - 1);
        int compared = 0;
        for (int trial = 0; trial < 400 && compared < 60; trial++) {
            const GridCell a{px(random), py(random)}, b{px(random), py(random)};
            if (!map.isWalkable(map.index(a)) || !map.isWalkable(map.index(b)) || !map.isReachable(a, b))
                continue;
            const nav::GridPath reference = solver.solve(map, a, b, plain, uniform);
            const nav::GridPath jps = solver.solve(map, a, b, nav::GridQuery(), uniform);
            ASSERT_EQ(reference.status, nav::PathStatus::Complete);
            ASSERT_EQ(jps.status, nav::PathStatus::Complete);
            EXPECT_NEAR(jps.cost, reference.cost, 1e-3f * std::max(1.0f, reference.cost));
            ASSERT_FALSE(jps.cells.empty());
            EXPECT_EQ(jps.cells.front(), a);
            EXPECT_EQ(jps.cells.back(), b);
            for (size_t i = 1; i < jps.cells.size(); i++) {
                const GridCell p = jps.cells[i - 1], q = jps.cells[i];
                ASSERT_LE(std::abs(q.x - p.x), 1);
                ASSERT_LE(std::abs(q.y - p.y), 1);
                EXPECT_TRUE(map.isWalkable(map.index(q)));
                if (q.x != p.x && q.y != p.y) {
                    EXPECT_TRUE(map.isWalkable(map.index(GridCell{q.x, p.y})));
                    EXPECT_TRUE(map.isWalkable(map.index(GridCell{p.x, q.y})));
                }
            }
            EXPECT_LE(jps.expanded, reference.expanded);
            compared++;
        }
        EXPECT_GT(compared, 20);
    }
}

// T3 R23: a cell over a hole is Not Walkable unless overridden
TEST_F(NavGridTest, GridHole) {
    std::vector<uint8_t> heights((size_t)17 * 17, 32);
    std::vector<uint8_t> holes((size_t)17 * 17, terraincore::SOLID_VALUE);
    for (int y = 0; y < 17; y++) {
        for (int x = 0; x < 8; x++)
            holes[(size_t)y * 17 + x] = terraincore::HOLE_VALUE;
    }
    const Entity terrainEntity = transformEntity(*scene, Vector3());
    TerrainComponent terrainComponent;
    terrainComponent.terrainSize = 16.0f;
    terrainComponent.maxHeight = 4.0f;
    terrainComponent.heightMap.setData("nav-grid-hole-height", TextureData(17, 17, (unsigned int)heights.size(), ColorFormat::RED, 1, heights.data()));
    terrainComponent.holeMap.setData("nav-grid-hole-holes", TextureData(17, 17, (unsigned int)holes.size(), ColorFormat::RED, 1, holes.data()));
    scene->addComponent<TerrainComponent>(terrainEntity, terrainComponent);

    const Entity gridEntity = transformEntity(*scene, Vector3(-8.0f, 0.0f, -8.0f), NULL_ENTITY, "Grid");
    NavGridComponent grid;
    grid.width = 16;
    grid.height = 16;
    grid.cellSize = 1.0f;
    grid.source = NavGridSource::TERRAIN;
    grid.sourceEntity = terrainEntity;
    scene->addComponent<NavGridComponent>(gridEntity, grid);
    resolveTransforms(*scene);

    std::string error;
    ASSERT_TRUE(nav->rebuildGrid(gridEntity, error)) << error;
    nav::NavGridMap* map = nav->getGridMap(gridEntity);
    ASSERT_NE(map, nullptr);
    EXPECT_EQ(map->getArea(GridCell{2, 12}), nav::AREA_NOT_WALKABLE);
    // a person's say on one cell of the hole stands through a rebuild
    map->setArea(GridCell{2, 12}, nav::AREA_WALKABLE);
    map->setOverride(GridCell{2, 12}, true);
    ASSERT_TRUE(nav->rebuildGrid(gridEntity, error)) << error;
    map = nav->getGridMap(gridEntity);
    EXPECT_EQ(map->getArea(GridCell{2, 2}), nav::AREA_NOT_WALKABLE);
    EXPECT_EQ(map->getArea(GridCell{12, 2}), nav::AREA_WALKABLE);
    EXPECT_EQ(map->getArea(GridCell{2, 12}), nav::AREA_WALKABLE) << "overridden";
}

// two grid agents walking: getGridReservedCells reports the cells their lookahead holds at the
// current fixed tick, and nothing once they stop (S74 AC25, R22)
TEST_F(NavGridTest, ReservedCellsReported){
    const Entity gridEntity = transformEntity(*scene, Vector3(), NULL_ENTITY, "Grid");
    NavGridComponent grid;
    grid.width = 20;
    grid.height = 6;
    grid.cellSize = 1.0f;
    scene->addComponent<NavGridComponent>(gridEntity, grid);
    NavGrid handle(scene.get(), gridEntity);
    std::vector<Entity> agents;
    for (int i = 0; i < 2; i++) {
        agents.push_back(agent(*scene, handle.cellToWorld(GridCell{1, 1 + 3 * i}), 2.0f));
        scene->getComponent<NavMeshAgentComponent>(agents.back()).surface = gridEntity;
    }
    std::vector<int> cells;
    EXPECT_FALSE(nav->getGridReservedCells(agents[0], cells));
    step();
    for (int i = 0; i < 2; i++)
        NavMeshAgent(scene.get(), agents[(size_t)i]).setDestination(handle.cellToWorld(GridCell{18, 1 + 3 * i}));
    steps(30);
    ASSERT_TRUE(nav->getGridReservedCells(gridEntity, cells));
    ASSERT_FALSE(cells.empty());
    EXPECT_TRUE(std::is_sorted(cells.begin(), cells.end()));
    // each agent's cell and some ahead of it, on its own row
    NavGridMap* map = nav->getGridMap(gridEntity);
    ASSERT_NE(map, nullptr);
    for (int i = 0; i < 2; i++) {
        const GridCell here = handle.worldToCell(position(agents[(size_t)i]));
        int ahead = 0;
        for (int cell : cells) {
            const GridCell c{cell % map->getWidth(), cell / map->getWidth()};
            if (c.y == here.y && c.x > here.x)
                ahead++;
        }
        EXPECT_GT(ahead, 0) << "agent " << i;
    }
    for (int cell : cells) {
        const int row = cell / map->getWidth();
        EXPECT_TRUE(row == 1 || row == 4) << "cell " << cell;
    }
    // arrived: nothing held
    steps(60 * 12);
    ASSERT_TRUE(nav->getGridReservedCells(gridEntity, cells));
    EXPECT_TRUE(cells.empty());
}
