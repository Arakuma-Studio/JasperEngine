// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Input's windows in the engine's real frame loop (Engine::systemDraw): a frame that runs its
// update latches the events, a paused frame or a held simulation keeps them pending, and the
// fixed steps see each edge exactly once (docs/specs/S22-gameplay.md R1, R3, D4).

#include "Engine.h"
#include "GpuEnvironment.h"
#include "Input.h"

#include <gtest/gtest.h>

#include <functional>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const char* callbackTag = "core-tests-input-frame-loop";

    class InputFrameLoopGpu : public ::testing::Test {
    protected:
        float updateTime = 0.0f;

        void SetUp() override{
            updateTime = Engine::getUpdateTime();
            Input::resetFrameState();
        }

        void TearDown() override{
            Engine::onUpdate.remove(callbackTag);
            Engine::onFixedUpdate.remove(callbackTag);
            Engine::setFixedDeltatime(0.0);
            Engine::setUpdateTime(updateTime);
            Engine::setSimulationHeld(false);
            // a failed expectation may have left the game paused
            Engine::systemResume();
            Input::resetFrameState();
        }
    };

}

TEST_F(InputFrameLoopGpu, PausedFramesKeepPending){
    JASPER_REQUIRE_GPU();
    std::vector<bool> downs;
    Engine::onUpdate.add(callbackTag, std::function<void()>([&](){ downs.push_back(Input::getKeyDown(D_KEY_P)); }));
    GpuEnvironment::drawFrames(1);

    Engine::systemPause();
    Engine::systemKeyDown(D_KEY_P, false, 0);
    Engine::systemKeyUp(D_KEY_P, false, 0);
    GpuEnvironment::drawFrames(3);
    EXPECT_FALSE(Input::getKeyDown(D_KEY_P));
    Engine::systemResume();

    Engine::setSimulationHeld(true);
    GpuEnvironment::drawFrames(3);
    EXPECT_FALSE(Input::getKeyDown(D_KEY_P));
    Engine::setSimulationHeld(false);

    downs.clear();
    GpuEnvironment::drawFrames(2);
    // the first running frame has the tap, the next one not
    EXPECT_EQ(downs, std::vector<bool>({true, false}));
}

TEST_F(InputFrameLoopGpu, FixedStepsSeeEachEdgeOnce){
    JASPER_REQUIRE_GPU();
    // two frames per step: frames alternate between 0 and 1 fixed steps
    Engine::setUpdateTime(1.0f / 30.0f);
    Engine::setFixedDeltatime(1.0 / 60.0);
    int stepsThisFrame = 0;
    int seenInSteps = 0;
    Engine::onFixedUpdate.add(callbackTag, std::function<void()>([&](){
        stepsThisFrame++;
        if (Input::getKeyDown(D_KEY_SPACE)) seenInSteps++;
    }));
    // settle the accumulator so the next frame runs no step
    for (int i = 0; i < 4; i++){
        stepsThisFrame = 0;
        GpuEnvironment::drawFrames(1);
        if (stepsThisFrame > 0)
            break;
    }

    Engine::systemKeyDown(D_KEY_SPACE, false, 0);
    Engine::systemKeyUp(D_KEY_SPACE, false, 0);
    stepsThisFrame = 0;
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(stepsThisFrame, 0);
    EXPECT_EQ(seenInSteps, 0);
    EXPECT_TRUE(Input::getKeyDown(D_KEY_SPACE));

    // the step of the next frame sees it, and no later step
    for (int i = 0; i < 6; i++)
        GpuEnvironment::drawFrames(1);
    EXPECT_EQ(seenInSteps, 1);

    // three steps a frame: only the first sees a tap
    Engine::setFixedDeltatime(0.11);
    seenInSteps = 0;
    Engine::systemKeyDown(D_KEY_SPACE, false, 0);
    Engine::systemKeyUp(D_KEY_SPACE, false, 0);
    stepsThisFrame = 0;
    GpuEnvironment::drawFrames(1);
    EXPECT_GE(stepsThisFrame, 3);
    EXPECT_EQ(seenInSteps, 1);
}
