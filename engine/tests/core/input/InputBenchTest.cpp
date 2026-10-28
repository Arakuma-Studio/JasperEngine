// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Input's budgets (docs/specs/S22-gameplay.md R35, AC17): a latch with no events within 2 us,
// and the latch plus the evaluation of 100 actions with 300 bindings within 50 us (the mobile
// budget; a desktop runner has it with room), neither allocating in a steady frame.
// Like every *Bench suite the budget is enforced with JASPER_TEST_PERF_SLACK (the benchmark run
// on server E), failing at 1.5x it, and only printed otherwise (docs/testing.md).

#include "Engine.h"
#include "Input.h"
#include "TestSystem.h"
#include "input/InputActions.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>

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
    if (testsupport::perfSlack() > 0.0){
        EXPECT_LE(us, 2.0 * 1.5 * testsupport::perfSlack());
    }

    Engine::systemTouchCancel();
    Input::resetFrameState();
}

TEST(InputBench, Evaluate100Actions300Bindings){
    test::TestSystem::get().setScreenSize(640, 360);
    Engine::setCanvasSize(640, 360);
    Engine::systemViewChanged();
    Input::resetFrameState();
    Engine::systemGamepadConnect(0, "Pad");

    // 10 maps of 10 actions: buttons on a key, a pad button and a modifier binding; sticks;
    // axes; composites
    std::string json = "{\"version\":1,\"maps\":[";
    for (int m = 0; m < 10; m++){
        json += std::string(m ? "," : "") + "{\"name\":\"Map" + std::to_string(m) + "\",\"priority\":" + std::to_string(m % 3) +
                ",\"consume\":" + (m % 2 ? "true" : "false") + ",\"actions\":[";
        for (int a = 0; a < 10; a++){
            const char* letter = "abcdefghijklmnopqrstuvwxyz";
            const std::string k(1, letter[(m * 10 + a) % 26]);
            std::string bindings;
            std::string type = "BUTTON";
            if (a % 4 == 0){
                type = "VECTOR2";
                bindings = "{\"composite\":\"dpad2d\",\"parts\":{\"up\":{\"path\":\"keyboard/w\"},\"down\":{\"path\":\"keyboard/s\"}}},"
                           "{\"path\":\"gamepad/left_stick\"},{\"path\":\"virtual/stick\"}";
            }else if (a % 4 == 1){
                type = "AXIS";
                bindings = "{\"path\":\"gamepad/left_x\"},{\"path\":\"gamepad/right_trigger\"},{\"path\":\"mouse/scroll/y\"}";
            }else{
                bindings = "{\"path\":\"keyboard/" + k + "\"},{\"path\":\"gamepad/a\"},{\"path\":\"keyboard/" + k + "\",\"requiredMods\":[\"ctrl\"]}";
            }
            json += std::string(a ? "," : "") + "{\"name\":\"A" + std::to_string(a) + "\",\"type\":\"" + type + "\",\"bindings\":[" + bindings + "]}";
        }
        json += "]}";
    }
    json += "]}";
    ASSERT_TRUE(InputActions::loadFromText("bench.inputactions", json));
    size_t bindings = 0;
    for (const InputActionMapDesc& map : InputActions::getAsset().maps)
        for (const InputActionDesc& action : map.actions)
            bindings += action.bindings.size();
    ASSERT_EQ(InputActions::getActions().size(), 100u);
    ASSERT_EQ(bindings, 300u);

    // warm up
    for (int i = 0; i < 10; i++){
        Engine::systemKeyDown(D_KEY_W, false, 0);
        Input::beginFrame();
        InputActions::evaluate(1.0f / 60.0f);
    }
    constexpr int frames = 20000;
    double total = 0.0;
    uint64_t allocations = 0;
    for (int i = 0; i < frames; i++){
        // a press and a stick move every frame
        Engine::systemKeyDown(D_KEY_A + i % 26, false, 0);
        Engine::systemKeyUp(D_KEY_A + (i + 13) % 26, false, 0);
        Engine::systemGamepadAxisMove(0, D_GAMEPAD_AXIS_LEFT_X, (i % 100) / 100.0f);
        testsupport::ThreadAllocationCount count;
        const auto start = std::chrono::steady_clock::now();
        Input::beginFrame();
        InputActions::evaluate(1.0f / 60.0f);
        total += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        allocations += count.count();
    }
    const double us = total / frames;
    std::printf("[bench] Input latch + 100 actions / 300 bindings %.2f us (budget 50 us)\n", us);
    EXPECT_EQ(allocations, 0u);
    if (testsupport::perfSlack() > 0.0){
        EXPECT_LE(us, 50.0 * 1.5 * testsupport::perfSlack());
    }

    InputActions::unload();
    Engine::systemGamepadDisconnect(0);
    Input::resetFrameState();
}
