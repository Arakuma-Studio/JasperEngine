// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S94 F27, F28 (AC28): an orthographic camera's Orthographic Size centres its bounds, +-size up and
// down and +-size x aspect sideways, and follows each canvas (1920 x 1080, then 1080 x 1920) ahead
// of Auto Resize; 0 leaves the bounds to Auto Resize as before; a bound set by hand turns both off
// (Camera's setters, R46). Pixel Perfect precedence is CameraGpu's (it needs the renderer's frame).

#include "Scene.h"
#include "component/CameraComponent.h"
#include "object/Camera.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

using namespace jasperengine;

namespace {

    class CameraTest : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override{
            camera.setType(CameraType::CAMERA_ORTHO);
        }

        CameraComponent& component(){ return scene.getComponent<CameraComponent>(camera.getEntity()); }

        void canvas(float width, float height){
            scene.getSystem<RenderSystem>()->updateCameraSizeForCanvas(camera.getEntity(), width, height);
        }
    };

}

TEST_F(CameraTest, OrthographicSizeFollowsAspect){
    camera.setOrthographicSize(5.0f);
    canvas(1920.0f, 1080.0f);
    const CameraComponent& c = component();
    EXPECT_FLOAT_EQ(c.topClip, 5.0f);
    EXPECT_FLOAT_EQ(c.bottomClip, -5.0f);
    EXPECT_FLOAT_EQ(c.rightClip, 5.0f * 1920.0f / 1080.0f);
    EXPECT_FLOAT_EQ(c.leftClip, -5.0f * 1920.0f / 1080.0f);
    EXPECT_TRUE(c.needUpdate);

    canvas(1080.0f, 1920.0f);
    EXPECT_FLOAT_EQ(c.topClip, 5.0f);
    EXPECT_FLOAT_EQ(c.rightClip, 5.0f * 1080.0f / 1920.0f);
    EXPECT_FLOAT_EQ(c.leftClip, -5.0f * 1080.0f / 1920.0f);

    // without Auto Resize too: the Size follows the aspect, as Unity's
    camera.setAutoResize(false);
    canvas(1000.0f, 500.0f);
    EXPECT_FLOAT_EQ(c.rightClip, 10.0f);
    EXPECT_FLOAT_EQ(c.topClip, 5.0f);

    // off: Auto Resize's pixel bounds of before
    camera.setAutoResize(true);
    camera.setOrthographicSize(0.0f);
    canvas(320.0f, 180.0f);
    EXPECT_FLOAT_EQ(c.leftClip, 0.0f);
    EXPECT_FLOAT_EQ(c.rightClip, 320.0f);
    EXPECT_FLOAT_EQ(c.topClip, 180.0f);
}

// a perspective or UI camera keeps its bounds: the Size is an orthographic camera's
TEST_F(CameraTest, OrthographicSizeIsOrthographicOnly){
    camera.setType(CameraType::CAMERA_UI);
    camera.setOrthographicSize(5.0f);
    canvas(320.0f, 180.0f);
    EXPECT_FLOAT_EQ(component().rightClip, 320.0f);
    EXPECT_FLOAT_EQ(component().topClip, 180.0f);
}

// R46: a bound set by hand stays (Auto Resize and the Size go off), a negative Size is refused
TEST_F(CameraTest, ClipSetterTurnsSizeAndAutoResizeOff){
    camera.setOrthographicSize(5.0f);
    canvas(200.0f, 100.0f);
    camera.setTopClip(3.0f);
    EXPECT_FLOAT_EQ(camera.getOrthographicSize(), 0.0f);
    EXPECT_FALSE(camera.isAutoResize());
    canvas(100.0f, 200.0f);
    EXPECT_FLOAT_EQ(component().topClip, 3.0f);
    EXPECT_FLOAT_EQ(component().rightClip, 10.0f);

    camera.setOrthographicSize(-1.0f);
    EXPECT_FLOAT_EQ(camera.getOrthographicSize(), 0.0f);
}
