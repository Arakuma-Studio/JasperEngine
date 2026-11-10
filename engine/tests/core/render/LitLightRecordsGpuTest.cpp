// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The built-in Lit past GLES3 / WebGL2's 224 fragment uniform vectors or 8 blocks reads its lights,
// capsules and 2D lights from the frame's u_lightRecords (ShaderVariantCompiler::compile,
// includes/lights.glsl LIGHTS_FS_RECORDS: docs/superpowers/specs/2026-11-04-water-reflections-design.md,
// Follow-ups). Its goldens were captured with the blocks (the capsules' 67 vectors, the 2D lights'
// 50): a ground lit by a shadowed sun with S57's capsule and contact shadows, twelve point lights
// (the sixteen-light tier) and a 2D light, per object and from screen tiles.

#include "Engine.h"
#include "Scene.h"
#include "component/CapsuleShadowCasterComponent.h"
#include "component/LightComponent.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/CapsuleShadowCaster.h"
#include "object/Light.h"
#include "object/Light2D.h"
#include "object/Primitive.h"
#include "pool/ShaderPool.h"
#include "render/LightingTiers.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace lit_light_records_gpu_test {

    // glcore against its own goldens: exact; another backend: the cross-backend bound
    CompareOptions litRecordsOptions(){
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

    class LitLightRecordsGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override{
            GpuEnvironment::resetView();
            Engine::setPinnedShaderTime(1.5);
            LightingTiers::setTier(LightingTier::HIGH);
            scene.setBackgroundColor(0.12f, 0.12f, 0.16f, 1.0f);
            scene.setMaxLightsPerObject(16);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 6.0f, 8.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
            LightingTiers::setLightMode(LightMode::AUTOMATIC);
            LightingTiers::resetTier();
            Engine::setPinnedShaderTime(-1.0);
        }

        bool render(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }
    };

}

using namespace lit_light_records_gpu_test;

TEST_F(LitLightRecordsGpu, CapsulesContactAnd2DLightsDrawTheBlocksImage){
    JASPER_REQUIRE_GPU();
    Primitive ground(&scene);
    ground.createPlane(12.0f, 12.0f);
    ground.setColor(0.85f, 0.85f, 0.85f);
    Primitive body(&scene);
    body.createBox(0.6f, 1.8f, 0.4f);
    body.setPosition(0.0f, 0.9f, 0.0f);
    body.setColor(0.8f, 0.4f, 0.2f);

    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.5f, -1.0f, -0.3f);
    sun.setIntensity(2.0f);
    sun.setShadows(true);
    LightComponent& sunLight = scene.getComponent<LightComponent>(sun.getEntity());
    sunLight.contactShadows = true;
    sunLight.contactShadowLength = 0.4f;
    sunLight.contactShadowThickness = 0.1f;
    sunLight.contactShadowSampleCount = 16;

    scene.addComponent<CapsuleShadowCasterComponent>(body.getEntity(), CapsuleShadowCasterComponent());
    CapsuleShadowCaster caster(&scene, body.getEntity());
    caster.setCastShadows(CapsuleCastShadows::CAPSULES_ONLY);
    const int torso = caster.addCapsule("", 0.3f, 0.6f);
    caster.setCapsuleOffset(torso, Vector3(0.0f, 0.35f, 0.0f));
    for (float side : {-0.15f, 0.15f}){
        const int leg = caster.addCapsule("", 0.12f, 0.7f);
        caster.setCapsuleOffset(leg, Vector3(side, -0.45f, 0.0f));
    }
    scene.setCapsuleShadowsEnabled(true);
    scene.setCapsuleShadowSoftness(0.25f);

    std::vector<std::unique_ptr<Light>> points;
    for (int i = 0; i < 12; i++){
        points.push_back(std::make_unique<Light>(&scene));
        Light& light = *points.back();
        light.setType(LightType::POINT);
        light.setPosition(-5.0f + 0.9f * (float)i, 0.5f, (i % 2 == 0) ? -2.5f : 2.5f);
        light.setRange(2.5f);
        light.setIntensity(2.5f);
        light.setColor(0.3f + 0.05f * (float)i, 1.0f - 0.06f * (float)i, 0.6f);
        light.setShadows(false);
    }
    Light2D light2d(&scene);
    // the golden is drawn with the 2D light at height 0 (pure radial)
    light2d.setHeight(0.0f);
    light2d.setPosition(1.5f, 0.0f, 0.0f);
    light2d.setRange(4.0f);
    light2d.setIntensity(1.5f);
    light2d.setColor(1.0f, 0.5f, 0.2f);

    for (LightMode mode : {LightMode::PER_OBJECT, LightMode::TILED}){
        LightingTiers::setLightMode(mode);
        ASSERT_TRUE(render()) << "the scene never finished loading";
        const RenderSystem::LightingStats stats = scene.getSystem<RenderSystem>()->getLightingStats();
        EXPECT_TRUE(stats.capsuleShadows);
        EXPECT_TRUE(stats.contactShadows);
        const MeshComponent& mesh = scene.getComponent<MeshComponent>(ground.getEntity());
        // (per object over desktop GL's 256 fragment vectors in its blocks; from screen tiles, and on
        // Vulkan, which has no such total, it keeps them)
        if (mode == LightMode::PER_OBJECT && std::strcmp(HeadlessContext::getBackendName(), "glcore") == 0){
            EXPECT_NE(mesh.submeshes[0].slotLightRecords.first, -1) << "the ground's Lit does not read the light records";
        }
        const std::string name = mode == LightMode::TILED ? "lit_records_tiled" : "lit_records_per_object";
        const CompareResult result = compareTargetWithGolden(name, litRecordsOptions());
        EXPECT_TRUE(result.passed) << result.message;
    }

    // the capsules' shadow, plain: the point lights off (the variant keeps them), the sun's shadow
    // of the capsules against Intensity 0, which keeps the variant too
    LightingTiers::setLightMode(LightMode::PER_OBJECT);
    for (auto& light : points)
        light->setEnabled(false);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    std::vector<uint8_t> withCapsules, without;
    int width = 0, height = 0;
    {
        Framebuffer& target = GpuEnvironment::target();
        width = (int)target.getWidth();
        height = (int)target.getHeight();
        readTexture(target.getRender().getColorTexture(), width, height, withCapsules);
    }
    scene.setCapsuleShadowIntensity(0.0f);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    readTexture(GpuEnvironment::target().getRender().getColorTexture(), width, height, without);
    size_t darker = 0;
    int largest = 0;
    for (size_t i = 0; i + 3 < withCapsules.size() && i + 3 < without.size(); i += 4){
        int most = 0;
        for (int c = 0; c < 3; c++)
            most = std::max(most, (int)without[i + c] - (int)withCapsules[i + c]);
        largest = std::max(largest, most);
        darker += most > 24 ? 1 : 0;
    }
    std::printf("[gpu] lit records capsule shadow on %s: %zu pixels darker by over 24 (largest %d)\n",
                HeadlessContext::getBackendName(), darker, largest);
    EXPECT_GT(darker, (size_t)(width * height) / 1000) << "the capsules cast no shadow worth the name";
    EXPECT_GT(largest, 40);
    EXPECT_NO_ENGINE_ERRORS();
}

// S78 R4: the Light Steps reach a lit variant that reads its 2D lights from the records (the count
// is the records tail's atlasInfo.z, as the block's): at 1 step every lit pixel of the 2D light
// takes its full light, so no pixel darkens and the light's falloff brightens
TEST_F(LitLightRecordsGpu, LightStepsBandThe2DLightsOfTheRecords){
    JASPER_REQUIRE_GPU();
    Primitive ground(&scene);
    ground.createPlane(12.0f, 12.0f);
    ground.setColor(0.85f, 0.85f, 0.85f);
    Primitive body(&scene);
    body.createBox(0.6f, 1.8f, 0.4f);
    body.setPosition(0.0f, 0.9f, 0.0f);
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.5f, -1.0f, -0.3f);
    sun.setIntensity(1.0f);
    sun.setShadows(true);
    scene.getComponent<LightComponent>(sun.getEntity()).contactShadows = true;
    scene.addComponent<CapsuleShadowCasterComponent>(body.getEntity(), CapsuleShadowCasterComponent());
    CapsuleShadowCaster caster(&scene, body.getEntity());
    caster.setCastShadows(CapsuleCastShadows::CAPSULES_ONLY);
    caster.addCapsule("", 0.3f, 0.6f);
    scene.setCapsuleShadowsEnabled(true);
    std::vector<std::unique_ptr<Light>> points;
    for (int i = 0; i < 12; i++){
        points.push_back(std::make_unique<Light>(&scene));
        points.back()->setType(LightType::POINT);
        points.back()->setPosition(-5.0f + 0.9f * (float)i, 0.5f, -5.0f);
        points.back()->setRange(1.0f);
        points.back()->setShadows(false);
    }
    Light2D light2d(&scene);
    light2d.setPosition(1.5f, 0.0f, 0.0f);
    light2d.setRange(4.0f);
    light2d.setIntensity(1.0f);
    light2d.setFalloff(2.0f);
    light2d.setColor(1.0f, 0.5f, 0.2f);
    LightingTiers::setLightMode(LightMode::PER_OBJECT);

    ASSERT_TRUE(render()) << "the scene never finished loading";
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(ground.getEntity());
    if (std::strcmp(HeadlessContext::getBackendName(), "glcore") == 0){
        ASSERT_NE(mesh.submeshes[0].slotLightRecords.first, -1) << "the ground's Lit does not read the light records";
    }
    const int width = (int)GpuEnvironment::target().getWidth();
    const int height = (int)GpuEnvironment::target().getHeight();
    std::vector<uint8_t> smooth, stepped;
    readTexture(GpuEnvironment::target().getRender().getColorTexture(), width, height, smooth);

    scene.setLight2DSteps(1);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    ASSERT_NE(mesh.submeshes[0].shaderProperties & ShaderPool::getLight2DStepsProperty(), 0u);
    if (std::strcmp(HeadlessContext::getBackendName(), "glcore") == 0){
        ASSERT_NE(mesh.submeshes[0].slotLightRecords.first, -1) << "the stepped variant reads the light records too";
    }
    readTexture(GpuEnvironment::target().getRender().getColorTexture(), width, height, stepped);
    size_t darker = 0, brighter = 0;
    for (size_t i = 0; i + 3 < smooth.size() && i + 3 < stepped.size(); i += 4){
        const int before = smooth[i];
        const int after = stepped[i];
        darker += after + 2 < before ? 1 : 0;
        brighter += after > before + 8 ? 1 : 0;
    }
    std::printf("[gpu] lit records light steps on %s: %zu pixels brighter by over 8, %zu darker\n",
                HeadlessContext::getBackendName(), brighter, darker);
    EXPECT_EQ(darker, 0u);
    EXPECT_GT(brighter, (size_t)(width * height) / 100) << "the steps did not reach the records' 2D light";
    EXPECT_NO_ENGINE_ERRORS();
}
