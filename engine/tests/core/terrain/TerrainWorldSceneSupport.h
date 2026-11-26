// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_WORLD_SCENE_SUPPORT_H
#define TERRAIN_WORLD_SCENE_SUPPORT_H

// A cooked Terrain World on disk and a scene running it, pumped by hand (T4's streaming tests and
// the Simulation Culling ground rule's, TerrainWorldResidencyTest.cpp).

#include "TerrainWorldTestSupport.h"

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "component/TerrainWorldComponent.h"
#include "component/Transform.h"
#include "jobs/JobSystem.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "subsystem/TerrainWorldSystem.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace terrainworldtest {

    using jasperengine::Entity;
    using jasperengine::Scene;
    using jasperengine::TerrainComponent;
    using jasperengine::TerrainWorldComponent;
    using jasperengine::TerrainWorldRuntime;
    using jasperengine::TerrainWorldSystem;
    using jasperengine::Transform;

    inline void writeFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    // tiles low..high a side of the fixture's surface, cooked as an export writes them
    inline void writeCookedWorld(const std::filesystem::path& folder, int low, int high) {
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

#endif // TERRAIN_WORLD_SCENE_SUPPORT_H
