// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Terrain foliage chunks (docs/superpowers/specs/2026-11-04-render-correctness-design.md 6.6,
// 6.8). The chunk ring was sized from the current view span, which changes as the camera crosses
// chunk borders (Draw Distance 50: 4 then 5 chunks a side): each change destroyed or created chunk
// entities, each parsing the mesh again, and refilled every slot (Doriax de70fa31c). It is sized
// for the widest span now, so walking never destroys a chunk. Min Draw Distance sets the chunks'
// near fade, and the Foliage Distance quality scale multiplies both distances (0 draws none).
// glcore and Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "component/InstancedMeshComponent.h"
#include "component/TerrainComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Terrain.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "TempDir.h"

#include "stb_image_write.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    void writeFoliageGrey(const std::string& path, uint8_t value){
        std::vector<uint8_t> pixels(16 * 16 * 4, value);
        for (size_t i = 3; i < pixels.size(); i += 4)
            pixels[i] = 255;
        ASSERT_TRUE(stbi_write_png(path.c_str(), 16, 16, 4, pixels.data(), 16 * 4)) << path;
    }

    // a one-triangle glTF, its mesh at the root (what a foliage layer instances)
    std::string writeFoliageBlade(TempDir& dir){
        const std::array<float, 9> positions = {0, 0, 0, 0.5f, 0, 0, 0, 1, 0};
        const std::array<float, 9> normals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
        const std::array<uint16_t, 4> indices = {0, 1, 2, 0}; // (padded to 4 bytes)
        std::string bin(reinterpret_cast<const char*>(positions.data()), sizeof(positions));
        bin.append(reinterpret_cast<const char*>(normals.data()), sizeof(normals));
        bin.append(reinterpret_cast<const char*>(indices.data()), sizeof(indices));
        std::ofstream(dir.file("blade.bin"), std::ios::binary) << bin;
        std::ofstream(dir.file("blade.gltf")) << R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
"meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2}]}],
"buffers": [{"uri": "blade.bin", "byteLength": 80}],
"bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, "byteOffset": 36, "byteLength": 36},
                {"buffer": 0, "byteOffset": 72, "byteLength": 6}],
"accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [0.5, 1, 0]},
              {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
              {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}]})";
        return dir.file("blade.gltf").generic_string();
    }

    class TerrainFoliageGpu : public ::testing::Test {
    protected:
        TempDir dir;
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};
        Terrain terrain{&scene};

        void SetUp() override{
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 2.0f, 0.0f);
            camera.setTarget(0.0f, 2.0f, -10.0f);
            camera.activate();
            sun.setType(LightType::DIRECTIONAL);
            sun.setShadows(false);

            const std::string height = dir.file("height.png").generic_string();
            const std::string density = dir.file("density.png").generic_string();
            writeFoliageGrey(height, 0);
            writeFoliageGrey(density, 255);
            terrain.setSize(256.0f);
            terrain.setMaxHeight(1.0f);
            terrain.setHeightMap(height);

            TerrainFoliageLayer layer;
            layer.meshPath = writeFoliageBlade(dir);
            layer.densityMap.setPath(density);
            layer.density = 0.05f;
            layer.drawDistance = 50.0f;
            layer.castShadows = false;
            TerrainComponent& component = scene.getComponent<TerrainComponent>(terrain.getEntity());
            component.foliageLayers.push_back(layer);
            component.needUpdateFoliage = true;
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
            Engine::resetFoliageDistanceScale();
        }

        MeshSystem* meshes(){
            return scene.getSystem<MeshSystem>().get();
        }

        // frames until every chunk the view needs has its entity and mesh
        void settle(){
            Scene* current = &scene;
            MeshSystem* system = meshes();
            int frames = 0;
            EXPECT_TRUE(GpuEnvironment::runFrames([current, system, &frames](){
                frames++;
                return current->getSystem<RenderSystem>()->isAllLoaded() && !system->hasPendingFoliageUpdates() && frames > 2;
            }, 600)) << "the foliage never settled";
        }

        std::set<Entity> chunkEntities(){
            const std::vector<Entity> entities = meshes()->getFoliageEntities(terrain.getEntity());
            return std::set<Entity>(entities.begin(), entities.end());
        }
    };

}

TEST_F(TerrainFoliageGpu, WalkingKeepsTheChunkEntities){
    JASPER_REQUIRE_GPU();
    settle();
    std::set<Entity> seen = chunkEntities();
    ASSERT_FALSE(seen.empty()) << "the layer made no chunk";
    // across chunk borders (32 m chunks): the span is 4 then 5 chunks a side
    for (int step = 1; step <= 40; step++){
        camera.setPosition(1.6f * (float)step, 2.0f, 0.0f);
        GpuEnvironment::drawFrames(1);
        const std::set<Entity> now = chunkEntities();
        for (Entity entity : seen){
            EXPECT_TRUE(now.count(entity)) << "a chunk entity was destroyed at x = " << 1.6f * (float)step;
        }
        seen.insert(now.begin(), now.end());
    }
    // one ring: 5 x 5 chunks at most (ceil(2 * 50 / 32) + 1)
    EXPECT_LE(seen.size(), 25u);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(TerrainFoliageGpu, MinDrawDistanceAndTheScaleSetTheChunksFades){
    JASPER_REQUIRE_GPU();
    scene.getComponent<TerrainComponent>(terrain.getEntity()).foliageLayers[0].minDrawDistance = 20.0f;
    Engine::setFoliageDistanceScale(0.5f);
    settle();
    const std::set<Entity> entities = chunkEntities();
    ASSERT_FALSE(entities.empty());
    const InstancedMeshComponent& chunk = scene.getComponent<InstancedMeshComponent>(*entities.begin());
    EXPECT_FLOAT_EQ(chunk.fadeEnd, 25.0f) << "Draw Distance is not scaled";
    EXPECT_FLOAT_EQ(chunk.fadeStart, 25.0f * 0.75f);
    EXPECT_FLOAT_EQ(chunk.nearFadeEnd, 10.0f) << "Min Draw Distance is not scaled";
    EXPECT_FLOAT_EQ(chunk.nearFadeStart, 10.0f * 0.75f);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(TerrainFoliageGpu, ZeroScaleRemovesTheLayer){
    JASPER_REQUIRE_GPU();
    settle();
    ASSERT_FALSE(chunkEntities().empty());
    Engine::setFoliageDistanceScale(0.0f);
    GpuEnvironment::drawFrames(2);
    EXPECT_TRUE(chunkEntities().empty()) << "Foliage Distance 0 still draws foliage";

    // the editor's terrain preview shows the layer whatever the game's scale
    meshes()->setFoliagePreviewEntity(terrain.getEntity());
    settle();
    EXPECT_FALSE(chunkEntities().empty()) << "the preview follows the game's Foliage Distance";
    meshes()->setFoliagePreviewEntity(NULL_ENTITY);
    EXPECT_NO_ENGINE_ERRORS();
}
