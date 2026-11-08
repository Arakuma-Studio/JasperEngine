// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The built-in Lit's painted Terrain ('Tpb') reads its pixel lights' records from u_lightRecords
// where its blocks are over the backend's fragment uniforms (ShaderVariantCompiler::compile,
// includes/lights.glsl LIGHTS_FS_RECORDS: docs/superpowers/specs/2026-11-04-water-reflections-design.md,
// Follow-ups): GLES3 / WebGL2's 224 vectors take it with its nine layers' block and sixteen
// lights; desktop GL's 256 do not, and it keeps its blocks there. Its golden was captured with the
// per-object block form: a painted terrain lit by a shadowed sun, a shadowed point and spot light
// and ten more points (the sixteen-light tier), in the per-object Light Mode.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "object/Terrain.h"
#include "pool/TextureDataPool.h"
#include "render/LightingTiers.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace terrain_light_records_gpu_test {

    constexpr int TERRAIN_HEIGHT_RESOLUTION = 65;
    constexpr int TERRAIN_BLEND_RESOLUTION = 64;
    constexpr float TERRAIN_RECORDS_SIZE = 32.0f;

    // glcore against its own golden: exact; another backend: the cross-backend bound
    CompareOptions terrainRecordsOptions(){
        CompareOptions options;
        if (std::strcmp(HeadlessContext::getBackendName(), "glcore") == 0){
            options.pixelThreshold = 0;
            options.maxBadFraction = 0.0;
            options.maxMeanDiff = 0.0;
        }else{
            options.pixelThreshold = 16;
            options.maxBadFraction = 0.03;
            options.maxMeanDiff = 3.0;
        }
        return options;
    }

    class TerrainLightRecordsGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};
        std::vector<std::unique_ptr<Light>> lights;
        std::unique_ptr<Terrain> terrain;
        // what TextureData made from memory points at: alive as long as the scene
        std::vector<unsigned char> heightPixels;
        std::vector<unsigned char> blendPixels;

        void SetUp() override{
            GpuEnvironment::resetView();
            Engine::setPinnedShaderTime(2.0);
            LightingTiers::setLightMode(LightMode::PER_OBJECT);
            scene.setBackgroundColor(0.1f, 0.1f, 0.12f, 1.0f);
            scene.setMaxLightsPerObject(16);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(-0.3f, -1.0f, -0.4f);
            sun.setIntensity(1.0f);
            sun.setShadows(true);
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
            LightingTiers::setLightMode(LightMode::AUTOMATIC);
            Engine::setPinnedShaderTime(-1.0);
            TextureDataPool::remove("terrain-records-height");
            TextureDataPool::remove("terrain-records-blend");
        }

        Light& addLight(LightType type, const Vector3& position, const Vector3& color, float intensity, float range){
            lights.push_back(std::make_unique<Light>(&scene));
            Light& light = *lights.back();
            light.setType(type);
            light.setPosition(position);
            light.setColor(color);
            light.setIntensity(intensity);
            light.setRange(range);
            return light;
        }

        // a gentle hill painted with three PBR layers along X
        void makePaintedTerrain(){
            terrain = std::make_unique<Terrain>(&scene);
            terrain->setSize(TERRAIN_RECORDS_SIZE);
            terrain->setMaxHeight(2.0f);
            heightPixels.assign((size_t)TERRAIN_HEIGHT_RESOLUTION * TERRAIN_HEIGHT_RESOLUTION * 2, 0);
            for (int j = 0; j < TERRAIN_HEIGHT_RESOLUTION; j++){
                for (int i = 0; i < TERRAIN_HEIGHT_RESOLUTION; i++){
                    const float u = (float)i / (TERRAIN_HEIGHT_RESOLUTION - 1) - 0.5f;
                    const float v = (float)j / (TERRAIN_HEIGHT_RESOLUTION - 1) - 0.5f;
                    const float h = 0.3f + 0.2f * (1.0f - 4.0f * (u * u + v * v));
                    const unsigned q = (unsigned)(std::max(h, 0.0f) * 65535.0f);
                    const size_t at = ((size_t)j * TERRAIN_HEIGHT_RESOLUTION + i) * 2;
                    heightPixels[at] = (unsigned char)(q & 0xFF);
                    heightPixels[at + 1] = (unsigned char)(q >> 8);
                }
            }
            blendPixels.assign((size_t)TERRAIN_BLEND_RESOLUTION * TERRAIN_BLEND_RESOLUTION * 4, 0);
            for (int j = 0; j < TERRAIN_BLEND_RESOLUTION; j++){
                for (int i = 0; i < TERRAIN_BLEND_RESOLUTION; i++){
                    const size_t at = ((size_t)j * TERRAIN_BLEND_RESOLUTION + i) * 4;
                    const int third = i * 3 / TERRAIN_BLEND_RESOLUTION;
                    blendPixels[at + (size_t)third] = 255;
                    blendPixels[at + 3] = 255;
                }
            }
            TerrainComponent& component = scene.getComponent<TerrainComponent>(terrain->getEntity());
            component.heightMap.setData("terrain-records-height", TextureData(TERRAIN_HEIGHT_RESOLUTION, TERRAIN_HEIGHT_RESOLUTION,
                                        (unsigned int)heightPixels.size(), ColorFormat::RED16, 1, heightPixels.data()));
            component.heightMap.setReleaseDataAfterLoad(false);
            Texture blend;
            blend.setData("terrain-records-blend", TextureData(TERRAIN_BLEND_RESOLUTION, TERRAIN_BLEND_RESOLUTION,
                          (unsigned int)blendPixels.size(), ColorFormat::RGBA, 4, blendPixels.data()));
            blend.setReleaseDataAfterLoad(false);
            component.blendMaps = {blend};
            const Vector4 colors[3] = {Vector4(0.6f, 0.5f, 0.35f, 1.0f), Vector4(0.3f, 0.55f, 0.25f, 1.0f), Vector4(0.55f, 0.55f, 0.6f, 1.0f)};
            component.surfaceLayers.assign(3, TerrainSurfaceLayer());
            for (int l = 0; l < 3; l++){
                component.surfaceLayers[l].pbr = true;
                component.surfaceLayers[l].colorFactor = colors[l];
                component.surfaceLayers[l].roughnessFactor = 0.35f + 0.2f * (float)l;
            }
            component.needUpdateTexture = true;
            component.needUpdateTerrain = true;
        }

        bool render(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }
    };

}

using namespace terrain_light_records_gpu_test;

TEST_F(TerrainLightRecordsGpu, PaintedTerrainDrawsTheBlockFormsImage){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 14.0f, 18.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();

    Primitive caster(&scene);
    caster.createBox(2.0f, 0.3f, 2.0f);
    caster.setPosition(-2.0f, 4.0f, 1.0f);
    caster.setColor(0.8f, 0.3f, 0.2f);

    Light& point = addLight(LightType::POINT, Vector3(4.0f, 2.5f, 2.0f), Vector3(1.0f, 0.6f, 0.3f), 10.0f, 10.0f);
    point.setShadows(true);
    Light& spot = addLight(LightType::SPOT, Vector3(-5.0f, 6.0f, -2.0f), Vector3(0.3f, 0.7f, 1.0f), 40.0f, 16.0f);
    spot.setDirection(0.3f, -1.0f, 0.2f);
    spot.setConeAngle(20.0f, 35.0f);
    spot.setShadows(true);
    // ten more small points over the hill: thirteen lights, the sixteen-light tier
    for (int i = 0; i < 10; i++){
        const float x = -10.0f + 2.2f * (float)i;
        const float z = (i % 2 == 0) ? -5.0f : 5.0f;
        addLight(LightType::POINT, Vector3(x, 2.0f, z), Vector3(0.4f + 0.06f * (float)i, 1.0f - 0.07f * (float)i, 0.5f), 4.0f, 4.0f);
    }

    makePaintedTerrain();
    ASSERT_TRUE(render()) << "the scene never finished loading";
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(terrain->getEntity());
    ASSERT_GT(mesh.numSubmeshes, 0u);
    EXPECT_TRUE(mesh.submeshes[0].shaderProperties & shaderPropertyBit(27)) << "not the painted terrain";
    EXPECT_TRUE(mesh.submeshes[0].shaderProperties & shaderPropertyBit(30)) << "not the sixteen-light tier";
    if (std::strcmp(HeadlessContext::getBackendName(), "glcore") == 0){
        EXPECT_EQ(mesh.submeshes[0].slotLightRecords.first, -1) << "within desktop GL's 256 fragment vectors the terrain keeps its blocks";
    }
    const CompareResult result = compareTargetWithGolden("terrain_painted_lights", terrainRecordsOptions());
    EXPECT_TRUE(result.passed) << result.message;
    EXPECT_NO_ENGINE_ERRORS();
}
