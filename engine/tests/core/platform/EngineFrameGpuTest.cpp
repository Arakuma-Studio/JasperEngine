// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The frame after the app comes back (S95 REQ-17): its measured delta holds the time spent in the
// background, which no game clock may jump by. It is clamped to the fixed step, as after a hold.

#include "Engine.h"
#include "GpuEnvironment.h"

#include "sokol_time.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <functional>

namespace jasperengine {

    struct EngineTestAccess {
        // the last frame started `seconds` earlier (not before the clock's start: 1 tick at least)
        static void rewindFrameClock(double seconds){
            const uint64_t ticks = (uint64_t)(seconds / stm_sec(1));
            Engine::lastTime = Engine::lastTime > ticks ? Engine::lastTime - ticks : 1;
        }
    };

}

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const char* callbackTag = "core-tests-engine-frame";

}

TEST(EngineFrameGpu, ResumeClampsDelta){
    JASPER_REQUIRE_GPU();
    const float updateTime = Engine::getUpdateTime();
    GpuEnvironment::drawFrames(2);

    Engine::systemPause();
    GpuEnvironment::drawFrames(1);
    // five seconds in the background, then the first frame back
    EngineTestAccess::rewindFrameClock(5.0);
    Engine::systemResume();

    double delta = -1.0;
    Engine::onUpdate.add(callbackTag, std::function<void()>([&delta](){
        if (delta < 0.0) delta = Engine::getDeltatime();
    }));
    GpuEnvironment::drawFrames(1);
    Engine::onUpdate.remove(callbackTag);

    ASSERT_GE(delta, 0.0);
    EXPECT_LE(delta, updateTime + 1e-9);
}

TEST(EngineFrameGpu, ViewRestoreClampsDelta){
    JASPER_REQUIRE_GPU();
    const float updateTime = Engine::getUpdateTime();
    GpuEnvironment::drawFrames(2);

    // an Android return after the context went (LOW_MEMORY without a window): the frame that
    // brings the view back measured the whole absence
    Engine::systemViewDestroyed();
    EngineTestAccess::rewindFrameClock(5.0);
    Engine::systemViewLoaded();
    Engine::systemViewChanged();

    double delta = -1.0;
    Engine::onUpdate.add(callbackTag, std::function<void()>([&delta](){
        if (delta < 0.0) delta = Engine::getDeltatime();
    }));
    GpuEnvironment::drawFrames(1);
    Engine::onUpdate.remove(callbackTag);

    ASSERT_GE(delta, 0.0);
    EXPECT_LE(delta, updateTime + 1e-9);
}
