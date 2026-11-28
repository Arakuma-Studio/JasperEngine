// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The frame after the app comes back (S95 REQ-17): its measured delta holds the time spent in the
// background, which no game clock may jump by. It is clamped to the fixed step, as after a hold.
// And a loading thread's commits after the view came back: each batch still runs.

#include "Engine.h"
#include "GpuEnvironment.h"
#include "render/SystemRender.h"

#include "sokol_time.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

namespace jasperengine {

    struct EngineTestAccess {
        // the last frame started `seconds` earlier (not before the clock's start: 1 tick at least)
        static void rewindFrameClock(double seconds){
            const uint64_t ticks = (uint64_t)(seconds / stm_sec(1));
            Engine::lastTime = Engine::lastTime > ticks ? Engine::lastTime - ticks : 1;
        }
    };

}

namespace EngineFrameGpuTest_cpp {

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

// A loading thread commits twice in a row after the view came back: the second commit waits for
// the frame that runs the first batch, as before the view went. A permit the restore added let it
// commit at once, and the commit dropped the batch no frame had run yet (a preview's shaders, never
// made: TextureDataPoolThreadsGpu waited forever for a model whose variants a batch had lost).
TEST(EngineFrameGpu, AfterAViewRestoreEveryCommittedBatchRuns){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::drawFrames(2);
    Engine::systemViewDestroyed();
    Engine::systemViewLoaded();
    Engine::systemViewChanged();
    GpuEnvironment::drawFrames(2);

    std::atomic<int> ran{0};
    std::atomic<bool> secondCommit{false};
    std::atomic<bool> done{false};
    std::thread loader([&](){
        for (int batch = 0; batch < 2; batch++){
            Engine::AsyncThreadScope scope;
            SystemRender::addQueueCommand([](void* counter){ static_cast<std::atomic<int>*>(counter)->fetch_add(1); }, &ran);
            secondCommit.store(batch == 1);
        }
        done.store(true);
    });
    // no frame until the second commit had its chance to overtake the first batch
    while (!secondCommit.load())
        std::this_thread::yield();
    const auto overtake = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (!done.load() && std::chrono::steady_clock::now() < overtake)
        std::this_thread::yield();
    for (int frame = 0; frame < 600 && !done.load(); frame++)
        GpuEnvironment::drawFrames(1);
    loader.join();
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(ran.load(), 2) << "a committed batch never ran";
}

}  // namespace EngineFrameGpuTest_cpp
