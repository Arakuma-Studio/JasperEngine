// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/Scheduler.h"
#include "Engine.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;

TEST(SchedulerTest, OrderingFifoEqualDue) {
    Scheduler scheduler("test");
    std::vector<int> order;
    for (int i = 0; i < 5; i++) scheduler.after(1.0, [&order, i]() { order.push_back(i); }, TimeDomain::Game);
    scheduler.after(0.5, [&order]() { order.push_back(-1); }, TimeDomain::Game);
    scheduler.advance(TimeDomain::Game, 1.0);
    EXPECT_EQ(scheduler.process(TimeDomain::Game), 6u);
    EXPECT_EQ(order, (std::vector<int>{-1, 0, 1, 2, 3, 4}));
}

TEST(SchedulerTest, ScheduledDuringProcessRunsNextProcess) {
    Scheduler scheduler("test");
    std::vector<std::string> ran;
    scheduler.after(0.0, [&]() {
        ran.push_back("first");
        scheduler.after(0.0, [&]() { ran.push_back("scheduled inside"); }, TimeDomain::Game);
    }, TimeDomain::Game);
    scheduler.process(TimeDomain::Game);
    EXPECT_EQ(ran, (std::vector<std::string>{"first"}));
    scheduler.process(TimeDomain::Game);
    EXPECT_EQ(ran, (std::vector<std::string>{"first", "scheduled inside"}));
}

TEST(SchedulerTest, PostedRunFifoOnTheOwnerThread) {
    Scheduler scheduler("test");
    std::vector<int> order;
    std::thread worker([&]() {
        for (int i = 0; i < 100; i++) scheduler.post([&order, i]() { order.push_back(i); });
    });
    worker.join();
    EXPECT_TRUE(scheduler.hasPosted());
    EXPECT_EQ(scheduler.nextDueRealTime(), 0.0);
    EXPECT_EQ(scheduler.processPosted(), 100u);
    ASSERT_EQ(order.size(), 100u);
    for (int i = 0; i < 100; i++) EXPECT_EQ(order[i], i);
    // a task that throws does not stop the ones behind it
    int after = 0;
    scheduler.post([]() { throw std::runtime_error("boom"); });
    scheduler.post([&after]() { after++; });
    scheduler.processPosted();
    EXPECT_EQ(after, 1);
}

TEST(SchedulerTest, CancelBeforeRunNeverRuns) {
    Scheduler scheduler("test");
    std::atomic<int> ran{0};
    for (int i = 0; i < 10000; i++) {
        TimerHandle handle = scheduler.after(0.0, [&ran]() { ran++; }, TimeDomain::Game);
        bool cancelled = false;
        std::thread canceller([&]() { cancelled = handle.cancel(); });
        canceller.join();
        ASSERT_TRUE(cancelled);
        EXPECT_FALSE(handle.active());
        scheduler.process(TimeDomain::Game);
    }
    EXPECT_EQ(ran.load(), 0);
    // racing with the run: either it ran, or the cancel won and it never does
    for (int i = 0; i < 1000; i++) {
        std::atomic<int> runs{0};
        TimerHandle handle = scheduler.after(0.0, [&runs]() { runs++; }, TimeDomain::Game);
        std::thread canceller([handle]() { handle.cancel(); });
        scheduler.processPosted();
        scheduler.process(TimeDomain::Game);
        canceller.join();
        scheduler.process(TimeDomain::Game);
        EXPECT_LE(runs.load(), 1);
    }
}

TEST(SchedulerTest, CancelRunningStopsRepeat) {
    Scheduler scheduler("test");
    int runs = 0;
    TimerHandle handle;
    handle = scheduler.every(1.0, [&]() {
        runs++;
        EXPECT_TRUE(handle.cancel()) << "cancelling the running task succeeds";
    }, TimeDomain::Game);
    for (int i = 0; i < 5; i++) {
        scheduler.advance(TimeDomain::Game, 1.0);
        scheduler.process(TimeDomain::Game);
    }
    EXPECT_EQ(runs, 1);
    EXPECT_FALSE(handle.active());
}

TEST(SchedulerTest, RepeatCountAndPhase) {
    Scheduler scheduler("test");
    std::vector<double> times;
    TimerOpts opts;
    opts.count = 3;
    scheduler.every(0.5, [&]() { times.push_back(scheduler.now(TimeDomain::Game)); }, TimeDomain::Game, opts);
    for (int i = 0; i < 10; i++) {
        scheduler.advance(TimeDomain::Game, 0.25);
        scheduler.process(TimeDomain::Game);
    }
    EXPECT_EQ(times, (std::vector<double>{0.5, 1.0, 1.5}));
}

TEST(SchedulerTest, RepeatNoBurstAfterStall) {
    Scheduler scheduler("test");
    int runs = 0;
    scheduler.every(0.1, [&]() { runs++; }, TimeDomain::Game);
    scheduler.advance(TimeDomain::Game, 1.05);   // a stall of ten periods
    scheduler.process(TimeDomain::Game);
    EXPECT_EQ(runs, 1);
    // the phase is kept: next due at 1.1
    scheduler.advance(TimeDomain::Game, 0.04);
    scheduler.process(TimeDomain::Game);
    EXPECT_EQ(runs, 1);
    scheduler.advance(TimeDomain::Game, 0.02);
    scheduler.process(TimeDomain::Game);
    EXPECT_EQ(runs, 2);
}

TEST(SchedulerTest, CatchUpCapped8) {
    Scheduler scheduler("test");
    int runs = 0;
    TimerOpts opts;
    opts.catchUp = true;
    scheduler.every(0.1, [&]() { runs++; }, TimeDomain::Game, opts);
    scheduler.advance(TimeDomain::Game, 2.05);   // twenty periods
    scheduler.process(TimeDomain::Game);
    EXPECT_EQ(runs, 8);
    scheduler.advance(TimeDomain::Game, 0.1);
    scheduler.process(TimeDomain::Game);
    EXPECT_EQ(runs, 9);
}

TEST(SchedulerTest, GameClockStopsWhenGameEventsPaused) {
    Scheduler scheduler("test");
    int game = 0;
    int real = 0;
    scheduler.after(0.5, [&]() { game++; }, TimeDomain::Game);
    scheduler.after(0.5, [&]() { real++; }, TimeDomain::Real);
    const bool wasPaused = Engine::areGameEventsPaused();
    Engine::pauseGameEvents(true);
    for (int i = 0; i < 60; i++) {
        EXPECT_FALSE(Engine::advanceGameClocks(scheduler, 1.0 / 60.0));
        scheduler.advance(TimeDomain::Real, 1.0 / 60.0);
        scheduler.process(TimeDomain::Game);
        scheduler.process(TimeDomain::Real);
    }
    EXPECT_EQ(game, 0) << "the editor's Pause stops the game clock";
    EXPECT_EQ(real, 1);
    Engine::pauseGameEvents(false);
    for (int i = 0; i < 31; i++) {
        EXPECT_TRUE(Engine::advanceGameClocks(scheduler, 1.0 / 60.0));
        scheduler.process(TimeDomain::Game);
    }
    EXPECT_EQ(game, 1);
    Engine::pauseGameEvents(wasPaused);
}

TEST(SchedulerTest, UnscaledIgnoresTimeScale) {
    Scheduler scheduler("test");
    int game = 0;
    int unscaled = 0;
    scheduler.after(0.5, [&]() { game++; }, TimeDomain::Game);
    scheduler.after(0.5, [&]() { unscaled++; }, TimeDomain::Unscaled);
    const bool wasPaused = Engine::areGameEventsPaused();
    const float scale = Engine::getTimeScale();
    Engine::pauseGameEvents(false);
    Engine::setTimeScale(0.0f);
    for (int i = 0; i < 40; i++) {
        Engine::advanceGameClocks(scheduler, 1.0 / 60.0);
        scheduler.process(TimeDomain::Game);
        scheduler.process(TimeDomain::Unscaled);
    }
    EXPECT_EQ(game, 0) << "time scale 0 freezes the game clock";
    EXPECT_EQ(unscaled, 1);
    Engine::setTimeScale(scale);
    Engine::pauseGameEvents(wasPaused);
}

TEST(SchedulerTest, RealRunsWhilePaused) {
    Scheduler scheduler("test");
    int real = 0;
    scheduler.after(0.2, [&]() { real++; }, TimeDomain::Real);
    // paused: no game clock, the real one still goes
    scheduler.advance(TimeDomain::Real, 0.25);
    scheduler.process(TimeDomain::Real);
    EXPECT_EQ(real, 1);
}

TEST(SchedulerTest, FramesAndFixedSteps) {
    Scheduler scheduler("test");
    int frames = 0;
    int steps = 0;
    scheduler.nextFrame([&]() { frames++; });
    scheduler.afterFrames(3, [&]() { frames += 10; });
    scheduler.afterFixedSteps(2, [&]() { steps++; });
    for (int frame = 1; frame <= 3; frame++) {
        // two fixed steps a frame
        for (int step = 0; step < 2; step++) {
            scheduler.advance(TimeDomain::FixedSteps, 1.0);
            scheduler.process(TimeDomain::FixedSteps);
        }
        if (frame == 1) {
            EXPECT_EQ(steps, 1) << "two fixed-step iterations";
        }
        scheduler.advance(TimeDomain::Frames, 1.0);
        scheduler.process(TimeDomain::Frames);
        if (frame <= 2) {
            EXPECT_EQ(frames, 1) << "frame " << frame;
        }
    }
    EXPECT_EQ(frames, 11);
}

TEST(SchedulerTest, FixedStepsCountIterations) {
    Scheduler scheduler("test");
    std::vector<double> at;
    scheduler.every(1.0, [&]() { at.push_back(scheduler.now(TimeDomain::FixedSteps)); }, TimeDomain::FixedSteps);
    for (int i = 0; i < 4; i++) {
        scheduler.advance(TimeDomain::FixedSteps, 1.0);
        scheduler.process(TimeDomain::FixedSteps);
    }
    EXPECT_EQ(at, (std::vector<double>{1.0, 2.0, 3.0, 4.0}));
}

TEST(SchedulerTest, ClearAndOwnersAndGroups) {
    Scheduler scheduler("test");
    int runs = 0;
    int owner = 0;
    TimerOpts opts;
    opts.ownerKey = ownerKeyOf(&owner);
    scheduler.after(0.0, [&]() { runs++; }, TimeDomain::Game, opts);
    scheduler.after(0.0, [&]() { runs += 10; }, TimeDomain::Game);
    EXPECT_EQ(scheduler.cancelOwner(ownerKeyOf(&owner)), 1u);
    {
        TimerGroup group;
        group.add(scheduler.after(0.0, [&]() { runs += 100; }, TimeDomain::Game));
        EXPECT_EQ(group.size(), 1u);
    }
    scheduler.process(TimeDomain::Game);
    EXPECT_EQ(runs, 10);
    scheduler.after(0.0, [&]() { runs += 1000; }, TimeDomain::Game);
    scheduler.post([&]() { runs += 1000; });
    scheduler.clear();
    scheduler.processPosted();
    scheduler.process(TimeDomain::Game);
    EXPECT_EQ(runs, 10);
    EXPECT_EQ(scheduler.stats().pending[0], 0u);
}

TEST(SchedulerTest, PendingListsTheNearest) {
    Scheduler scheduler("test");
    TimerOpts opts;
    opts.debugName = "named";
    scheduler.after(2.0, []() {}, TimeDomain::Game, opts);
    scheduler.after(1.0, []() {}, TimeDomain::Real);
    const std::vector<TimerInfo> all = scheduler.pending();
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].domain, TimeDomain::Real);
    EXPECT_EQ(all[1].debugName, "named");
    EXPECT_DOUBLE_EQ(scheduler.nextDueRealTime(), 1.0);
}

TEST(SchedulerTest, ZeroAllocSmallCallable) {
    Scheduler scheduler("test");
    int sum = 0;
    // warm: the slot pool and the heap grow once
    for (int i = 0; i < 1000; i++) scheduler.after(0.0, [&sum]() { sum++; }, TimeDomain::Game);
    scheduler.process(TimeDomain::Game);
    testsupport::ThreadAllocationCount count;
    for (int round = 0; round < 10; round++) {
        for (int i = 0; i < 1000; i++) {
            void* a = &sum;
            void* b = &round;
            scheduler.after(0.0, [&sum, a, b]() { if (a != b) sum++; }, TimeDomain::Game);
        }
        scheduler.process(TimeDomain::Game);
    }
    EXPECT_EQ(count.count(), 0u);
    EXPECT_EQ(sum, 11000);
}

TEST(SchedulerTest, Process10kUnder2ms) {
    Scheduler scheduler("test");
    int sum = 0;
    for (int i = 0; i < 10000; i++) scheduler.after(0.0, [&sum]() { sum++; }, TimeDomain::Game);
    scheduler.process(TimeDomain::Game);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; i++) scheduler.after(static_cast<double>(i % 97), [&sum]() { sum++; }, TimeDomain::Game);
    scheduler.advance(TimeDomain::Game, 100.0);
    scheduler.process(TimeDomain::Game);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ(sum, 20000);
    std::printf("[bench] 10k timers inserted and processed: %.3f ms (budget 2)\n", ms);
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LE(ms, 2.0 * testsupport::perfSlack());
    }
}

TEST(SchedulerBench, AfterAndIdleProcess) {
    Scheduler scheduler("bench");
    int sum = 0;
    for (int i = 0; i < 100000; i++) scheduler.after(1e9, [&sum]() { sum++; }, TimeDomain::Game);
    scheduler.clear();
    constexpr int count = 100000;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < count; i++) scheduler.after(1e9, [&sum]() { sum++; }, TimeDomain::Game);
    const double afterNs = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / count;
    start = std::chrono::steady_clock::now();
    size_t ran = 0;
    for (int i = 0; i < count; i++) ran += scheduler.process(TimeDomain::Game);
    const double processNs = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / count;
    EXPECT_EQ(ran, 0u);
    std::printf("[bench] after(): %.1f ns (budget 150), process() with nothing due: %.1f ns (budget 30)\n", afterNs, processNs);
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LE(afterNs, 150.0 * testsupport::perfSlack());
        EXPECT_LE(processNs, 30.0 * testsupport::perfSlack());
    }
}
