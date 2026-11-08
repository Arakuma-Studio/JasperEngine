// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Navmesh bakes (docs/specs/S19-navigation.md R2-R6, R10, R40a; AC2-AC5, AC7, AC11): what the
// collector takes, incremental and cancelled bakes, byte-identical results for any worker count
// and tile order, the 32-bit reference limit and runtime tile updates.

#include "NavTestUtil.h"

#include "TestPaths.h"

#include "component/NavMeshObstacleComponent.h"
#include "htmlui/compat/Hash.h"
#include "jobs/JobSystem.h"
#include "navigation/NavBakeCollector.h"
#include "navigation/NavBakeScheduler.h"
#include "navigation/NavMesh.h"
#include "navigation/NavMeshBuilder.h"
#include "object/navigation/NavMeshAgent.h"
#include "subsystem/RenderSystem.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "component/RigidbodyComponent.h"
#endif

#include <algorithm>
#include <cstdlib>
#include <fstream>
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
    navtest::terrain(*scene, Vector3(0.0f, -1.0f, 0.0f), 30.0f, 0.5f, heights);
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

TEST_F(NavBakeTest, RuntimeUpdateTiles){
    // an agent keeps a valid corridor through every tick of a runtime updateTiles (AC11)
    const Entity surface = planeSurface(40.0f, 40.0f, 0.25f);
    const Entity crate = box(*scene, Vector3(5.0f, 1.0f, 5.0f), Vector3(2.0f, 2.0f, 2.0f));
    bake(surface);
    const Entity walker = agent(*scene, Vector3(-15.0f, 0.0f, 0.0f));
    step();
    NavMeshAgent handle(scene.get(), walker);
    ASSERT_TRUE(handle.setDestination(Vector3(15.0f, 0.0f, 0.0f)));
    steps(10);
    ASSERT_TRUE(handle.isOnNavMesh());
    scene->getComponent<Transform>(crate).position = Vector3(-5.0f, 1.0f, 0.0f);
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
    }
    ASSERT_FALSE(nav->isBaking(surface));
    ASSERT_NE(nav->getLastBakeResult(surface), nullptr);
    EXPECT_TRUE(nav->getLastBakeResult(surface)->ok);
    EXPECT_GT(nav->getLastBakeResult(surface)->tilesBuilt, 0);
    // the crate's new place has no walkable polygon now
    EXPECT_FALSE(walkable(Vector3(-5.0f, 0.0f, 0.0f), 0.2f));
    steps(30);
    EXPECT_TRUE(handle.isOnNavMesh());
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
