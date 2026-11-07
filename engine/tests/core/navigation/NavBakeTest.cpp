// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Navmesh bakes (docs/specs/S19-navigation.md R2-R6, R10, R40a; AC2-AC5, AC7, AC11): what the
// collector takes, incremental and cancelled bakes, byte-identical results for any worker count
// and tile order, the 32-bit reference limit and runtime tile updates.

#include "NavTestUtil.h"

#include "AllocationCounter.h"
#include "TestPaths.h"

#include "component/NavMeshObstacleComponent.h"
#include "htmlui/compat/Hash.h"
#include "jobs/JobSystem.h"
#include "navigation/NavBakeCollector.h"
#include "navigation/NavBakeScheduler.h"
#include "navigation/NavMesh.h"
#include "navigation/NavMeshBuilder.h"
#include "object/navigation/NavMeshAgent.h"
#include "object/navigation/NavMeshSurface.h"
#include "subsystem/RenderSystem.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "component/RigidbodyComponent.h"
#endif

#if JASPERENGINE_NAVIGATION
#include "navigation/NavDetourSupport.h"
#include "RecastAssert.h"
#endif

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::navtest;

namespace {

    class NavBakeTest : public NavSceneTest {
    protected:
        std::shared_ptr<nav::NavBakeInput> collect(Entity surface, nav::NavCollectStats* stats = nullptr) {
            resolveTransforms(*scene);
            std::string error;
            std::shared_ptr<nav::NavBakeInput> input = nav::NavBakeCollector::collect(scene.get(), surface, error, stats);
            EXPECT_TRUE(input) << error;
            return input;
        }

        // whether a world point has walkable navmesh within `reach`
        bool walkable(const Vector3& point, float reach = 0.3f) {
            nav::NavHit hit;
            return nav->samplePosition(point, reach, hit) && hit.position.distance(point) <= reach;
        }

        void runtimeUpdateTiles(bool carving);

        // `count` 3 x 3 m slabs 0.2 m thick, stacked 2.5 m apart over (5, 5)
        void slabs(int count) {
            for (int i = 0; i < count; i++)
                box(*scene, Vector3(5.0f, 0.1f + 2.5f * (float)i, 5.0f), Vector3(3.0f, 0.2f, 3.0f));
        }
    };

    // the .navmesh bytes of every tile of `input`, built by `results` (tile-index order)
    std::vector<uint8_t> assemble(const nav::NavBakeInput& input, std::vector<nav::NavTileBuildResult> results) {
        std::sort(results.begin(), results.end(), [&](const nav::NavTileBuildResult& a, const nav::NavTileBuildResult& b) {
            return input.tileIndex(a.coord.x, a.coord.y) < input.tileIndex(b.coord.x, b.coord.y);
        });
        nav::NavMeshData data;
        data.header.config = input.config;
        data.header.maxTiles = input.tileCount();
        data.header.linkAreaCosts = input.linkAreaCosts;
        for (nav::NavTileBuildResult& result : results) {
            EXPECT_TRUE(result.ok) << result.error;
            for (nav::NavTileBlob& blob : result.blobs)
                data.tiles.push_back(std::move(blob));
        }
        data.sortTiles();
        return nav::NavMeshFile::write(data);
    }

    std::string sha256(const std::vector<uint8_t>& bytes) {
        return htmlui::compat::sha256Hex(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }

    // the platform the recorded bench_nav_city poly count belongs to (bakes are bit-exact per
    // platform and compiler only, spec R40b)
    std::string platformTag() {
        std::string tag;
#if defined(__EMSCRIPTEN__)
        tag = "wasm";
#elif defined(_WIN32)
        tag = "windows";
#elif defined(__APPLE__)
        tag = "apple";
#elif defined(__ANDROID__)
        tag = "android";
#else
        tag = "linux";
#endif
#if defined(__clang__)
        tag += "-clang";
#elif defined(__GNUC__)
        tag += "-gcc";
#elif defined(_MSC_VER)
        tag += "-msvc";
#endif
#if defined(__x86_64__) || defined(_M_X64)
        tag += "-x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
        tag += "-arm64";
#endif
        return tag;
    }

}

TEST_F(NavBakeTest, NoPhysicsWorld){
    // bench_nav_city's render meshes and terrain, read without any physics world: the poly count
    // is the one recorded for this platform, which a build with both physics options off must
    // give alike (AC2)
    City fixture = city(*scene);
    const NavBakeResult result = bake(fixture.surface);
    ASSERT_GT(result.polys, 0);
    const std::string path = test::testPaths().assetDir + "/navigation/bench_nav_city_polys.txt";
    const std::string tag = platformTag();
    std::map<std::string, int> recorded;
    {
        std::ifstream in(path);
        std::string key;
        int value = 0;
        while (in >> key >> value)
            recorded[key] = value;
    }
    if (test::envFlag("JASPER_TEST_UPDATE_FIXTURES")) {
        recorded[tag] = result.polys;
        std::ofstream out(path);
        for (const auto& entry : recorded)
            out << entry.first << " " << entry.second << "\n";
        return;
    }
    ASSERT_TRUE(recorded.count(tag)) << "no poly count recorded for " << tag << " in " << path
                                     << " (JASPER_TEST_UPDATE_FIXTURES=1 records " << result.polys << ")";
    EXPECT_EQ(result.polys, recorded[tag]);
#if !defined(JASPERENGINE_PHYSICS_2D) && !defined(JASPERENGINE_PHYSICS_3D)
    SUCCEED() << "built without physics";
#endif
}


TEST_F(NavBakeTest, CollectPolicy){
    plane(*scene, Vector3(), 40.0f, 40.0f);
    const Entity statics = box(*scene, Vector3(-5.0f, 0.5f, 0.0f), Vector3(2.0f, 1.0f, 2.0f));
    // an agent and an obstacle (and what they carry) are left out
    const Entity agentEntity = agent(*scene, Vector3(5.0f, 0.0f, 0.0f));
    scene->addComponent<MeshComponent>(agentEntity);
    scene->getSystem<MeshSystem>()->createBox(scene->getComponent<MeshComponent>(agentEntity), 2.0f, 1.0f, 2.0f);
    const Entity carried = box(*scene, Vector3(0.0f, 0.5f, 3.0f), Vector3(2.0f, 1.0f, 2.0f), agentEntity);
    const Entity obstacle = box(*scene, Vector3(10.0f, 0.5f, 0.0f), Vector3(2.0f, 1.0f, 2.0f));
    scene->addComponent<NavMeshObstacleComponent>(obstacle, NavMeshObstacleComponent());
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, 0.25f, 32);

    nav::NavCollectStats stats;
    auto input = collect(surface, &stats);
    ASSERT_TRUE(input);
    // the plane and the static box only
    EXPECT_EQ(stats.meshes, 2);
    EXPECT_EQ(stats.excluded, 3);
    (void)statics;
    (void)carried;

    // a Modifier with Include brings the obstacle back
    NavMeshModifierComponent include;
    include.overrideInclusion = NavModifierInclusion::INCLUDE;
    scene->addComponent<NavMeshModifierComponent>(obstacle, include);
    collect(surface, &stats);
    EXPECT_EQ(stats.meshes, 3);
    // Exclude leaves a static mesh out
    NavMeshModifierComponent exclude;
    exclude.overrideInclusion = NavModifierInclusion::EXCLUDE;
    scene->addComponent<NavMeshModifierComponent>(statics, exclude);
    collect(surface, &stats);
    EXPECT_EQ(stats.meshes, 2);

    // Children: only the surface's subtree (a child box)
    const Entity childSurface = transformEntity(*scene, Vector3(30.0f, 0.0f, 0.0f), NULL_ENTITY, "Child Surface");
    addSurface(*scene, childSurface, 0.25f, 32).collectObjects = NavCollectObjects::CHILDREN;
    plane(*scene, Vector3(0.0f, 0.0f, 0.0f), 6.0f, 6.0f, childSurface);
    collect(childSurface, &stats);
    EXPECT_EQ(stats.meshes, 1);

    // Volume: what lies in the box (the grid is clipped to it)
    const Entity volumeSurface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Volume Surface");
    NavMeshSurfaceComponent& volume = addSurface(*scene, volumeSurface, 0.25f, 32);
    volume.collectObjects = NavCollectObjects::VOLUME;
    volume.volumeCenter = Vector3(-5.0f, 0.5f, 0.0f);
    volume.volumeSize = Vector3(6.0f, 4.0f, 6.0f);
    auto clipped = collect(volumeSurface, &stats);
    ASSERT_TRUE(clipped);
    EXPECT_LE(clipped->bmax[0] - clipped->bmin[0], 6.0f + 1e-3f);
    EXPECT_LE(clipped->tileCount(), 4);

#ifdef JASPERENGINE_PHYSICS_3D
    // PhysicsColliders: a dynamic body is out, a static one in, and the terrain stays collected
    const Entity dynamicBox = transformEntity(*scene, Vector3(0.0f, 0.5f, -8.0f));
    RigidbodyComponent dynamicBody;
    dynamicBody.type = BodyType::DYNAMIC;
    dynamicBody.colliders.resize(1);
    dynamicBody.colliders[0].type = Collider3DType::BOX;
    scene->addComponent<RigidbodyComponent>(dynamicBox, dynamicBody);
    const Entity staticBox = transformEntity(*scene, Vector3(0.0f, 0.5f, 8.0f));
    RigidbodyComponent staticBody;
    staticBody.type = BodyType::STATIC;
    staticBody.colliders.resize(1);
    staticBody.colliders[0].type = Collider3DType::BOX;
    scene->addComponent<RigidbodyComponent>(staticBox, staticBody);
    const Heightmap heights = hills(33, 3);
    terrain(*scene, Vector3(0.0f, -1.0f, 0.0f), 30.0f, 0.5f, heights);
    scene->getComponent<NavMeshSurfaceComponent>(surface).useGeometry = NavUseGeometry::PHYSICS_COLLIDERS;
    collect(surface, &stats);
    EXPECT_EQ(stats.meshes, 0);
    EXPECT_EQ(stats.colliders, 1);
    EXPECT_EQ(stats.terrains, 1);
#endif
}

TEST_F(NavBakeTest, IncrementalDirtyTiles){
    const Entity surface = planeSurface(40.0f, 40.0f, 0.25f);
    const Entity moved = box(*scene, Vector3(-6.0f, 1.0f, -6.0f), Vector3(2.0f, 2.0f, 2.0f));
    box(*scene, Vector3(6.0f, 1.0f, 6.0f), Vector3(2.0f, 2.0f, 2.0f));
    const NavBakeResult full = bake(surface);
    ASSERT_GT(full.tilesBuilt, 4);

    // nothing changed: nothing rebuilt
    const NavBakeResult untouched = bake(surface, NavBakeMode::DIRTY);
    EXPECT_EQ(untouched.tilesBuilt, 0);
    EXPECT_EQ(nav->countDirtyTiles(surface), 0);

    // a 2 m box moved by 1 m: at most 4 tiles (its old and new tiles with their borders)
    scene->getComponent<Transform>(moved).position = Vector3(-5.0f, 1.0f, -6.0f);
    resolveTransforms(*scene);
    const int dirty = nav->countDirtyTiles(surface);
    EXPECT_GT(dirty, 0);
    EXPECT_LE(dirty, 4);
    const NavBakeResult incremental = bake(surface, NavBakeMode::DIRTY);
    EXPECT_GT(incremental.tilesBuilt, 0);
    EXPECT_LE(incremental.tilesBuilt, 4);
    EXPECT_EQ(nav->countDirtyTiles(surface), 0);

    // the same result as a full bake of the moved scene
    const std::vector<uint8_t> afterIncremental = nav->getNavMeshFile(surface);
    bake(surface, NavBakeMode::FULL);
    EXPECT_EQ(sha256(afterIncremental), sha256(nav->getNavMeshFile(surface)));

    // a cancelled bake keeps the previous tiles, hashes and query results
    const std::vector<uint8_t> before = nav->getNavMeshFile(surface);
    const nav::NavPath pathBefore = nav->calculatePath(Vector3(-15.0f, 0.0f, -15.0f), Vector3(15.0f, 0.0f, 15.0f));
    scene->getComponent<Transform>(moved).position = Vector3(5.0f, 1.0f, -6.0f);
    resolveTransforms(*scene);
    std::string error;
    ASSERT_TRUE(nav->bake(surface, NavBakeMode::FULL, error)) << error;
    EXPECT_TRUE(nav->cancelBake(surface));
    while (nav->isBaking(surface))
        nav->processBakes(1000.0);
    ASSERT_NE(nav->getLastBakeResult(surface), nullptr);
    EXPECT_TRUE(nav->getLastBakeResult(surface)->cancelled);
    EXPECT_EQ(sha256(before), sha256(nav->getNavMeshFile(surface)));
    const nav::NavPath pathAfter = nav->calculatePath(Vector3(-15.0f, 0.0f, -15.0f), Vector3(15.0f, 0.0f, 15.0f));
    ASSERT_EQ(pathAfter.corners.size(), pathBefore.corners.size());
    for (size_t i = 0; i < pathAfter.corners.size(); i++)
        EXPECT_EQ(pathAfter.corners[i], pathBefore.corners[i]);
}

TEST_F(NavBakeTest, ByteIdenticalAcrossThreads){
    City fixture = city(*scene, 64.0f, 12, 5);
    auto input = collect(fixture.surface);
    ASSERT_TRUE(input);
    ASSERT_GE(input->nonEmptyTiles.size(), 9u);

    std::vector<std::string> hashes;
    // 1, 2 and 8 workers
    for (int workers : {1, 2, 8}) {
        jobs::Config config;
        config.workers = workers;
        jobs::JobSystem system(config);
        nav::NavBakeOptions options;
        options.mode = nav::NavBakeOptions::Mode::Jobs;
        options.system = &system;
        nav::NavBake bake(input, input->nonEmptyTiles, options);
        bake.wait();
        ASSERT_TRUE(bake.isDone());
        hashes.push_back(sha256(assemble(*input, bake.getResults())));
        system.shutdown();
    }
    // tiles built in a shuffled order, one by one
    std::vector<uint32_t> order = input->nonEmptyTiles;
    std::mt19937 random(7);
    std::shuffle(order.begin(), order.end(), random);
    std::vector<nav::NavTileBuildResult> shuffled;
    for (uint32_t index : order)
        shuffled.push_back(nav::NavMeshBuilder::buildTile(*input, index));
    hashes.push_back(sha256(assemble(*input, shuffled)));
    // time-sliced on this thread (the single-threaded web bake, spec R23)
    nav::NavBakeOptions sliced;
    sliced.mode = nav::NavBakeOptions::Mode::TimeSliced;
    nav::NavBake slicedBake(input, input->nonEmptyTiles, sliced);
    while (!slicedBake.isDone())
        slicedBake.pump(0.5);
    hashes.push_back(sha256(assemble(*input, slicedBake.getResults())));

    for (size_t i = 1; i < hashes.size(); i++)
        EXPECT_EQ(hashes[i], hashes[0]) << "run " << i;
}

TEST_F(NavBakeTest, TooLargeRejected){
    // 0.05 m cells over 2 km: 1,600 x 1,600 / 64^2 = 1,563 tiles... the limit is the reference's bits
    plane(*scene, Vector3(), 2000.0f, 2000.0f);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Huge");
    addSurface(*scene, surface, 0.05f, 16);
    resolveTransforms(*scene);
    std::string error;
    const std::shared_ptr<nav::NavBakeInput> input = nav::NavBakeCollector::collect(scene.get(), surface, error);
    EXPECT_FALSE(input);
    // 2000 / (16 * 0.05) = 2500 tiles a side: 6,250,000 tiles of 128 polygons (16 x 16 / 2)
    EXPECT_EQ(error, "Surface too large: 6250000 tiles \xC3\x97 128 polys; increase cell/tile size or split the surface");
    EXPECT_FALSE(nav->bake(surface, NavBakeMode::FULL, error));
    EXPECT_FALSE(nav->isBaking(surface));
}

// an agent keeps a valid corridor through every tick of a runtime updateTiles (S19 AC11); on a
// carving surface the tiles build within the carving budget, the agent on the old ones till then
// (S74 AC19)
void NavBakeTest::runtimeUpdateTiles(bool carving){
    const Entity surface = planeSurface(40.0f, 40.0f, 0.25f);
    scene->getComponent<NavMeshSurfaceComponent>(surface).runtimeCarving = carving;
    const Entity crate = box(*scene, Vector3(5.0f, 1.0f, 5.0f), Vector3(2.0f, 2.0f, 2.0f));
    bake(surface);
    const Entity walker = agent(*scene, Vector3(-15.0f, 0.0f, 0.0f));
    step();
    NavMeshAgent handle(scene.get(), walker);
    ASSERT_TRUE(handle.setDestination(Vector3(15.0f, 0.0f, 0.0f)));
    steps(10);
    ASSERT_TRUE(handle.isOnNavMesh());
    // beside the walker's line, not on it: the tiles land after a wall-clock bake, and an agent
    // the crate lands on stands in a hole, off the navmesh
    scene->getComponent<Transform>(crate).position = Vector3(-5.0f, 1.0f, 4.0f);
    resolveTransforms(*scene);
    AABB box(Vector3(-8.0f, -1.0f, -3.0f), Vector3(8.0f, 3.0f, 8.0f));
    std::string error;
    ASSERT_TRUE(nav->updateTiles(surface, box, error)) << error;
    int ticks = 0;
    Vector3 previous = position(walker);
    while (nav->isBaking(surface) && ticks < 600) {
        step();
        ticks++;
        EXPECT_TRUE(handle.isOnNavMesh()) << "tick " << ticks;
        EXPECT_FALSE(handle.isPathPending() && handle.getPathStatus() == nav::PathStatus::Invalid) << "tick " << ticks;
        // it keeps moving
        EXPECT_GT(position(walker).distance(previous), 0.0f) << "tick " << ticks;
        previous = position(walker);
        if (carving)
            EXPECT_LE(nav->getCarveTileBuilds(), nav::getSettings().effectiveCarveTilesPerTick()) << "tick " << ticks;
    }
    ASSERT_FALSE(nav->isBaking(surface));
    ASSERT_NE(nav->getLastBakeResult(surface), nullptr);
    EXPECT_TRUE(nav->getLastBakeResult(surface)->ok);
    EXPECT_GT(nav->getLastBakeResult(surface)->tilesBuilt, 0);
    // the crate's new place has no walkable polygon now
    EXPECT_FALSE(walkable(Vector3(-5.0f, 0.0f, 4.0f), 0.2f));
    steps(30);
    EXPECT_TRUE(handle.isOnNavMesh());
}

TEST_F(NavBakeTest, RuntimeUpdateTiles){
    runtimeUpdateTiles(false);
}

TEST_F(NavBakeTest, RuntimeUpdateTilesCarving){
    runtimeUpdateTiles(true);
}

TEST_F(NavBakeTest, HeightmapReleasedIsReadAgain){
    // a terrain whose CPU heightmap is gone: the collector reads its file again (spec risk table)
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, 0.5f, 32);
    const Entity entity = transformEntity(*scene, Vector3());
    TerrainComponent component;
    component.terrainSize = 40.0f;
    component.maxHeight = 4.0f;
    // a 33 x 33 grey PNG: a hill in the middle (fixtures/navigation/hill.png)
    component.heightMap.setPath("navigation/hill.png");
    scene->addComponent<TerrainComponent>(entity, component);
    ASSERT_FALSE(scene->getComponent<TerrainComponent>(entity).heightMap.hasData());
    nav::NavCollectStats stats;
    auto input = collect(surface, &stats);
    ASSERT_TRUE(input);
    ASSERT_EQ(input->terrains.size(), 1u);
    EXPECT_TRUE(stats.warnings.empty());
    // the hill: the centre of the map is higher than its corner
    const nav::NavTerrainInput& lattice = input->terrains[0];
    const float corner = lattice.heightAt(0, 0);
    const float middle = lattice.heightAt(lattice.samples / 2, lattice.samples / 2);
    EXPECT_GT(middle, corner + 1.0f);
}

// ---------------------------------------------------------------- S74 (docs/specs/S74-navigation-fixes.md)

#if JASPERENGINE_NAVIGATION
// The tile builds' rcContext keeps Recast's warnings and errors, deterministic (pointers as "?"),
// without the allocation notices, and costs nothing while Recast logs nothing (AC7, R4)
TEST_F(NavBakeTest, RecastLogContext){
    nav::detail::NavRecastLog log;
    if (testsupport::countsAllocations()) {
        testsupport::ThreadAllocationCount allocations;
        log.resetLog();
        log.startTimer(RC_TIMER_TOTAL);
        log.stopTimer(RC_TIMER_TOTAL);
        log.log(RC_LOG_PROGRESS, "a progress line %d", 1);
        EXPECT_EQ(allocations.count(), 0u);
    }
    EXPECT_TRUE(log.getMessages().empty());
    EXPECT_EQ(log.getAccumulatedTime(RC_TIMER_TOTAL), -1);
    int a = 0, b = 0;
    log.log(RC_LOG_WARNING, "mergeHoles: Failed to merge contours %p and %p.", (void*)&a, (void*)&b);
    log.log(RC_LOG_WARNING, "rcBuildContours: Expanding max contours from %d to %d.", 8, 16);
    log.log(RC_LOG_ERROR, "rcBuildRegions: %d overlapping regions.", 2);
    ASSERT_EQ(log.getMessages().size(), 2u);
    EXPECT_EQ(log.getMessages()[0], "mergeHoles: Failed to merge contours ? and ?.");
    EXPECT_EQ(log.getMessages()[1], "rcBuildRegions: 2 overlapping regions.");
    EXPECT_EQ(log.getLastError(), "rcBuildRegions: 2 overlapping regions.");
    EXPECT_EQ(log.getErrorCount(), 1);
}
#endif

// 70 stacked slabs: a column of more spans than Recast connects. A static bake succeeds and
// reports Recast's warning. Over a ground plate with 10 more slabs beside the stack, the plate
// overlaps more walkable platforms than a tile-cache layer set holds: a carving bake fails with
// Recast's own reason (AC7, R4)
TEST_F(NavBakeTest, RecastWarningsReported){
    slabs(70);
    box(*scene, Vector3(6.0f, -2.4f, 6.0f), Vector3(14.0f, 0.2f, 14.0f));
    for (int i = 0; i < 10; i++)
        box(*scene, Vector3(10.0f, 0.1f + 2.5f * (float)i, 10.0f), Vector3(3.0f, 0.2f, 3.0f));
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Stack");
    addSurface(*scene, surface, 0.1667f, 64);
    resolveTransforms(*scene);
    const NavBakeResult result = nav->bakeNow(surface, NavBakeMode::FULL);
    ASSERT_TRUE(result.ok) << result.error;
    const auto found = std::find_if(result.warnings.begin(), result.warnings.end(),
                                    [](const std::string& w) { return w.find("Heightfield has too many layers") != std::string::npos; });
    ASSERT_NE(found, result.warnings.end()) << result.warnings.size() << " warnings";
    EXPECT_EQ(found->rfind("tile (", 0), 0u) << *found;

    scene->getComponent<NavMeshSurfaceComponent>(surface).runtimeCarving = true;
    const NavBakeResult carving = nav->bakeNow(surface, NavBakeMode::FULL);
    EXPECT_FALSE(carving.ok);
    EXPECT_NE(carving.error.find("Recast could not build the tile-cache layers"), std::string::npos) << carving.error;
    EXPECT_NE(carving.error.find("layer overflow"), std::string::npos) << carving.error;
}

// geometry 700 m tall at the default voxel height (0.083 m): spans clamp at 8191 voxels, a warning
// says so, and the bake goes on (AC8, R5)
TEST_F(NavBakeTest, TallSurfaceWarns){
    plane(*scene, Vector3(), 20.0f, 20.0f);
    box(*scene, Vector3(5.0f, 350.0f, 5.0f), Vector3(1.0f, 700.0f, 1.0f));
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Tower");
    addSurface(*scene, surface, 0.1667f, 64);
    resolveTransforms(*scene);
    const NavBakeResult result = nav->bakeNow(surface, NavBakeMode::FULL);
    ASSERT_TRUE(result.ok) << result.error;
    const auto found = std::find_if(result.warnings.begin(), result.warnings.end(),
                                    [](const std::string& w) { return w.find("more than the 8191 voxels") != std::string::npos; });
    ASSERT_NE(found, result.warnings.end());
    EXPECT_NE(found->find("increase the voxel size or split the surface"), std::string::npos) << *found;
    EXPECT_NE(found->find("geometry above"), std::string::npos) << *found;
}

// 40 stacked slabs on a carving surface: 40 layers, the lowest 32 kept, with a warning (AC8, R5)
TEST_F(NavBakeTest, LayerCapWarns){
    slabs(40);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Stack");
    addSurface(*scene, surface, 0.1667f, 64).runtimeCarving = true;
    resolveTransforms(*scene);
    const NavBakeResult result = nav->bakeNow(surface, NavBakeMode::FULL);
    ASSERT_TRUE(result.ok) << result.error;
    const auto found = std::find_if(result.warnings.begin(), result.warnings.end(),
                                    [](const std::string& w) { return w.find("40 walkable layers, the lowest 32 kept") != std::string::npos; });
    ASSERT_NE(found, result.warnings.end()) << result.warnings.size() << " warnings";
    EXPECT_EQ(found->rfind("tile (", 0), 0u) << *found;
    // the lowest slab is walkable, the top one is not
    EXPECT_TRUE(walkable(Vector3(5.0f, 0.2f, 5.0f)));
    EXPECT_FALSE(walkable(Vector3(5.0f, 0.2f + 2.5f * 39.0f, 5.0f)));
}

#if JASPERENGINE_NAVIGATION && !defined(NDEBUG)
namespace {

    int recastAsserts = 0;
    std::string firstRecastAssert;

    void recordRecastAssert(const char* expression, const char* file, int line) {
        if (recastAsserts++ == 0)
            firstRecastAssert = std::string(expression) + " (" + file + ":" + std::to_string(line) + ")";
    }

}
#endif

// Slopes 100 km from the origin, where float precision degrades the detail mesh: the bake succeeds
// and no Recast assert fires (upstream 9f4ce64, an empty triangle list indexed; AC3, R1)
TEST_F(NavBakeTest, DetailSamplingDegenerate){
#if JASPERENGINE_NAVIGATION && !defined(NDEBUG)
    recastAsserts = 0;
    firstRecastAssert.clear();
    rcAssertFailFunc* previous = rcAssertFailGetCustom();
    rcAssertFailSetCustom(&recordRecastAssert);
#endif
    const Vector3 far(100000.0f, 0.0f, 100000.0f);
    for (int i = 0; i < 6; i++) {
        const Entity ramp = plane(*scene, far + Vector3(-15.0f + 6.0f * (float)i, 1.0f + 0.37f * (float)i, 0.0f), 5.0f, 30.0f);
        Transform& transform = scene->getComponent<Transform>(ramp);
        transform.rotation = Quaternion(10.0f + 6.0f * (float)i, Vector3(0.0f, 0.0f, 1.0f)) * Quaternion(3.0f * (float)i, Vector3(1.0f, 0.0f, 0.0f));
    }
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Far");
    addSurface(*scene, surface, 0.1f, 64);
    resolveTransforms(*scene);
    const NavBakeResult result = nav->bakeNow(surface, NavBakeMode::FULL);
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_GT(result.polys, 0);
#if JASPERENGINE_NAVIGATION && !defined(NDEBUG)
    rcAssertFailSetCustom(previous);
    EXPECT_EQ(recastAsserts, 0) << firstRecastAssert;
#endif
}

// In play, a full bakeAsync of a 64-tile carving surface: the old navmesh stays in service while
// the new one's tiles build within the carving budget; the swap fires onBakeCompleted (AC19, R15)
TEST_F(NavBakeTest, RuntimeCarvingBakeDeferred){
    // 25.6 m on 3.2 m tiles: 8 x 8
    plane(*scene, Vector3(), 25.6f, 25.6f);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, 0.2f, 16).runtimeCarving = true;
    bake(surface);
    nav->setCarveTimeCeiling(0.0);
    const nav::NavMesh* before = nav->getNavMesh(surface);
    ASSERT_NE(before, nullptr);
    ASSERT_EQ(before->getTileCount(), 64);
    const Entity walker = agent(*scene, Vector3(-11.0f, 0.0f, -11.0f));
    step();
    NavMeshAgent handle(scene.get(), walker);
    ASSERT_TRUE(handle.setDestination(Vector3(11.0f, 0.0f, 11.0f)));
    steps(5);
    int completedTick = -1;
    const nav::NavMesh* completedWith = nullptr;
    int tick = 0;
    scene->getComponent<NavMeshSurfaceComponent>(surface).onBakeCompleted =
        std::function<void(NavMeshSurface)>([&](NavMeshSurface) {
            completedTick = tick;
            completedWith = nav->getNavMesh(surface);
        });
    std::string error;
    ASSERT_TRUE(nav->bake(surface, NavBakeMode::FULL, error)) << error;
    const int budget = nav::getSettings().effectiveCarveTilesPerTick();
    int stagedTicks = 0;
    int swapTick = -1;
    const auto since = std::chrono::steady_clock::now();
    for (tick = 0; bakeRunning(surface, since); tick++) {
        step();
        const NavSurfaceStatus status = nav->getSurfaceStatus(surface);
        if (status.queuedBuilds > 0) {
            // landed, not swapped: the old navmesh serves
            stagedTicks++;
            EXPECT_EQ(nav->getNavMesh(surface), before) << "tick " << tick;
        }
        if (swapTick < 0 && nav->getNavMesh(surface) != before)
            swapTick = tick;
        EXPECT_LE(nav->getCarveTileBuilds(), budget) << "tick " << tick;
        EXPECT_TRUE(handle.isOnNavMesh()) << "tick " << tick;
        EXPECT_FALSE(handle.isPathPending() && handle.getPathStatus() == nav::PathStatus::Invalid) << "tick " << tick;
    }
    ASSERT_FALSE(nav->isBaking(surface));
    ASSERT_TRUE(nav->getLastBakeResult(surface) && nav->getLastBakeResult(surface)->ok);
    // 64 tile builds at `budget` a tick
    EXPECT_GE(stagedTicks, 64 / budget - 1);
    ASSERT_GE(swapTick, 0);
    EXPECT_EQ(completedTick, swapTick);
    EXPECT_EQ(completedWith, nav->getNavMesh(surface));
    EXPECT_EQ(nav->getNavMesh(surface)->getTileCount(), 64);
    steps(10);
    EXPECT_TRUE(handle.isOnNavMesh());
}

// S75 R12 (AC9): the probe's sources ride along with the input but never into its tile hashes
TEST_F(NavBakeTest, SourcesDoNotChangeHashes){
    const Entity floor = plane(*scene, Vector3(), 40.0f, 40.0f);
    const Entity statics = box(*scene, Vector3(-5.0f, 0.5f, 0.0f), Vector3(2.0f, 1.0f, 2.0f));
    const Entity obstacle = box(*scene, Vector3(10.0f, 0.5f, 0.0f), Vector3(2.0f, 1.0f, 2.0f));
    scene->addComponent<NavMeshObstacleComponent>(obstacle, NavMeshObstacleComponent());
    const Entity hidden = box(*scene, Vector3(0.0f, 0.5f, 10.0f), Vector3(2.0f, 1.0f, 2.0f));
    scene->setEntityLayer(hidden, 5);
    areaVolume(Vector3(0.0f, 0.0f, -10.0f), Vector3(4.0f, 4.0f, 4.0f), nav::AREA_NOT_WALKABLE);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, 0.25f, 32).includeLayers = ~(1u << 5);
    resolveTransforms(*scene);

    std::string error;
    nav::NavCollectStats plain, recorded;
    const auto without = nav::NavBakeCollector::collect(scene.get(), surface, error, &plain);
    nav::NavCollectOptions options;
    options.recordSources = true;
    const auto with = nav::NavBakeCollector::collect(scene.get(), surface, error, &recorded, options);
    ASSERT_TRUE(without);
    ASSERT_TRUE(with);
    EXPECT_EQ(without->tileHashes, with->tileHashes);
    EXPECT_EQ(without->triangleAreas, with->triangleAreas);
    // without the option nothing is kept
    EXPECT_TRUE(without->triangleSources.empty());
    EXPECT_TRUE(without->volumeSources.empty());
    EXPECT_TRUE(plain.excludedEntities.empty());
    // with it: one GameObject per triangle and volume, the exclusions with their reason
    ASSERT_EQ(with->triangleSources.size(), with->triangleAreas.size());
    ASSERT_EQ(with->volumeSources.size(), with->volumes.size());
    EXPECT_EQ(with->terrainSources.size(), with->terrains.size());
    EXPECT_NE(std::find(with->triangleSources.begin(), with->triangleSources.end(), floor), with->triangleSources.end());
    EXPECT_NE(std::find(with->triangleSources.begin(), with->triangleSources.end(), statics), with->triangleSources.end());
    EXPECT_EQ(std::find(with->triangleSources.begin(), with->triangleSources.end(), obstacle), with->triangleSources.end());
    std::map<Entity, std::string> why(recorded.excludedEntities.begin(), recorded.excludedEntities.end());
    EXPECT_EQ(why[obstacle], "obstacle");
    EXPECT_EQ(why[hidden], "layer");
    EXPECT_EQ(with->excludedEntities, recorded.excludedEntities);
    // the counts are the same either way (the layer is not a policy exclusion)
    EXPECT_EQ(plain.excluded, recorded.excluded);
    EXPECT_EQ(plain.excluded, 1);
}
