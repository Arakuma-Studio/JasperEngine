// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The distance fogs and a scene without fog render exactly as before the atmosphere
// (docs/specs/S12-atmosphere.md AC1): the goldens atmosphere_legacy_*.png were made by this file
// on the engine before S12 (the oe-port merge base, glcore) and must match to the bit.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "object/environment/Fog.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class AtmosphereLegacyFogGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};
        std::vector<std::unique_ptr<Primitive>> boxes;

        void SetUp() override{
            scene.setBackgroundColor(0.42f, 0.58f, 0.82f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 6.0f, 25.0f);
            camera.setTarget(0.0f, 3.0f, -60.0f);
            camera.setFarClip(500.0f);
            camera.activate();
            Engine::setScene(&scene);
        }

        // the scene's objects, made once the device exists (shaders then compile synchronously)
        void build(){
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(-0.35f, -0.6f, -1.0f);
            sun.setIntensity(1.6f);
            sun.setShadows(false);
            boxes.push_back(std::make_unique<Primitive>(&scene));
            boxes.back()->createPlane(400.0f, 400.0f);
            boxes.back()->setColor(0.35f, 0.42f, 0.30f);
            for (int i = 0; i < 6; i++){
                boxes.push_back(std::make_unique<Primitive>(&scene));
                boxes.back()->createBox(4.0f, 6.0f + 3.0f * (float)i, 4.0f);
                boxes.back()->setPosition((i % 2 ? -1.0f : 1.0f) * (5.0f + 2.0f * (float)i), 3.0f + 1.5f * (float)i, -10.0f - 15.0f * (float)i);
                boxes.back()->setColor(0.7f, 0.66f, 0.6f);
            }
        }

        void TearDown() override{
            Engine::removeScene(&scene);
        }

        bool render(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }

        static CompareOptions exact(){
            CompareOptions options;
            options.pixelThreshold = 0;
            options.maxBadFraction = 0.0;
            options.maxMeanDiff = 0.0;
            return options;
        }
    };

}

TEST_F(AtmosphereLegacyFogGpu, NoFog){
    JASPER_REQUIRE_GPU();
    build();
    ASSERT_TRUE(render());
    EXPECT_GOLDEN_OPTIONS("atmosphere_legacy_none", exact());
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(AtmosphereLegacyFogGpu, LinearFog){
    JASPER_REQUIRE_GPU();
    build();
    Fog fog(&scene);
    fog.setType(FogType::LINEAR);
    fog.setColor(0.7f, 0.72f, 0.78f);
    fog.setLinearStartEnd(10.0f, 90.0f);
    ASSERT_TRUE(render());
    EXPECT_GOLDEN_OPTIONS("atmosphere_legacy_linear", exact());
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(AtmosphereLegacyFogGpu, ExponentialFog){
    JASPER_REQUIRE_GPU();
    build();
    Fog fog(&scene);
    fog.setType(FogType::EXPONENTIAL);
    fog.setColor(0.7f, 0.72f, 0.78f);
    fog.setDensity(0.02f);
    ASSERT_TRUE(render());
    EXPECT_GOLDEN_OPTIONS("atmosphere_legacy_exponential", exact());
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(AtmosphereLegacyFogGpu, ExponentialSquaredFog){
    JASPER_REQUIRE_GPU();
    build();
    Fog fog(&scene);
    fog.setType(FogType::EXPONENTIALSQUARED);
    fog.setColor(0.7f, 0.72f, 0.78f);
    fog.setDensity(0.015f);
    ASSERT_TRUE(render());
    EXPECT_GOLDEN_OPTIONS("atmosphere_legacy_exponential_squared", exact());
    EXPECT_NO_ENGINE_ERRORS();
}
