// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T4 R25, AC37: navigation on Terrain Worlds. A surface collects a World's tiles, one lattice each,
// from the tiles' own samples (demanded from the streamer, which loads them on its jobs, while the
// collection waits), cuts each tile's holes (T3 R23) and welds the seams; a World of more than 64
// tiles needs a Volume or Follow; Follow Terrain World Focus rebakes round the focus through
// updateTiles while agents keep walking.

#if JASPERENGINE_TERRAIN_WORLD

#include "NavTestUtil.h"
#include "../terrain/TerrainWorldTestSupport.h"

#include "component/MeshComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "component/TerrainComponent.h"
#include "component/TerrainWorldComponent.h"
#include "component/Transform.h"
#include "jobs/JobSystem.h"
#include "navigation/NavBakeCollector.h"
#include "navigation/NavTerrainWorld.h"
#include "object/navigation/NavMeshAgent.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "subsystem/TerrainWorldSystem.h"
#include "terrain/TerrainHoles.h"
#include "terrain/world/TerrainWorldData.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;

// (a named namespace, tests included: a unity build merges the test sources)
namespace terrain_world_nav_test {

    namespace tw = jasperengine::terraincore;

    // 64 m tiles of 64 quads at 1 m
    constexpr int QUADS = 64;
    constexpr float SPACING = 1.0f;
    constexpr float SIZE = QUADS * SPACING;
    constexpr float MAX_HEIGHT = 20.0f;

    void writeBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    // tiles [0, side) a side, cooked as an export writes them, with the fixture's heights; edit
    // changes a tile before it is written
    void writeWorld(const std::filesystem::path& folder, int side, const std::function<void(tw::WorldTile&)>& edit) {
        tw::WorldManifest manifest;
        manifest.guid = "25252525252525252525252525252525";
        manifest.lattice = terrainworldtest::smallLattice(QUADS, SPACING);
        manifest.maps.maxHeight = MAX_HEIGHT;
        tw::WorldOverview overview;
        tw::initOverview(overview, manifest.lattice, tw::TileCoord(0, 0), tw::TileCoord(side - 1, side - 1), 0);
        for (int z = 0; z < side; z++) {
            for (int x = 0; x < side; x++) {
                std::shared_ptr<tw::WorldTile> tile = terrainworldtest::fixtureTile(manifest.lattice, tw::TileCoord(x, z));
                if (edit) edit(*tile);
                const float colours[] = {0.5f, 0.5f, 0.5f, 1.0f};
                tw::buildOverviewColour(*tile, manifest.lattice, colours, 0);
                std::vector<uint8_t> bytes;
                ASSERT_TRUE(tw::encodeTile(*tile, terrainworldtest::zstdCodec(), tw::TileEncodeOptions{}, bytes));
                writeBytes(folder / ("tiles/" + std::to_string(x) + "_" + std::to_string(z) + ".jtile"), bytes);
                tw::setOverviewPatch(overview, *tile);
                const tw::TileMinMax whole = tw::tileNodeMinMax(*tile, manifest.lattice, manifest.lattice.tileLevel(), 0, 0);
                manifest.tiles.push_back(tw::WorldTileInfo{tw::TileCoord(x, z), whole.min, whole.max});
            }
        }
        std::vector<uint8_t> bytes;
        ASSERT_TRUE(tw::encodeOverview(overview, manifest.lattice, terrainworldtest::zstdCodec(), 3, bytes, nullptr));
        writeBytes(folder / "overview.jtile", bytes);
        tw::encodeWorldBin(manifest, bytes);
        writeBytes(folder / "world.bin", bytes);
    }

    class TerrainWorldNav : public navtest::NavSceneTest {
    protected:
        test::TempDir files;
        Entity world = NULL_ENTITY;

        void addWorld(int side, const std::function<void(tw::WorldTile&)>& edit = nullptr) {
            writeWorld(files.path() / "World.terrainworld", side, edit);
            world = scene->createEntity();
            scene->addComponent<Transform>(world, {});
            scene->addComponent<MeshComponent>(world, {});
            scene->addComponent<TerrainComponent>(world, {});
            TerrainWorldComponent component;
            component.worldData.path = (files.path() / "World.terrainworld").generic_string();
            component.useCameraFocus = false;
            scene->addComponent<TerrainWorldComponent>(world, component);
        }

        TerrainWorldRuntime* runtime() {
            std::string error;
            TerrainWorldRuntime* made = scene->getSystem<TerrainWorldSystem>()->ensureRuntime(world, &error);
            EXPECT_NE(made, nullptr) << error;
            return made;
        }

        // the world's pump, with the jobs' pump when they have no worker
        void pumpWorld() {
            if (jobs::hasSystem() && jobs::system().isSingleThreaded()) jobs::system().pump(4.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            scene->getSystem<TerrainWorldSystem>()->pump(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        template<typename F>
        bool pumpUntil(F&& condition, double seconds = 30.0) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (std::chrono::steady_clock::now() < until) {
                pumpWorld();
                if (condition()) return true;
            }
            return false;
        }

        // the World's ground at a world point (the World sits at the origin)
        Vector3 ground(float x, float z) {
            return Vector3(x, runtime()->query(x, z, true).height, z);
        }

        bool walkable(const Vector3& point, float reach = 1.0f) {
            nav::NavHit hit;
            return nav->samplePosition(point, reach, hit) && hit.position.distance(point) <= reach;
        }

        Entity surfaceEntity() {
            const Entity surface = navtest::transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
            navtest::addSurface(*scene, surface, 0.5f, 32);
            return surface;
        }

        // the lattice of a tile, by its centre
        const nav::NavTerrainInput* latticeOf(const nav::NavBakeInput& input, int x, int z) {
            for (const nav::NavTerrainInput& lattice : input.terrains) {
                if (lattice.centerX == ((float)x + 0.5f) * SIZE && lattice.centerZ == ((float)z + 0.5f) * SIZE) return &lattice;
            }
            return nullptr;
        }
    };

}

using namespace terrain_world_nav_test;

TEST_F(TerrainWorldNav, CollectsTiles) {
    // a 2 x 2 world; tile (1, 1) has a 10 m square of holes at world x, z in [84, 94)
    addWorld(2, [](tw::WorldTile& tile) {
        if (tile.coord.x != 1 || tile.coord.z != 1) return;
        tile.holes.allocate(QUADS, 1, 8, tw::SOLID_VALUE);
        for (int y = 20; y < 30; y++) {
            for (int x = 20; x < 30; x++) tile.holes.texel(x, y)[0] = tw::HOLE_VALUE;
        }
    });
    const Entity surface = surfaceEntity();
    TerrainWorldRuntime* worldRuntime = runtime();
    ASSERT_TRUE(worldRuntime);
    ASSERT_TRUE(pumpUntil([&] { return worldRuntime->isReady(); }));
    navtest::resolveTransforms(*scene);

    // no tile is in memory (no focus): the collection demands all four and waits, reading nothing itself
    std::string error;
    nav::NavCollectStats stats;
    nav::NavWorldContext context;
    EXPECT_FALSE(nav::NavBakeCollector::collect(scene.get(), surface, error, &stats, {}, nullptr, &context));
    EXPECT_EQ(stats.pendingTiles, 4) << error;
    EXPECT_EQ(worldRuntime->resident(tw::TileCoord(0, 0)), nullptr);
    std::shared_ptr<nav::NavBakeInput> input;
    // with the editor probe's sources recorded (S75): each tile's lattice is the World's
    nav::NavCollectOptions options;
    options.recordSources = true;
    ASSERT_TRUE(pumpUntil([&] {
        input = nav::NavBakeCollector::collect(scene.get(), surface, error, &stats, options, nullptr, &context);
        return input != nullptr || stats.pendingTiles == 0;
    })) << error;
    ASSERT_TRUE(input) << error;
    EXPECT_TRUE(context.held.empty());
    ASSERT_EQ(input->terrains.size(), 4u);
    EXPECT_EQ(stats.terrains, 4);
    EXPECT_EQ(input->terrainSources, std::vector<Entity>(4, world));

    // one lattice a tile at its sample spacing, its heights the world query's
    for (const nav::NavTerrainInput& lattice : input->terrains) {
        EXPECT_EQ(lattice.samples, QUADS + 1);
        EXPECT_FLOAT_EQ(lattice.spacing, SPACING);
        EXPECT_TRUE(lattice.tileData);
        for (int j = 0; j < lattice.samples; j += 7) {
            for (int i = 0; i < lattice.samples; i += 5) {
                const Vector3 local = lattice.localAt(i, j);
                EXPECT_NEAR(local.y, worldRuntime->query(local.x, local.z, true).height, 1e-4f) << local.x << ", " << local.z;
            }
        }
    }
    // the triangles cover the tiles
    EXPECT_FLOAT_EQ(input->bmin[0], 0.0f);
    EXPECT_FLOAT_EQ(input->bmin[2], 0.0f);
    EXPECT_FLOAT_EQ(input->bmax[0], 2.0f * SIZE);
    EXPECT_FLOAT_EQ(input->bmax[2], 2.0f * SIZE);

    // the seams weld: a tile's border samples are its neighbour's, bit for bit, in nav space
    const nav::NavTerrainInput* a = latticeOf(*input, 0, 0);
    const nav::NavTerrainInput* east = latticeOf(*input, 1, 0);
    const nav::NavTerrainInput* north = latticeOf(*input, 0, 1);
    const nav::NavTerrainInput* holed = latticeOf(*input, 1, 1);
    ASSERT_TRUE(a && east && north && holed);
    for (int k = 0; k <= QUADS; k++) {
        const Vector3 p = a->toNav * a->localAt(QUADS, k), q = east->toNav * east->localAt(0, k);
        EXPECT_TRUE(p.x == q.x && p.y == q.y && p.z == q.z) << "east seam, sample " << k;
        const Vector3 r = a->toNav * a->localAt(k, QUADS), t = north->toNav * north->localAt(k, 0);
        EXPECT_TRUE(r.x == t.x && r.y == t.y && r.z == t.z) << "north seam, sample " << k;
    }

    // T3 R23 per tile: the holed tile leaves its hole's cells out, the others have none
    EXPECT_TRUE(holed->isHoleCell(25, 25));
    EXPECT_FALSE(holed->isHoleCell(5, 5));
    EXPECT_TRUE(a->holeCells.empty());

    // baked: walkable on both sides of a seam and across it, never in the hole
    std::string bakeError;
    ASSERT_TRUE(nav->bake(surface, NavBakeMode::FULL, bakeError)) << bakeError;
    ASSERT_TRUE(pumpUntil([&] {
        nav->processBakes(0.0);
        return !nav->isBaking(surface);
    }));
    ASSERT_NE(nav->getLastBakeResult(surface), nullptr);
    ASSERT_TRUE(nav->getLastBakeResult(surface)->ok) << nav->getLastBakeResult(surface)->error;
    EXPECT_TRUE(walkable(ground(30.0f, 30.0f)));
    EXPECT_TRUE(walkable(ground(100.0f, 30.0f)));
    EXPECT_TRUE(walkable(ground(100.0f, 100.0f)));
    EXPECT_FALSE(walkable(ground(89.0f, 89.0f)));
    EXPECT_EQ(nav->calculatePath(ground(30.0f, 30.0f), ground(100.0f, 30.0f)).status, nav::PathStatus::Complete);
    EXPECT_EQ(nav->calculatePath(ground(30.0f, 30.0f), ground(30.0f, 100.0f)).status, nav::PathStatus::Complete);
}

TEST_F(TerrainWorldNav, RefusesUnboundedSurface) {
    // 81 tiles: a surface collecting All is refused before anything is collected or loaded
    addWorld(9);
    const Entity surface = surfaceEntity();
    TerrainWorldRuntime* worldRuntime = runtime();
    ASSERT_TRUE(worldRuntime);
    ASSERT_TRUE(pumpUntil([&] { return worldRuntime->isReady(); }));
    navtest::resolveTransforms(*scene);
    const std::string refusal = "Surface too large: 81 Terrain World tiles without a Volume (64 at most); set Collect Objects to Volume or turn on "
                                "Follow Terrain World Focus";
    std::string error;
    nav::NavCollectStats stats;
    nav::NavBakeError code = nav::NavBakeError::None;
    EXPECT_FALSE(nav::NavBakeCollector::collect(scene.get(), surface, error, &stats, {}, &code));
    EXPECT_EQ(error, refusal);
    // the bake report's too_large code and hint (S75 R16) go with the refusal
    EXPECT_EQ(code, nav::NavBakeError::TooLarge);
    EXPECT_EQ(stats.pendingTiles, 0);
    EXPECT_EQ(stats.terrains, 0);
    error.clear();
    EXPECT_FALSE(nav->bake(surface, NavBakeMode::FULL, error));
    EXPECT_EQ(error, refusal);
    EXPECT_FALSE(nav->isBaking(surface));

    // a Volume bounds it: the tiles it meets (x, z in [34, 94]: tiles 0 and 1 a side)
    NavMeshSurfaceComponent& component = scene->getComponent<NavMeshSurfaceComponent>(surface);
    component.collectObjects = NavCollectObjects::VOLUME;
    component.volumeCenter = Vector3(64.0f, 10.0f, 64.0f);
    component.volumeSize = Vector3(60.0f, 40.0f, 60.0f);
    nav::NavWorldContext context;
    std::shared_ptr<nav::NavBakeInput> input;
    ASSERT_TRUE(pumpUntil([&] {
        input = nav::NavBakeCollector::collect(scene.get(), surface, error, &stats, {}, nullptr, &context);
        return input != nullptr || stats.pendingTiles == 0;
    })) << error;
    ASSERT_TRUE(input) << error;
    EXPECT_EQ(input->terrains.size(), 4u);

    // so does Follow: the tiles within its radius of the focus (tile (1, 1) holds (100, 100))
    component.collectObjects = NavCollectObjects::ALL;
    component.followTerrainWorldFocus = true;
    component.followRadius = 20.0f;
    worldRuntime->addFocus(100.0f, 100.0f);
    nav::NavWorldContext followContext;
    ASSERT_TRUE(pumpUntil([&] {
        input = nav::NavBakeCollector::collect(scene.get(), surface, error, &stats, {}, nullptr, &followContext);
        return input != nullptr || stats.pendingTiles == 0;
    })) << error;
    ASSERT_TRUE(input) << error;
    ASSERT_EQ(input->terrains.size(), 1u);
    EXPECT_EQ(followContext.region, std::set<uint32_t>{tw::TileCoord(1, 1).key()});
}

TEST_F(TerrainWorldNav, FollowRebakes) {
    // a 2 x 2 world followed within 20 m of a focus GameObject: tile (0, 0) at first
    addWorld(2);
    const Entity focus = navtest::transformEntity(*scene, Vector3(16.0f, 0.0f, 16.0f));
    scene->getComponent<TerrainWorldComponent>(world).focus = focus;
    const Entity surface = surfaceEntity();
    NavMeshSurfaceComponent& component = scene->getComponent<NavMeshSurfaceComponent>(surface);
    component.followTerrainWorldFocus = true;
    component.followRadius = 20.0f;
    navtest::resolveTransforms(*scene);
    TerrainWorldRuntime* worldRuntime = runtime();
    ASSERT_TRUE(worldRuntime);
    ASSERT_TRUE(pumpUntil([&] { return worldRuntime->isReady(); }));

    // the fixed tick's first Follow step bakes round the focus
    const nav::NavWorldContext* context = nullptr;
    ASSERT_TRUE(pumpUntil([&] {
        step();
        context = nav->getTerrainWorldContext(surface);
        return context && context->followed && !nav->isBaking(surface) && nav->getNavMesh(surface);
    }));
    ASSERT_TRUE(nav->getLastBakeResult(surface) && nav->getLastBakeResult(surface)->ok);
    EXPECT_EQ(context->region, std::set<uint32_t>{tw::TileCoord(0, 0).key()});
    EXPECT_EQ(context->steps, 0);
    EXPECT_TRUE(walkable(ground(30.0f, 30.0f)));
    EXPECT_FALSE(walkable(ground(90.0f, 30.0f)));

    // an agent with a path across tile (0, 0)
    const Entity walker = navtest::agent(*scene, ground(8.0f, 30.0f));
    step();
    NavMeshAgent handle(scene.get(), walker);
    ASSERT_TRUE(handle.setDestination(ground(60.0f, 30.0f)));
    for (int i = 0; i < 10; i++) {
        pumpWorld();
        step();
    }
    ASSERT_TRUE(handle.isOnNavMesh());

    // the focus moves past S/4 (16 m): tile (1, 0) comes within the radius, and updateTiles runs
    // over its box while the agent keeps walking on the navmesh it has
    scene->getComponent<Transform>(focus).position = Vector3(60.0f, 0.0f, 16.0f);
    int ticks = 0;
    bool stepped = false;
    Vector3 previous = position(walker);
    while (ticks < 1200) {
        pumpWorld();
        step();
        ticks++;
        if (context->steps == 1) stepped = true;
        if (!stepped) continue;
        EXPECT_TRUE(handle.isOnNavMesh()) << "tick " << ticks;
        EXPECT_FALSE(handle.getPathStatus() == nav::PathStatus::Invalid) << "tick " << ticks;
        EXPECT_GT(position(walker).distance(previous), 0.0f) << "tick " << ticks;
        previous = position(walker);
        if (!nav->isBaking(surface)) break;
    }
    ASSERT_TRUE(stepped);
    ASSERT_FALSE(nav->isBaking(surface));
    ASSERT_TRUE(nav->getLastBakeResult(surface) && nav->getLastBakeResult(surface)->ok) << nav->getLastBakeResult(surface)->error;
    EXPECT_EQ(context->steps, 1);
    EXPECT_FLOAT_EQ(context->lastBounds.getMinimum().x, SIZE);
    EXPECT_FLOAT_EQ(context->lastBounds.getMaximum().x, 2.0f * SIZE);
    EXPECT_FLOAT_EQ(context->lastBounds.getMinimum().z, 0.0f);
    EXPECT_FLOAT_EQ(context->lastBounds.getMaximum().z, SIZE);
    EXPECT_EQ(context->region, (std::set<uint32_t>{tw::TileCoord(0, 0).key(), tw::TileCoord(1, 0).key()}));
    EXPECT_TRUE(walkable(ground(90.0f, 30.0f)));
    EXPECT_EQ(nav->calculatePath(ground(30.0f, 30.0f), ground(100.0f, 30.0f)).status, nav::PathStatus::Complete);
    EXPECT_TRUE(handle.setDestination(ground(100.0f, 30.0f)));
    for (int i = 0; i < 10; i++) step();
    EXPECT_TRUE(handle.isOnNavMesh());
    EXPECT_NE(handle.getPathStatus(), nav::PathStatus::Invalid);

    // the focus leaves tile (0, 0) by more than radius + S/2 (52 m): its navmesh tiles go
    scene->getComponent<Transform>(focus).position = Vector3(120.0f, 0.0f, 16.0f);
    ASSERT_TRUE(pumpUntil([&] {
        step();
        return context->steps == 2 && !nav->isBaking(surface);
    }));
    ASSERT_TRUE(nav->getLastBakeResult(surface) && nav->getLastBakeResult(surface)->ok);
    EXPECT_FLOAT_EQ(context->lastBounds.getMinimum().x, 0.0f);
    EXPECT_FLOAT_EQ(context->lastBounds.getMaximum().x, SIZE);
    EXPECT_EQ(context->region, std::set<uint32_t>{tw::TileCoord(1, 0).key()});
    EXPECT_FALSE(walkable(ground(30.0f, 30.0f)));
    EXPECT_TRUE(walkable(ground(90.0f, 30.0f)));
}

#endif // JASPERENGINE_TERRAIN_WORLD
