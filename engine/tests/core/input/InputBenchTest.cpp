// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Input's budgets (docs/specs/S22-gameplay.md R35, AC17): a latch with no events within 2 us.
// Like every *Bench suite the budget is enforced with JASPER_TEST_PERF_SLACK (the benchmark run
// on server E), failing at 1.5x it, and only printed otherwise (docs/testing.md).

#include "Engine.h"
#include "Input.h"
#include "TestSystem.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>

using namespace jasperengine;

TEST(InputBench, LatchWithoutEvents){
    test::TestSystem::get().setScreenSize(640, 360);
    Engine::setCanvasSize(640, 360);
    Engine::systemViewChanged();
    Input::resetFrameState();
    // a held key and a finger down: what a frame without events still carries
    Engine::systemKeyDown(D_KEY_W, false, 0);
    Engine::systemTouchStart(1, 10, 10);
    Input::beginFrame();

    constexpr int frames = 200000;
    testsupport::ThreadAllocationCount allocations;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; i++){
        Input::beginFrame();
        Input::beginFixedStep();
        Input::endFixedStep();
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / frames;
    std::printf("[bench] Input latch without events %.3f us (budget 2 us)\n", us);
    EXPECT_EQ(allocations.count(), 0u);
    if (testsupport::perfSlack() > 0.0)
        EXPECT_LE(us, 2.0 * 1.5 * testsupport::perfSlack());

    Engine::systemTouchCancel();
    Input::resetFrameState();
}
