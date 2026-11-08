// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A Terrain World's runtime end to end, with no GPU (T4 R14-R19, R24, R28, R31; AC17-AC19, AC25):
// a cooked folder read through AssetIOService and the jobs, the pump, focuses, area waits, queries.

#include "TerrainWorldTestSupport.h"

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "component/TerrainWorldComponent.h"
#include "component/Transform.h"
#include "io/AssetIOService.h"
#include "jobs/JobSystem.h"
#include "object/TerrainEdit.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "subsystem/TerrainWorldSystem.h"
#include "terrain/world/TerrainWorldData.h"

#include "TempDir.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

using namespace terrainworldtest;
using jasperengine::Entity;
using jasperengine::Scene;
using jasperengine::TerrainComponent;
using jasperengine::TerrainWorldComponent;
using jasperengine::TerrainWorldReadyLevel;
using jasperengine::TerrainWorldRuntime;
using jasperengine::TerrainWorldSystem;
using jasperengine::Transform;

namespace {

    void writeFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    // tiles low..high a side of the fixture's surface, cooked as an export writes them
    void writeCookedWorld(const std::filesystem::path& folder, int low, int high) {
        WorldManifest manifest;
        manifest.guid = "11112222333344445555666677778888";
        manifest.lattice = smallLattice(64, 1.0f);
        manifest.maps.maxHeight = 50.0f;
        WorldOverview overview;
        initOverview(overview, manifest.lattice, TileCoord(low, low), TileCoord(high, high), 0);
        for (int z = low; z <= high; z++) {
            for (int x = low; x <= high; x++) {
                auto tile = fixtureTile(manifest.lattice, TileCoord(x, z));
                const float colours[] = {0.5f, 0.5f, 0.5f, 1.0f};
                buildOverviewColour(*tile, manifest.lattice, colours, 0);
                std::vector<uint8_t> bytes;
                ASSERT_TRUE(encodeTile(*tile, zstdCodec(), TileEncodeOptions{}, bytes));
                writeFile(folder / ("tiles/" + std::to_string(x) + "_" + std::to_string(z) + ".jtile"), bytes);
                setOverviewPatch(overview, *tile);
                const TileMinMax whole = tileNodeMinMax(*tile, manifest.lattice, manifest.lattice.tileLevel(), 0, 0);
                manifest.tiles.push_back(WorldTileInfo{TileCoord(x, z), whole.min, whole.max});
            }
        }
        std::vector<uint8_t> bytes;
        ASSERT_TRUE(encodeOverview(overview, manifest.lattice, zstdCodec(), 3, bytes, nullptr));
        writeFile(folder / "overview.jtile", bytes);
        encodeWorldBin(manifest, bytes);
        writeFile(folder / "world.bin", bytes);
    }

    struct WorldScene {
        jasperengine::test::TempDir files;
        Scene scene;
        Entity entity = NULL_ENTITY;

        explicit WorldScene(int low = -2, int high = 1) {
            writeCookedWorld(files.path() / "World.terrainworld", low, high);
            entity = scene.createEntity();
            scene.addComponent<Transform>(entity, {});
            scene.addComponent<jasperengine::MeshComponent>(entity, {});
            scene.addComponent<TerrainComponent>(entity, {});
            TerrainWorldComponent world;
            world.worldData.path = (files.path() / "World.terrainworld").generic_string();
            world.useCameraFocus = false;
            scene.addComponent<TerrainWorldComponent>(entity, world);
        }

        TerrainWorldRuntime* runtime() {
            std::string error;
            TerrainWorldRuntime* made = scene.getSystem<TerrainWorldSystem>()->ensureRuntime(entity, &error);
            EXPECT_NE(made, nullptr) << error;
            return made;
        }

        // pumps (with the jobs' pump for 0 workers) until the condition holds; false at the timeout
        template<typename F>
        bool pumpUntil(F&& condition, double seconds = 20.0) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (std::chrono::steady_clock::now() < until) {
                if (jasperengine::jobs::hasSystem() && jasperengine::jobs::system().isSingleThreaded()) jasperengine::jobs::system().pump(4.0);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                scene.getSystem<TerrainWorldSystem>()->pump(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
                if (condition()) return true;
            }
            return false;
        }
    };

}

TEST(TerrainWorldStreaming, LoadsAroundAFocus) {
    WorldScene world;
    TerrainWorldRuntime* runtime = world.runtime();
    ASSERT_TRUE(runtime);
    EXPECT_EQ(runtime->occupancy().tileCount(), 16);
    // D10: the overview loads first; then the focus's tile reaches memory
    ASSERT_TRUE(world.pumpUntil([&] { return runtime->isReady(); }));
    const int focus = runtime->addFocus(10.0f, 10.0f);
    EXPECT_GT(focus, 0);
    ASSERT_TRUE(world.pumpUntil([&] { return runtime->resident(TileCoord(0, 0)) != nullptr; }));
    EXPECT_EQ(runtime->pointState(10.0f, 10.0f), jasperengine::TerrainWorldPointState::Full);
    EXPECT_EQ(runtime->pointState(1000.0f, 1000.0f), jasperengine::TerrainWorldPointState::Absent);
    EXPECT_TRUE(runtime->removeFocus(focus));
    EXPECT_FALSE(runtime->removeFocus(focus));
}

TEST(TerrainWorldStreaming, QueryEqualsTheTileKernel) {
    WorldScene world;
    TerrainWorldRuntime* runtime = world.runtime();
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(world.pumpUntil([&] { return runtime->isReady(); }));
    // over a cold tile, on the game's side, the overview answers and says so (R24)
    const jasperengine::terrain::SurfaceSample far = runtime->query(-100.0f, -100.0f, false);
    EXPECT_TRUE(far.present);
    EXPECT_FALSE(far.exact);
    // waiting (the editor, a job) reads the tile: the exact sample at lattice points, across borders
    for (int s : {-64, -1, 0, 1, 63, 64, 65}) {
        const jasperengine::terrain::SurfaceSample sample = runtime->query(static_cast<float>(s), 7.0f, true);
        EXPECT_TRUE(sample.exact);
        EXPECT_TRUE(sample.present);
        EXPECT_NEAR(sample.height, heightOfSample(fixtureSample(s, 7), 50.0f), 1e-4f) << s;
    }
    // off the world: absent, not a hole
    const jasperengine::terrain::SurfaceSample off = runtime->query(5000.0f, 0.0f, true);
    EXPECT_FALSE(off.present);
    EXPECT_FALSE(off.hole);
    // T1's querySurface takes the branch, so every caller of it works on a World
    TerrainComponent& terrain = world.scene.getComponent<TerrainComponent>(world.entity);
    EXPECT_NEAR(jasperengine::querySurface(terrain, 3.0f, 9.0f).height, heightOfSample(fixtureSample(3, 9), 50.0f), 1e-4f);
}

TEST(TerrainWorldStreaming, RaycastHitsTheSurface) {
    WorldScene world;
    TerrainWorldRuntime* runtime = world.runtime();
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(world.pumpUntil([&] { return runtime->isReady(); }));
    // straight down onto a border sample, and slanting across a tile border
    jasperengine::Vector3 point;
    float t = 0.0f;
    ASSERT_TRUE(runtime->raycast(jasperengine::Vector3(0.0f, 200.0f, 12.0f), jasperengine::Vector3(0.0f, -1.0f, 0.0f), 400.0f, false, true, point, t));
    EXPECT_NEAR(point.y, heightOfSample(fixtureSample(0, 12), 50.0f), 1e-3f);
    ASSERT_TRUE(runtime->raycast(jasperengine::Vector3(-30.0f, 100.0f, 5.0f), jasperengine::Vector3(1.0f, -1.0f, 0.0f), 300.0f, false, true, point, t));
    EXPECT_NEAR(point.y, runtime->query(point.x, point.z, true).height, 1e-2f);
    // nothing to hit past the world
    EXPECT_FALSE(runtime->raycast(jasperengine::Vector3(2000.0f, 100.0f, 0.0f), jasperengine::Vector3(0.0f, -1.0f, 0.0f), 300.0f, false, true, point, t));
}

TEST(TerrainWorldStreaming, AreaReadyWaits) {
    WorldScene world;
    TerrainWorldRuntime* runtime = world.runtime();
    ASSERT_TRUE(runtime);
    EXPECT_FALSE(runtime->isAreaReady(0.0f, 0.0f, 20.0f, TerrainWorldReadyLevel::Collision));
    // a pending area is a focus: waiting for it loads it, with no other focus
    const int wait = runtime->waitForArea(0.0f, 0.0f, 20.0f, TerrainWorldReadyLevel::Collision);
    ASSERT_TRUE(world.pumpUntil([&] { return runtime->isAreaReady(0.0f, 0.0f, 20.0f, TerrainWorldReadyLevel::Collision); }));
    EXPECT_TRUE(runtime->missingTiles(0.0f, 0.0f, 20.0f, TerrainWorldReadyLevel::Collision).empty());
    runtime->dropAreaWait(wait);
}

TEST(TerrainWorldStreaming, OnAreaReadyFiresInTheUpdate) {
    WorldScene world;
    TerrainWorldSystem& system = *world.scene.getSystem<TerrainWorldSystem>();
    int fired = 0;
    ASSERT_GT(system.onAreaReady(world.entity, 30.0f, 30.0f, 10.0f, TerrainWorldReadyLevel::Collision, [&] { fired++; }), 0);
    TerrainWorldRuntime* runtime = world.runtime();
    ASSERT_TRUE(world.pumpUntil([&] { return runtime->isAreaReady(30.0f, 30.0f, 10.0f, TerrainWorldReadyLevel::Collision); }));
    // the pump never calls it: only the update phase does (R18), so a paused game hears of it on resume
    EXPECT_EQ(fired, 0);
    system.update(0.016);
    EXPECT_EQ(fired, 1);
    system.update(0.016);
    EXPECT_EQ(fired, 1) << "once";
}

TEST(TerrainWorldStreaming, StoreEditsPinAndCap) {
    WorldScene world(-3, 2);
    TerrainWorldRuntime* runtime = world.runtime();
    ASSERT_TRUE(runtime);
    // T1's store over the component reaches the world's tiles (D12): a write over a tile border
    TerrainComponent& terrain = world.scene.getComponent<TerrainComponent>(world.entity);
    jasperengine::TerrainComponentStore store(&world.scene, world.entity, terrain);
    const MapFormat format = store.format(MapId{MapKind::Height, 0});
    EXPECT_TRUE(format.exists);
    EXPECT_EQ(format.bounds.w, runtime->lattice().heightLatticeSide());
    const int m = runtime->lattice().heightOrigin();
    const TexelRect rect{m + 60, m + 10, 8, 4};
    std::vector<float> values(static_cast<size_t>(rect.area()), 0.9f);
    ASSERT_TRUE(store.write(MapId{MapKind::Height, 0}, rect, values).ok);
    EXPECT_TRUE(runtime->tiles().pinned(TileCoord(0, 0)));
    EXPECT_TRUE(runtime->tiles().pinned(TileCoord(1, 0)));
    EXPECT_TRUE(runtime->hasDirtyFiles());
    EXPECT_NEAR(runtime->query(64.0f, 11.0f, true).height, 0.9f * 50.0f, 1e-3f);
}

TEST(TerrainWorldStreaming, PlayInPlaceRestartsTheRuntime) {
    // Play caps edits and waits differently (R29, R19): a scene the editor plays in place gets a playing
    // runtime, and the editing one hands its edits over before it goes
    WorldScene world;
    bool playing = false;
    int releasedEditing = 0;
    struct HostScope {
        ~HostScope() { TerrainWorldSystem::setHost({}); }
    } scope;
    TerrainWorldSystem::Host host;
    host.playing = [&](Scene*) { return playing; };
    host.released = [&](Scene*, Entity, TerrainWorldRuntime& runtime) { releasedEditing += runtime.playing() ? 0 : 1; };
    TerrainWorldSystem::setHost(host);
    TerrainWorldRuntime* editing = world.runtime();
    ASSERT_TRUE(editing);
    EXPECT_FALSE(editing->playing());
    playing = true;
    world.scene.getSystem<TerrainWorldSystem>()->pump(1.0);
    TerrainWorldRuntime* played = world.runtime();
    ASSERT_TRUE(played);
    EXPECT_TRUE(played->playing());
    EXPECT_EQ(releasedEditing, 1);
}

TEST(TerrainWorldStreaming, ZeroWorkersStillLoad) {
    // R17: no workers and no readers: the pump and the jobs' own pump do it all
    jasperengine::AssetIOService::instance().stop();
    jasperengine::jobs::shutdown();
    jasperengine::jobs::Config config;
    config.workers = 0;
    jasperengine::jobs::initialize(config);
    jasperengine::AssetIOService::instance().start(0);
    {
        WorldScene world;
        TerrainWorldRuntime* runtime = world.runtime();
        ASSERT_TRUE(runtime);
        runtime->addFocus(10.0f, 10.0f);
        EXPECT_TRUE(world.pumpUntil([&] { return runtime->isReady() && runtime->resident(TileCoord(0, 0)) != nullptr; }));
    }
    jasperengine::AssetIOService::instance().stop();
    jasperengine::jobs::shutdown();
    jasperengine::jobs::initialize(jasperengine::jobs::Config{});
    jasperengine::AssetIOService::instance().start();
}

TEST(TerrainWorldZeroCost, PlainTerrainCostsNothing) {
    // R37: with the module in and no world, the system's frame is an empty check, with no allocation
    if (!testsupport::countsAllocations()) GTEST_SKIP() << "this binary cannot count allocations";
    Scene scene;
    const Entity entity = scene.createEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<jasperengine::MeshComponent>(entity, {});
    scene.addComponent<TerrainComponent>(entity, {});
    TerrainWorldSystem& system = *scene.getSystem<TerrainWorldSystem>();
    system.pump(0.0);
    const auto start = std::chrono::steady_clock::now();
    uint64_t allocations = 0;
    {
        testsupport::AllocationCountScope scope;
        for (int frame = 0; frame < 10000; frame++) {
            system.pump(static_cast<double>(frame) * 0.016);
            system.update(0.016);
        }
        allocations = scope.count();
    }
    const double meanMicroseconds = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 10000.0;
    EXPECT_EQ(allocations, 0u);
    EXPECT_FALSE(system.hasWorlds());
    EXPECT_EQ(scene.getComponent<TerrainComponent>(entity).world, nullptr);
    std::printf("[bench] terrain_world_zero_cost_us %.3f\n", meanMicroseconds);
    // a wall-clock budget gates only on the benchmark run (JASPER_TEST_PERF_SLACK)
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LE(meanMicroseconds, 1.0 * testsupport::perfSlack());
    }
}
