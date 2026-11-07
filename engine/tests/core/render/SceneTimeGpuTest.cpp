// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The scene clock through the engine's frames (docs/superpowers/specs/2026-11-04-water-reflections-design.md
// 6.1): Engine::runFrame adds each frame's scaled delta to it, as to the game time, and a paused
// scene holds it.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"

#include "GpuEnvironment.h"

#include <gtest/gtest.h>

#include <cstdint>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class SceneTimeGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override{
            Engine::setPinnedShaderTime(-1.0);
            camera.activate();
            Engine::setScene(&scene);
            Engine::setFixedDeltatime(0.02);
            Engine::setTimeScale(0.5f);
        }

        void TearDown() override{
            Engine::setTimeScale(1.0f);
            Engine::setFixedDeltatime(0.0);
            Engine::removeScene(&scene);
        }
    };

}

TEST_F(SceneTimeGpu, RunFrameAddsTheScaledDelta){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::drawFrames(2);
    const double sceneStart = scene.getSceneTime();
    const double gameStart = Engine::getGameTime();
    const uint64_t framesStart = Engine::getGameFrames();
    GpuEnvironment::drawFrames(10);
    const uint64_t frames = Engine::getGameFrames() - framesStart;
    ASSERT_GT(frames, 0u) << "no frame ran its update";
    // a fixed 0.02 s frame at time scale 0.5
    EXPECT_NEAR(scene.getSceneTime() - sceneStart, (double)frames * 0.01, 1e-6);
    EXPECT_NEAR(scene.getSceneTime() - sceneStart, Engine::getGameTime() - gameStart, 1e-6);

    // a paused scene holds its clock while the frames run on
    scene.setSceneTimePaused(true);
    const double held = scene.getSceneTime();
    GpuEnvironment::drawFrames(5);
    EXPECT_DOUBLE_EQ(scene.getSceneTime(), held);
    scene.setSceneTimePaused(false);
}
