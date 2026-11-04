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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::navtest;

namespace {

    class NavCrowdTest : public NavSceneTest {};
    class NavObstacleTest : public NavSceneTest {};
    class NavSystemTest : public NavSceneTest {};
    class NavDeterminismTest : public NavSceneTest {};

    Entity obstacleBox(Scene& scene, const Vector3& center, const Vector3& size, bool carve) {
        const Entity entity = transformEntity(scene, center);
        NavMeshObstacleComponent obstacle;
        obstacle.shape = NavObstacleShape::BOX;
        obstacle.size = size;
        obstacle.carve = carve;
        obstacle.carveOnlyStationary = false;
        scene.addComponent<NavMeshObstacleComponent>(entity, obstacle);
        return entity;
    }

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

// a carving box on a corridor reroutes the path within 2 ticks; removed, the path is back (AC10)
TEST_F(NavObstacleTest, CarveBlocksAndRestores){
    // a 30 x 10 plane with a 2 m wide corridor wall gap: walls leave z in [-1, 1] open at x = 0
    plane(*scene, Vector3(), 30.0f, 10.0f);
    box(*scene, Vector3(0.0f, 1.0f, -3.25f), Vector3(1.0f, 2.0f, 3.5f));
    box(*scene, Vector3(0.0f, 1.0f, 3.25f), Vector3(1.0f, 2.0f, 3.5f));
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, 0.2f, 32).runtimeCarving = true;
    bake(surface);
    ASSERT_TRUE(nav->getNavMesh(surface)->isCarving());
    const Vector3 from(-10.0f, 0.0f, 0.0f), to(10.0f, 0.0f, 0.0f);
    const nav::NavPath open = nav->calculatePath(from, to);
    ASSERT_EQ(open.status, nav::PathStatus::Complete);

    // A tick rebuilds tiles for 1 ms of wall clock, and at least one (NavigationSystem's
    // CARVE_BUDGET_MS). Under a sanitizer's instrumentation one tile is over that budget on its
    // own: the 2 ticks are every other build's, there the state is waited for, in ticks
    const auto carvingCatchesUp = [&](const std::function<bool()>& caughtUp){
#if defined(JASPERENGINE_SANITIZE_ADDRESS) || defined(JASPERENGINE_SANITIZE_THREAD) || defined(JASPERENGINE_SANITIZE_UNDEFINED)
        const auto start = std::chrono::steady_clock::now();
        int ticks = 0;
        while (ticks < 240 && !caughtUp()){
            step();
            ticks++;
        }
        printf("[bench] the carving caught up in %d ticks, %.1f ms (under a sanitizer, no budget)\n", ticks,
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
#else
        steps(2);
#endif
    };

    const Entity blocker = obstacleBox(*scene, Vector3(0.0f, 1.0f, 0.0f), Vector3(1.5f, 2.0f, 3.0f), true);
    carvingCatchesUp([&](){ return nav->calculatePath(from, to).status != nav::PathStatus::Complete; });
    // blocked: the only way through is cut
    EXPECT_NE(nav->calculatePath(from, to).status, nav::PathStatus::Complete);

    scene->destroyEntity(blocker);
    carvingCatchesUp([&](){ return nav->calculatePath(from, to).status == nav::PathStatus::Complete; });
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
