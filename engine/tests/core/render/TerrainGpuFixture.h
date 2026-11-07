// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPER_TEST_TERRAIN_GPU_FIXTURE_H
#define JASPER_TEST_TERRAIN_GPU_FIXTURE_H

// The scenes the T3 GPU suites draw: a terrain made the way a game makes it, its maps written to a
// scratch directory (the detail array reads its layers from files), on the headless device.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Terrain.h"
#include "pool/TextureDataPool.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"
#include "Readback.h"
#include "TempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine { namespace test {

    // The layers the suites paint with: a flat colour broken by a soft check, so that tiling,
    // projection and filtering all show in the frame
    struct TerrainGpuColor {
        unsigned char r, g, b, a;
    };

    class TerrainGpuFixture : public ::testing::Test {
    protected:
        static constexpr int FRAME_WIDTH = 640;
        static constexpr int FRAME_HEIGHT = 360;
        static constexpr int HEIGHT_RESOLUTION = 129;
        static constexpr int BLEND_RESOLUTION = 128;
        static constexpr int LAYER_RESOLUTION = 64;
        static constexpr float TERRAIN_SIZE = 128.0f;

        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};
        std::unique_ptr<Terrain> terrain;
        TempDir files;
        // A TextureData made from memory points at these, so they live as long as the scene
        std::vector<std::vector<unsigned char>> memory;
        std::vector<std::string> poolIds;

        void SetUp() override{
            scene.setBackgroundColor(0.55f, 0.68f, 0.82f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 42.0f, 72.0f);
            camera.setTarget(0.0f, 2.0f, 4.0f);
            camera.setFarClip(400.0f);
            camera.activate();
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(-0.5f, -1.0f, -0.35f);
            sun.setIntensity(2.0f);
            sun.setShadows(false);
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
            for (const std::string& id : poolIds){
                TextureDataPool::remove(id);
            }
        }

        TerrainComponent& component(){
            return scene.getComponent<TerrainComponent>(terrain->getEntity());
        }

        // Heights in 0..1 by f(x, z), the terrain 128 x 128 at the origin
        void makeTerrain(float maxHeight, const std::function<float(float, float)>& f){
            terrain = std::make_unique<Terrain>(&scene);
            terrain->setSize(TERRAIN_SIZE);
            terrain->setMaxHeight(maxHeight);
            std::vector<unsigned char> pixels(static_cast<size_t>(HEIGHT_RESOLUTION) * HEIGHT_RESOLUTION * 2, 0);
            for (int j = 0; j < HEIGHT_RESOLUTION; j++){
                for (int i = 0; i < HEIGHT_RESOLUTION; i++){
                    const float x = ((static_cast<float>(i) + 0.5f) / HEIGHT_RESOLUTION - 0.5f) * TERRAIN_SIZE;
                    const float z = ((static_cast<float>(j) + 0.5f) / HEIGHT_RESOLUTION - 0.5f) * TERRAIN_SIZE;
                    const float h = std::fmin(std::fmax(f(x, z), 0.0f), 1.0f);
                    const unsigned q = static_cast<unsigned>(std::lround(h * 65535.0f));
                    const size_t at = (static_cast<size_t>(j) * HEIGHT_RESOLUTION + i) * 2;
                    pixels[at] = static_cast<unsigned char>(q & 0xFF);
                    pixels[at + 1] = static_cast<unsigned char>(q >> 8);
                }
            }
            memory.push_back(std::move(pixels));
            const std::vector<unsigned char>& kept = memory.back();
            const std::string id = poolId("height");
            TerrainComponent& terrainComponent = component();
            terrainComponent.heightMap.setData(id, TextureData(HEIGHT_RESOLUTION, HEIGHT_RESOLUTION,
                                               static_cast<unsigned int>(kept.size()), ColorFormat::RED16, 1,
                                               const_cast<unsigned char*>(kept.data())));
            terrainComponent.heightMap.setReleaseDataAfterLoad(false);
            terrainComponent.needUpdateTexture = true;
            terrainComponent.needUpdateTerrain = true;
            terrain->setColor(0.42f, 0.55f, 0.3f);
        }

        // Writes a PNG of the fixture's layer resolution and returns its path
        std::string writeMap(const std::string& name, const std::function<TerrainGpuColor(int, int)>& f){
            std::vector<uint8_t> rgba(static_cast<size_t>(LAYER_RESOLUTION) * LAYER_RESOLUTION * 4);
            for (int y = 0; y < LAYER_RESOLUTION; y++){
                for (int x = 0; x < LAYER_RESOLUTION; x++){
                    const TerrainGpuColor c = f(x, y);
                    const size_t at = (static_cast<size_t>(y) * LAYER_RESOLUTION + x) * 4;
                    rgba[at] = c.r;
                    rgba[at + 1] = c.g;
                    rgba[at + 2] = c.b;
                    rgba[at + 3] = c.a;
                }
            }
            const std::string path = files.file(name + ".png").generic_string();
            EXPECT_TRUE(writePng(path, rgba, LAYER_RESOLUTION, LAYER_RESOLUTION));
            return path;
        }

        // A colour with a soft check and height in alpha from a bump per tile
        std::string writeColorMap(const std::string& name, unsigned char r, unsigned char g, unsigned char b){
            return writeMap(name, [=](int x, int y){
                const float check = ((x / 8 + y / 8) % 2 == 0) ? 1.0f : 0.72f;
                const float dx = (static_cast<float>(x) + 0.5f) / LAYER_RESOLUTION - 0.5f;
                const float dy = (static_cast<float>(y) + 0.5f) / LAYER_RESOLUTION - 0.5f;
                const float height = std::fmax(0.3f, 1.0f - 1.2f * std::sqrt(dx * dx + dy * dy));
                return TerrainGpuColor{static_cast<unsigned char>(r * check), static_cast<unsigned char>(g * check),
                                       static_cast<unsigned char>(b * check), static_cast<unsigned char>(height * 255.0f)};
            });
        }

        // Ridges across the tile, tangent space
        std::string writeNormalMap(const std::string& name){
            return writeMap(name, [](int x, int){
                const float slope = std::sin(static_cast<float>(x) / LAYER_RESOLUTION * 6.2831853f * 4.0f) * 0.5f;
                const float length = std::sqrt(slope * slope + 1.0f);
                return TerrainGpuColor{static_cast<unsigned char>((slope / length * 0.5f + 0.5f) * 255.0f), 128,
                                       static_cast<unsigned char>((1.0f / length * 0.5f + 0.5f) * 255.0f), 255};
            });
        }

        // Occlusion, roughness, metallic in RGB (read by channel) and a height bump in R of its own map
        std::string writeSurfaceMap(const std::string& name){
            return writeMap(name, [](int x, int y){
                const float dx = (static_cast<float>(x) + 0.5f) / LAYER_RESOLUTION - 0.5f;
                const float dy = (static_cast<float>(y) + 0.5f) / LAYER_RESOLUTION - 0.5f;
                const float bump = std::fmax(0.2f, 1.0f - 1.5f * std::sqrt(dx * dx + dy * dy));
                return TerrainGpuColor{static_cast<unsigned char>(bump * 255.0f), static_cast<unsigned char>((0.4f + 0.5f * bump) * 255.0f),
                                       32, 255};
            });
        }

        // An RGB blend map made of the weights f(u, v) in 0..1
        void setBlendMap(unsigned int index, const std::function<TerrainGpuColor(float, float)>& f){
            std::vector<unsigned char> pixels(static_cast<size_t>(BLEND_RESOLUTION) * BLEND_RESOLUTION * 4);
            for (int y = 0; y < BLEND_RESOLUTION; y++){
                for (int x = 0; x < BLEND_RESOLUTION; x++){
                    const TerrainGpuColor c = f((static_cast<float>(x) + 0.5f) / BLEND_RESOLUTION, (static_cast<float>(y) + 0.5f) / BLEND_RESOLUTION);
                    const size_t at = (static_cast<size_t>(y) * BLEND_RESOLUTION + x) * 4;
                    pixels[at] = c.r;
                    pixels[at + 1] = c.g;
                    pixels[at + 2] = c.b;
                    pixels[at + 3] = c.a;
                }
            }
            memory.push_back(std::move(pixels));
            const std::vector<unsigned char>& kept = memory.back();
            TerrainComponent& terrainComponent = component();
            if (terrainComponent.blendMaps.size() <= index){
                terrainComponent.blendMaps.resize(index + 1);
            }
            terrainComponent.blendMaps[index].setData(poolId("blend" + std::to_string(index)),
                TextureData(BLEND_RESOLUTION, BLEND_RESOLUTION, static_cast<unsigned int>(kept.size()), ColorFormat::RGBA, 4,
                            const_cast<unsigned char*>(kept.data())));
            terrainComponent.blendMaps[index].setReleaseDataAfterLoad(false);
            terrainComponent.needUpdateTexture = true;
        }

        bool render(){
            GpuEnvironment::setView(FRAME_WIDTH, FRAME_HEIGHT, FRAME_WIDTH, FRAME_HEIGHT, Scaling::STRETCH);
            Scene* current = &scene;
            // a tier or a feature swaps the variant while the old one draws (needVariantReload): the frame
            // is the new one's only once that is done
            const auto settled = [current](){
                if (!current->getSystem<RenderSystem>()->isAllLoaded()){
                    return false;
                }
                auto meshes = current->getComponentArray<MeshComponent>();
                for (size_t i = 0; i < meshes->size(); i++){
                    const MeshComponent& mesh = meshes->getComponentFromIndex(i);
                    if (mesh.needReload || mesh.needVariantReload){
                        return false;
                    }
                }
                return true;
            };
            const bool ready = GpuEnvironment::runFrames(settled, 600);
            GpuEnvironment::drawFrames(3);
            return ready && settled();
        }

        bool readFrame(std::vector<uint8_t>& rgba){
            return readTexture(GpuEnvironment::target().getRender().getColorTexture(), FRAME_WIDTH, FRAME_HEIGHT, rgba);
        }

        std::string poolId(const std::string& name){
            const ::testing::TestInfo* test = ::testing::UnitTest::GetInstance()->current_test_info();
            const std::string id = std::string("terrain-gpu-") + test->name() + "-" + name;
            poolIds.push_back(id);
            return id;
        }
    };

} }

#endif //JASPER_TEST_TERRAIN_GPU_FIXTURE_H
