// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S94 F28 (AC28): a Pixel Perfect Camera sets its camera's span itself (S41 R3), also over an
// Orthographic Size: the bounds a frame draws with are the ones it gives without the Size. And
// without one, the Size's bounds are what a frame draws with (the camera loop applies a Size set
// between canvas changes). glcore and Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "object/Camera.h"
#include "object/PixelPerfectCamera.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

namespace CameraGpuTest_cpp {

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class CameraGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override{
            GpuEnvironment::setView(320, 180, 320, 180, Scaling::STRETCH);
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            camera.setType(CameraType::CAMERA_ORTHO);
            camera.setPosition(0.0f, 0.0f, 1.0f);
            camera.activate();
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
            Engine::setPixelPerfectForcedScale(0);
            GpuEnvironment::resetView();
        }

        CameraComponent& component(){ return scene.getComponent<CameraComponent>(camera.getEntity()); }

        void frames(){
            Scene* current = &scene;
            ASSERT_TRUE(GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); }));
            GpuEnvironment::drawFrames(2);
        }
    };

}

TEST_F(CameraGpu, SizeSetBetweenCanvasChangesDraws){
    JASPER_REQUIRE_GPU();
    frames();
    camera.setOrthographicSize(4.0f);
    frames();
    const CameraComponent& c = component();
    EXPECT_FLOAT_EQ(c.topClip, 4.0f);
    EXPECT_FLOAT_EQ(c.bottomClip, -4.0f);
    EXPECT_NEAR(c.rightClip, 4.0f * 320.0f / 180.0f, 1e-4f);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(CameraGpu, PixelPerfectWinsOverOrthographicSize){
    JASPER_REQUIRE_GPU();
    PixelPerfectCamera ppc(&scene, camera.getEntity());
    ppc.setReferenceResolution(Vector2(160.0f, 90.0f));
    frames();
    const CameraComponent& c = component();
    const float left = c.leftClip, right = c.rightClip, bottom = c.bottomClip, top = c.topClip;
    ASSERT_GT(right - left, 10.0f) << "the Pixel Perfect Camera's span is in pixels";

    camera.setOrthographicSize(4.0f);
    scene.getSystem<RenderSystem>()->updateCameraSizeForCanvas(camera.getEntity(), 320.0f, 180.0f);
    frames();
    EXPECT_FLOAT_EQ(c.leftClip, left);
    EXPECT_FLOAT_EQ(c.rightClip, right);
    EXPECT_FLOAT_EQ(c.bottomClip, bottom);
    EXPECT_FLOAT_EQ(c.topClip, top);
    EXPECT_NO_ENGINE_ERRORS();
}

}  // namespace CameraGpuTest_cpp
