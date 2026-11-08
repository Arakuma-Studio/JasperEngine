// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// library:water reads its pixel lights' records from the frame's u_lightRecords (includes/lights.glsl,
// LIGHTS_FS_RECORDS: docs/superpowers/specs/2026-11-04-water-reflections-design.md, Follow-ups), so
// sixteen lights with shadows fit GLES3 / WebGL2's 224 fragment uniform vectors. It must draw what
// the block form drew: the same water as a fork without the define (the per-object block form), lit
// by a shadowed sun, a shadowed point and spot light and ten more points (the sixteen-light tier),
// in the per-object Light Mode (the Tiled form has its own lists).

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "render/LightingTiers.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderUniforms.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"
#include "texture/Material.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace water_light_records_gpu_test {

    constexpr double RECORDS_PINNED_TIME = 3.5;

    // library:water's entry points as a project fork; the .frag without LIGHTS_FS_RECORDS draws
    // the lights from the per-object block
    bool readBlockFormFork(const std::string& path, std::string& content){
        const auto& sources = ShaderVariantCompiler::getLibrarySources();
        if (path == "shaders/BlockWater.vert"){
            auto found = sources.find("library/water.vert");
            if (found == sources.end())
                return false;
            content = found->second;
            return true;
        }
        if (path == "shaders/BlockWater.frag"){
            auto found = sources.find("library/water.frag");
            if (found == sources.end())
                return false;
            content = found->second;
            const std::string define = "#define LIGHTS_FS_RECORDS";
            const size_t at = content.find(define);
            if (at == std::string::npos)
                return false;
            content.erase(at, define.size());
            return true;
        }
        return false;
    }

    Material litWater(const std::string& shader){
        Material material;
        material.name = "Water";
        material.shader = shader;
        material.shaderFeatures = {"BUILTIN_TEXTURES", "REFLECTION"};
        material.lighting = MaterialLighting::PIXEL;
        ShaderUniforms::set(material.shaderParams, "normalStrength", Vector4(0.4f, 0.0f, 0.0f, 0.0f));
        ShaderUniforms::set(material.shaderParams, "foamAmount", Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        ShaderUniforms::set(material.shaderParams, "gloss", Vector4(0.6f, 0.0f, 0.0f, 0.0f));
        return material;
    }

    std::vector<uint8_t> readRecordsTarget(int& width, int& height){
        Framebuffer& target = GpuEnvironment::target();
        width = (int)target.getWidth();
        height = (int)target.getHeight();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), width, height, rgba);
        return rgba;
    }

    // the pixels whose largest channel difference is above threshold, and the largest
    size_t differingPixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int threshold, int& largest){
        size_t count = 0;
        largest = 0;
        for (size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4){
            int most = 0;
            for (int c = 0; c < 3; c++)
                most = std::max(most, std::abs((int)a[i + c] - (int)b[i + c]));
            largest = std::max(largest, most);
            if (most > threshold)
                count++;
        }
        return count;
    }

    class WaterLightRecordsGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};
        std::vector<std::unique_ptr<Light>> lights;

        void SetUp() override{
            GpuEnvironment::resetView();
            Engine::setPinnedShaderTime(RECORDS_PINNED_TIME);
            LightingTiers::setLightMode(LightMode::PER_OBJECT);
            ShaderManifests::setForkReader(readBlockFormFork);
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
            ShaderManifests::setForkReader(nullptr);
            ShaderManifests::clearForks();
            LightingTiers::setLightMode(LightMode::AUTOMATIC);
            Engine::setPinnedShaderTime(-1.0);
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

        bool render(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }
    };

}

using namespace water_light_records_gpu_test;

TEST_F(WaterLightRecordsGpu, RecordsDrawWhatTheBlockDrew){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 9.0f, 10.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();

    Primitive caster(&scene);
    caster.createBox(1.5f, 0.2f, 1.5f);
    caster.setPosition(-1.0f, 2.0f, 0.5f);
    caster.setColor(0.8f, 0.3f, 0.2f);

    Light& point = addLight(LightType::POINT, Vector3(2.5f, 1.5f, 1.0f), Vector3(1.0f, 0.6f, 0.3f), 6.0f, 8.0f);
    point.setShadows(true);
    Light& spot = addLight(LightType::SPOT, Vector3(-3.0f, 4.0f, -1.0f), Vector3(0.3f, 0.7f, 1.0f), 30.0f, 12.0f);
    spot.setDirection(0.3f, -1.0f, 0.2f);
    spot.setConeAngle(20.0f, 35.0f);
    spot.setShadows(true);
    // ten more small points over the surface: thirteen lights, the sixteen-light tier
    for (int i = 0; i < 10; i++){
        const float x = -6.0f + 1.3f * (float)i;
        const float z = (i % 2 == 0) ? -3.0f : 3.0f;
        addLight(LightType::POINT, Vector3(x, 0.6f, z), Vector3(0.4f + 0.06f * (float)i, 1.0f - 0.07f * (float)i, 0.5f), 2.0f, 2.5f);
    }

    Primitive water(&scene);
    water.createPlane(16.0f, 16.0f);

    auto shoot = [&](const std::string& shader, std::vector<uint8_t>& rgba, int& width, int& height){
        water.setMaterial(litWater(shader));
        EXPECT_TRUE(render()) << "the scene never finished loading";
        rgba = readRecordsTarget(width, height);
    };

    int width = 0, height = 0;
    std::vector<uint8_t> records, block, sunOnly;
    shoot("library:water", records, width, height);
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(water.getEntity());
    EXPECT_NE(mesh.submeshes[0].slotLightRecords.first, -1) << "library:water does not read the lights' records";
    EXPECT_TRUE(mesh.submeshes[0].shaderProperties & shaderPropertyBit(30)) << "not the sixteen-light tier";
    shoot("shaders/BlockWater", block, width, height);
    EXPECT_EQ(mesh.submeshes[0].slotLightRecords.first, -1) << "the fork without the define reads the records";

    // the lights matter here: without the point and spot lights the water differs widely
    for (auto& light : lights)
        light->setEnabled(false);
    shoot("library:water", sunOnly, width, height);

    int largest = 0;
    const size_t differing = differingPixels(records, block, 2, largest);
    int lightsLargest = 0;
    const size_t lit = differingPixels(records, sunOnly, 8, lightsLargest);
    std::printf("[gpu] water light records on %s: %zu pixels over 2 apart from the block form (largest %d); "
                "the twelve lights change %zu pixels by over 8\n",
                HeadlessContext::getBackendName(), differing, largest, lit);
    writePng(getOutputDirectory() + "/water_light_records.png", records, width, height);
    writePng(getOutputDirectory() + "/water_light_block.png", block, width, height);
    ASSERT_GT(lit, 2000u) << "the point and spot lights do not light the water: the test proves nothing";
    EXPECT_EQ(differing, 0u) << "the records form draws another water than the block form";
    EXPECT_LE(largest, 2);
    EXPECT_NO_ENGINE_ERRORS();
}

// past 128 scene lights the records take a second row of u_lightRecords: the water's lights are
// scene lights 131 to 135 (130 small ones far away come first), and it still draws the block
// form's image; with no light changing the frame's records are not uploaded again, and moving a
// light uploads them
TEST_F(WaterLightRecordsGpu, LightsPastTheFirstRowAndUploadsOnlyOnChange){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 9.0f, 10.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();
    sun.setShadows(false);

    for (int i = 0; i < 130; i++)
        addLight(LightType::POINT, Vector3(400.0f + 3.0f * (float)i, 1.0f, 400.0f), Vector3(1.0f, 1.0f, 1.0f), 1.0f, 1.0f);
    std::vector<Light*> near;
    for (int i = 0; i < 5; i++){
        const float x = -4.0f + 2.0f * (float)i;
        near.push_back(&addLight(LightType::POINT, Vector3(x, 0.8f, (i % 2 == 0) ? -1.5f : 1.5f),
                                 Vector3(0.3f + 0.15f * (float)i, 1.0f - 0.15f * (float)i, 0.6f), 4.0f, 3.5f));
    }

    Primitive water(&scene);
    water.createPlane(16.0f, 16.0f);
    RenderSystem* render = scene.getSystem<RenderSystem>().get();

    int width = 0, height = 0;
    std::vector<uint8_t> records, block;
    water.setMaterial(litWater("library:water"));
    ASSERT_TRUE(this->render()) << "the scene never finished loading";
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(water.getEntity());
    ASSERT_NE(mesh.submeshes[0].slotLightRecords.first, -1) << "library:water does not read the lights' records";
    EXPECT_GE(render->getLightRecordsTexture().getHeight(), 2) << "the records take one row";
    records = readRecordsTarget(width, height);

    // nothing changes: no upload; a light moves: one upload a frame it moves in
    const uint64_t uploads = render->lightRecordUploads;
    GpuEnvironment::drawFrames(3);
    EXPECT_EQ(render->lightRecordUploads, uploads) << "the records were uploaded again with no light changing";
    near[0]->setPosition(-4.0f, 0.9f, -1.5f);
    GpuEnvironment::drawFrames(2);
    EXPECT_GE(render->lightRecordUploads, uploads + 1) << "a moved light's record was not uploaded";
    EXPECT_LE(render->lightRecordUploads, uploads + 2);
    near[0]->setPosition(-4.0f, 0.8f, -1.5f);
    GpuEnvironment::drawFrames(2);

    water.setMaterial(litWater("shaders/BlockWater"));
    ASSERT_TRUE(this->render()) << "the scene never finished loading";
    EXPECT_EQ(mesh.submeshes[0].slotLightRecords.first, -1);
    block = readRecordsTarget(width, height);

    int largest = 0;
    const size_t differing = differingPixels(records, block, 2, largest);
    std::printf("[gpu] water light records past 128 lights on %s: %zu pixels over 2 apart from the block form (largest %d)\n",
                HeadlessContext::getBackendName(), differing, largest);
    EXPECT_EQ(differing, 0u) << "the records form draws another water than the block form past the first row";
    EXPECT_NO_ENGINE_ERRORS();
}
