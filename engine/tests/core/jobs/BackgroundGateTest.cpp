// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The BackgroundGate (S06 AC7). The AssetDecodeGate cases are ported from OpenEngine's
// Tests/Assets/AssetDecodeGateTests.cpp, Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors, MIT License. See NOTICE.md.

#include "jobs/JobSystem.h"

#include "JobsTestUtil.h"

#include <gtest/gtest.h>

#include <atomic>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::jobs;

// ---- ported AssetDecodeGateTests

TEST(AssetDecodeGate, LeavesTwoWorkersToTheFrameAndTwoSlotsToOtherDecodes){
    BackgroundGate sixteen(16, 2);
    EXPECT_EQ(sixteen.maxBackground(), 14);
    EXPECT_EQ(sixteen.heavyMax(), 12);

    BackgroundGate eight(8, 2);
    EXPECT_EQ(eight.maxBackground(), 6);
    EXPECT_EQ(eight.heavyMax(), 4);

    // small pools: one slot, shared
    BackgroundGate three(3, 2);
    EXPECT_EQ(three.maxBackground(), 1);
    EXPECT_EQ(three.heavyMax(), 1);
    BackgroundGate one(1, 2);
    EXPECT_EQ(one.maxBackground(), 1);
    EXPECT_EQ(one.heavyMax(), 1);
    BackgroundGate four(4, 2);
    EXPECT_EQ(four.maxBackground(), 2);
    EXPECT_EQ(four.heavyMax(), 1);
}

TEST(AssetDecodeGate, HeavyWorkStopsAtItsShare){
    BackgroundGate gate(8, 2);
    for (int i = 0; i < 4; i++)
        EXPECT_TRUE(gate.tryAcquire(GateClass::Heavy));
    EXPECT_FALSE(gate.tryAcquire(GateClass::Heavy)) << "Heavy work takes at most max - 2 slots";
    EXPECT_TRUE(gate.tryAcquire(GateClass::Default));
    EXPECT_TRUE(gate.tryAcquire(GateClass::Default));
    EXPECT_FALSE(gate.tryAcquire(GateClass::Default));
    EXPECT_EQ(gate.taken(), 6);
    EXPECT_EQ(gate.heavyTaken(), 4);
    gate.release(GateClass::Heavy);
    EXPECT_TRUE(gate.tryAcquire(GateClass::Default)) << "a released Heavy slot admits other work";
    EXPECT_FALSE(gate.tryAcquire(GateClass::Heavy));
}

TEST(AssetDecodeGate, OtherDecodesMayFillTheGate){
    BackgroundGate gate(8, 2);
    for (int i = 0; i < 6; i++)
        EXPECT_TRUE(gate.tryAcquire(GateClass::Default));
    EXPECT_FALSE(gate.tryAcquire(GateClass::Default));
    EXPECT_FALSE(gate.tryAcquire(GateClass::Heavy));
}

TEST(AssetDecodeGate, EveryReleaseCallsTheHook){
    int released = 0;
    BackgroundGate gate(8, 2, [&]{ released++; });
    ASSERT_TRUE(gate.tryAcquire(GateClass::Default));
    ASSERT_TRUE(gate.tryAcquire(GateClass::Heavy));
    gate.release(GateClass::Default);
    gate.release(GateClass::Heavy);
    EXPECT_EQ(released, 2);
}

TEST(AssetDecodeGate, ReserveAndOverrideChangeLive){
    BackgroundGate gate(8, 2);
    gate.setReserve(5);
    EXPECT_EQ(gate.maxBackground(), 3);
    EXPECT_EQ(gate.heavyMax(), 1);
    gate.setMaxOverride(1);
    EXPECT_EQ(gate.maxBackground(), 1);
    gate.setMaxOverride(0);
    EXPECT_EQ(gate.maxBackground(), 3);
}

// ---- the gate in the scheduler

namespace{
    void raiseMax(std::atomic<int>& maximum, int value){
        int seen = maximum.load();
        while (value > seen && !maximum.compare_exchange_weak(seen, value)){
        }
    }
}

TEST(JobsGate, BackgroundNeverExceedsMax){
    Config config = testsupport::configFor(8);
    config.reserveForFrame = 2;
    JobSystem system(config);
    ASSERT_EQ(system.getGate().maxBackground(), 6);
    std::atomic<int> running{0};
    std::atomic<int> maximum{0};
    SubmitOptions options;
    options.lane = Lane::Background;
    auto runBatch = [&](int count){
        std::vector<Task<void>> tasks;
        for (int i = 0; i < count; i++){
            tasks.push_back(system.submit([&]{
                raiseMax(maximum, ++running);
                testsupport::spinFor(500.0);
                running--;
            }, options));
        }
        // polled: a wait would run them inline, outside the gate
        for (auto& task : tasks)
            ASSERT_TRUE(testsupport::spinUntil([&]{ return task.isReady(); }, 30000.0));
    };
    runBatch(200);
    EXPECT_LE(maximum.load(), 6);
    EXPECT_GE(maximum.load(), 2) << "Background work ran on several workers";

    // the reserve applies live; the pool is never rebuilt
    system.getGate().setReserve(5);
    maximum = 0;
    runBatch(100);
    EXPECT_LE(maximum.load(), 3);
}

TEST(JobsGate, HeavyLeavesTwoSlots){
    Config config = testsupport::configFor(8);
    config.reserveForFrame = 2;
    JobSystem system(config);
    std::atomic<int> heavyRunning{0};
    std::atomic<int> heavyMaximum{0};
    std::atomic<bool> stop{false};
    SubmitOptions heavy;
    heavy.lane = Lane::Background;
    heavy.gate = GateClass::Heavy;
    std::vector<Task<void>> cooks;
    for (int i = 0; i < 100; i++){
        cooks.push_back(system.submit([&]{
            raiseMax(heavyMaximum, ++heavyRunning);
            for (int k = 0; k < 40 && !stop.load(); k++)
                testsupport::spinFor(250.0);
            heavyRunning--;
        }, heavy));
    }
    ASSERT_TRUE(testsupport::spinUntil([&]{ return heavyRunning.load() == 4; }, 5000.0));

    // with the Heavy share full, a short decode still finds a slot at once
    SubmitOptions decode;
    decode.lane = Lane::Background;
    const auto start = std::chrono::steady_clock::now();
    Task<double> shortDecode = system.submit([start]{ return testsupport::elapsedMs(start); }, decode);
    ASSERT_TRUE(testsupport::spinUntil([&]{ return shortDecode.isReady(); }, 5000.0));
    EXPECT_LE(shortDecode.get(), 20.0 * testsupport::timeScale());
    EXPECT_LE(heavyMaximum.load(), 4);
    stop = true;
    for (auto& cook : cooks)
        ASSERT_TRUE(testsupport::spinUntil([&]{ return cook.isReady(); }, 30000.0));
    EXPECT_LE(heavyMaximum.load(), 4);
}

TEST(JobsGate, AutoReserveRestoresPlatformDefault){
    // the Engine re-initializes the running process instance when the settings change
    // (Project Settings, undo): Auto (-1) must bring the platform's reserve back
    JobSystem& system = jobs::system();
    const int platformReserve = system.getGate().getReserve();
    Config config;
    config.reserveForFrame = 3;
    jobs::initialize(config);
    EXPECT_EQ(system.getGate().getReserve(), 3);
    config.reserveForFrame = -1;
    EXPECT_EQ(&jobs::initialize(config), &system) << "the running pool is kept";
    EXPECT_EQ(system.getGate().getReserve(), platformReserve);
}

TEST(JobsGate, PeaksKeepTheHighWaterMarkUntilTaken){
    BackgroundGate gate(8, 2);
    ASSERT_TRUE(gate.tryAcquire(GateClass::Heavy));
    ASSERT_TRUE(gate.tryAcquire(GateClass::Heavy));
    ASSERT_TRUE(gate.tryAcquire(GateClass::Default));
    gate.release(GateClass::Heavy);
    gate.release(GateClass::Default);
    EXPECT_EQ(gate.peeks().taken, 3);
    EXPECT_EQ(gate.peeks().heavy, 2);
    const BackgroundGate::Peaks peaks = gate.takePeaks();
    EXPECT_EQ(peaks.taken, 3);
    EXPECT_EQ(peaks.heavy, 2);
    // the next interval starts at what is taken now: one Heavy slot
    EXPECT_EQ(gate.peeks().taken, 1);
    EXPECT_EQ(gate.peeks().heavy, 1);
    gate.release(GateClass::Heavy);
}
