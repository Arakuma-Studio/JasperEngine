// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// je-bench-nav: the navigation budgets (docs/specs/S19-navigation.md R25-R30, AC21) on
// bench_nav_city (256 x 256 m, cell 0.2 m, 400 tiles, terrain + 200 buildings) and on grids,
// written as JSON for tools/perf/check_bench.py (docs/perf/navigation.md).
//
//   je-bench-nav [--workers N] [--count-allocs] --json nav.json
//
// The 8-core bake budget is projected from the single-thread time at R24's least parallel
// efficiency (0.6) on machines with fewer cores, as je-bench-particles does.

#include "AllocationCounter.h"
#include "BenchUtil.h"
#include "JobsTestSystem.h"

#include "../ecs/EcsTestUtil.h"
#include "../navigation/NavFixtures.h"

#include "Engine.h"
#include "Scene.h"
#include "component/NavMeshLinkComponent.h"
#include "component/NavMeshObstacleComponent.h"
#include "jobs/JobSystem.h"
#include "navigation/GridPathfinder.h"
#include "navigation/NavBakeCollector.h"
#include "navigation/NavBakeScheduler.h"
#include "navigation/NavDiagnostics.h"
#include "navigation/NavGridMap.h"
#include "navigation/NavMesh.h"
#include "navigation/NavRandom.h"
#include "object/navigation/NavMeshAgent.h"
#include "subsystem/NavigationSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::navtest;

namespace {

    int kWorkers = 8;
    constexpr float kTick = 1.0f / 60.0f;

    jobs::Config benchConfig(int workers) {
        jobs::Config config;
        config.workers = workers;
        config.reserveForFrame = 0;
        config.detectWaitCycles = false;
        return config;
    }

    double msSince(bench::Clock::time_point start) {
        return bench::secondsSince(start) * 1000.0;
    }

    // the tiles of `input` on `system`, assembled into a runtime mesh
    nav::NavMeshData bakeAll(const std::shared_ptr<nav::NavBakeInput>& input, jobs::JobSystem& system, double& milliseconds) {
        const auto start = bench::Clock::now();
        nav::NavBakeOptions options;
        options.mode = nav::NavBakeOptions::Mode::Jobs;
        options.system = &system;
        nav::NavBake bake(input, input->nonEmptyTiles, options);
        bake.wait();
        milliseconds = msSince(start);
        nav::NavMeshData data;
        data.header.config = input->config;
        data.header.maxTiles = input->tileCount();
        data.header.linkAreaCosts = input->linkAreaCosts;
        for (nav::NavTileBuildResult& result : bake.getResults()) {
            for (nav::NavTileBlob& blob : result.blobs)
                data.tiles.push_back(std::move(blob));
        }
        data.sortTiles();
        return data;
    }

    // a serpentine maze: walls every 4th column, open at alternating ends (the worst reachable case)
    nav::NavGridMap serpentine(int size) {
        nav::NavGridMap map;
        map.reset(size, size);
        map.setLayout(nav::GridLayout::Square, 8);
        for (int x = 2; x < size - 1; x += 4) {
            const bool openTop = (x / 4) % 2 == 0;
            map.fillRect(nav::GridCell{x, openTop ? 1 : 0}, nav::GridCell{x, openTop ? size - 1 : size - 2}, nav::AREA_NOT_WALKABLE);
        }
        return map;
    }

}

int main(int argc, char** argv) {
    bool countAllocs = false;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--workers" && i + 1 < argc)
            kWorkers = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--count-allocs")
            countAllocs = true;
    }
    const unsigned hardware = std::thread::hardware_concurrency();
    if (hardware > 1 && (int)hardware - 1 < kWorkers)
        kWorkers = (int)hardware - 1;
    Engine::systemInit(argc, argv, new testsupport::JobsTestSystem());
    Engine::setUpdateTime(kTick);
    std::printf("je-bench-nav: %d workers, %u hardware threads\n", kWorkers, hardware);
    bench::Results results("je-bench-nav");
    jobs::JobSystem pool(benchConfig(kWorkers));
    jobs::JobSystem single(benchConfig(0));

    // ---- bench_nav_city: collect, full bake (all workers and one thread), memory and size
    Scene scene;
    test::disableSubSystems(scene);
    scene.getScheduler().setEnabled("NavigationSystem", true);
    City fixture = city(scene);
    std::string error;
    auto start = bench::Clock::now();
    std::shared_ptr<nav::NavBakeInput> input = nav::NavBakeCollector::collect(&scene, fixture.surface, error);
    if (!input) {
        std::fprintf(stderr, "collect failed: %s\n", error.c_str());
        return 1;
    }
    results.add("city_collect_ms", msSince(start), "ms");
    results.add("city_tiles", (double)input->tileCount(), "tiles");
    double bakeMs = 0.0, bake1Ms = 0.0;
    nav::NavMeshData data = bakeAll(input, pool, bakeMs);
    bakeAll(input, single, bake1Ms);
    results.add("city_bake_full_s", bakeMs / 1000.0, "s");
    results.add("city_bake_full_1t_s", bake1Ms / 1000.0, "s");
    results.add("city_bake_full_8core_projected_s", bake1Ms / 1000.0 / (8.0 * 0.6), "s");
    const std::vector<uint8_t> file = nav::NavMeshFile::write(data);
    results.add("city_navmesh_disk_mb", (double)file.size() / (1024.0 * 1024.0), "MB");
    nav::NavMeshData loaded;
    nav::NavMeshFile::read(file.data(), file.size(), loaded, error);
    nav::NavMesh runtime;
    runtime.create(std::move(loaded), false, error);
    results.add("city_polys", (double)runtime.getPolyCount(), "polys");
    results.add("city_navmesh_ram_mb", (double)runtime.getMemoryBytes() / (1024.0 * 1024.0), "MB");

    // ---- incremental: one building moved, its tiles found and rebuilt (collect included)
    NavigationSystem* nav = scene.getSystem<NavigationSystem>().get();
    nav->setKeepBakeData(true);
    nav->setNavMeshData(fixture.surface, nav::NavMeshData(data), error);
    {
        Transform& building = scene.getComponent<Transform>(fixture.buildings[37]);
        building.position = building.position + Vector3(0.6f, 0.0f, 0.0f);
        resolveTransforms(scene);
        start = bench::Clock::now();
        const NavBakeResult result = nav->bakeNow(fixture.surface, NavBakeMode::DIRTY);
        results.add("city_bake_incremental_ms", msSince(start), "ms");
        results.add("city_bake_incremental_tiles", (double)result.tilesBuilt, "tiles");
    }

    // ---- queries: 1000 random pairs, p95 of each
    {
        std::mt19937 random(3);
        std::uniform_real_distribution<float> coordinate(-120.0f, 120.0f);
        std::vector<double> paths, unreachable, samples, raycasts;
        const nav::NavFilter filter = nav::NavFilter::fromSettings(nav::ALL_AREAS);
        const Vector3 extents(1.0f, 2.0f, 1.0f);
        int complete = 0;
        for (int i = 0; i < 1000; i++) {
            const Vector3 a(coordinate(random), 3.0f, coordinate(random)), b(coordinate(random), 3.0f, coordinate(random));
            const nav::NavHit sa = runtime.samplePosition(a, 6.0f, filter);
            const nav::NavHit sb = runtime.samplePosition(b, 6.0f, filter);
            if (!sa.hit || !sb.hit)
                continue;
            auto t = bench::Clock::now();
            const nav::NavPath path = runtime.findPath(sa.position, sb.position, filter, extents);
            const double ms = msSince(t);
            // R26 times paths between connected points; a pair across islands (a roof) searches
            // the whole node budget and fails, timed on its own
            if (path.status == nav::PathStatus::Complete) {
                paths.push_back(ms);
                complete++;
            } else {
                unreachable.push_back(ms);
            }
            t = bench::Clock::now();
            runtime.samplePosition(a, 6.0f, filter);
            samples.push_back(msSince(t));
            t = bench::Clock::now();
            runtime.raycast(sa.position, sb.position, filter, extents);
            raycasts.push_back(msSince(t));
        }
        results.add("query_path_p95_ms", bench::percentile(paths, 0.95), "ms");
        results.add("query_path_complete_share", (double)complete / std::max<size_t>(1, paths.size() + unreachable.size()), "of pairs");
        results.add("query_path_unreachable_p95_ms", unreachable.empty() ? 0.0 : bench::percentile(unreachable, 0.95), "ms");
        results.add("query_sample_p95_ms", bench::percentile(samples, 0.95), "ms");
        results.add("query_raycast_p95_ms", bench::percentile(raycasts, 0.95), "ms");
    }

    // ---- S76 queries (docs/specs/S76-navigation-gameplay-api.md R36): random points (the area
    // table's first build, then cached), around a point, the closest wall within 5 m, 2 m steps
    {
        const Vector3 extents(1.0f, 2.0f, 1.0f);
        nav::NavRandom random(11);
        nav::NavHit hit;
        // each mask has a table of its own: five first calls
        std::vector<double> firsts;
        for (int area = 3; area < 8; area++) {
            const nav::NavFilter masked = nav::NavFilter::fromSettings((uint16_t)(nav::ALL_AREAS & ~(1u << area)));
            const auto t = bench::Clock::now();
            runtime.findRandomPoint(masked, random, -1, hit);
            firsts.push_back(msSince(t));
        }
        results.add("query_random_first_ms", bench::percentile(firsts, 0.5), "ms");
        const nav::NavFilter filter = nav::NavFilter::fromSettings(nav::ALL_AREAS);
        runtime.findRandomPoint(filter, random, -1, hit);
        std::vector<double> anywhere, around, walls, moves;
        std::vector<Vector3> points;
        for (int i = 0; i < 1000; i++) {
            auto t = bench::Clock::now();
            runtime.findRandomPoint(filter, random, -1, hit);
            anywhere.push_back(msSince(t));
            if (hit.hit)
                points.push_back(hit.position);
        }
        std::mt19937 direction(5);
        std::uniform_real_distribution<float> angle(0.0f, 6.2831853f);
        for (const Vector3& p : points) {
            auto t = bench::Clock::now();
            runtime.findRandomPointAround(p, 10.0f, filter, extents, random, -1, hit);
            around.push_back(msSince(t));
            t = bench::Clock::now();
            runtime.findClosestEdge(p, filter, extents, 5.0f);
            walls.push_back(msSince(t));
            const float a = angle(direction);
            const Vector3 to = p + Vector3(std::cos(a) * 2.0f, 0.0f, std::sin(a) * 2.0f);
            t = bench::Clock::now();
            runtime.moveAlongSurface(p, to, filter, extents, hit);
            moves.push_back(msSince(t));
        }
        results.add("query_random_p95_ms", bench::percentile(anywhere, 0.95), "ms");
        results.add("query_random_circle_p95_ms", bench::percentile(around, 0.95), "ms");
        results.add("query_wall_p95_ms", bench::percentile(walls, 0.95), "ms");
        results.add("query_move_along_p95_ms", bench::percentile(moves, 0.95), "ms");
    }

    // ---- S75 diagnostics: island labelling, a disconnected reject, locate (islands current)
    {
        static_assert(sizeof(nav::NavIsland) <= 48, "R21: an island is at most 48 bytes");
        nav::NavIslandLabels labels;
        std::vector<double> times;
        for (int i = 0; i < 20; i++) {
            const auto t = bench::Clock::now();
            nav::labelIslands(runtime.detourNavMesh(), labels);
            times.push_back(msSince(t));
        }
        results.add("city_islands_ms", bench::percentile(times, 0.5), "ms");
        results.add("city_islands", (double)labels.islands.size(), "islands");
        const size_t islandBytes = labels.islands.capacity() * sizeof(nav::NavIsland);
        results.add("islands_bytes_per_poly", (double)(labels.memoryBytes() - islandBytes) / std::max(1, runtime.getPolyCount()), "B");

        const nav::NavFilter filter = nav::NavFilter::fromSettings(nav::ALL_AREAS);
        const Vector3 extents(1.0f, 2.0f, 1.0f);
        const std::vector<nav::NavIsland>& islands = runtime.getIslands();
        int largest = 0;
        for (const nav::NavIsland& island : islands) {
            if (island.area > islands[(size_t)largest].area)
                largest = island.id;
        }
        std::mt19937 random(11);
        std::uniform_real_distribution<float> coordinate(-120.0f, 120.0f);
        std::uniform_real_distribution<float> height(0.0f, 16.0f);
        // a pair on two islands (the ground and a roof): rejected without a search
        Vector3 ground, roof;
        bool hasGround = false, hasRoof = false;
        std::vector<double> locates;
        for (int i = 0; i < 2000; i++) {
            const Vector3 p(coordinate(random), height(random), coordinate(random));
            const auto t = bench::Clock::now();
            const nav::NavLocation location = runtime.locate(p, filter, extents, 5.0f);
            locates.push_back(msSince(t));
            if (!location.onMesh || location.island < 0)
                continue;
            if (!hasGround && location.island == largest) {
                ground = location.position;
                hasGround = true;
            } else if (!hasRoof && location.island != largest) {
                roof = location.position;
                hasRoof = true;
            }
        }
        results.add("query_locate_p95_ms", bench::percentile(locates, 0.95), "ms");
        if (hasGround && hasRoof) {
            const uint64_t searches = runtime.getSearchCount();
            std::vector<double> rejects;
            for (int b = 0; b < 20; b++) {
                const auto t = bench::Clock::now();
                for (int i = 0; i < 1000; i++)
                    runtime.isReachable(ground, roof, filter, extents);
                rejects.push_back(msSince(t));
            }
            results.add("query_reject_disconnected_us", bench::percentile(rejects, 0.5), "us");
            results.add("query_reject_searches", (double)(runtime.getSearchCount() - searches), "searches");
        } else {
            std::fprintf(stderr, "no pair of islands found for the reject\n");
        }
    }

    // ---- S75 findings: bench_nav_city with 100 agents and 20 links, stale counts cached
    {
        Scene findingsScene;
        test::disableSubSystems(findingsScene);
        findingsScene.getScheduler().setEnabled("NavigationSystem", true);
        City findingsCity = city(findingsScene);
        NavigationSystem* system = findingsScene.getSystem<NavigationSystem>().get();
        system->setKeepBakeData(true);
        system->setNavMeshData(findingsCity.surface, nav::NavMeshData(data), error);
        std::mt19937 random(23);
        std::uniform_real_distribution<float> coordinate(-110.0f, 110.0f);
        int agents = 0;
        while (agents < 100) {
            nav::NavHit hit;
            if (!system->samplePosition(Vector3(coordinate(random), 3.0f, coordinate(random)), 6.0f, hit))
                continue;
            agent(findingsScene, hit.position);
            agents++;
        }
        for (int i = 0; i < 20; i++) {
            nav::NavHit a, b;
            const Vector3 p(coordinate(random), 3.0f, coordinate(random));
            if (!system->samplePosition(p, 6.0f, a) || !system->samplePosition(p + Vector3(3.0f, 0.0f, 0.0f), 6.0f, b)) {
                i--;
                continue;
            }
            const Entity link = transformEntity(findingsScene, Vector3());
            NavMeshLinkComponent component;
            component.startPoint = a.position;
            component.endPoint = b.position;
            findingsScene.addComponent<NavMeshLinkComponent>(link, component);
        }
        resolveTransforms(findingsScene);
        // the links' tiles only
        system->bakeNow(findingsCity.surface, NavBakeMode::DIRTY);
        nav::NavFindingOptions options;
        options.dirtyTilesOf = [](Entity) { return 0; };
        size_t count = system->getFindings(options).size();
        std::vector<double> times;
        for (int i = 0; i < 20; i++) {
            const auto t = bench::Clock::now();
            count = system->getFindings(options).size();
            times.push_back(msSince(t));
        }
        results.add("findings_city_ms", bench::percentile(times, 0.5), "ms");
        results.add("findings_city_count", (double)count, "findings");
    }

    // ---- crowds: 100 and 500 agents (Good) walking across the city; the system's fixed tick
    for (int count : {100, 500}) {
        Scene crowdScene;
        test::disableSubSystems(crowdScene);
        crowdScene.getScheduler().setEnabled("NavigationSystem", true);
        nav::NavigationSettings settings = nav::NavigationSettings::defaults();
        settings.crowdMaxAgents = 512;
        nav::setSettings(settings);
        City crowdCity = city(crowdScene);
        NavigationSystem* system = crowdScene.getSystem<NavigationSystem>().get();
        system->setNavMeshData(crowdCity.surface, nav::NavMeshData(data), error);
        std::mt19937 random(9);
        std::uniform_real_distribution<float> coordinate(-110.0f, 110.0f);
        std::vector<Entity> agents;
        while ((int)agents.size() < count) {
            nav::NavHit hit;
            if (!system->samplePosition(Vector3(coordinate(random), 3.0f, coordinate(random)), 6.0f, hit))
                continue;
            const Entity entity = agent(crowdScene, hit.position);
            crowdScene.getComponent<NavMeshAgentComponent>(entity).obstacleAvoidance = nav::AvoidanceQuality::Good;
            agents.push_back(entity);
        }
        resolveTransforms(crowdScene);
        crowdScene.fixedUpdate(kTick);
        for (Entity entity : agents) {
            nav::NavHit hit;
            while (!system->samplePosition(Vector3(coordinate(random), 3.0f, coordinate(random)), 6.0f, hit)) {
            }
            NavMeshAgent(&crowdScene, entity).setDestination(hit.position);
        }
        for (int warm = 0; warm < 60; warm++)
            crowdScene.fixedUpdate(kTick);
        std::vector<double> ticks;
        for (int tick = 0; tick < 300; tick++) {
            const auto t = bench::Clock::now();
            crowdScene.fixedUpdate(kTick);
            ticks.push_back(msSince(t));
        }
        results.add("crowd_" + std::to_string(count) + "_tick_ms", bench::percentile(ticks, 0.5), "ms");
        results.add("crowd_" + std::to_string(count) + "_tick_p95_ms", bench::percentile(ticks, 0.95), "ms");
    }

    // ---- S76 agents (docs/specs/S76-navigation-gameplay-api.md R38): what the stuck watchdog adds
    // to 100 walking agents. Two copies of the crowd (the same agents and destinations) step in
    // turns, only the second watched; the median of the paired tick differences
    {
        nav::NavigationSettings settings = nav::NavigationSettings::defaults();
        settings.crowdMaxAgents = 512;
        nav::setSettings(settings);
        auto makeCrowd = [&](bool watched) {
            auto crowdScene = std::make_unique<Scene>();
            test::disableSubSystems(*crowdScene);
            crowdScene->getScheduler().setEnabled("NavigationSystem", true);
            City crowdCity = city(*crowdScene);
            NavigationSystem* system = crowdScene->getSystem<NavigationSystem>().get();
            system->setNavMeshData(crowdCity.surface, nav::NavMeshData(data), error);
            std::mt19937 random(9);
            std::uniform_real_distribution<float> coordinate(-110.0f, 110.0f);
            std::vector<Entity> agents;
            while ((int)agents.size() < 100) {
                nav::NavHit hit;
                if (!system->samplePosition(Vector3(coordinate(random), 3.0f, coordinate(random)), 6.0f, hit))
                    continue;
                const Entity entity = agent(*crowdScene, hit.position);
                NavMeshAgentComponent& component = crowdScene->getComponent<NavMeshAgentComponent>(entity);
                component.obstacleAvoidance = nav::AvoidanceQuality::Good;
                component.stuckDetection = watched;
                agents.push_back(entity);
            }
            resolveTransforms(*crowdScene);
            crowdScene->fixedUpdate(kTick);
            for (Entity entity : agents) {
                nav::NavHit hit;
                while (!system->samplePosition(Vector3(coordinate(random), 3.0f, coordinate(random)), 6.0f, hit)) {
                }
                NavMeshAgent(crowdScene.get(), entity).setDestination(hit.position);
            }
            for (int warm = 0; warm < 60; warm++)
                crowdScene->fixedUpdate(kTick);
            return crowdScene;
        };
        std::unique_ptr<Scene> plain = makeCrowd(false);
        std::unique_ptr<Scene> watched = makeCrowd(true);
        std::vector<double> extra;
        for (int tick = 0; tick < 600; tick++) {
            auto t = bench::Clock::now();
            plain->fixedUpdate(kTick);
            const double without = msSince(t);
            t = bench::Clock::now();
            watched->fixedUpdate(kTick);
            extra.push_back(msSince(t) - without);
        }
        results.add("stuck_100_extra_ms", std::max(0.0, bench::percentile(extra, 0.5)), "ms");
    }
    nav::setSettings(nav::NavigationSettings::defaults());

    // ---- carving: one obstacle moved on a carving surface; tile-cache work per tick at Carve
    // Tiles Per Tick Auto (desktop: 4) with the wall-clock ceiling off, ticks to land
    // (S19 R28, S74 R14, R19)
    {
        Scene carveScene;
        test::disableSubSystems(carveScene);
        carveScene.getScheduler().setEnabled("NavigationSystem", true);
        plane(carveScene, Vector3(), 60.0f, 60.0f);
        const Entity surface = transformEntity(carveScene, Vector3(), NULL_ENTITY, "Carving");
        addSurface(carveScene, surface, 0.2f, 64).runtimeCarving = true;
        resolveTransforms(carveScene);
        NavigationSystem* system = carveScene.getSystem<NavigationSystem>().get();
        system->bakeNow(surface, NavBakeMode::FULL);
        system->setCarveTimeCeiling(0.0);
        const Entity obstacle = transformEntity(carveScene, Vector3(-10.0f, 0.5f, 0.0f));
        NavMeshObstacleComponent component;
        component.carve = true;
        component.carveOnlyStationary = false;
        component.size = Vector3(3.0f, 1.0f, 3.0f);
        carveScene.addComponent<NavMeshObstacleComponent>(obstacle, component);
        resolveTransforms(carveScene);
        for (int i = 0; i < 10; i++)
            carveScene.fixedUpdate(kTick);
        std::vector<double> worst;
        int maxTicks = 0;
        for (int move = 0; move < 20; move++) {
            carveScene.getComponent<Transform>(obstacle).position = Vector3(-10.0f + (float)move * 1.0f, 0.5f, 0.0f);
            resolveTransforms(carveScene);
            nav::NavHit hit;
            int ticks = 0;
            double maxMs = 0.0;
            do {
                const auto t = bench::Clock::now();
                carveScene.fixedUpdate(kTick);
                maxMs = std::max(maxMs, msSince(t));
                ticks++;
            } while (system->samplePosition(Vector3(-10.0f + (float)move, 0.0f, 0.0f), 0.3f, hit) && ticks < 10);
            worst.push_back(maxMs);
            maxTicks = std::max(maxTicks, ticks);
        }
        results.add("carve_tick_max_ms", bench::percentile(worst, 1.0), "ms");
        results.add("carve_ticks_to_land", (double)maxTicks, "ticks");
    }

    // ---- grids
    {
        float costs[nav::AREA_COUNT];
        for (float& cost : costs)
            cost = 1.0f;
        nav::NavGridMap maze = serpentine(256);
        nav::GridSolver solver;
        std::vector<double> times;
        for (int i = 0; i < 30; i++) {
            const auto t = bench::Clock::now();
            const nav::GridPath path = solver.solve(maze, nav::GridCell{0, 0}, nav::GridCell{255, 255}, nav::GridQuery(), costs);
            times.push_back(msSince(t));
            if (path.status != nav::PathStatus::Complete)
                std::fprintf(stderr, "the maze path is not complete\n");
        }
        results.add("grid_256_worst_ms", bench::percentile(times, 0.5), "ms");
        // unreachable: a closed wall, rejected by the index
        nav::NavGridMap closed;
        closed.reset(256, 256);
        closed.fillRect(nav::GridCell{128, 0}, nav::GridCell{128, 255}, nav::AREA_NOT_WALKABLE);
        closed.ensureComponents();
        times.clear();
        for (int i = 0; i < 1000; i++) {
            const auto t = bench::Clock::now();
            solver.solve(closed, nav::GridCell{1, 1}, nav::GridCell{250, 250}, nav::GridQuery(), costs);
            times.push_back(msSince(t) * 1000.0);
        }
        results.add("grid_reject_us", bench::percentile(times, 0.5), "us");
        // a batch of 200 paths on 128^2 over 4 threads (3 workers and the caller)
        nav::NavGridMap field;
        field.reset(128, 128);
        std::mt19937 random(4);
        std::uniform_real_distribution<float> roll(0.0f, 1.0f);
        for (int y = 0; y < 128; y++)
            for (int x = 0; x < 128; x++)
                if (roll(random) < 0.15f)
                    field.setArea(nav::GridCell{x, y}, nav::AREA_NOT_WALKABLE);
        std::vector<nav::GridRequest> requests;
        std::uniform_int_distribution<int> cell(0, 127);
        while (requests.size() < 200) {
            nav::GridRequest request;
            request.start = nav::GridCell{cell(random), cell(random)};
            request.goal = nav::GridCell{cell(random), cell(random)};
            request.query.allowPartial = true;
            if (field.isWalkable(field.index(request.start)))
                requests.push_back(request);
        }
        nav::GridSolverPool pool4;
        std::vector<nav::GridPath> paths;
        nav::solveGridBatch(field, requests, costs, pool4, paths, 4);
        times.clear();
        for (int i = 0; i < 10; i++) {
            const auto t = bench::Clock::now();
            nav::solveGridBatch(field, requests, costs, pool4, paths, 4);
            times.push_back(msSince(t));
        }
        results.add("grid_batch_200_4t_ms", bench::percentile(times, 0.5), "ms");
        results.add("grid_batch_threads", (double)std::min<size_t>(4, jobs::system().getWorkerCount() + 1), "threads");
        times.clear();
        for (int i = 0; i < 10; i++) {
            const auto t = bench::Clock::now();
            nav::solveGridBatch(field, requests, costs, pool4, paths, 1);
            times.push_back(msSince(t));
        }
        results.add("grid_batch_200_1t_ms", bench::percentile(times, 0.5), "ms");
        // where the single-thread time goes: without smoothing, and the unreachable goals alone
        {
            std::vector<nav::GridRequest> centres = requests, unreachable;
            for (nav::GridRequest& request : centres)
                request.query.movement = nav::GridMovement::CellCentres;
            for (const nav::GridRequest& request : requests)
                if (!field.isReachable(request.start, request.goal) || !field.isWalkable(field.index(request.goal)))
                    unreachable.push_back(request);
            auto timed = [&](const std::vector<nav::GridRequest>& list) {
                std::vector<double> samples;
                for (int i = 0; i < 10; i++) {
                    const auto t = bench::Clock::now();
                    nav::solveGridBatch(field, list, costs, pool4, paths, 1);
                    samples.push_back(msSince(t));
                }
                return bench::percentile(samples, 0.5);
            };
            results.add("grid_batch_200_1t_centres_ms", timed(centres), "ms");
            nav::GridReservations none;
            none.reset(field.getCellCount());
            std::vector<nav::GridRequest> plain = requests;
            for (nav::GridRequest& request : plain)
                request.query.reservations = &none;
            results.add("grid_batch_200_1t_astar_ms", timed(plain), "ms");
            results.add("grid_batch_unreachable_count", (double)unreachable.size(), "requests");
            results.add("grid_batch_unreachable_1t_ms", timed(unreachable), "ms");
        }
        results.add("grid_bytes_per_cell", (double)field.memoryBytes() / (128.0 * 128.0), "B");
    }

    // ---- an empty scene: what NavigationSystem's fixed tick costs, and allocates
    {
        Scene empty;
        test::disableSubSystems(empty);
        NavigationSystem* system = empty.getSystem<NavigationSystem>().get();
        std::vector<double> batches;
        for (int b = 0; b < 20; b++) {
            const auto t = bench::Clock::now();
            for (int i = 0; i < 10000; i++)
                system->fixedUpdate(kTick);
            batches.push_back(msSince(t) * 1000.0 / 10000.0);
        }
        results.add("empty_tick_us", bench::percentile(batches, 0.5), "us");
        if (countAllocs && testsupport::countsAllocations()) {
            testsupport::AllocationCountScope scope;
            for (int i = 0; i < 10000; i++)
                system->fixedUpdate(kTick);
            results.add("empty_tick_allocations", (double)scope.count(), "allocations");
        }
    }

    pool.shutdown();
    single.shutdown();
    const char* path = bench::jsonPath(argc, argv, "nav.json");
    if (!results.write(path)) {
        std::fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    return 0;
}
