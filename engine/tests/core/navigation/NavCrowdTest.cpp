// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Crowds (docs/specs/S19-navigation.md R9, R13, R14, R25, R40c; AC10, AC14, AC15, AC18): the
// avoidance priority patch, obstacles (avoided and carving), crowd growth, an empty scene's cost
// and deterministic replays.

#include "NavTestUtil.h"

#include "AllocationCounter.h"
#include "LogCapture.h"
#include "component/NavMeshObstacleComponent.h"
#include "object/navigation/NavMeshAgent.h"

#include "TestTempDir.h"
#include "navigation/NavMesh.h"

#if JASPERENGINE_NAVIGATION
#include "DetourAssert.h"
#include "DetourPathCorridor.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::navtest;

namespace {

    class NavCrowdTest : public NavSceneTest {};
    class NavObstacleTest : public NavSceneTest {};
    class NavSystemTest : public NavSceneTest {};
    class NavDeterminismTest : public NavSceneTest {};

}

// head-on in a corridor: the important agent (10) keeps a straighter line than the other (90)
TEST_F(NavCrowdTest, PriorityYields){
    const Entity surface = planeSurface(30.0f, 4.0f);
    bake(surface);
    const Entity important = agent(*scene, Vector3(-12.0f, 0.0f, 0.05f));
    const Entity yielding = agent(*scene, Vector3(12.0f, 0.0f, -0.05f));
    scene->getComponent<NavMeshAgentComponent>(important).avoidancePriority = 10;
    scene->getComponent<NavMeshAgentComponent>(yielding).avoidancePriority = 90;
    step();
    NavMeshAgent(scene.get(), important).setDestination(Vector3(12.0f, 0.0f, 0.05f));
    NavMeshAgent(scene.get(), yielding).setDestination(Vector3(-12.0f, 0.0f, -0.05f));
    float importantDeviation = 0.0f, yieldingDeviation = 0.0f;
    for (int tick = 0; tick < 60 * 12; tick++) {
        step();
        importantDeviation = std::max(importantDeviation, std::fabs(position(important).z - 0.05f));
        yieldingDeviation = std::max(yieldingDeviation, std::fabs(position(yielding).z + 0.05f));
        // never through each other
        EXPECT_GE(Vector3(position(important).x - position(yielding).x, 0.0f, position(important).z - position(yielding).z).length(),
                  2.0f * 0.5f - 0.1f) << "tick " << tick;
    }
    EXPECT_GT(yieldingDeviation, 0.3f);
    EXPECT_LT(importantDeviation, 0.25f * yieldingDeviation) << importantDeviation << " vs " << yieldingDeviation;
    // both got through
    EXPECT_GT(position(important).x, 10.0f);
    EXPECT_LT(position(yielding).x, -10.0f);
}

// more agents than the crowd holds: it grows at the next tick, with a warning (R14)
TEST_F(NavCrowdTest, GrowsWhenFull){
    nav::NavigationSettings settings = nav::NavigationSettings::defaults();
    settings.crowdMaxAgents = 4;
    nav::setSettings(settings);
    const Entity surface = planeSurface(40.0f, 40.0f);
    bake(surface);
    test::LogCapture log;
    std::vector<Entity> agents;
    for (int i = 0; i < 9; i++)
        agents.push_back(agent(*scene, Vector3(-15.0f + 3.0f * (float)i, 0.0f, -10.0f)));
    steps(4);
    for (Entity entity : agents)
        EXPECT_TRUE(NavMeshAgent(scene.get(), entity).isOnNavMesh());
    EXPECT_GE(nav->getSurfaceStatus(surface).crowdCapacity, 9);
    EXPECT_GE(log.count(D_LOG_WARN, "grows"), 1u);
}

// a carving box on a corridor reroutes the path within 2 ticks; removed, the path is back (AC10).
// The budget counts tile builds (spec S53 R14): every build, sanitizers included, lands in 2 ticks
TEST_F(NavObstacleTest, CarveBlocksAndRestores){
    const Entity surface = gapSurface();
    bake(surface);
    nav->setCarveTimeCeiling(0.0);
    ASSERT_TRUE(nav->getNavMesh(surface)->isCarving());
    const Vector3 from(-10.0f, 0.0f, 0.0f), to(10.0f, 0.0f, 0.0f);
    const nav::NavPath open = nav->calculatePath(from, to);
    ASSERT_EQ(open.status, nav::PathStatus::Complete);

    const Entity blocker = obstacleBox(*scene, Vector3(0.0f, 1.0f, 0.0f), Vector3(1.5f, 2.0f, 3.0f), true);
    steps(2);
    // blocked: the only way through is cut
    EXPECT_NE(nav->calculatePath(from, to).status, nav::PathStatus::Complete);

    scene->destroyEntity(blocker);
    steps(2);
    const nav::NavPath restored = nav->calculatePath(from, to);
    ASSERT_EQ(restored.status, nav::PathStatus::Complete);
    EXPECT_NEAR(restored.length, open.length, 1e-3f);
}

// a carving surface's paths match a static bake's (tile-cache layers vs Detour tiles, +-2 %)
TEST_F(NavObstacleTest, TileCacheMatchesStatic){
    plane(*scene, Vector3(), 30.0f, 30.0f);
    box(*scene, Vector3(0.0f, 1.0f, 0.0f), Vector3(4.0f, 2.0f, 12.0f));
    const Entity staticSurface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Static");
    addSurface(*scene, staticSurface, 0.2f, 32);
    bake(staticSurface);
    const nav::NavPath reference = nav->calculatePathOn(staticSurface, Vector3(-10.0f, 0.0f, 0.0f), Vector3(10.0f, 0.0f, 0.0f));
    ASSERT_EQ(reference.status, nav::PathStatus::Complete);
    scene->getComponent<NavMeshSurfaceComponent>(staticSurface).runtimeCarving = true;
    bake(staticSurface);
    const nav::NavPath carving = nav->calculatePathOn(staticSurface, Vector3(-10.0f, 0.0f, 0.0f), Vector3(10.0f, 0.0f, 0.0f));
    ASSERT_EQ(carving.status, nav::PathStatus::Complete);
    EXPECT_NEAR(carving.length, reference.length, 0.02f * reference.length);
}

// 20 agents pass a non-carving obstacle without overlapping it (AC10)
TEST_F(NavObstacleTest, AvoidedWithoutOverlap){
    const Entity surface = planeSurface(40.0f, 12.0f);
    bake(surface);
    const Vector3 obstacleCenter(0.0f, 0.5f, 0.0f);
    const Entity obstacle = transformEntity(*scene, obstacleCenter);
    NavMeshObstacleComponent component;
    component.shape = NavObstacleShape::CAPSULE;
    component.radius = 1.0f;
    component.height = 1.0f;
    scene->addComponent<NavMeshObstacleComponent>(obstacle, component);
    std::vector<Entity> agents;
    for (int i = 0; i < 20; i++) {
        const float z = -4.0f + 8.0f * (float)(i % 5) / 4.0f;
        const float x = -18.0f + 1.6f * (float)(i / 5);
        agents.push_back(agent(*scene, Vector3(x, 0.0f, z)));
    }
    step();
    for (Entity entity : agents) {
        const Vector3 start = position(entity);
        NavMeshAgent(scene.get(), entity).setDestination(Vector3(17.0f - (start.x + 18.0f), 0.0f, -start.z));
    }
    int overlaps = 0;
    for (int tick = 0; tick < 60 * 25; tick++) {
        step();
        for (Entity entity : agents) {
            const Vector3 p = position(entity);
            const float distance = Vector3(p.x - obstacleCenter.x, 0.0f, p.z - obstacleCenter.z).length();
            if (distance < component.radius + 0.5f - 1e-3f)
                overlaps++;
        }
    }
    EXPECT_EQ(overlaps, 0);
    int across = 0;
    for (Entity entity : agents)
        across += position(entity).x > 5.0f ? 1 : 0;
    EXPECT_EQ(across, 20);
}

TEST_F(NavObstacleTest, CarvingWithoutRuntimeCarvingFallsBack){
    const Entity surface = planeSurface();
    bake(surface);
    test::LogCapture log;
    obstacleBox(*scene, Vector3(0.0f, 0.5f, 0.0f), Vector3(2.0f, 1.0f, 2.0f), true);
    agent(*scene, Vector3(-5.0f, 0.0f, 0.0f));
    steps(5);
    EXPECT_EQ(log.count(D_LOG_WARN, "Runtime Carving"), 1u);
    // not cut: the mesh under it is still there
    nav::NavHit hit;
    EXPECT_TRUE(nav->samplePosition(Vector3(0.0f, 0.0f, 0.0f), 0.1f, hit));
}

// no navigation component: < 1 us per fixed tick, nothing allocated (AC15)
TEST_F(NavSystemTest, ZeroCostEmptyScene){
    // an ordinary scene: a mesh, no navigation
    box(*scene, Vector3(), Vector3(1.0f, 1.0f, 1.0f));
    NavigationSystem* system = scene->getSystem<NavigationSystem>().get();
    for (int i = 0; i < 10; i++)
        system->fixedUpdate(DT);
    const uint64_t before = system->getWorkingTicks();
    if (testsupport::countsAllocations()) {
        testsupport::ThreadAllocationCount allocations;
        for (int i = 0; i < 1000; i++)
            system->fixedUpdate(DT);
        EXPECT_EQ(allocations.count(), 0u);
    }
    EXPECT_EQ(system->getWorkingTicks(), before);
    EXPECT_EQ(system->getCrowdCount(), 0);
    EXPECT_EQ(system->getMemoryBytes(), 0u);
    // the time of a tick, the median of 20 batches of 1000
    std::vector<double> batches;
    for (int b = 0; b < 20; b++) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 1000; i++)
            system->fixedUpdate(DT);
        batches.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 1000.0);
    }
    std::sort(batches.begin(), batches.end());
    const double perTick = batches[batches.size() / 2];
    std::printf("[bench] NavSystemTest.ZeroCostEmptyScene %.4f us per fixed tick (budget 1)\n", perTick);
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LT(perTick, 1.0 * testsupport::perfSlack());
    }
}

// 2 runs x 600 ticks x 50 agents: bit-identical positions (AC18)
TEST_F(NavDeterminismTest, CrowdReplay){
    auto run = [&]() {
        scene = std::make_unique<Scene>();
        test::disableSubSystems(*scene);
        scene->getScheduler().setEnabled("NavigationSystem", true);
        nav = scene->getSystem<NavigationSystem>().get();
        const Entity surface = planeSurface(40.0f, 40.0f);
        box(*scene, Vector3(0.0f, 1.0f, 0.0f), Vector3(6.0f, 2.0f, 6.0f));
        bake(surface);
        std::vector<Entity> agents;
        for (int i = 0; i < 50; i++)
            agents.push_back(agent(*scene, Vector3(-15.0f + 0.6f * (float)(i % 10), 0.0f, -15.0f + 1.2f * (float)(i / 10))));
        step();
        for (size_t i = 0; i < agents.size(); i++)
            NavMeshAgent(scene.get(), agents[i]).setDestination(Vector3(15.0f - 0.6f * (float)(i % 10), 0.0f, 15.0f - 1.2f * (float)(i / 10)));
        steps(600);
        std::vector<float> positions;
        for (Entity entity : agents) {
            const Vector3 p = position(entity);
            positions.insert(positions.end(), {p.x, p.y, p.z});
        }
        return positions;
    };
    const std::vector<float> first = run();
    const std::vector<float> second = run();
    ASSERT_EQ(first.size(), second.size());
    EXPECT_EQ(std::memcmp(first.data(), second.data(), first.size() * sizeof(float)), 0);
}

// ---- S55 queries (docs/specs/S55-navigation-gameplay-api.md R9, R10)

// AC10: an agent with its own cost on the mud detours through the gap, its neighbour walks through
// the mud; two agents with the same costs share one filter
TEST_F(NavCrowdTest, PerAgentAreaCosts){
    // a strip of area 3 (project cost 1) across a 30 x 10 plane but for a gap at z in [3, 5]
    const Entity surface = planeSurface(30.0f, 10.0f);
    areaVolume(Vector3(0.0f, 0.0f, -1.0f), Vector3(2.0f, 4.0f, 8.0f), 3);
    bake(surface);
    // the plain agent walks far from the careful one's way to the gap (no avoidance between them)
    const Entity careful = agent(*scene, Vector3(-10.0f, 0.0f, 0.5f));
    const Entity alike = agent(*scene, Vector3(-13.0f, 0.0f, 4.0f));
    const Entity plain = agent(*scene, Vector3(-10.0f, 0.0f, -3.5f));
    NavMeshAgent(scene.get(), careful).setAreaCost(3, 10.0f);
    NavMeshAgent(scene.get(), alike).setAreaCost(3, 10.0f);
    EXPECT_FLOAT_EQ(NavMeshAgent(scene.get(), careful).getAreaCost(3), 10.0f);
    EXPECT_FLOAT_EQ(NavMeshAgent(scene.get(), plain).getAreaCost(3), 1.0f);
    step();
    // the all-areas filter and one for both careful agents
    EXPECT_EQ(nav->getSurfaceStatus(surface).crowdFilters, 2);
    EXPECT_EQ(nav->getSurfaceStatus(surface).crowdFilterOverflow, 0);
    // the agent filter carries its costs to queries
    EXPECT_FLOAT_EQ(nav->getAgentQueryFilter(careful).getAreaCost(3), 10.0f);
    EXPECT_EQ(nav->getAgentQueryFilter(plain).costMask, 0);

    NavMeshAgent(scene.get(), careful).setDestination(Vector3(10.0f, 0.0f, 0.5f));
    NavMeshAgent(scene.get(), plain).setDestination(Vector3(10.0f, 0.0f, -3.5f));
    float carefulMaxZ = -1e9f, plainMaxZ = -1e9f;
    bool plainInMud = false, carefulInMud = false;
    for (int tick = 0; tick < 60 * 15; tick++) {
        step();
        const Vector3 c = position(careful), p = position(plain);
        carefulMaxZ = std::max(carefulMaxZ, c.z);
        plainMaxZ = std::max(plainMaxZ, p.z);
        auto inMud = [](const Vector3& at) { return std::fabs(at.x) < 0.8f && at.z < 2.8f; };
        carefulInMud = carefulInMud || inMud(c);
        plainInMud = plainInMud || inMud(p);
    }
    EXPECT_GT(carefulMaxZ, 3.0f - 0.2f);
    EXPECT_FALSE(carefulInMud);
    EXPECT_TRUE(plainInMud);
    EXPECT_LT(plainMaxZ, 0.0f);
    EXPECT_GT(position(careful).x, 9.0f);
    EXPECT_GT(position(plain).x, 9.0f);

    // a reset cost follows the project's again: the careful agent's filter is the plain one's
    NavMeshAgent(scene.get(), careful).resetAreaCost(3);
    NavMeshAgent(scene.get(), alike).resetAreaCost(3);
    EXPECT_FLOAT_EQ(NavMeshAgent(scene.get(), careful).getAreaCost(3), 1.0f);
    step();
    EXPECT_EQ(nav->getSurfaceStatus(surface).crowdFilters, 1);
}

// AC10: 16 filters per crowd: a 17th key falls back to a filter with its mask (its masked area is
// still never entered), counted and warned once; freed filters fit new keys (S19 kept a mask's
// filter forever, so a new mask fell back to every area and walked the masked strip)
TEST_F(NavCrowdTest, PerAgentAreaCostsFilterSlots){
    // a strip of area 3 across the whole plane: masked out, nobody crosses it
    const Entity surface = planeSurface(30.0f, 40.0f);
    areaVolume(Vector3(0.0f, 0.0f, 0.0f), Vector3(2.0f, 4.0f, 44.0f), 3);
    bake(surface);
    const uint16_t noMud = (uint16_t)(nav::ALL_AREAS & ~(1u << 3));
    auto spawn = [&](int i, uint16_t mask) {
        const Entity entity = agent(*scene, Vector3(-8.0f, 0.0f, -18.0f + 1.2f * (float)i), 3.5f, 0.3f);
        scene->getComponent<NavMeshAgentComponent>(entity).areaMask = mask;
        return entity;
    };
    // walks every masked agent at the strip and checks none enters it
    auto walk = [&](const std::vector<Entity>& walkers) {
        step();
        for (Entity entity : walkers) {
            const Vector3 start = position(entity);
            NavMeshAgent(scene.get(), entity).setDestination(Vector3(10.0f, 0.0f, start.z));
        }
        float maxX = -1e9f;
        for (int tick = 0; tick < 60 * 4; tick++) {
            step();
            for (Entity entity : walkers)
                maxX = std::max(maxX, position(entity).x);
        }
        return maxX;
    };

    test::LogCapture log;
    // 15 keys of one mask and different costs fill the 15 free filters, a 16th falls back
    std::vector<Entity> first;
    for (int i = 0; i < 16; i++) {
        first.push_back(spawn(i, noMud));
        nav->setAgentAreaCost(first.back(), 4, 2.0f + (float)i);
    }
    const float firstMaxX = walk(first);
    EXPECT_EQ(nav->getSurfaceStatus(surface).crowdFilters, 16);
    EXPECT_EQ(nav->getSurfaceStatus(surface).crowdFilterOverflow, 1);
    EXPECT_EQ(log.count(D_LOG_WARN, "no free query filter"), 1u);
    // the fallback kept its mask: nobody crossed into the strip
    EXPECT_LT(firstMaxX, -1.0f + 0.2f);

    // they leave: their filters are freed
    for (Entity entity : first)
        scene->destroyEntity(entity);
    step();
    EXPECT_EQ(nav->getSurfaceStatus(surface).crowdFilters, 1);
    EXPECT_EQ(nav->getSurfaceStatus(surface).crowdFilterOverflow, 0);

    // 15 new masks (each without the mud, and without some unused areas) all get a filter
    std::vector<Entity> second;
    for (int i = 0; i < 15; i++)
        second.push_back(spawn(i, (uint16_t)(noMud & ~((unsigned)(i + 1) << 5))));
    const float secondMaxX = walk(second);
    EXPECT_EQ(nav->getSurfaceStatus(surface).crowdFilters, 16);
    EXPECT_EQ(nav->getSurfaceStatus(surface).crowdFilterOverflow, 0);
    EXPECT_LT(secondMaxX, -1.0f + 0.2f);
    // one warning per crowd
    EXPECT_EQ(log.count(D_LOG_WARN, "no free query filter"), 1u);
}

// ---------------------------------------------------------------- S53 (docs/specs/S53-navigation-fixes.md)

#if JASPERENGINE_NAVIGATION && !defined(NDEBUG)
namespace {

    // Detour's asserts, recorded instead of aborting (the asserts exist where NDEBUG is off: the
    // sanitizer builds)
    int detourAsserts = 0;
    std::string firstDetourAssert;

    void recordDetourAssert(const char* expression, const char* file, int line) {
        if (detourAsserts++ == 0)
            firstDetourAssert = std::string(expression) + " (" + file + ":" + std::to_string(line) + ")";
    }

}
#endif

// A path end to end of the strip is longer than the crowd's 256-polygon corridor: the corridor is
// filled to capacity (upstream 9147d09's assert), the agent's path is Complete while the corridor
// is truncated, and dtCrowd's extension takes it to the destination, where it arrives once (AC1)
TEST_F(NavCrowdTest, CorridorAtCapacity){
#if JASPERENGINE_NAVIGATION && !defined(NDEBUG)
    detourAsserts = 0;
    firstDetourAssert.clear();
    dtAssertFailFunc* previous = dtAssertFailGetCustom();
    dtAssertFailSetCustom(&recordDetourAssert);
#endif
    const Entity surface = stripSurface();
    bake(surface);
    const Vector3 start(-295.0f, 0.0f, 0.0f), destination(295.0f, 0.0f, 0.0f);
    ASSERT_EQ(nav->calculatePath(start, destination).status, nav::PathStatus::Complete);
    const Entity runner = agent(*scene, start, 20.0f);
    step();
    NavMeshAgent handle(scene.get(), runner);
    ASSERT_TRUE(handle.isOnNavMesh());
    int reached = 0, failed = 0;
    handle.getOnDestinationReached() = std::function<void(NavMeshAgent)>([&](NavMeshAgent) { reached++; });
    handle.getOnPathFailed() = std::function<void(NavMeshAgent)>([&](NavMeshAgent) { failed++; });
    ASSERT_TRUE(handle.setDestination(destination));
    bool truncated = false;
    std::vector<Vector3> corners;
    for (int tick = 0; tick < 60 * 60 && reached == 0; tick++) {
        step();
        if (handle.isPathPending() || !handle.hasPath())
            continue;
        EXPECT_EQ(handle.getPathStatus(), nav::PathStatus::Complete) << "tick " << tick;
        // the corridor ends short of the destination, and remainingDistance reaches past its end
        if (nav->getAgentPath(runner, corners) && !corners.empty() && corners.back().distance(destination) > 50.0f) {
            truncated = true;
            EXPECT_GE(handle.getRemainingDistance(), position(runner).distance(destination) - 0.5f) << "tick " << tick;
        }
    }
    EXPECT_TRUE(truncated);
    EXPECT_EQ(reached, 1);
    EXPECT_EQ(failed, 0);
    EXPECT_LT(Vector3(position(runner).x - destination.x, 0.0f, position(runner).z - destination.z).length(), 0.1f);
    steps(60);
    EXPECT_EQ(reached, 1);
#if JASPERENGINE_NAVIGATION && !defined(NDEBUG)
    dtAssertFailSetCustom(previous);
    EXPECT_EQ(detourAsserts, 0) << firstDetourAssert;
#endif
}

#if JASPERENGINE_NAVIGATION
// More polygons visited than the path may hold (upstream 599fd0f): no negative-size memmove, no
// write past maxPath, the newest visited polygons kept (AC2)
TEST_F(NavCrowdTest, MergeCorridorStartMovedBounds){
    constexpr int MAX_PATH = 4;
    constexpr dtPolyRef CANARY = 0xC0FFEE11u;
    dtPolyRef buffer[MAX_PATH + 8];
    for (dtPolyRef& value : buffer)
        value = CANARY;
    const dtPolyRef path[MAX_PATH] = {1, 2, 3, 4};
    std::memcpy(buffer, path, sizeof(path));
    // the only polygon in common is the first visited: 8 visited polygons to keep
    const dtPolyRef visited[8] = {1, 20, 21, 22, 23, 24, 25, 26};
    const int count = dtMergeCorridorStartMoved(buffer, MAX_PATH, MAX_PATH, visited, 8);
    EXPECT_LE(count, MAX_PATH);
    for (int i = MAX_PATH; i < MAX_PATH + 8; i++)
        EXPECT_EQ(buffer[i], CANARY) << "slot " << i;
    for (int i = 0; i < std::min(count, MAX_PATH); i++)
        EXPECT_EQ(buffer[i], visited[7 - i]) << "slot " << i;
}
#endif

// updateTiles grows the tile count past Detour's pool: create() runs inside the same NavMesh, its
// generation changes and the crowd is made again, every agent placed on the new mesh (AC12, R9)
TEST_F(NavCrowdTest, CrowdRebuiltOnNewMesh){
    const Entity surface = planeSurface(12.0f, 12.0f, 0.2f);
    bake(surface);
    const nav::NavMesh* mesh = nav->getNavMesh(surface);
    ASSERT_NE(mesh, nullptr);
    const uint64_t generation = mesh->getGeneration();
    const int tiles = mesh->getTileCount();
    std::vector<Entity> agents;
    for (int i = 0; i < 4; i++)
        agents.push_back(agent(*scene, Vector3(-3.0f + 2.0f * (float)i, 0.0f, -3.0f)));
    steps(2);
    for (size_t i = 0; i < agents.size(); i++)
        NavMeshAgent(scene.get(), agents[i]).setDestination(Vector3(-3.0f + 2.0f * (float)i, 0.0f, 3.0f));
    steps(5);
    // a second ground far along x: more tiles than the pool holds
    plane(*scene, Vector3(40.0f, 0.0f, 0.0f), 30.0f, 30.0f);
    resolveTransforms(*scene);
    std::string error;
    ASSERT_TRUE(nav->updateTiles(surface, AABB(Vector3(24.0f, -1.0f, -16.0f), Vector3(56.0f, 2.0f, 16.0f)), error)) << error;
    const auto since = std::chrono::steady_clock::now();
    for (int tick = 0; bakeRunning(surface, since); tick++)
        step();
    ASSERT_FALSE(nav->isBaking(surface));
    ASSERT_EQ(nav->getNavMesh(surface), mesh);
    EXPECT_GT(mesh->getTileCount(), tiles);
    EXPECT_NE(mesh->getGeneration(), generation);
    steps(2);
    for (size_t i = 0; i < agents.size(); i++) {
        EXPECT_TRUE(NavMeshAgent(scene.get(), agents[i]).isOnNavMesh()) << "agent " << i;
        // placed where it stood, not moved elsewhere
        EXPECT_LT(std::fabs(position(agents[i]).x - (-3.0f + 2.0f * (float)i)), 0.5f) << "agent " << i;
    }
    // they still walk: the crowd's corridors are the new mesh's
    steps(240);
    for (size_t i = 0; i < agents.size(); i++)
        EXPECT_GT(position(agents[i]).z, 2.0f) << "agent " << i;
}

namespace {

    // a carving surface over a w x d plane on tiles of 16 cells of 0.2 m (3.2 m)
    Entity smallTileCarvingSurface(NavSceneTest& test, Scene& scene, float width, float depth) {
        (void)test;
        plane(scene, Vector3(), width, depth);
        const Entity surface = transformEntity(scene, Vector3(), NULL_ENTITY, "Surface");
        addSurface(scene, surface, 0.2f, 16).runtimeCarving = true;
        return surface;
    }

    bool walkableAt(NavigationSystem& nav, const Vector3& point) {
        nav::NavHit hit;
        return nav.samplePosition(point, 0.15f, hit) && Vector3(hit.position.x - point.x, 0.0f, hit.position.z - point.z).length() < 0.15f;
    }

}

// 60 carving boxes are cut; in one tick all 60 are disabled while 10 others are enabled: 70
// requests, more than Detour's 64-entry request list takes at once. Every cut is taken back and
// every new one made, none lost (AC14, R11)
TEST_F(NavObstacleTest, RemoveNeverLost){
    const Entity surface = smallTileCarvingSurface(*this, *scene, 40.0f, 40.0f);
    bake(surface);
    nav->setCarveTimeCeiling(0.0);
    // at tile centres (3.2 m tiles): one tile each, so no update list overflows on the way in
    const auto centre = [](int i) { return Vector3(-16.0f + 1.6f + 3.2f * (float)(i % 10), 0.5f, -16.0f + 1.6f + 3.2f * (float)(i / 10)); };
    // the 10 enabled later come first in entity order: their adds are requested before the removals
    std::vector<Entity> late;
    for (int i = 60; i < 70; i++) {
        late.push_back(obstacleBox(*scene, centre(i), Vector3(0.8f, 1.0f, 0.8f), true));
        scene->getComponent<NavMeshObstacleComponent>(late.back()).enabled = false;
    }
    std::vector<Entity> early;
    for (int i = 0; i < 60; i++)
        early.push_back(obstacleBox(*scene, centre(i), Vector3(0.8f, 1.0f, 0.8f), true));
    steps(30);
    int cut = 0;
    for (int i = 0; i < 60; i++)
        cut += walkableAt(*nav, Vector3(centre(i).x, 0.0f, centre(i).z)) ? 0 : 1;
    ASSERT_EQ(cut, 60);
    for (Entity entity : early)
        scene->getComponent<NavMeshObstacleComponent>(entity).enabled = false;
    for (Entity entity : late)
        scene->getComponent<NavMeshObstacleComponent>(entity).enabled = true;
    // 70 tile builds at the budget's 4 a tick
    steps(60);
    int left = 0, made = 0;
    for (int i = 0; i < 60; i++)
        left += walkableAt(*nav, Vector3(centre(i).x, 0.0f, centre(i).z)) ? 0 : 1;
    for (int i = 60; i < 70; i++)
        made += walkableAt(*nav, Vector3(centre(i).x, 0.0f, centre(i).z)) ? 0 : 1;
    EXPECT_EQ(left, 0);
    EXPECT_EQ(made, 10);
    const nav::NavMesh* mesh = nav->getNavMesh(surface);
    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->getTileCacheObstacleCount(), 10);
    EXPECT_EQ(mesh->getQueuedBuilds(), 0);
}

// 40 boxes, each straddling 4 tiles of a 12 x 12 tile surface, appear in one tick: within the
// budget every one blocks its own probe path (AC14, R11)
TEST_F(NavObstacleTest, ManyObstaclesManyTiles){
    const Entity surface = smallTileCarvingSurface(*this, *scene, 38.4f, 38.4f);
    bake(surface);
    nav->setCarveTimeCeiling(0.0);
    std::vector<Vector3> centres;
    for (int i = 0; i < 40; i++) {
        const Vector3 centre(-16.0f + 3.2f * (float)(i % 8), 0.5f, -12.8f + 6.4f * (float)(i / 8));
        centres.push_back(centre);
        obstacleBox(*scene, centre, Vector3(1.2f, 1.0f, 1.2f), true);
    }
    // 160 tile builds at 4 a tick
    steps(60);
    for (size_t i = 0; i < centres.size(); i++) {
        const Vector3& c = centres[i];
        EXPECT_FALSE(walkableAt(*nav, Vector3(c.x, 0.0f, c.z))) << "box " << i;
        // each quarter of the box (one per tile) is cut
        for (int q = 0; q < 4; q++)
            EXPECT_FALSE(walkableAt(*nav, Vector3(c.x + ((q & 1) ? 0.3f : -0.3f), 0.0f, c.z + ((q & 2) ? 0.3f : -0.3f))))
                << "box " << i << " quarter " << q;
    }
}

// Max Carving Obstacles 4: 6 carving obstacles give 4 cuts; the other 2 are avoided like agents,
// with a warning each (AC15, R11, R17)
TEST_F(NavObstacleTest, MaxCarvingObstaclesRespected){
    const Entity surface = smallTileCarvingSurface(*this, *scene, 30.0f, 30.0f);
    scene->getComponent<NavMeshSurfaceComponent>(surface).maxCarvingObstacles = 4;
    bake(surface);
    nav->setCarveTimeCeiling(0.0);
    EXPECT_EQ(nav->getNavMesh(surface)->getHeader().maxObstacles, 4);
    test::LogCapture log;
    agent(*scene, Vector3(-12.0f, 0.0f, -12.0f));
    std::vector<Vector3> centres;
    for (int i = 0; i < 6; i++) {
        centres.push_back(Vector3(-10.0f + 4.0f * (float)i, 0.5f, 5.0f));
        obstacleBox(*scene, centres.back(), Vector3(1.0f, 1.0f, 1.0f), true);
    }
    steps(30);
    int cut = 0;
    for (const Vector3& c : centres)
        cut += walkableAt(*nav, Vector3(c.x, 0.0f, c.z)) ? 0 : 1;
    EXPECT_EQ(cut, 4);
    // the agent and the 2 obstacles it avoids
    EXPECT_EQ(nav->getSurfaceStatus(surface).crowdAgents, 3);
    EXPECT_EQ(log.count(D_LOG_WARN, "holds its Max Carving Obstacles (4)"), 2u);
    steps(30);
    EXPECT_EQ(log.count(D_LOG_WARN, "holds its Max Carving Obstacles (4)"), 2u);
}

// a stationary cut survives a full runtime rebake and a reload of the asset (AC16, R12)
TEST_F(NavObstacleTest, CutsSurviveRebakeAndReload){
    const Entity surface = gapSurface();
    bake(surface);
    nav->setCarveTimeCeiling(0.0);
    const Vector3 from(-10.0f, 0.0f, 0.0f), to(10.0f, 0.0f, 0.0f);
    obstacleBox(*scene, Vector3(0.0f, 1.0f, 0.0f), Vector3(1.5f, 2.0f, 3.0f), true);
    steps(3);
    ASSERT_NE(nav->calculatePath(from, to).status, nav::PathStatus::Complete);

    std::string error;
    ASSERT_TRUE(nav->bake(surface, NavBakeMode::FULL, error)) << error;
    const auto since = std::chrono::steady_clock::now();
    for (int tick = 0; bakeRunning(surface, since); tick++) {
        step();
        EXPECT_NE(nav->calculatePath(from, to).status, nav::PathStatus::Complete) << "tick " << tick;
    }
    ASSERT_FALSE(nav->isBaking(surface));
    ASSERT_TRUE(nav->getLastBakeResult(surface) && nav->getLastBakeResult(surface)->ok);
    steps(3);
    EXPECT_NE(nav->calculatePath(from, to).status, nav::PathStatus::Complete);

    // the asset read again
    test::TestTempDir dir("navcuts");
    const std::vector<uint8_t> bytes = nav->getNavMeshFile(surface);
    ASSERT_FALSE(bytes.empty());
    const std::string file = (dir / "Surface.navmesh").string();
    test::TestTempDir::write(file, std::string(bytes.begin(), bytes.end()));
    scene->getComponent<NavMeshSurfaceComponent>(surface).navMeshData = AssetRef(file);
    ASSERT_TRUE(nav->reloadNavMesh(surface, error)) << error;
    steps(3);
    EXPECT_NE(nav->calculatePath(from, to).status, nav::PathStatus::Complete);
}

// updateTiles over a carving obstacle: the rebuilt tiles keep its cut (AC16, R12)
TEST_F(NavObstacleTest, CutsSurviveUpdateTiles){
    const Entity surface = gapSurface();
    bake(surface);
    nav->setCarveTimeCeiling(0.0);
    const Vector3 from(-10.0f, 0.0f, 0.0f), to(10.0f, 0.0f, 0.0f);
    obstacleBox(*scene, Vector3(0.0f, 1.0f, 0.0f), Vector3(1.5f, 2.0f, 3.0f), true);
    steps(3);
    ASSERT_NE(nav->calculatePath(from, to).status, nav::PathStatus::Complete);
    std::string error;
    ASSERT_TRUE(nav->updateTiles(surface, AABB(Vector3(-2.0f, -1.0f, -2.0f), Vector3(2.0f, 3.0f, 2.0f)), error)) << error;
    const auto since = std::chrono::steady_clock::now();
    for (int tick = 0; bakeRunning(surface, since); tick++)
        step();
    ASSERT_FALSE(nav->isBaking(surface));
    ASSERT_TRUE(nav->getLastBakeResult(surface) && nav->getLastBakeResult(surface)->ok);
    EXPECT_GT(nav->getLastBakeResult(surface)->tilesBuilt, 0);
    steps(3);
    EXPECT_NE(nav->calculatePath(from, to).status, nav::PathStatus::Complete);
}

// Unity's thresholds (AC17, R13): a 6 x 6 m box (half diagonal 4.24 m) with a 0.1 m threshold
// re-cuts past 0.1 / 4.24 rad of yaw (1.35 degrees); a capsule's turn never re-cuts; a scale
// change does; 1 mm of jitter still counts as stationary
TEST_F(NavObstacleTest, RecutThresholds){
    const Entity surface = smallTileCarvingSurface(*this, *scene, 40.0f, 40.0f);
    bake(surface);
    nav->setCarveTimeCeiling(0.0);
    const nav::NavMesh* mesh = nav->getNavMesh(surface);
    const Entity crate = obstacleBox(*scene, Vector3(-8.0f, 0.5f, 0.0f), Vector3(6.0f, 1.0f, 6.0f), true);
    steps(10);
    ASSERT_FALSE(walkableAt(*nav, Vector3(-8.0f, 0.0f, 0.0f)));
    auto yawTo = [&](Entity entity, float degrees) {
        scene->getComponent<Transform>(entity).rotation = Quaternion(degrees, Vector3(0.0f, 1.0f, 0.0f));
        scene->getComponent<Transform>(entity).needUpdate = true;
    };
    uint64_t revision = mesh->getRevision();
    yawTo(crate, 1.0f);
    steps(5);
    EXPECT_EQ(mesh->getRevision(), revision) << "1 degree re-cut";
    yawTo(crate, 3.0f);
    steps(5);
    EXPECT_NE(mesh->getRevision(), revision) << "3 degrees did not re-cut";

    // a capsule turned 90 degrees about its axis
    const Entity post = transformEntity(*scene, Vector3(8.0f, 1.0f, 0.0f));
    NavMeshObstacleComponent capsule;
    capsule.shape = NavObstacleShape::CAPSULE;
    capsule.radius = 1.0f;
    capsule.height = 2.0f;
    capsule.carve = true;
    capsule.carveOnlyStationary = false;
    scene->addComponent<NavMeshObstacleComponent>(post, capsule);
    steps(10);
    ASSERT_FALSE(walkableAt(*nav, Vector3(8.0f, 0.0f, 0.0f)));
    revision = mesh->getRevision();
    yawTo(post, 90.0f);
    steps(5);
    EXPECT_EQ(mesh->getRevision(), revision) << "the capsule's turn re-cut";
    // its scale re-cuts
    scene->getComponent<Transform>(post).scale = Vector3(2.0f, 1.0f, 2.0f);
    scene->getComponent<Transform>(post).needUpdate = true;
    steps(5);
    EXPECT_NE(mesh->getRevision(), revision) << "the scale change did not re-cut";
    EXPECT_FALSE(walkableAt(*nav, Vector3(9.6f, 0.0f, 0.0f)));

    // carve only stationary, +-1 mm of rest jitter a tick: stationary after Time To Stationary
    const Entity jittery = obstacleBox(*scene, Vector3(0.0f, 0.5f, 12.0f), Vector3(2.0f, 1.0f, 2.0f), true);
    scene->getComponent<NavMeshObstacleComponent>(jittery).carveOnlyStationary = true;
    scene->getComponent<NavMeshObstacleComponent>(jittery).timeToStationary = 0.5f;
    for (int tick = 0; tick < 90; tick++) {
        scene->getComponent<Transform>(jittery).position = Vector3((tick % 2) ? 0.001f : -0.001f, 0.5f, 12.0f);
        scene->getComponent<Transform>(jittery).needUpdate = true;
        step();
    }
    EXPECT_FALSE(walkableAt(*nav, Vector3(0.0f, 0.0f, 12.0f)));
}

// Carve Tiles Per Tick 1, no wall-clock ceiling: an obstacle over 3 tiles is cut one tile a tick,
// whole exactly at tick 3 (AC18, R14)
TEST_F(NavObstacleTest, CarveCountBudget){
    nav::NavigationSettings settings = nav::NavigationSettings::defaults();
    settings.carveTilesPerTick = 1;
    nav::setSettings(settings);
    // one row of 3.2 m tiles (z in [0, 3.2))
    plane(*scene, Vector3(0.0f, 0.0f, 1.6f), 30.0f, 3.0f);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, 0.2f, 16).runtimeCarving = true;
    bake(surface);
    nav->setCarveTimeCeiling(0.0);
    // x in [-1.2, 4.4]: Detour's bounds of the box (its half diagonal) touch tiles x = -1, 0 and 1
    obstacleBox(*scene, Vector3(1.6f, 0.5f, 1.6f), Vector3(5.6f, 1.0f, 1.0f), true);
    const Vector3 probes[3] = {Vector3(-0.6f, 0.0f, 1.6f), Vector3(1.6f, 0.0f, 1.6f), Vector3(4.0f, 0.0f, 1.6f)};
    auto cut = [&]() {
        int count = 0;
        for (const Vector3& probe : probes)
            count += walkableAt(*nav, probe) ? 0 : 1;
        return count;
    };
    for (int tick = 1; tick <= 3; tick++) {
        step();
        EXPECT_EQ(cut(), tick) << "tick " << tick;
        EXPECT_LE(nav->getCarveTileBuilds(), 1) << "tick " << tick;
    }
    steps(3);
    EXPECT_EQ(cut(), 3);
}

// 2 runs x 600 ticks, moving carving obstacles and 20 agents: bit-identical positions (AC18, R14)
TEST_F(NavDeterminismTest, CarvingReplay){
    auto run = [&]() {
        scene = std::make_unique<Scene>();
        test::disableSubSystems(*scene);
        scene->getScheduler().setEnabled("NavigationSystem", true);
        nav = scene->getSystem<NavigationSystem>().get();
        const Entity surface = smallTileCarvingSurface(*this, *scene, 30.0f, 30.0f);
        bake(surface);
        nav->setCarveTimeCeiling(0.0);
        std::vector<Entity> movers;
        for (int i = 0; i < 3; i++)
            movers.push_back(obstacleBox(*scene, Vector3(-6.0f + 6.0f * (float)i, 0.5f, 0.0f), Vector3(2.0f, 1.0f, 2.0f), true));
        std::vector<Entity> agents;
        for (int i = 0; i < 20; i++)
            agents.push_back(agent(*scene, Vector3(-12.0f + 1.2f * (float)(i % 10), 0.0f, -12.0f + 1.5f * (float)(i / 10))));
        step();
        for (size_t i = 0; i < agents.size(); i++)
            NavMeshAgent(scene.get(), agents[i]).setDestination(Vector3(12.0f - 1.2f * (float)(i % 10), 0.0f, 12.0f - 1.5f * (float)(i / 10)));
        for (int tick = 0; tick < 600; tick++) {
            for (size_t i = 0; i < movers.size(); i++) {
                Transform& transform = scene->getComponent<Transform>(movers[i]);
                transform.position = Vector3(-6.0f + 6.0f * (float)i, 0.5f, 6.0f * std::sin(0.02f * (float)tick + (float)i));
                transform.needUpdate = true;
            }
            step();
        }
        std::vector<float> positions;
        for (Entity entity : agents) {
            const Vector3 p = position(entity);
            positions.insert(positions.end(), {p.x, p.y, p.z});
        }
        return positions;
    };
    const std::vector<float> first = run();
    const std::vector<float> second = run();
    ASSERT_EQ(first.size(), second.size());
    EXPECT_EQ(std::memcmp(first.data(), second.data(), first.size() * sizeof(float)), 0);
}

// a 20 m box on 3.2 m tiles touches more tile layers than Detour cuts: one warning names the count
// (AC20, R16)
TEST_F(NavObstacleTest, LargeObstacleWarns){
    const Entity surface = smallTileCarvingSurface(*this, *scene, 40.0f, 40.0f);
    bake(surface);
    test::LogCapture log;
    obstacleBox(*scene, Vector3(0.0f, 0.5f, 0.0f), Vector3(20.0f, 1.0f, 20.0f), true);
    steps(10);
    const std::vector<test::TestSystem::LogEntry> warnings = log.entries(D_LOG_WARN, "and is cut in 8");
    ASSERT_EQ(warnings.size(), 1u);
    // Detour bounds a box by its half diagonal, 1.41 x 10 m: x and z in [-14.1, 14.1], tiles -5..4
    EXPECT_NE(warnings[0].message.find("covers 100 tile layers of 'Surface'"), std::string::npos) << warnings[0].message;
}

// 10 walking agents (navmesh and grid) and 2 standing obstacles, one carving: a steady fixed tick
// allocates nothing (AC22, R19)
TEST_F(NavSystemTest, SteadyStateNoAllocations){
    const Entity surface = smallTileCarvingSurface(*this, *scene, 60.0f, 20.0f);
    bake(surface);
    nav->setCarveTimeCeiling(0.0);
    obstacleBox(*scene, Vector3(0.0f, 0.5f, 8.0f), Vector3(1.0f, 1.0f, 1.0f), true);
    obstacleBox(*scene, Vector3(10.0f, 0.5f, 8.0f), Vector3(1.0f, 1.0f, 1.0f), false);
    std::vector<Entity> walkers;
    for (int i = 0; i < 5; i++)
        walkers.push_back(agent(*scene, Vector3(-28.0f, 0.0f, -6.0f + 3.0f * (float)i), 1.0f));
    const Entity gridEntity = transformEntity(*scene, Vector3(0.0f, 0.0f, 40.0f), NULL_ENTITY, "Grid");
    NavGridComponent grid;
    grid.width = 60;
    grid.height = 10;
    grid.cellSize = 1.0f;
    scene->addComponent<NavGridComponent>(gridEntity, grid);
    std::vector<Entity> gridWalkers;
    for (int i = 0; i < 5; i++) {
        gridWalkers.push_back(agent(*scene, Vector3(1.5f, 0.0f, 40.5f + 2.0f * (float)i), 1.0f));
        scene->getComponent<NavMeshAgentComponent>(gridWalkers.back()).surface = gridEntity;
    }
    step();
    for (size_t i = 0; i < walkers.size(); i++)
        NavMeshAgent(scene.get(), walkers[i]).setDestination(Vector3(28.0f, 0.0f, -6.0f + 3.0f * (float)i));
    for (size_t i = 0; i < gridWalkers.size(); i++)
        NavMeshAgent(scene.get(), gridWalkers[i]).setDestination(Vector3(58.5f, 0.0f, 40.5f + 2.0f * (float)i));
    for (int tick = 0; tick < 120; tick++) {
        resolveTransforms(*scene);
        nav->fixedUpdate(DT);
    }
    uint64_t allocations = 0;
    for (int tick = 120; tick < 240; tick++) {
        resolveTransforms(*scene);
        testsupport::ThreadAllocationCount count;
        nav->fixedUpdate(DT);
        allocations += count.count();
    }
    // (counted where the test binary wraps malloc: Linux)
    if (testsupport::countsAllocations())
        EXPECT_EQ(allocations, 0u);
    // still walking: nothing arrived, so every tick above moved agents
    for (Entity entity : walkers)
        EXPECT_LT(position(entity).x, 20.0f);
    for (Entity entity : gridWalkers) {
        EXPECT_GT(position(entity).x, 4.0f);
        EXPECT_LT(position(entity).x, 50.0f);
    }
}
