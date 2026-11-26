// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Area Lights and IES Profiles in the built-in Lit (spec S99 AC8, R41): a Rectangle and a Disc on
// the Standard and High Lit Quality, per object and from screen tiles, a shadowed Area Light and a
// spot shaped by a synthetic IES Profile (engine/tests/core/fixtures/ies/oval_beam.ies). A scene
// without either keeps its variants and makes no profiles texture.

#include "Engine.h"
#include "Scene.h"
#include "component/LightComponent.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "render/LightingTiers.h"
#include "subsystem/RenderSystem.h"
#include "texture/MaterialPbr.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>

namespace AreaLightGpuTest_cpp {

using namespace jasperengine;
using namespace jasperengine::test;

namespace area_light_gpu_test {

    // glcore against its own goldens: exact; another backend (GLES3, Vulkan): the cross-backend bound
    CompareOptions areaLightOptions(){
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

    // the same image from the other light path (screen tiles): the records are the same, the
    // order of the sums may differ in the last bit
    CompareOptions otherPathOptions(){
        CompareOptions options;
        options.pixelThreshold = 2;
        options.maxBadFraction = 0.001;
        options.maxMeanDiff = 0.2;
        return options;
    }

    class AreaLightGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        std::unique_ptr<Primitive> ground;
        std::unique_ptr<Primitive> box;

        void SetUp() override{
            GpuEnvironment::resetView();
            Engine::setPinnedShaderTime(1.5);
            LightingTiers::setTier(LightingTier::HIGH);
            LightingTiers::setLightMode(LightMode::PER_OBJECT);
            scene.setBackgroundColor(0.05f, 0.05f, 0.07f, 1.0f);
            scene.setGlobalIllumination(0.02f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 4.5f, 6.5f);
            camera.setTarget(0.0f, 0.3f, 0.0f);
            camera.activate();

            ground = std::make_unique<Primitive>(&scene);
            ground->createPlane(10.0f, 10.0f);
            ground->setColor(0.8f, 0.8f, 0.8f);
            box = std::make_unique<Primitive>(&scene);
            box->createBox(0.6f, 0.6f, 0.6f);
            box->setPosition(1.2f, 0.3f, 0.6f);
            box->setColor(0.85f, 0.45f, 0.25f);
            for (Primitive* primitive : {ground.get(), box.get()}){
                Material& material = scene.getComponent<MeshComponent>(primitive->getEntity()).submeshes[0].material;
                material.metallicFactor = 0.0f;
                material.roughnessFactor = 0.35f;
            }
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

        uint64_t groundLitDefines() const{
            return scene.getComponent<MeshComponent>(ground->getEntity()).submeshes[0].litDefineBits;
        }

        std::unique_ptr<Light> makeAreaLight(AreaLightShape shape){
            auto light = std::make_unique<Light>(&scene);
            light->setType(LightType::AREA);
            light->setPosition(-0.4f, 1.6f, 0.0f);
            // facing down, tilted towards the camera so the face's highlight shows on the ground
            light->setDirection(0.0f, -1.0f, 0.35f);
            light->setRange(12.0f);
            light->setIntensity(3.0f);
            light->setColor(1.0f, 0.92f, 0.8f);
            light->setAreaShape(shape);
            light->setAreaWidth(2.0f);
            light->setAreaHeight(0.6f);
            light->setAreaRadius(0.6f);
            return light;
        }
    };

}

using namespace area_light_gpu_test;

TEST_F(AreaLightGpu, RectangleAndDiscDrawOnStandardAndHigh){
    JASPER_REQUIRE_GPU();
    const uint64_t areaBit = MaterialPbr::getDefineBit(MaterialPbr::AREA_LIGHTS_DEFINE);
    for (AreaLightShape shape : {AreaLightShape::RECTANGLE, AreaLightShape::DISC}){
        std::unique_ptr<Light> light = makeAreaLight(shape);
        for (LitQuality quality : {LitQuality::STANDARD, LitQuality::HIGH}){
            scene.setLitQuality(quality);
            LightingTiers::setLightMode(LightMode::PER_OBJECT);
            ASSERT_TRUE(render()) << "the scene never finished loading";
            EXPECT_NE(groundLitDefines() & areaBit, 0u) << "the ground's Lit has no Area Light path";
            // the profiles texture (the LTC tables) is made for a scene with an Area Light
            EXPECT_TRUE(scene.getSystem<RenderSystem>()->hasLightProfiles());

            const std::string name = std::string("area_") + (shape == AreaLightShape::DISC ? "disc" : "rectangle") +
                                     (quality == LitQuality::HIGH ? "_high" : "_standard");
            const CompareResult result = compareTargetWithGolden(name, areaLightOptions());
            EXPECT_TRUE(result.passed) << name << ": " << result.message;

            // from screen tiles: the same shading
            LightingTiers::setLightMode(LightMode::TILED);
            ASSERT_TRUE(render()) << "the scene never finished loading";
            const CompareResult tiled = compareTargetWithGolden(name, otherPathOptions());
            EXPECT_TRUE(tiled.passed) << name << " (tiled): " << tiled.message;
        }
        light->setEnabled(false);
        scene.getComponent<LightComponent>(light->getEntity()).intensity = 0.0f;
    }
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(AreaLightGpu, ShadowedAreaLightDrawsItsShadow){
    JASPER_REQUIRE_GPU();
    scene.setLitQuality(LitQuality::HIGH);
    std::unique_ptr<Light> light = makeAreaLight(AreaLightShape::RECTANGLE);
    light->setPosition(0.6f, 2.0f, 0.2f);
    light->setShadows(true);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    // its map drawn again once everything has loaded, so the golden does not depend on the frame
    // the casters' depth programs arrived in (a warm shader cache loads them sooner)
    light->requestShadowUpdate();
    ASSERT_TRUE(render()) << "the scene never finished loading";
    EXPECT_GE(scene.getComponent<LightComponent>(light->getEntity()).shadowMapIndex, 0) << "the Area Light got no shadow map";
    const CompareResult result = compareTargetWithGolden("area_rectangle_shadowed", areaLightOptions());
    EXPECT_TRUE(result.passed) << result.message;
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(AreaLightGpu, IesProfileShapesASpot){
    JASPER_REQUIRE_GPU();
    scene.setLitQuality(LitQuality::HIGH);
    Light spot(&scene);
    spot.setType(LightType::SPOT);
    spot.setPosition(0.0f, 2.5f, 0.0f);
    spot.setDirection(0.0f, -1.0f, 0.0f);
    spot.setConeAngle(150.0f, 170.0f);
    spot.setRange(12.0f);
    spot.setIntensity(6.0f);
    spot.setIesProfile(JASPER_TEST_SOURCE_DIR "/fixtures/ies/oval_beam.ies");
    ASSERT_TRUE(render()) << "the scene never finished loading";

    std::shared_ptr<RenderSystem> renderSystem = scene.getSystem<RenderSystem>();
    EXPECT_EQ(renderSystem->getLightProfileBands(), 1);
    EXPECT_EQ(scene.getComponent<LightComponent>(spot.getEntity()).iesProfileBand, 0);
    EXPECT_NE(groundLitDefines() & MaterialPbr::getDefineBit(MaterialPbr::IES_DEFINE), 0u) << "the ground's Lit has no IES path";
    const CompareResult result = compareTargetWithGolden("ies_spot", areaLightOptions());
    EXPECT_TRUE(result.passed) << result.message;
    EXPECT_NO_ENGINE_ERRORS();
}

// R41: point and spot lights without a profile keep the variants they had, and the renderer
// makes no profiles texture
TEST_F(AreaLightGpu, SceneWithoutThemKeepsItsVariants){
    JASPER_REQUIRE_GPU();
    Light point(&scene);
    point.setType(LightType::POINT);
    point.setPosition(0.0f, 2.0f, 1.0f);
    point.setRange(8.0f);
    point.setIntensity(4.0f);
    Light spot(&scene);
    spot.setType(LightType::SPOT);
    spot.setPosition(-1.0f, 2.5f, 0.0f);
    spot.setDirection(0.0f, -1.0f, 0.0f);
    spot.setRange(8.0f);
    spot.setIntensity(4.0f);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    const uint64_t bits = MaterialPbr::getDefineBit(MaterialPbr::AREA_LIGHTS_DEFINE) | MaterialPbr::getDefineBit(MaterialPbr::IES_DEFINE);
    EXPECT_EQ(groundLitDefines() & bits, 0u);
    EXPECT_FALSE(scene.getSystem<RenderSystem>()->hasLightProfiles());
    EXPECT_EQ(scene.getSystem<RenderSystem>()->getLightProfileBands(), 0);
    EXPECT_NO_ENGINE_ERRORS();
}

}  // namespace AreaLightGpuTest_cpp
