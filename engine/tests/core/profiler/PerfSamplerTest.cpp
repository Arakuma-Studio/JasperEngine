// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Performance sampler (docs/specs/release-performance-and-gpu-quirks.md P1, AC1-AC8): gameplay
// frames into a fixed histogram, a second at a time committed or discarded for its cause, the frame
// thread's CPU from its thread clock, windows handed to the game, and its cost on and off. The
// timelines run on the sampler's own clock (tickForTest), so no test waits for wall seconds.

#include "profiler/FrameTimes.h"
#include "profiler/PerfSampler.h"
#include "profiler/ThreadCpuClock.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace PerfSamplerTest_cpp {

using namespace jasperengine;

namespace {

    using Clock = std::chrono::steady_clock;

    bool perfTestLoading = false;
    bool perfTestLoadingProbe(){ return perfTestLoading; }

    class PerfSamplerTest : public ::testing::Test {
    protected:
        void SetUp() override{
            PerfSampler::resetSession();
            perfTestLoading = false;
            PerfSampler::setLoadingProbeForTest(&perfTestLoadingProbe);
        }
        void TearDown() override{
            PerfSampler::onWindow.remove("perf_test");
            PerfSampler::resetSession();
        }

        // one second on the sampler's clock: `frames` frames of `ms`, then the tick
        static void second(int frames = 60, float ms = 16.0f){
            for (int i = 0; i < frames; i++)
                FrameTimes::recordSample(ms, ms);
            PerfSampler::tickForTest(1.0);
        }

        static double reason(const PerfSummary& summary, const std::string& name){
            for (const auto& [key, seconds] : summary.excludedReasons)
                if (key == name) return seconds;
            return -1.0;
        }
    };

    void expectWithin(double measured, double budget, const char* what){
        const double slack = testsupport::perfSlack();
        std::printf("[bench] %s: %.3f (budget %.3f)\n", what, measured, budget);
        if (slack > 0.0){
            EXPECT_LE(measured, budget * slack) << what;
        }
    }

}

// AC1: 600 frames of 16 ms with 60 of 33 ms
TEST_F(PerfSamplerTest, PercentilesFromFixedBins){
    PerfSampler::setSampling(true);
    for (int s = 0; s < 10; s++){
        for (int i = 0; i < 54; i++)
            FrameTimes::recordSample(16.0f, 12.0f);
        for (int i = 0; i < 6; i++)
            FrameTimes::recordSample(33.0f, 30.0f);
        PerfSampler::tickForTest(1.0);
    }
    const PerfSummary summary = PerfSampler::summary();
    EXPECT_EQ(summary.frames, 600u);
    EXPECT_DOUBLE_EQ(summary.seconds, 10.0);
    EXPECT_GE(summary.frameMsP50, 15.75);
    EXPECT_LE(summary.frameMsP50, 16.25);
    EXPECT_GE(summary.frameMsP90, 32.75);
    EXPECT_LE(summary.frameMsP90, 33.25);
    EXPECT_NEAR(summary.fpsP10, 1000.0 / 33.0, 0.3);
    EXPECT_NEAR(summary.fpsAvg, 60.0, 1e-9);
    EXPECT_FLOAT_EQ((float)summary.frameMsMax, 33.0f);
    EXPECT_NEAR(summary.cpuMsAvg, (540 * 12.0 + 60 * 30.0) / 600.0, 1e-6);
    EXPECT_EQ(summary.hitches, 0u);
    const PerfSummary withBins = PerfSampler::summary(true);
    ASSERT_EQ(withBins.histogramCounts.size(), (size_t)PerfSampler::BINS);
    EXPECT_EQ(withBins.histogramCounts[(size_t)PerfSampler::binOf(16.0f)], 540u);
    EXPECT_FLOAT_EQ(withBins.histogramEdgesMs[100], 50.0f);
    EXPECT_FLOAT_EQ(withBins.histogramEdgesMs[200], 250.0f);
}

// AC2: pause 3 s, resume, the 2 s after it, a scene load, a loading second, a 1.2 s frame
TEST_F(PerfSamplerTest, ExcludesPausedResumeLoadingAndStallSeconds){
    PerfSampler::setSampling(true);
    second();                                   // committed
    PerfSampler::notePaused(true);
    for (int i = 0; i < 3; i++)
        second(0);                              // paused, nothing drawn
    PerfSampler::notePaused(false);             // the resume
    second();
    second();                                   // the 2 s after it
    PerfSampler::noteSceneLoaded();
    second();                                   // the second after a scene load
    perfTestLoading = true;
    second();                                   // still loading at the tick
    perfTestLoading = false;
    FrameTimes::recordSample(1200.0f);
    second(30);                                 // a stall
    second();                                   // committed
    const PerfSummary summary = PerfSampler::summary();
    EXPECT_DOUBLE_EQ(summary.seconds, 2.0);
    EXPECT_EQ(summary.frames, 120u);
    EXPECT_DOUBLE_EQ(summary.excludedPaused, 3.0);
    EXPECT_DOUBLE_EQ(summary.excludedResume, 2.0);
    EXPECT_DOUBLE_EQ(summary.excludedLoading, 2.0);
    EXPECT_DOUBLE_EQ(summary.excludedStall, 1.0);
    EXPECT_DOUBLE_EQ(summary.excludedGame, 0.0);
    // a hidden window counts as paused too
    PerfSampler::noteHidden(true);
    second();
    PerfSampler::noteHidden(false);
    EXPECT_DOUBLE_EQ(PerfSampler::summary().excludedPaused, 4.0);
}

// a second's frames are held until its tick decides: past 512 frames a second (menus and loading
// screens with vsync off) they must not reach the window before a paused second is discarded
TEST_F(PerfSamplerTest, FastSecondsAreExcludedWhole){
    PerfSampler::setSampling(true);
    second();                                   // committed
    PerfSampler::notePaused(true);
    second(2000, 0.4f);                         // paused, 2000 frames drawn
    PerfSampler::notePaused(false);
    PerfSummary summary = PerfSampler::summary();
    EXPECT_EQ(summary.frames, 60u) << "the paused second's frames past 512 reached the window";
    EXPECT_DOUBLE_EQ(summary.seconds, 1.0);
    EXPECT_DOUBLE_EQ(summary.excludedPaused, 1.0);
    second();
    second();                                   // the 2 s after the resume
    second(1500, 0.6f);                         // a committed fast second, every frame counted
    summary = PerfSampler::summary();
    EXPECT_EQ(summary.frames, 1560u);
    EXPECT_DOUBLE_EQ(summary.seconds, 2.0);
    EXPECT_NEAR(summary.fpsAvg, 780.0, 1e-9);
    EXPECT_LT(summary.frameMsP50, 1.0);
}

// AC3: the game's own exclusions, named; past 8 names they are "other"
TEST_F(PerfSamplerTest, GameReasonsNameExcludedTime){
    PerfSampler::setSampling(true);
    second();
    PerfSampler::setSampling(false, "menu");
    for (int i = 0; i < 4; i++)
        second();
    PerfSampler::setSampling(true);
    second();
    PerfSummary summary = PerfSampler::summary();
    EXPECT_DOUBLE_EQ(reason(summary, "menu"), 4.0);
    EXPECT_DOUBLE_EQ(summary.excludedGame, 4.0);
    EXPECT_DOUBLE_EQ(summary.seconds, 2.0);
    for (int r = 2; r <= 9; r++){
        PerfSampler::setSampling(false, "reason" + std::to_string(r));
        second();
        PerfSampler::setSampling(true);
    }
    summary = PerfSampler::summary();
    EXPECT_DOUBLE_EQ(reason(summary, "reason8"), 1.0);
    EXPECT_DOUBLE_EQ(reason(summary, "reason9"), -1.0) << "a 9th name is not kept";
    EXPECT_DOUBLE_EQ(reason(summary, "other"), 1.0);
    // R10: off without a reason still counts as the game's
    PerfSampler::setSampling(false);
    second();
    EXPECT_DOUBLE_EQ(PerfSampler::summary().excludedGame, 13.0);
}

// AC4: the editor's frames that ran an MCP tool are not the game's
TEST_F(PerfSamplerTest, SkipsHostWorkFrames){
    PerfSampler::setSampling(true);
    const auto now = Clock::now();
    for (int i = 0; i < 60; i++){
        FrameTimeSample sample;
        sample.frameMs = 16.0f;
        sample.cpuMs = 10.0f;
        sample.flags = (i % 2) ? FrameTimes::FLAG_HOST_WORK : 0u;
        PerfSampler::onFrame(sample, now);
    }
    PerfSampler::tickForTest(1.0);
    EXPECT_EQ(PerfSampler::summary().frames, 30u);
}

// AC5: frames that sleep 20 ms and compute 2 ms: the thread clock sees the 2 ms
TEST_F(PerfSamplerTest, ThreadCpuSeparatesWaitingFromWork){
    ASSERT_TRUE(ThreadCpuClock::isAvailable());
    // on the sampler's clock before the frames: a loaded machine's sleeps can pass a wall second,
    // which would close the second early and leave the measured one without frames
    PerfSampler::tickForTest(1.0);
    PerfSampler::setSampling(true);
    for (int i = 0; i < 10; i++){
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto spinEnd = Clock::now() + std::chrono::milliseconds(2);
        volatile uint64_t work = 0;
        while (Clock::now() < spinEnd)
            work = work + 1;
        FrameTimes::recordSample(22.0f, 22.0f);
    }
    PerfSampler::tickForTest(1.0);
    const PerfSummary summary = PerfSampler::summary();
    EXPECT_TRUE(summary.threadClock);
    EXPECT_LT(summary.mainThreadCpuMs, 6.0);
    EXPECT_GE(summary.mainThreadCpuMs, 0.0);
    EXPECT_GT(summary.cpuMsAvg, 20.0);
    EXPECT_GE(PerfSampler::mainThreadCpuMsLastSecond(), 0.0);
    const std::string json = PerfSampler::toJson(summary);
    EXPECT_NE(json.find("\"main_thread_cpu\":\"thread_clock\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"frame_ms\""), std::string::npos) << json;
}

// A running game's seconds close on its own frames (no tickForTest): the frame that passes the
// second is counted in the second it closes, so the newest CPU slot always has frames and the
// Game view's and the profiler's last-second reading is never the empty -1 right after a rollover
TEST_F(PerfSamplerTest, WallClockRolloverKeepsTheClosedSecondsCpu){
    ASSERT_TRUE(ThreadCpuClock::isAvailable());
    PerfSampler::setSampling(true);
    const auto start = Clock::now();
    for (int i = 0; i <= 10; i++){
        FrameTimeSample sample;
        sample.frameMs = 100.0f;
        sample.cpuMs = 2.0f;
        PerfSampler::onFrame(sample, start + std::chrono::milliseconds(100 * i));
        if (i < 10) {
            EXPECT_LT(PerfSampler::mainThreadCpuMsLastSecond(), 0.0) << "no second has closed yet (frame " << i << ")";
        }
    }
    EXPECT_GE(PerfSampler::mainThreadCpuMsLastSecond(), 0.0) << "the frame at 1 s closed the second it is counted in";
}

// R5: the thread clock resolves one frame's work: a 3 ms spin reads more than nothing and no more
// than the wall time around it (a clock that advances at the scheduler tick reads 0 or 15.6 ms)
TEST(ThreadCpuClock, ResolvesAFramesWork){
    ASSERT_TRUE(ThreadCpuClock::isAvailable());
    for (int i = 0; i < 5; i++){
        const auto wallStart = Clock::now();
        const double cpuStart = ThreadCpuClock::nowMs();
        const auto spinEnd = Clock::now() + std::chrono::milliseconds(3);
        volatile uint64_t work = 0;
        while (Clock::now() < spinEnd)
            work = work + 1;
        const double cpuEnd = ThreadCpuClock::nowMs();
        const double wallMs = std::chrono::duration<double, std::milli>(Clock::now() - wallStart).count();
        ASSERT_GE(cpuStart, 0.0);
        EXPECT_GT(cpuEnd - cpuStart, 0.0) << "spin " << i;
        EXPECT_LE(cpuEnd - cpuStart, wallMs + 0.1) << "spin " << i << " of " << wallMs << " ms";
    }
}

// AC6: a window of 30 committed seconds goes to the game, then a new one starts
TEST_F(PerfSamplerTest, WindowCallbackFiresAndRestarts){
    int calls = 0;
    double seconds = 0.0;
    PerfSampler::setWindowSeconds(30);
    PerfSampler::onWindow.add("perf_test", [&calls, &seconds](const PerfSummary& summary){
        calls++;
        seconds = summary.seconds;
    });
    PerfSampler::setSampling(true);
    for (int i = 0; i < 31; i++)
        second();
    EXPECT_EQ(calls, 1);
    EXPECT_DOUBLE_EQ(seconds, 30.0);
    EXPECT_DOUBLE_EQ(PerfSampler::summary().seconds, 1.0);
    PerfSampler::setWindowSeconds(5);
    EXPECT_EQ(PerfSampler::getWindowSeconds(), 30) << "clamped to 30..3600";
}

// AC7: off, frames allocate nothing and read no clock
TEST_F(PerfSamplerTest, OffAllocatesNothingAndReadsNoClock){
    PerfSampler::resetSession();
    ASSERT_FALSE(PerfSampler::isActive());
    const uint64_t reads = ThreadCpuClock::readCount();
    testsupport::AllocationCountScope allocations;
    for (int i = 0; i < 10000; i++)
        FrameTimes::beginFrame();
    EXPECT_EQ(allocations.count(), 0u);
    EXPECT_EQ(ThreadCpuClock::readCount(), reads);
    EXPECT_EQ(PerfSampler::summary().frames, 0u);
}

// AC8: a frame's cost on and off, a tick's cost (budgets enforced by the benchmark run)
TEST(PerfSamplerBench, FrameCost){
    PerfSampler::resetSession();
    FrameTimeSample sample;
    sample.frameMs = 16.0f;
    sample.cpuMs = 9.0f;
    const auto now = Clock::now();
    constexpr int frames = 2000000;

    // off: the one branch FrameTimes takes. The fence makes each frame read the flag again without
    // adding work of its own: a volatile counter here was a store-to-load chain of ~5 cycles, 2.4 ns
    // on the 2.4 GHz benchmark box, more than the budget whatever the branch cost
    auto start = Clock::now();
    for (int i = 0; i < frames; i++){
        if (PerfSampler::isActive())
            PerfSampler::onFrame(sample, now);
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    const double offNs = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / frames;

    // on, on the sampler's own clock (no tick inside the loop)
    PerfSampler::setSampling(true);
    PerfSampler::tickForTest(1.0);
    start = Clock::now();
    for (int i = 0; i < frames; i++){
        sample.frameMs = 16.0f + (float)(i & 7);
        if (PerfSampler::isActive())
            PerfSampler::onFrame(sample, now);
    }
    const double onNs = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / frames;
    PerfSampler::resetSession();
    expectWithin(offNs, 2.0, "PerfSampler frame off, ns");
    expectWithin(onNs, 15.0, "PerfSampler frame on, ns");
}

TEST(PerfSamplerBench, TickCost){
    PerfSampler::resetSession();
    PerfSampler::setSampling(true);
    constexpr int ticks = 2000;
    double worstUs = 0.0;
    double totalUs = 0.0;
    for (int i = 0; i < ticks; i++){
        for (int f = 0; f < 60; f++)
            FrameTimes::recordSample(16.0f, 10.0f);
        // the real loading check (Engine::areScenesLoaded(false)) and thread-clock read
        const auto start = Clock::now();
        PerfSampler::tickForTest(1.0);
        const double us = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
        totalUs += us;
        worstUs = std::max(worstUs, us);
    }
    PerfSampler::resetSession();
    std::printf("[bench] PerfSampler tick worst: %.3f us\n", worstUs);
    expectWithin(totalUs / ticks, 50.0, "PerfSampler tick, us");
}

}  // namespace PerfSamplerTest_cpp
