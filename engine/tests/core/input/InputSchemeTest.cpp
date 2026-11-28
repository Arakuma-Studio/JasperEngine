// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The input scheme (docs/specs/input-device-awareness.md AC1-AC7): evidence through
// Engine::system* (and the tracker's note* where a test needs its own clock), each frame latched
// with Input::beginFrame(dt) as Engine::systemDraw does.

#include "Engine.h"
#include "Input.h"
#include "TestSystem.h"
#include "input/InputActions.h"
#include "input/InputScheme.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace InputSchemeTest_cpp {

using namespace jasperengine;

namespace {

    class InputSchemeTest : public ::testing::Test {
    protected:
        void SetUp() override{
            test::TestSystem::get().setScreenSize(640, 360);
            Engine::setCanvasSize(640, 360);
            Engine::setScalingMode(Scaling::FITWIDTH);
            Engine::systemViewChanged();
            Input::resetFrameState();
            InputSchemeTracker::setOverride(std::nullopt);
            InputSchemeTracker::reset(false);
        }

        void TearDown() override{
            InputSchemeTracker::onSchemeChanged.clear();
            InputSchemeTracker::setOverride(std::nullopt);
            InputActions::unload();
            Engine::systemTouchCancel();
            for (int pad : {0, 1})
                Engine::systemGamepadDisconnect(pad);
            Input::resetFrameState();
            InputSchemeTracker::reset(false);
        }

        static void frame(){
            Input::beginFrame(1.0 / 60.0);
            Input::endFrame();
        }

        static InputScheme current(){
            return InputSchemeTracker::getCurrent();
        }
    };

}

TEST_F(InputSchemeTest, SwitchesOnEachDeviceKind){
    EXPECT_EQ(current(), InputScheme::KeyboardMouse);
    EXPECT_EQ(InputSchemeTracker::getLastGamepad(), -1);
    Engine::systemGamepadConnect(0, "Xbox Wireless Controller");
    frame();
    // a pad connected does not switch until it is used
    EXPECT_EQ(current(), InputScheme::KeyboardMouse);

    Engine::systemGamepadButtonDown(0, D_GAMEPAD_BUTTON_A);
    // not before the next latch
    EXPECT_EQ(current(), InputScheme::KeyboardMouse);
    frame();
    EXPECT_EQ(current(), InputScheme::Gamepad);
    EXPECT_EQ(InputSchemeTracker::getLastGamepad(), 0);
    EXPECT_EQ(InputSchemeTracker::getGamepadFamily(), GamepadFamily::Xbox);
    // a pad never changes the pointer
    EXPECT_EQ(InputSchemeTracker::getPointer(), InputScheme::KeyboardMouse);
    Engine::systemGamepadButtonUp(0, D_GAMEPAD_BUTTON_A);

    Engine::systemTouchStart(1, 100, 100);
    frame();
    EXPECT_EQ(current(), InputScheme::Touch);
    EXPECT_EQ(InputSchemeTracker::getPointer(), InputScheme::Touch);
    Engine::systemTouchEnd(1, 100, 100);

    Engine::systemMouseScroll(0, 1, 0);
    frame();
    EXPECT_EQ(current(), InputScheme::KeyboardMouse);
    EXPECT_EQ(InputSchemeTracker::getPointer(), InputScheme::KeyboardMouse);

    Engine::systemTouchStart(1, 300, 200);
    Engine::systemTouchEnd(1, 300, 200);
    frame();
    EXPECT_EQ(current(), InputScheme::Touch);
    Engine::systemKeyDown(D_KEY_SPACE, false, 0);
    frame();
    EXPECT_EQ(current(), InputScheme::KeyboardMouse);
    Engine::systemKeyUp(D_KEY_SPACE, false, 0);

    // a stick past half way is pad use too
    Engine::systemGamepadAxisMove(0, D_GAMEPAD_AXIS_LEFT_Y, 0.8f);
    frame();
    EXPECT_EQ(current(), InputScheme::Gamepad);
    EXPECT_EQ(InputSchemeTracker::getChangeCount(), 6u);

    // a touch device starts on Touch, without an event
    InputSchemeTracker::reset(true);
    EXPECT_EQ(current(), InputScheme::Touch);
    EXPECT_EQ(InputSchemeTracker::getPointer(), InputScheme::Touch);
    EXPECT_EQ(InputSchemeTracker::getChangeCount(), 0u);
}

TEST_F(InputSchemeTest, FingerWhileKeyHeldKeepsKeyboard){
    Engine::systemKeyDown(D_KEY_W, false, 0);
    frame();
    EXPECT_EQ(current(), InputScheme::KeyboardMouse);

    // looking with a finger while walking with W
    Engine::systemTouchStart(1, 400, 200);
    Engine::systemTouchMove(1, 450, 210);
    frame();
    EXPECT_EQ(current(), InputScheme::KeyboardMouse);
    EXPECT_EQ(InputSchemeTracker::getPointer(), InputScheme::Touch);
    Engine::systemTouchEnd(1, 450, 210);

    Engine::systemKeyUp(D_KEY_W, false, 0);
    frame();
    EXPECT_EQ(current(), InputScheme::KeyboardMouse);
    Engine::systemTouchStart(1, 400, 200);
    frame();
    EXPECT_EQ(current(), InputScheme::Touch);
    Engine::systemTouchEnd(1, 400, 200);

    // a modifier held is not walking
    Engine::systemKeyDown(D_KEY_E, false, 0);
    Engine::systemKeyUp(D_KEY_E, false, 0);
    Engine::systemKeyDown(D_KEY_LEFT_SHIFT, false, D_MODIFIER_SHIFT);
    frame();
    EXPECT_EQ(current(), InputScheme::KeyboardMouse);
    Engine::systemTouchStart(1, 400, 200);
    frame();
    EXPECT_EQ(current(), InputScheme::Touch);
    Engine::systemTouchEnd(1, 400, 200);
    Engine::systemKeyUp(D_KEY_LEFT_SHIFT, false, 0);
}

TEST_F(InputSchemeTest, TaggedAndSameDeviceFingersAreMouse){
    InputSchemeTracker::reset(true);
    // a mouse the platform delivers as a finger (Android's mouse tool type, iOS indirect pointer)
    Engine::setInputSource(InputSourceTag::Mouse);
    Engine::systemTouchStart(1, 100, 100);
    frame();
    EXPECT_EQ(current(), InputScheme::KeyboardMouse);
    EXPECT_EQ(InputSchemeTracker::getPointer(), InputScheme::KeyboardMouse);
    Engine::setInputSource(InputSourceTag::Mouse);
    Engine::systemTouchEnd(1, 100, 100);
    // the tag lasts one event
    EXPECT_EQ(InputSchemeTracker::getSourceTag(), InputSourceTag::None);

    // device 8 is a touch screen
    Engine::setInputSource(InputSourceTag::None, 8);
    Engine::systemTouchStart(2, 200, 100);
    Engine::systemTouchEnd(2, 200, 100);
    frame();
    EXPECT_EQ(current(), InputScheme::Touch);

    // device 7 sent a key: its fingers are an emulator's PC mouse
    Engine::setInputSource(InputSourceTag::None, 7);
    Engine::systemKeyDown(D_KEY_A, false, 0);
    Engine::systemKeyUp(D_KEY_A, false, 0);
    Engine::setInputSource(InputSourceTag::None, 8);
    Engine::systemTouchStart(2, 200, 100);
    Engine::systemTouchEnd(2, 200, 100);
    frame();
    EXPECT_EQ(current(), InputScheme::Touch);
    Engine::setInputSource(InputSourceTag::None, 7);
    Engine::systemTouchStart(3, 300, 100);
    Engine::systemTouchEnd(3, 300, 100);
    frame();
    EXPECT_EQ(current(), InputScheme::KeyboardMouse);
    Engine::setInputSource(InputSourceTag::None, 8);
    Engine::systemTouchStart(2, 200, 100);
    Engine::systemTouchEnd(2, 200, 100);
    frame();
    EXPECT_EQ(current(), InputScheme::Touch);
    // not after 10 s
    InputSchemeTracker::setSource(InputSourceTag::None, 7);
    InputSchemeTracker::noteKey(D_KEY_B, false, false, 100.0);
    InputSchemeTracker::setSource(InputSourceTag::None, 8);
    InputSchemeTracker::noteTouchStart(10, 10, 100.5);
    InputSchemeTracker::setSource(InputSourceTag::None, 7);
    InputSchemeTracker::noteTouchStart(10, 10, 111.0);
    InputSchemeTracker::setSource(InputSourceTag::None, -1);
    InputSchemeTracker::latch();
    EXPECT_EQ(current(), InputScheme::Touch);
}

TEST_F(InputSchemeTest, NoiseDoesNotSwitch){
    InputSchemeTracker::reset(true);
    Engine::systemGamepadConnect(0, "Pad");

    // 11 px of mouse motion
    Engine::systemMouseMove(100, 100, 0);
    Engine::systemMouseMove(105, 100, 0);
    Engine::systemMouseMove(111, 100, 0);
    frame();
    EXPECT_EQ(current(), InputScheme::Touch);

    // a compatibility mouse event 100 ms after a touch ended at the same place
    InputSchemeTracker::noteTouchEnd(50, 50, 1000.0);
    InputSchemeTracker::noteMouseButton(52, 50, 1000.1);
    InputSchemeTracker::noteMouseMove(53, 51, 1000.1);
    InputSchemeTracker::latch();
    EXPECT_EQ(current(), InputScheme::Touch);

    // a key repeat, Shift alone, a key typed in a text field, the engine's Escape
    Engine::systemKeyDown(D_KEY_A, true, 0);
    Engine::systemKeyUp(D_KEY_A, false, 0);
    Engine::systemKeyDown(D_KEY_LEFT_SHIFT, false, D_MODIFIER_SHIFT);
    Engine::systemKeyUp(D_KEY_LEFT_SHIFT, false, 0);
    InputSchemeTracker::noteKey(D_KEY_H, false, true, 1001.0);
    Engine::setInputSource(InputSourceTag::Synthetic);
    Engine::systemKeyDown(D_KEY_ESCAPE, false, 0);
    Engine::setInputSource(InputSourceTag::Synthetic);
    Engine::systemKeyUp(D_KEY_ESCAPE, false, 0);
    // a stick at 0.3, a trigger short of half way
    Engine::systemGamepadAxisMove(0, D_GAMEPAD_AXIS_LEFT_X, 0.3f);
    Engine::systemGamepadAxisMove(0, D_GAMEPAD_AXIS_RIGHT_TRIGGER, -1.0f);
    Engine::systemGamepadAxisMove(0, D_GAMEPAD_AXIS_RIGHT_TRIGGER, -0.2f);
    frame();
    EXPECT_EQ(current(), InputScheme::Touch);
    EXPECT_EQ(InputSchemeTracker::getChangeCount(), 0u);

    // the controls: 12 px within 0.25 s (spread over more time it is not), a trigger past half way
    InputSchemeTracker::noteMouseMove(200, 200, 1999.0);   // where the mouse is (a jump counts as motion)
    InputSchemeTracker::latch();
    ASSERT_EQ(current(), InputScheme::KeyboardMouse);
    InputSchemeTracker::noteTouchStart(5, 5, 1999.5);
    InputSchemeTracker::latch();
    ASSERT_EQ(current(), InputScheme::Touch);
    InputSchemeTracker::noteMouseMove(200, 200, 2000.0);
    InputSchemeTracker::noteMouseMove(206, 200, 2000.1);
    InputSchemeTracker::noteMouseMove(212, 200, 2000.4);
    InputSchemeTracker::latch();
    EXPECT_EQ(current(), InputScheme::Touch);
    InputSchemeTracker::noteMouseMove(218, 200, 2000.45);
    InputSchemeTracker::latch();
    EXPECT_EQ(current(), InputScheme::KeyboardMouse);
    Engine::systemGamepadAxisMove(0, D_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.2f);
    frame();
    EXPECT_EQ(current(), InputScheme::Gamepad);
}

TEST_F(InputSchemeTest, OneChangePerFrame){
    ASSERT_TRUE(InputActions::loadFromText("scheme.inputactions", R"({"version":1,"maps":[{"name":"Gameplay","actions":[
     {"name":"Jump","bindings":[{"path":"gamepad/a"}]}]}]})"));
    InputSchemeTracker::reset(true);
    Engine::systemGamepadConnect(0, "Pad");
    std::vector<std::pair<std::string, std::string>> changes;
    InputSchemeTracker::onSchemeChanged.add("test", std::function<void(const std::string&, const std::string&)>(
        [&](const std::string& scheme, const std::string& previous){ changes.emplace_back(scheme, previous); }));
    InputScheme seenByAction = InputScheme::Touch;
    InputActions::get("Gameplay/Jump")->onPerformed.add("test", std::function<void(InputActionEvent&)>(
        [&](InputActionEvent&){ seenByAction = InputSchemeTracker::getCurrent(); }));

    // Touch -> Gamepad -> Touch in one window: nothing
    Engine::systemGamepadButtonDown(0, D_GAMEPAD_BUTTON_X);
    Engine::systemGamepadButtonUp(0, D_GAMEPAD_BUTTON_X);
    Engine::systemTouchStart(1, 10, 10);
    Engine::systemTouchEnd(1, 10, 10);
    frame();
    EXPECT_TRUE(changes.empty());
    EXPECT_EQ(current(), InputScheme::Touch);

    // Touch -> Gamepad: once, before the actions' evaluation
    Engine::systemGamepadButtonDown(0, D_GAMEPAD_BUTTON_A);
    frame();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].first, "Gamepad");
    EXPECT_EQ(changes[0].second, "Touch");
    EXPECT_EQ(seenByAction, InputScheme::Gamepad);
    frame();
    EXPECT_EQ(changes.size(), 1u);
    Engine::systemGamepadButtonUp(0, D_GAMEPAD_BUTTON_A);
}

TEST_F(InputSchemeTest, OverrideWinsAndResumes){
    int changes = 0;
    InputSchemeTracker::onSchemeChanged.add("test", std::function<void(const std::string&, const std::string&)>(
        [&](const std::string&, const std::string&){ changes++; }));
    InputSchemeTracker::setOverride(InputScheme::Gamepad);
    frame();
    EXPECT_EQ(current(), InputScheme::Gamepad);
    EXPECT_EQ(changes, 1);

    Engine::systemTouchStart(1, 10, 10);
    Engine::systemTouchEnd(1, 10, 10);
    frame();
    EXPECT_EQ(current(), InputScheme::Gamepad);
    EXPECT_EQ(changes, 1);

    InputSchemeTracker::setOverride(std::nullopt);
    frame();
    EXPECT_EQ(current(), InputScheme::Touch);
    EXPECT_EQ(changes, 2);

    // forced Touch also forces the pointer; a reset starts on the forced scheme, without an event
    InputSchemeTracker::setOverride(InputScheme::Touch);
    InputSchemeTracker::reset(false);
    EXPECT_EQ(current(), InputScheme::Touch);
    EXPECT_EQ(InputSchemeTracker::getPointer(), InputScheme::Touch);
    EXPECT_EQ(InputActions::getSchemeOverride(), "Touch");
    EXPECT_TRUE(InputActions::setSchemeOverride(""));
    EXPECT_FALSE(InputActions::setSchemeOverride("Joystick"));
    EXPECT_EQ(InputActions::getSchemeOverride(), "");
}

TEST_F(InputSchemeTest, ZeroAllocations){
    if (!testsupport::countsAllocations())
        GTEST_SKIP() << "this test binary cannot count allocations";
    InputSchemeTracker::noteGamepadConnected(0, "DualSense Wireless Controller");
    int subscribed = 0;
    // a subscriber is called with the static names: no allocation either
    InputSchemeTracker::onSchemeChanged.add("test", std::function<void(const std::string&, const std::string&)>(
        [&](const std::string&, const std::string&){ subscribed++; }));
    InputSchemeTracker::latch();
    const uint32_t before = InputSchemeTracker::getChangeCount();
    double now = 10.0;
    testsupport::ThreadAllocationCount allocations;
    for (int latch = 0; latch < 600; latch++){
        // a change every 15th latch: 40 changes
        const int kind = (latch / 15) % 3;
        for (int e = 0; e < 17; e++){
            now += 0.001;
            if (latch % 15 == 0 && e == 16){
                if (kind == 0) InputSchemeTracker::noteGamepadButton(0);
                else if (kind == 1) InputSchemeTracker::noteTouchStart(5, 5, now);
                else InputSchemeTracker::noteMouseScroll();
            }else{
                // noise of the scheme in use: drift, repeats, motion under the threshold
                InputSchemeTracker::noteGamepadAxis(0, D_GAMEPAD_AXIS_LEFT_X, 0.1f, 0.2f);
                InputSchemeTracker::noteKey(D_KEY_A, true, false, now);
                InputSchemeTracker::noteKeyUp(D_KEY_A);
                InputSchemeTracker::noteMouseMove(100.0f, 100.0f, now);
            }
        }
        InputSchemeTracker::latch();
    }
    EXPECT_EQ(allocations.count(), 0u);
    EXPECT_EQ(InputSchemeTracker::getChangeCount() - before, 40u);
    EXPECT_EQ(subscribed, 40);
}

}  // namespace InputSchemeTest_cpp
