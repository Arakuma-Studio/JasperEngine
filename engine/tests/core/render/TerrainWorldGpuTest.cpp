// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A Terrain World drawn on the headless device (T4 R7-R13; AC6, AC8): its pages and overview from a
// cooked folder, with no crack at any residency and the overview covering the frame from its first.

#include "Engine.h"
#include "Scene.h"
#include "component/TerrainComponent.h"
#include "component/TerrainWorldComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Terrain.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "terrain/world/TerrainTileFormat.h"
#include "terrain/world/TerrainWorldData.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "Readback.h"
#include "TempDir.h"

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <memory>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    constexpr int FRAME_WIDTH = 320;
    constexpr int FRAME_HEIGHT = 180;
    constexpr int TILE_QUADS = 64;
    constexpr float MAX_HEIGHT = 40.0f;

    // bumps that differ between LOD levels, so a missing skirt opens a visible gap
    uint16_t bumpySample(int sx, int sz) {
        const double value = 0.45 + 0.2 * std::sin(sx * 0.21) * std::cos(sz * 0.17) + 0.15 * std::sin((sx - sz) * 0.53);
        return static_cast<uint16_t>(std::lround(std::fmin(std::fmax(value, 0.0), 1.0) * 65535.0));
    }

    void writeBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    // A cooked world (world.bin, overview.jtile, tiles), as an export writes it; raw sections
    void writeWorld(const std::filesystem::path& folder, int low, int high) {
        WorldManifest manifest;
        manifest.guid = "7d3a1b2c4e5f60718293a4b5c6d7e8f9";
        manifest.lattice.tileQuads = TILE_QUADS;
        manifest.lattice.spacing = 1.0f;
        manifest.maps.maxHeight = MAX_HEIGHT;
        WorldOverview overview;
        initOverview(overview, manifest.lattice, TileCoord(low, low), TileCoord(high, high), 0);
        for (int z = low; z <= high; z++) {
            for (int x = low; x <= high; x++) {
                WorldTile tile;
                initTile(tile, manifest.lattice, TileCoord(x, z), 0);
                for (int j = 0; j < TILE_QUADS + 3; j++) {
                    for (int i = 0; i < TILE_QUADS + 3; i++) tile.setSample(i, j, bumpySample(x * TILE_QUADS + i - 1, z * TILE_QUADS + j - 1));
                }
                const float colours[] = {0.35f, 0.6f, 0.3f, 1.0f};
                buildDerived(tile, manifest.lattice, colours, 0);
                std::vector<uint8_t> bytes;
                ASSERT_TRUE(encodeTile(tile, TileCodec{}, TileEncodeOptions{}, bytes));
                writeBytes(folder / ("tiles/" + std::to_string(x) + "_" + std::to_string(z) + ".jtile"), bytes);
                setOverviewPatch(overview, tile);
                const TileMinMax whole = tileNodeMinMax(tile, manifest.lattice, manifest.lattice.tileLevel(), 0, 0);
                manifest.tiles.push_back(WorldTileInfo{TileCoord(x, z), whole.min, whole.max});
                manifest.skirtDepth = std::fmax(manifest.skirtDepth, tile.skirtDepth);
            }
        }
        std::vector<uint8_t> bytes;
        ASSERT_TRUE(encodeOverview(overview, manifest.lattice, TileCodec{}, 0, bytes, nullptr));
        writeBytes(folder / "overview.jtile", bytes);
        encodeWorldBin(manifest, bytes);
        writeBytes(folder / "world.bin", bytes);
    }

    class TerrainWorldGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};
        std::unique_ptr<Terrain> terrain;
        jasperengine::test::TempDir files;

        void SetUp() override {
            if (!jasperengine::test::GpuEnvironment::ensure()) GTEST_SKIP() << jasperengine::test::GpuEnvironment::unavailableReason();
            // magenta shows wherever the ground leaves a gap (AC8)
            scene.setBackgroundColor(1.0f, 0.0f, 1.0f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setFarClip(2000.0f);
            camera.activate();
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(-0.4f, -1.0f, -0.3f);
            sun.setShadows(false);
            Engine::setScene(&scene);
        }

        void TearDown() override {
            Engine::removeScene(&scene);
        }

        void makeWorld(int low, int high, int poolSize) {
            writeWorld(files.path() / "Island.terrainworld", low, high);
            terrain = std::make_unique<Terrain>(&scene);
            terrain->setColor(0.35f, 0.6f, 0.3f);
            TerrainWorldComponent world;
            world.worldData.path = (files.path() / "Island.terrainworld").generic_string();
            world.useCameraFocus = true;
            world.poolSize = poolSize;
            scene.addComponent<TerrainWorldComponent>(terrain->getEntity(), world);
        }

        TerrainWorldRuntime* runtime() {
            return scene.getComponent<TerrainComponent>(terrain->getEntity()).world;
        }

        // frames until the world is ready, its wanted tiles resident (when asked) and everything loaded
        bool settle(bool allResident) {
            jasperengine::test::GpuEnvironment::setView(FRAME_WIDTH, FRAME_HEIGHT, FRAME_WIDTH, FRAME_HEIGHT, Scaling::STRETCH);
            Scene* current = &scene;
            const auto ready = [this, current, allResident]() {
                TerrainWorldRuntime* world = runtime();
                if (!world || !world->isReady() || !current->getSystem<RenderSystem>()->isAllLoaded()) return false;
                if (!allResident) return true;
                const TerrainWorldStats stats = world->stats();
                return stats.queue == 0 && stats.inFlight == 0 && stats.states[static_cast<int>(TileState::CpuReady)] == 0;
            };
            const bool done = jasperengine::test::GpuEnvironment::runFrames(ready, 900);
            jasperengine::test::GpuEnvironment::drawFrames(3);
            return done && ready();
        }

        int magentaPixels() {
            std::vector<uint8_t> rgba;
            if (!jasperengine::test::readTexture(jasperengine::test::GpuEnvironment::target().getRender().getColorTexture(), FRAME_WIDTH, FRAME_HEIGHT, rgba)) {
                return -1;
            }
            int count = 0;
            for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
                if (rgba[i] > 200 && rgba[i + 1] < 60 && rgba[i + 2] > 200) count++;
            }
            return count;
        }

        // looking down over the 4-tile corner at the origin, ground filling the frame
        void lookDown(float x, float z, float height) {
            camera.setPosition(x + 0.5f, height, z + 30.0f);
            camera.setTarget(x, 0.0f, z);
        }
    };

}

TEST_F(TerrainWorldGpu, OverviewFirstFrame) {
    makeWorld(-3, 2, 0);
    lookDown(0.0f, 0.0f, 120.0f);
    ASSERT_TRUE(settle(false));
    // R10: before any page is up, the overview covers every present tile
    runtime()->setFrozen(true);
    jasperengine::test::GpuEnvironment::drawFrames(3);
    EXPECT_EQ(magentaPixels(), 0);
    EXPECT_TRUE(runtime()->isReady());
}

TEST_F(TerrainWorldGpu, NoCracksAnyResidency) {
    makeWorld(-2, 1, 0);
    lookDown(0.0f, 0.0f, 70.0f);
    // all resident: near tiles meet near tiles
    ASSERT_TRUE(settle(true));
    EXPECT_GT(runtime()->stats().states[static_cast<int>(TileState::Resident)], 0);
    EXPECT_EQ(magentaPixels(), 0) << "all resident";
    // one page: near tiles meet the overview on every side
    scene.getComponent<TerrainWorldComponent>(terrain->getEntity()).poolSize = 1;
    jasperengine::test::GpuEnvironment::drawFrames(20);
    EXPECT_LE(runtime()->stats().states[static_cast<int>(TileState::Resident)], 1);
    EXPECT_EQ(magentaPixels(), 0) << "one resident page";
}

TEST_F(TerrainWorldGpu, DrawsAtTheWorldsLimit) {
    // R6, D13: the same world far from the origin draws the same, with no crack
    makeWorld(200, 203, 0);
    const float origin = 200.0f * TILE_QUADS + 2.0f * TILE_QUADS;
    lookDown(origin, origin, 70.0f);
    ASSERT_TRUE(settle(true));
    EXPECT_EQ(magentaPixels(), 0);
}
