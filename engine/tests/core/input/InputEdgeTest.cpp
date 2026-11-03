// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Input's frame edges (docs/specs/S22-gameplay.md R1-R7, AC1-AC3): platform events go through
// Engine::system*, and a frame that runs its update latches them with Input::beginFrame(), as
// Engine::systemDraw does (InputFrameLoopGpu runs the real loop).

#include "Engine.h"
#include "Input.h"
#include "TestSystem.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <functional>

using namespace jasperengine;

namespace {

    const char* callbackTag = "core-tests-input-edges";

    // positions go through the canvas transform (Engine::transformCoordPos): a last-bit difference
    void expectVector(const Vector2& actual, const Vector2& expected){
        EXPECT_NEAR(actual.x, expected.x, 1e-3f);
        EXPECT_NEAR(actual.y, expected.y, 1e-3f);
    }

    class InputEdgeTest : public ::testing::Test {
    protected:
        void SetUp() override{
            // screen and canvas 640 x 360: the system* coordinates are canvas pixels
            test::TestSystem::get().setScreenSize(640, 360);
            Engine::setCanvasSize(640, 360);
            Engine::setScalingMode(Scaling::FITWIDTH);
            Engine::systemViewChanged();
            Input::resetFrameState();
        }

        void TearDown() override{
            Engine::systemTouchCancel();
            for (int pad : {0, 1, 3})
                Engine::systemGamepadDisconnect(pad);
            Engine::onKeyUp.remove(callbackTag);
            Engine::setMouseMode(MouseMode::NORMAL);
            Engine::systemFocusChanged(true);
            Input::resetFrameState();
        }

        // what Engine::systemDraw does for a frame that runs its update: the latch, then the
        // fixed steps, each between beginFixedStep and endFixedStep
        static void runFrame(int fixedSteps = 0, const std::function<void(int)>& step = nullptr){
            Input::beginFrame();
            for (int i = 0; i < fixedSteps; i++){
                Input::beginFixedStep();
                if (step)
                    step(i);
                Input::endFixedStep();
            }
        }
    };

}

TEST_F(InputEdgeTest, PressReleaseSameWindow){
    Engine::systemKeyDown(D_KEY_SPACE, false, 0);
    Engine::systemKeyUp(D_KEY_SPACE, false, 0);
    // the window is still pending: the frame has not started
    EXPECT_FALSE(Input::getKeyDown(D_KEY_SPACE));

    runFrame();
    EXPECT_TRUE(Input::getKeyDown(D_KEY_SPACE));
    EXPECT_TRUE(Input::getKeyUp(D_KEY_SPACE));
    EXPECT_FALSE(Input::getKey(D_KEY_SPACE));
    EXPECT_TRUE(Input::anyKeyDown());

    runFrame();
    EXPECT_FALSE(Input::getKeyDown(D_KEY_SPACE));
    EXPECT_FALSE(Input::getKeyUp(D_KEY_SPACE));
    EXPECT_FALSE(Input::getKey(D_KEY_SPACE));
    EXPECT_FALSE(Input::anyKeyDown());

    // the mouse buttons follow the same rules
    Engine::systemMouseDown(D_MOUSE_BUTTON_RIGHT, 10, 10, 0);
    Engine::systemMouseUp(D_MOUSE_BUTTON_RIGHT, 10, 10, 0);
    runFrame();
    EXPECT_TRUE(Input::getMouseButtonDown(D_MOUSE_BUTTON_RIGHT));
    EXPECT_TRUE(Input::getMouseButtonUp(D_MOUSE_BUTTON_RIGHT));
    EXPECT_FALSE(Input::getMouseButton(D_MOUSE_BUTTON_RIGHT));
    EXPECT_TRUE(Input::anyKeyDown());
}

TEST_F(InputEdgeTest, HeldAcrossWindows){
    Engine::systemKeyDown(D_KEY_W, false, 0);
    runFrame();
    EXPECT_TRUE(Input::getKeyDown(D_KEY_W));
    EXPECT_FALSE(Input::getKeyUp(D_KEY_W));
    EXPECT_TRUE(Input::getKey(D_KEY_W));
    EXPECT_TRUE(Input::isKeyPressed(D_KEY_W));

    runFrame();
    runFrame();
    EXPECT_FALSE(Input::getKeyDown(D_KEY_W));
    EXPECT_TRUE(Input::getKey(D_KEY_W));

    Engine::systemKeyUp(D_KEY_W, false, 0);
    // the level follows the device at once, the edge waits for the frame
    EXPECT_FALSE(Input::getKey(D_KEY_W));
    EXPECT_FALSE(Input::getKeyUp(D_KEY_W));
    runFrame();
    EXPECT_TRUE(Input::getKeyUp(D_KEY_W));
    EXPECT_FALSE(Input::getKeyDown(D_KEY_W));
}

TEST_F(InputEdgeTest, RepeatIsNotPress){
    Engine::systemKeyDown(D_KEY_A, false, 0);
    runFrame();
    EXPECT_TRUE(Input::getKeyDown(D_KEY_A));
    for (int i = 0; i < 3; i++)
        Engine::systemKeyDown(D_KEY_A, true, 0);
    runFrame();
    EXPECT_FALSE(Input::getKeyDown(D_KEY_A));
    EXPECT_FALSE(Input::anyKeyDown());
    EXPECT_TRUE(Input::getKey(D_KEY_A));

    // a repeat of a key pressed before the window had the focus holds it without a press
    Engine::systemKeyDown(D_KEY_B, true, 0);
    runFrame();
    EXPECT_TRUE(Input::getKey(D_KEY_B));
    EXPECT_FALSE(Input::getKeyDown(D_KEY_B));

    // a second down without an up (a platform sending it twice) is not a second press
    Engine::systemKeyDown(D_KEY_A, false, 0);
    runFrame();
    EXPECT_FALSE(Input::getKeyDown(D_KEY_A));
}

TEST_F(InputEdgeTest, PausedFramesKeepPending){
    runFrame();
    Engine::systemKeyDown(D_KEY_P, false, 0);
    Engine::systemKeyUp(D_KEY_P, false, 0);
    // frames that skip the update (paused, the simulation held) latch nothing: the tap waits
    for (int frame = 0; frame < 5; frame++){
        EXPECT_FALSE(Input::getKeyDown(D_KEY_P));
        EXPECT_FALSE(Input::getKeyUp(D_KEY_P));
    }
    runFrame();
    EXPECT_TRUE(Input::getKeyDown(D_KEY_P));
    EXPECT_TRUE(Input::getKeyUp(D_KEY_P));
    runFrame();
    EXPECT_FALSE(Input::getKeyDown(D_KEY_P));
}

TEST_F(InputEdgeTest, ResetOnPlayStart){
    Engine::systemMouseEnter();
    Engine::systemKeyDown(D_KEY_LEFT_SHIFT, false, D_MODIFIER_SHIFT);
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, 20, 20, 0);
    Engine::systemMouseMove(20, 20, 0);
    Engine::systemMouseMove(40, 20, 0);
    Engine::systemMouseScroll(0, 3, 0);
    Engine::systemTouchStart(4, 100, 100);
    runFrame();
    Engine::systemKeyDown(D_KEY_Q, false, 0);

    // the editor's Play start and stop
    Input::resetFrameState();
    EXPECT_FALSE(Input::getKey(D_KEY_LEFT_SHIFT));
    EXPECT_FALSE(Input::getMouseButton(D_MOUSE_BUTTON_LEFT));
    EXPECT_EQ(Input::getModifiers(), 0);
    EXPECT_EQ(Input::getMouseScroll(), Vector2(0, 0));
    EXPECT_EQ(Input::numTouches(), 0u);
    EXPECT_FALSE(Input::getKeyDown(D_KEY_LEFT_SHIFT));
    expectVector(Input::getMouseDelta(), Vector2(0, 0));
    EXPECT_EQ(Input::getTouchCount(), 0u);

    runFrame();
    EXPECT_FALSE(Input::getKeyDown(D_KEY_Q));
    EXPECT_FALSE(Input::anyKeyDown());
    expectVector(Input::getMouseDelta(), Vector2(0, 0));
    EXPECT_EQ(Input::getMouseScrollDelta(), Vector2(0, 0));
    EXPECT_EQ(Input::getTouchCount(), 0u);
    EXPECT_FALSE(Input::getTouchDown(4));

    // the first move of the new session is not a delta from the last one's pointer
    Engine::systemMouseMove(300, 200, 0);
    runFrame();
    expectVector(Input::getMouseDelta(), Vector2(0, 0));
}

TEST_F(InputEdgeTest, FocusLossReleases){
    int gameKeyUps = 0;
    Engine::onKeyUp.add(callbackTag, std::function<void(int, bool, int)>([&](int key, bool, int){
        if (key == D_KEY_W) gameKeyUps++;
    }));
    Engine::setMouseMode(MouseMode::CAPTURED);
    EXPECT_EQ(Input::getPointerLockState(), PointerLockState::LOCKED);

    Engine::systemKeyDown(D_KEY_W, false, 0);
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, 50, 50, 0);
    Engine::systemTouchStart(2, 60, 60);
    runFrame();
    ASSERT_TRUE(Input::getKey(D_KEY_W));

    Engine::systemFocusChanged(false);
    // released at once, through the events (the game heard the key come up)
    EXPECT_FALSE(Input::getKey(D_KEY_W));
    EXPECT_FALSE(Input::getMouseButton(D_MOUSE_BUTTON_LEFT));
    EXPECT_EQ(Input::numTouches(), 0u);
    EXPECT_EQ(gameKeyUps, 1);
    EXPECT_EQ(Input::getPointerLockState(), PointerLockState::PENDING);

    runFrame();
    EXPECT_TRUE(Input::getKeyUp(D_KEY_W));
    EXPECT_TRUE(Input::getMouseButtonUp(D_MOUSE_BUTTON_LEFT));
    EXPECT_TRUE(Input::getTouchUp(2));
    ASSERT_EQ(Input::getTouchCount(), 1u);
    EXPECT_EQ(Input::getTouch(0).phase, TouchPhase::CANCELED);

    Engine::systemFocusChanged(true);
    EXPECT_EQ(Input::getPointerLockState(), PointerLockState::LOCKED);
    Engine::setMouseMode(MouseMode::NORMAL);
    EXPECT_EQ(Input::getPointerLockState(), PointerLockState::UNLOCKED);
}

TEST_F(InputEdgeTest, FixedPhaseExactlyOnce){
    Engine::systemKeyDown(D_KEY_SPACE, false, 0);
    Engine::systemKeyUp(D_KEY_SPACE, false, 0);

    // a frame without a fixed step: the update sees the tap, no step does yet
    runFrame(0);
    EXPECT_TRUE(Input::getKeyDown(D_KEY_SPACE));

    // the next frame's first step sees it, the later ones do not; the update no longer does
    std::vector<int> seenDown, seenUp, seenAny;
    runFrame(3, [&](int step){
        EXPECT_TRUE(Input::isFixedPhase());
        if (Input::getKeyDown(D_KEY_SPACE)) seenDown.push_back(step);
        if (Input::getKeyUp(D_KEY_SPACE)) seenUp.push_back(step);
        if (Input::anyKeyDown()) seenAny.push_back(step);
        // levels are the same in both phases
        EXPECT_FALSE(Input::getKey(D_KEY_SPACE));
    });
    EXPECT_FALSE(Input::isFixedPhase());
    EXPECT_EQ(seenDown, std::vector<int>({0}));
    EXPECT_EQ(seenUp, std::vector<int>({0}));
    EXPECT_EQ(seenAny, std::vector<int>({0}));
    EXPECT_FALSE(Input::getKeyDown(D_KEY_SPACE));

    // latched in a frame with 3 steps: only the first sees it, and the update of that frame too
    seenDown.clear();
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, 5, 5, 0);
    runFrame(3, [&](int step){
        if (Input::getMouseButtonDown(D_MOUSE_BUTTON_LEFT)) seenDown.push_back(step);
        EXPECT_TRUE(Input::getMouseButton(D_MOUSE_BUTTON_LEFT));
    });
    EXPECT_EQ(seenDown, std::vector<int>({0}));
    EXPECT_TRUE(Input::getMouseButtonDown(D_MOUSE_BUTTON_LEFT));

    // two frames without steps, then one: the step sees both frames' edges once
    seenDown.clear();
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, 5, 5, 0);
    runFrame(0);
    Engine::systemKeyDown(D_KEY_E, false, 0);
    runFrame(0);
    int ups = 0, downs = 0;
    runFrame(2, [&](int){
        if (Input::getMouseButtonUp(D_MOUSE_BUTTON_LEFT)) ups++;
        if (Input::getKeyDown(D_KEY_E)) downs++;
    });
    EXPECT_EQ(ups, 1);
    EXPECT_EQ(downs, 1);
}

TEST_F(InputEdgeTest, MouseDeltaSumsMoves){
    Engine::systemMouseEnter();
    Engine::systemMouseMove(100, 100, 0);
    Engine::systemMouseMove(110, 105, 0);
    Engine::systemMouseMove(130, 100, 0);
    runFrame();
    expectVector(Input::getMouseDelta(), Vector2(30, 0));
    expectVector(Input::getMousePosition(), Vector2(130, 100));

    runFrame();
    expectVector(Input::getMouseDelta(), Vector2(0, 0));

    Engine::systemMouseMove(120, 90, 0);
    Engine::systemMouseMove(100, 80, 0);
    runFrame(2, [](int){
        // the frame's delta in every step (read deltas in the update)
        expectVector(Input::getMouseDelta(), Vector2(-30, -20));
    });
    expectVector(Input::getMouseDelta(), Vector2(-30, -20));

    // a finger moving as the mouse (callMouseInTouchEvent) moves it too, from where it touched
    Engine::setCallMouseInTouchEvent(true);
    Engine::systemTouchStart(1, 400, 300);
    Engine::systemTouchMove(1, 410, 300);
    Engine::systemTouchEnd(1, 410, 300);
    Engine::setCallMouseInTouchEvent(false);
    runFrame();
    expectVector(Input::getMouseDelta(), Vector2(10, 0));
}

TEST_F(InputEdgeTest, MouseDeltaExcludesWarpAndEnter){
    Engine::systemMouseEnter();
    Engine::systemMouseMove(100, 100, 0);
    Engine::systemMouseMove(120, 100, 0);
    // a warp is no motion: the next move counts from where it put the pointer
    Engine::setMousePosition(300, 300);
    Engine::systemMouseMove(305, 300, 0);
    runFrame();
    expectVector(Input::getMouseDelta(), Vector2(25, 0));
    expectVector(Input::getMousePosition(), Vector2(305, 300));

    // out and in again: the first move after entering comes from outside
    Engine::systemMouseLeave();
    Engine::systemMouseEnter();
    Engine::systemMouseMove(10, 10, 0);
    Engine::systemMouseMove(12, 13, 0);
    runFrame();
    expectVector(Input::getMouseDelta(), Vector2(2, 3));
}

TEST_F(InputEdgeTest, ScrollDeltaPerWindow){
    Engine::systemMouseScroll(0, 1, 0);
    Engine::systemMouseScroll(0.5f, 2, 0);
    runFrame();
    EXPECT_EQ(Input::getMouseScrollDelta(), Vector2(0.5f, 3));
    // the deprecated sticky value: the last event
    EXPECT_EQ(Input::getMouseScroll(), Vector2(0.5f, 2));

    runFrame();
    EXPECT_EQ(Input::getMouseScrollDelta(), Vector2(0, 0));
    EXPECT_EQ(Input::getMouseScroll(), Vector2(0.5f, 2));
}

TEST_F(InputEdgeTest, TouchBeganEndedSameWindow){
    Engine::systemTouchStart(1, 50, 50);
    Engine::systemTouchMove(1, 60, 50);
    Engine::systemTouchEnd(1, 60, 50);
    EXPECT_TRUE(Input::getTouches().empty());
    runFrame();
    ASSERT_EQ(Input::getTouchCount(), 1u);
    Touch touch = Input::getTouch(0);
    EXPECT_EQ(touch.pointer, 1);
    EXPECT_EQ(touch.phase, TouchPhase::ENDED);
    EXPECT_TRUE(touch.began);
    expectVector(touch.startPosition, Vector2(50, 50));
    expectVector(touch.position, Vector2(60, 50));
    expectVector(touch.delta, Vector2(10, 0));
    EXPECT_TRUE(Input::getTouchDown(1));
    EXPECT_TRUE(Input::getTouchUp(1));
    EXPECT_TRUE(Input::getTouches().empty());

    runFrame();
    EXPECT_EQ(Input::getTouchCount(), 0u);
    EXPECT_FALSE(Input::getTouchDown(1));
    EXPECT_FALSE(Input::getTouchUp(1));

    // a held finger: BEGAN, then STATIONARY, then MOVED with the window's delta
    Engine::systemTouchStart(7, 200, 100);
    runFrame();
    ASSERT_EQ(Input::getTouchCount(), 1u);
    EXPECT_EQ(Input::getTouch(0).phase, TouchPhase::BEGAN);
    EXPECT_TRUE(Input::getTouchDown(7));
    ASSERT_EQ(Input::getTouches().size(), 1u);
    runFrame();
    EXPECT_EQ(Input::getTouch(0).phase, TouchPhase::STATIONARY);
    EXPECT_FALSE(Input::getTouchDown(7));
    EXPECT_GE(Input::getTouch(0).time, 0.0f);
    Engine::systemTouchMove(7, 210, 120);
    runFrame();
    EXPECT_EQ(Input::getTouch(0).phase, TouchPhase::MOVED);
    expectVector(Input::getTouch(0).delta, Vector2(10, 20));
    expectVector(Input::getTouch(0).startPosition, Vector2(200, 100));
    Engine::systemTouchCancel(7);
    runFrame();
    ASSERT_EQ(Input::getTouchCount(), 1u);
    EXPECT_EQ(Input::getTouch(0).phase, TouchPhase::CANCELED);
    EXPECT_TRUE(Input::getTouchUp(7));

    // the mouse as a finger (callTouchInMouseEvent) feeds the same records
    Engine::setCallTouchInMouseEvent(true);
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, 30, 40, 0);
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, 30, 40, 0);
    Engine::setCallTouchInMouseEvent(false);
    runFrame();
    ASSERT_EQ(Input::getTouchCount(), 1u);
    EXPECT_EQ(Input::getTouch(0).pointer, 0);
    EXPECT_EQ(Input::getTouch(0).phase, TouchPhase::ENDED);
    EXPECT_TRUE(Input::getTouchDown(0));
    EXPECT_TRUE(Input::getTouchUp(0));
}

TEST_F(InputEdgeTest, GamepadEdgesPerPad){
    Engine::systemGamepadConnect(0, "Pad Zero");
    Engine::systemGamepadConnect(3, "Pad Three");
    EXPECT_EQ(Input::numGamepads(), 2u);
    EXPECT_EQ(Input::getGamepadId(1), 3);
    EXPECT_EQ(Input::getGamepadAxis(3, D_GAMEPAD_AXIS_LEFT_TRIGGER), -1.0f);

    Engine::systemGamepadButtonDown(3, D_GAMEPAD_BUTTON_A);
    Engine::systemGamepadButtonUp(3, D_GAMEPAD_BUTTON_A);
    Engine::systemGamepadButtonDown(3, D_GAMEPAD_BUTTON_B);
    runFrame();
    EXPECT_TRUE(Input::getGamepadButtonDown(3, D_GAMEPAD_BUTTON_A));
    EXPECT_TRUE(Input::getGamepadButtonUp(3, D_GAMEPAD_BUTTON_A));
    EXPECT_FALSE(Input::isGamepadButtonPressed(3, D_GAMEPAD_BUTTON_A));
    EXPECT_FALSE(Input::getGamepadButtonDown(0, D_GAMEPAD_BUTTON_A));
    EXPECT_TRUE(Input::isGamepadButtonPressed(3, D_GAMEPAD_BUTTON_B));
    EXPECT_TRUE(Input::anyKeyDown());

    runFrame();
    EXPECT_FALSE(Input::getGamepadButtonDown(3, D_GAMEPAD_BUTTON_A));
    EXPECT_FALSE(Input::anyKeyDown());

    // a pad that goes away lets go of what it held: the game sees the release
    Engine::systemGamepadDisconnect(3);
    EXPECT_FALSE(Input::isGamepadConnected(3));
    EXPECT_EQ(Input::numGamepads(), 1u);
    runFrame();
    EXPECT_TRUE(Input::getGamepadButtonUp(3, D_GAMEPAD_BUTTON_B));
    EXPECT_FALSE(Input::isGamepadButtonPressed(3, D_GAMEPAD_BUTTON_B));

    // reconnected: a fresh pad
    Engine::systemGamepadConnect(3, "Pad Three");
    runFrame();
    EXPECT_FALSE(Input::getGamepadButtonUp(3, D_GAMEPAD_BUTTON_B));
    EXPECT_EQ(Input::getGamepadName(3), "Pad Three");
}

TEST_F(InputEdgeTest, LatchAllocatesNothing){
    if (!testsupport::countsAllocations())
        GTEST_SKIP() << "this binary cannot count allocations";
    Engine::systemGamepadConnect(1, "Pad");
    Engine::systemTouchStart(3, 10, 10);
    // warm up: every window seen once
    for (int i = 0; i < 3; i++){
        Engine::systemKeyDown(D_KEY_A + i, false, 0);
        Engine::systemKeyUp(D_KEY_A + i, false, 0);
        runFrame(1);
    }
    // the latch and the queries, not the event dispatch around them
    uint64_t allocations = 0;
    for (int i = 0; i < 100; i++){
        Engine::systemKeyDown(D_KEY_A + (i % 26), false, 0);
        Engine::systemKeyUp(D_KEY_A + (i % 26), false, 0);
        Engine::systemGamepadButtonDown(1, i % Input::GAMEPAD_BUTTONS);
        Engine::systemGamepadButtonUp(1, i % Input::GAMEPAD_BUTTONS);
        Engine::systemTouchMove(3, 10.0f + i, 10);
        Engine::systemMouseScroll(0, 1, 0);
        testsupport::ThreadAllocationCount count;
        runFrame(i % 3);
        (void)Input::getKeyDown(D_KEY_A);
        (void)Input::getGamepadButtonUp(1, 0);
        (void)Input::getTouch(0);
        (void)Input::getTouchDown(3);
        (void)Input::getMouseDelta();
        allocations += count.count();
    }
    EXPECT_EQ(allocations, 0u);
}
