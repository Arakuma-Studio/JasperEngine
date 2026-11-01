// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// engine/core/Input and engine/core/input (spec docs/superpowers/specs/2026-10-31-input-design.md):
// edges per input frame (InputEdgesTest), touches with Unity's phases (InputTouchTest), the
// input actions with Unity's Input Manager numbers, virtual sources, rebinding and the JSON
// shape (InputActionsTest), the control-name table (InputControlNamesTest). The device state
// is written as the platforms do between frames; Input::beginFrame / endFrame bracket a frame
// that runs the update.

#include <gtest/gtest.h>

#include "Engine.h"
#include "Input.h"
#include "Out.h"
#include "PhysicsTestScene.h"
#include "input/InputActions.h"
#include "input/InputControlNames.h"

#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr double frameTime = 1.0 / 60.0;

    class InputFixture : public ::testing::Test {
    protected:
        void SetUp() override {
            Input::reset();
            while (Input::numGamepads() > 0) Input::removeGamepad(Input::getGamepadId(0));
            InputActions::loadDefaults();
            InputActions::resetRuntimeState();
            // a frame of nothing, so the removals above are seen and gone
            begin();
            end();
        }

        void TearDown() override {
            if (Input::isInFrame()) Input::endFrame();
            while (Input::numGamepads() > 0) Input::removeGamepad(Input::getGamepadId(0));
            Input::reset();
            InputActions::loadDefaults();
            InputActions::resetRuntimeState();
        }

        static void begin(double dt = frameTime, InputSuppression suppression = InputSuppression()) {
            Input::beginFrame(dt, suppression);
        }
        static void end() {
            Input::endFrame();
        }
        // a whole frame
        static void frame(double dt = frameTime, InputSuppression suppression = InputSuppression()) {
            begin(dt, suppression);
            end();
        }
    };

    class InputEdgesTest : public InputFixture {};
    class InputTouchTest : public InputFixture {};
    class InputActionsTest : public InputFixture {};

    // what the engine logged since `mark` (Log reaches Out in the editor)
    std::string loggedSince(uint64_t mark) {
        std::string text;
        for (const editor::LogEntry& entry : editor::Out::getLogsSince(mark)) text += entry.message + "\n";
        return text;
    }

    size_t count(const std::string& text, const std::string& what) {
        size_t n = 0;
        for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) n++;
        return n;
    }

    std::string utf8(const char32_t* text) {
        std::string out;
        for (; *text; text++) {
            const char32_t c = *text;
            if (c < 0x80) out += static_cast<char>(c);
            else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
            else if (c < 0x10000) { out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
            else { out += static_cast<char>(0xF0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
        }
        return out;
    }

    InputAction actionNamed(const std::string& name) {
        for (const InputAction& action : InputActions::getActions()) {
            if (action.name == name) return action;
        }
        ADD_FAILURE() << "no action " << name;
        return InputAction();
    }

    std::vector<std::string> names(const std::vector<InputControl>& controls) {
        std::vector<std::string> out;
        for (const InputControl& control : controls) out.push_back(InputControlNames::name(control));
        return out;
    }

}

// ================================================================== edges

TEST_F(InputEdgesTest, DownAndUpAreSeenByOneFrame) {
    Input::addKeyPressed(D_KEY_SPACE);
    begin();
    EXPECT_TRUE(Input::getKeyDown(D_KEY_SPACE));
    EXPECT_TRUE(Input::isKeyPressed(D_KEY_SPACE));
    EXPECT_FALSE(Input::getKeyUp(D_KEY_SPACE));
    end();

    // held frames: no edges
    for (int i = 0; i < 3; i++) {
        begin();
        EXPECT_FALSE(Input::getKeyDown(D_KEY_SPACE));
        EXPECT_TRUE(Input::isKeyPressed(D_KEY_SPACE));
        end();
    }

    Input::releaseKeyPressed(D_KEY_SPACE);
    begin();
    EXPECT_TRUE(Input::getKeyUp(D_KEY_SPACE));
    EXPECT_FALSE(Input::isKeyPressed(D_KEY_SPACE));
    end();
    begin();
    EXPECT_FALSE(Input::getKeyUp(D_KEY_SPACE));
    end();
}

TEST_F(InputEdgesTest, PressAndReleaseInOneFrameGiveBothEdges) {
    Input::addKeyPressed(D_KEY_A);
    Input::releaseKeyPressed(D_KEY_A);
    begin();
    EXPECT_TRUE(Input::getKeyDown(D_KEY_A));
    EXPECT_TRUE(Input::getKeyUp(D_KEY_A));
    EXPECT_FALSE(Input::isKeyPressed(D_KEY_A));
    end();

    // down, up, down
    Input::addKeyPressed(D_KEY_B);
    Input::releaseKeyPressed(D_KEY_B);
    Input::addKeyPressed(D_KEY_B);
    begin();
    EXPECT_TRUE(Input::getKeyDown(D_KEY_B));
    EXPECT_TRUE(Input::getKeyUp(D_KEY_B));
    EXPECT_TRUE(Input::isKeyPressed(D_KEY_B));
    end();
}

TEST_F(InputEdgesTest, KeyRepeatMakesNoEdge) {
    Input::addKeyPressed(D_KEY_W);
    frame();
    Input::addKeyPressed(D_KEY_W, true);
    Input::addKeyPressed(D_KEY_W, true);
    // a platform's second down without an up is no press either
    Input::addKeyPressed(D_KEY_W, false);
    begin();
    EXPECT_FALSE(Input::getKeyDown(D_KEY_W));
    EXPECT_TRUE(Input::isKeyPressed(D_KEY_W));
    end();
}

TEST_F(InputEdgesTest, EdgesWaitForAFrameThatRunsTheUpdate) {
    // a paused or held frame calls neither beginFrame nor endFrame
    Input::addMousePressed(D_MOUSE_BUTTON_LEFT);
    begin();
    EXPECT_TRUE(Input::getMouseButtonDown(D_MOUSE_BUTTON_LEFT));
    end();
    Input::releaseMousePressed(D_MOUSE_BUTTON_LEFT);
    begin();
    EXPECT_TRUE(Input::getMouseButtonUp(D_MOUSE_BUTTON_LEFT));
    end();
}

TEST_F(InputEdgesTest, APressDuringAFrameBelongsToTheNext) {
    begin();
    Input::addKeyPressed(D_KEY_ENTER);
    EXPECT_FALSE(Input::getKeyDown(D_KEY_ENTER));
    EXPECT_TRUE(Input::isKeyPressed(D_KEY_ENTER));
    end();
    begin();
    EXPECT_TRUE(Input::getKeyDown(D_KEY_ENTER));
    end();
}

TEST_F(InputEdgesTest, AnyKeyAndAnyKeyDown) {
    begin();
    EXPECT_FALSE(Input::isAnyKeyPressed());
    EXPECT_FALSE(Input::isAnyKeyDown());
    end();

    Input::addMousePressed(D_MOUSE_BUTTON_RIGHT);
    begin();
    EXPECT_TRUE(Input::isAnyKeyPressed());
    EXPECT_TRUE(Input::isAnyKeyDown());
    EXPECT_EQ(Input::getPressedControl(), "mouse_right");
    end();
    begin();
    EXPECT_TRUE(Input::isAnyKeyPressed());
    EXPECT_FALSE(Input::isAnyKeyDown());
    EXPECT_EQ(Input::getPressedControl(), "");
    end();

    Input::addGamepad(3, "pad");
    Input::addGamepadButtonPressed(3, D_GAMEPAD_BUTTON_Y);
    Input::addKeyPressed(D_KEY_Q);
    begin();
    EXPECT_TRUE(Input::isAnyKeyDown());
    // the first one
    EXPECT_EQ(Input::getPressedControl(), "gamepad_y");
    EXPECT_TRUE(Input::getGamepadButtonDown(3, D_GAMEPAD_BUTTON_Y));
    EXPECT_TRUE(Input::getGamepadButtonDown(-1, D_GAMEPAD_BUTTON_Y));
    EXPECT_FALSE(Input::getGamepadButtonDown(2, D_GAMEPAD_BUTTON_Y));
    end();
}

TEST_F(InputEdgesTest, PressedControlNamesAGamepadAxisPastHalf) {
    Input::addGamepad(0, "pad");
    Input::setGamepadAxisValue(0, D_GAMEPAD_AXIS_LEFT_X, 0.3f);
    begin();
    EXPECT_EQ(Input::getPressedControl(), "");
    end();
    Input::setGamepadAxisValue(0, D_GAMEPAD_AXIS_LEFT_X, -0.8f);
    begin();
    EXPECT_EQ(Input::getPressedControl(), "left_x-");
    end();
    Input::setGamepadAxisValue(0, D_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.2f);
    begin();
    EXPECT_EQ(Input::getPressedControl(), "right_trigger");
    end();
}

TEST_F(InputEdgesTest, GamepadDisconnectReleasesItsButtons) {
    Input::addGamepad(1, "pad");
    Input::addGamepadButtonPressed(1, D_GAMEPAD_BUTTON_A);
    frame();
    Input::removeGamepad(1);
    begin();
    EXPECT_TRUE(Input::getGamepadButtonUp(1, D_GAMEPAD_BUTTON_A));
    EXPECT_TRUE(Input::getGamepadButtonUp(-1, D_GAMEPAD_BUTTON_A));
    EXPECT_FALSE(Input::isGamepadConnected(1));
    end();
    begin();
    EXPECT_FALSE(Input::getGamepadButtonUp(-1, D_GAMEPAD_BUTTON_A));
    end();
}

TEST_F(InputEdgesTest, ReleaseAllGivesTheUps) {
    Input::addKeyPressed(D_KEY_D);
    Input::addMousePressed(D_MOUSE_BUTTON_LEFT);
    Input::addTouch(4, 10, 10);
    frame();
    Input::releaseAll();
    // the finger is left to the platform's touch cancel (it reaches the UI and onTouchCancel)
    EXPECT_EQ(Input::numTouches(), 1u);
    begin();
    EXPECT_TRUE(Input::getKeyUp(D_KEY_D));
    EXPECT_TRUE(Input::getMouseButtonUp(D_MOUSE_BUTTON_LEFT));
    EXPECT_FALSE(Input::isAnyKeyPressed());
    ASSERT_EQ(Input::getTouchCount(), 1);
    EXPECT_EQ(Input::getTouch(0).phase, TouchPhase::Canceled);
    end();
}

TEST_F(InputEdgesTest, AnAppPauseStillLetsThePlatformCancelReachScripts) {
    // review fix: Engine::systemPause released everything, so the platform's systemTouchCancel
    // found no finger and onTouchCancel (and the documents' cancel) never came
    // (systemPause saves the user settings through System::instance(): one must be set, also
    // when this suite runs on its own)
    if (!System::hasInstance()) physicstest::installTestSystem();
    std::vector<int> cancelled;
    struct Listener {
        std::vector<int>* log;
        void cancel(int pointer, float, float) { log->push_back(pointer); }
    } listener{&cancelled};
    const bool enabled = Engine::onTouchCancel.isEnabled();
    Engine::onTouchCancel.setEnabled(true);
    Engine::onTouchCancel.add<Listener, &Listener::cancel>("InputTest", &listener);

    Input::addTouch(6, 10, 10);
    frame();
    Engine::systemPause();
    EXPECT_EQ(Input::numTouches(), 1u);
    Engine::systemTouchCancel();
    EXPECT_EQ(Input::numTouches(), 0u);
    Engine::systemResume();
    Engine::onTouchCancel.remove("InputTest");
    Engine::onTouchCancel.setEnabled(enabled);
    EXPECT_EQ(cancelled, std::vector<int>{6});

    begin();
    ASSERT_EQ(Input::getTouchCount(), 1);
    EXPECT_EQ(Input::getTouch(0).phase, TouchPhase::Canceled);
    end();
    begin();
    EXPECT_EQ(Input::getTouchCount(), 0);
    end();
}

TEST_F(InputEdgesTest, InputStringIsUtf8WithAstralCharacters) {
    Input::addInputCharacter('h');
    Input::addInputCharacter(0x00E9);      // é
    Input::addInputCharacter(0x1F600);     // an emoji, beyond the BMP
    Input::addInputCharacter('\b');
    Input::addInputCharacter('\r');
    Input::addInputCharacter(0x1B);        // escape: not text
    begin();
    EXPECT_EQ(Input::getInputString(), utf8(U"hé\U0001F600\b\n"));
    end();
    begin();
    EXPECT_EQ(Input::getInputString(), "");
    end();

    // through the engine: committed text and characters (surrogate pairs combined where wchar_t is 16 bits)
    Engine::systemTextInput(utf8(U"ñ\U0001F680"));
    begin();
    EXPECT_EQ(Input::getInputString(), utf8(U"ñ\U0001F680"));
    end();
}

TEST_F(InputEdgesTest, MouseDeltaAccumulatesEveryMoveIncludingOutsideTheCanvas) {
    Input::addMouseMovement(10, 10);
    Input::addMouseMovement(15, 12);
    // a captured or dragged mouse outside the canvas still moves
    Input::addMouseMovement(-50, 40);
    begin();
    EXPECT_FLOAT_EQ(Input::getMouseDelta().x, -60.0f);
    EXPECT_FLOAT_EQ(Input::getMouseDelta().y, 30.0f);
    end();
    begin();
    EXPECT_FLOAT_EQ(Input::getMouseDelta().x, 0.0f);
    end();
    // movements from the last known position on
    Input::addMouseMovement(-40, 40);
    begin();
    EXPECT_FLOAT_EQ(Input::getMouseDelta().x, 10.0f);
    end();
    // a jump after the pointer came back is no movement
    Input::resetMouseMovement();
    Input::addMouseMovement(500, 500);
    begin();
    EXPECT_FLOAT_EQ(Input::getMouseDelta().x, 0.0f);
    end();
}

TEST_F(InputEdgesTest, ScrollDeltaSumsTheWheelEvents) {
    Input::setMouseScroll(0, 1);
    Input::setMouseScroll(0.5f, 1);
    begin();
    EXPECT_FLOAT_EQ(Input::getMouseScrollDelta().y, 2.0f);
    EXPECT_FLOAT_EQ(Input::getMouseScrollDelta().x, 0.5f);
    // the last event's offsets, as before
    EXPECT_FLOAT_EQ(Input::getMouseScroll().y, 1.0f);
    end();
    begin();
    EXPECT_FLOAT_EQ(Input::getMouseScrollDelta().y, 0.0f);
    end();
}

// ================================================================== touches

TEST_F(InputTouchTest, BeganMovedStationaryEnded) {
    Input::addTouch(7, 100, 200);
    begin();
    ASSERT_EQ(Input::getTouchCount(), 1);
    Touch touch = Input::getTouch(0);
    EXPECT_EQ(touch.fingerId, 7);
    EXPECT_EQ(touch.phase, TouchPhase::Began);
    EXPECT_FLOAT_EQ(touch.position.x, 100);
    EXPECT_FLOAT_EQ(touch.deltaPosition.x, 0);
    EXPECT_EQ(touch.tapCount, 1);
    end();

    Input::setTouchPosition(7, 105, 190);
    begin(0.02);
    touch = Input::getTouch(0);
    EXPECT_EQ(touch.phase, TouchPhase::Moved);
    EXPECT_FLOAT_EQ(touch.deltaPosition.x, 5);
    EXPECT_FLOAT_EQ(touch.deltaPosition.y, -10);
    EXPECT_NEAR(touch.deltaTime, 0.02, 1e-5);
    EXPECT_FLOAT_EQ(touch.startPosition.x, 100);
    end();

    begin(0.02);
    touch = Input::getTouch(0);
    EXPECT_EQ(touch.phase, TouchPhase::Stationary);
    EXPECT_FLOAT_EQ(touch.deltaPosition.x, 0);
    // since its last position change
    EXPECT_NEAR(touch.deltaTime, 0.02, 1e-5);
    end();

    Input::setTouchPosition(7, 110, 190);
    Input::removeTouch(7);
    // the legacy list drops the finger at once
    EXPECT_EQ(Input::numTouches(), 0u);
    begin(0.02);
    ASSERT_EQ(Input::getTouchCount(), 1);
    touch = Input::getTouch(0);
    EXPECT_EQ(touch.phase, TouchPhase::Ended);
    EXPECT_FLOAT_EQ(touch.position.x, 110);
    EXPECT_FLOAT_EQ(touch.deltaPosition.x, 5);
    EXPECT_NEAR(touch.deltaTime, 0.04, 1e-5);
    end();

    begin();
    EXPECT_EQ(Input::getTouchCount(), 0);
    EXPECT_EQ(Input::getTouch(0).fingerId, -1);
    end();
}

TEST_F(InputTouchTest, ATouchThatBeginsAndEndsBetweenFramesIsBeganThenEnded) {
    Input::addTouch(0, 10, 10);
    Input::removeTouch(0);
    begin();
    ASSERT_EQ(Input::getTouchCount(), 1);
    EXPECT_EQ(Input::getTouch(0).phase, TouchPhase::Began);
    end();
    begin();
    ASSERT_EQ(Input::getTouchCount(), 1);
    EXPECT_EQ(Input::getTouch(0).phase, TouchPhase::Ended);
    end();
    begin();
    EXPECT_EQ(Input::getTouchCount(), 0);
    end();
}

TEST_F(InputTouchTest, CancelIsCanceled) {
    Input::addTouch(2, 10, 10);
    frame();
    Input::cancelTouch(2);
    begin();
    ASSERT_EQ(Input::getTouchCount(), 1);
    EXPECT_EQ(Input::getTouch(0).phase, TouchPhase::Canceled);
    end();
}

TEST_F(InputTouchTest, QuickTapsCountUpAndResetAfterTimeOrDistance) {
    auto tap = [](float x, float y) {
        Input::addTouch(0, x, y);
        frame(0.05);
        Input::removeTouch(0);
        frame(0.05);
        frame(0.05);
    };
    int last = 0;
    auto tapCountOfNext = [&last](float x, float y) {
        Input::addTouch(0, x, y);
        begin(0.05);
        last = Input::getTouch(0).tapCount;
        end();
        Input::removeTouch(0);
        frame(0.05);
        frame(0.05);
        return last;
    };
    EXPECT_EQ(tapCountOfNext(100, 100), 1);
    EXPECT_EQ(tapCountOfNext(105, 100), 2);
    EXPECT_EQ(tapCountOfNext(100, 110), 3);
    // too late
    frame(0.4);
    EXPECT_EQ(tapCountOfNext(100, 100), 1);
    // too far (24 px)
    EXPECT_EQ(tapCountOfNext(130, 100), 1);
    // a long press is no tap: the next one starts at 1
    Input::addTouch(0, 130, 100);
    frame(0.2);
    frame(0.2);
    Input::removeTouch(0);
    frame(0.05);
    EXPECT_EQ(tapCountOfNext(130, 100), 1);
    (void)tap;
}

TEST_F(InputTouchTest, AFingerIdReusedInOneFrameIsListedTwiceEndedFirst) {
    Input::addTouch(5, 10, 10);
    frame();
    Input::removeTouch(5);
    Input::addTouch(5, 300, 300);
    begin();
    ASSERT_EQ(Input::getTouchCount(), 2);
    EXPECT_EQ(Input::getTouch(0).phase, TouchPhase::Ended);
    EXPECT_EQ(Input::getTouch(1).phase, TouchPhase::Began);
    Touch byId;
    ASSERT_TRUE(Input::findTouchById(5, byId));
    EXPECT_EQ(byId.phase, TouchPhase::Began);
    EXPECT_FLOAT_EQ(byId.position.x, 300);
    end();
    EXPECT_EQ(Input::numTouches(), 1u);
}

TEST_F(InputTouchTest, LegacyListIsTheFingersDownNow) {
    Input::addTouch(1, 10, 20);
    Input::addTouch(2, 30, 40);
    EXPECT_EQ(Input::numTouches(), 2u);
    EXPECT_EQ(Input::findTouchIndex(2), 1u);
    EXPECT_FLOAT_EQ(Input::getTouchPosition(2).x, 30);
    EXPECT_TRUE(Input::isTouch());
    std::vector<Touch> fingers = Input::getTouches();
    ASSERT_EQ(fingers.size(), 2u);
    EXPECT_EQ(fingers[0].pointer(), 1);
    EXPECT_FLOAT_EQ(fingers[1].position.y, 40);
    Input::removeTouch(1);
    EXPECT_EQ(Input::numTouches(), 1u);
    EXPECT_EQ(Input::findTouchIndex(1), (size_t)-1);
    EXPECT_FLOAT_EQ(Input::getTouchPosition(1).x, -1);
}

TEST_F(InputTouchTest, MultiTouchDisabledReportsOnlyTheFirstFinger) {
    Input::setMultiTouchEnabled(false);
    Input::addTouch(1, 10, 10);
    Input::addTouch(2, 50, 50);
    begin();
    ASSERT_EQ(Input::getTouchCount(), 1);
    EXPECT_EQ(Input::getTouch(0).fingerId, 1);
    EXPECT_TRUE(Input::isTouchVisible(1));
    EXPECT_FALSE(Input::isTouchVisible(2));
    end();
    // the legacy list and the UI still get every finger
    EXPECT_EQ(Input::numTouches(), 2u);
    // the second finger stays hidden after the first lifts
    Input::removeTouch(1);
    frame();
    begin();
    EXPECT_EQ(Input::getTouchCount(), 0);
    end();
    Input::setMultiTouchEnabled(true);
}

// ================================================================== actions

TEST_F(InputActionsTest, DefaultsAreUnitysInputManager) {
    const std::vector<std::string> expected = {"Horizontal", "Vertical", "Fire1", "Fire2", "Fire3", "Jump", "Mouse X",
                                               "Mouse Y", "Mouse ScrollWheel", "Submit", "Cancel"};
    EXPECT_EQ(InputActions::getActionNames(), expected);

    const InputAction horizontal = actionNamed("Horizontal");
    EXPECT_EQ(horizontal.type, InputActionType::Axis);
    ASSERT_EQ(horizontal.bindings.size(), 2u);
    const InputBinding& keys = horizontal.bindings[0];
    EXPECT_EQ(keys.kind, InputBindingKind::Buttons);
    EXPECT_EQ(names(keys.negative), (std::vector<std::string>{"left", "a"}));
    EXPECT_EQ(names(keys.positive), (std::vector<std::string>{"right", "d"}));
    EXPECT_FLOAT_EQ(keys.gravity, 3);
    EXPECT_FLOAT_EQ(keys.sensitivity, 3);
    EXPECT_FLOAT_EQ(keys.dead, 0.001f);
    EXPECT_TRUE(keys.snap);
    const InputBinding& stick = horizontal.bindings[1];
    EXPECT_EQ(stick.kind, InputBindingKind::GamepadAxis);
    EXPECT_EQ(stick.axis, D_GAMEPAD_AXIS_LEFT_X);
    EXPECT_EQ(stick.gamepad, -1);
    EXPECT_FLOAT_EQ(stick.dead, 0.19f);
    EXPECT_FLOAT_EQ(stick.sensitivity, 1);
    EXPECT_FALSE(stick.invert);

    const InputAction vertical = actionNamed("Vertical");
    EXPECT_EQ(names(vertical.bindings[0].negative), (std::vector<std::string>{"down", "s"}));
    EXPECT_EQ(names(vertical.bindings[0].positive), (std::vector<std::string>{"up", "w"}));
    EXPECT_EQ(vertical.bindings[1].axis, D_GAMEPAD_AXIS_LEFT_Y);
    EXPECT_TRUE(vertical.bindings[1].invert);

    auto buttonsOf = [](const std::string& name) {
        std::vector<std::vector<std::string>> out;
        for (const InputBinding& binding : actionNamed(name).bindings) out.push_back(names(binding.positive));
        return out;
    };
    using Lists = std::vector<std::vector<std::string>>;
    EXPECT_EQ(buttonsOf("Fire1"), (Lists{{"left_control", "mouse_left"}, {"gamepad_a"}}));
    EXPECT_EQ(buttonsOf("Fire2"), (Lists{{"left_alt", "mouse_right"}, {"gamepad_b"}}));
    EXPECT_EQ(buttonsOf("Fire3"), (Lists{{"left_shift", "mouse_middle"}, {"gamepad_x"}}));
    EXPECT_EQ(buttonsOf("Jump"), (Lists{{"space"}, {"gamepad_y"}}));
    EXPECT_EQ(buttonsOf("Submit"), (Lists{{"enter", "kp_enter", "space"}, {"gamepad_a"}}));
    EXPECT_EQ(buttonsOf("Cancel"), (Lists{{"escape"}, {"gamepad_b"}}));
    EXPECT_EQ(actionNamed("Fire1").type, InputActionType::Button);
    EXPECT_FLOAT_EQ(actionNamed("Fire1").bindings[0].gravity, 1000);
    EXPECT_FLOAT_EQ(actionNamed("Fire1").bindings[0].sensitivity, 1000);

    const InputAction mouseX = actionNamed("Mouse X");
    ASSERT_EQ(mouseX.bindings.size(), 1u);
    EXPECT_EQ(mouseX.bindings[0].kind, InputBindingKind::MouseDelta);
    EXPECT_EQ(mouseX.bindings[0].mouseAxis, MouseDeltaAxis::X);
    EXPECT_FLOAT_EQ(mouseX.bindings[0].sensitivity, 0.1f);
    EXPECT_EQ(actionNamed("Mouse Y").bindings[0].mouseAxis, MouseDeltaAxis::Y);
    EXPECT_EQ(actionNamed("Mouse ScrollWheel").bindings[0].mouseAxis, MouseDeltaAxis::Wheel);
}

TEST_F(InputActionsTest, KeyboardAxisRampsAtSensitivityAndFallsAtGravity) {
    Input::addKeyPressed(D_KEY_RIGHT);
    begin();
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.05f, 1e-5);
    EXPECT_FLOAT_EQ(InputActions::getAxisRaw("Horizontal"), 1.0f);
    EXPECT_TRUE(InputActions::getButton("Horizontal"));
    EXPECT_TRUE(InputActions::getButtonDown("Horizontal"));
    end();
    for (int i = 2; i <= 20; i++) frame();
    begin();
    EXPECT_FLOAT_EQ(InputActions::getAxis("Horizontal"), 1.0f);
    EXPECT_FALSE(InputActions::getButtonDown("Horizontal"));
    end();

    Input::releaseKeyPressed(D_KEY_RIGHT);
    begin();
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.95f, 1e-5);
    EXPECT_FLOAT_EQ(InputActions::getAxisRaw("Horizontal"), 0.0f);
    EXPECT_TRUE(InputActions::getButtonUp("Horizontal"));
    end();
}

TEST_F(InputActionsTest, SnapJumpsThroughZero) {
    Input::addKeyPressed(D_KEY_D);
    for (int i = 0; i < 30; i++) frame();
    Input::releaseKeyPressed(D_KEY_D);
    Input::addKeyPressed(D_KEY_A);
    begin();
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), -0.05f, 1e-5);
    end();
    // left and right held cancel out (Unity)
    Input::addKeyPressed(D_KEY_D);
    begin();
    EXPECT_FLOAT_EQ(InputActions::getAxisRaw("Horizontal"), 0.0f);
    end();
}

TEST_F(InputActionsTest, GamepadDeadZoneIsRescaled) {
    Input::addGamepad(0, "pad");
    Input::setGamepadAxisValue(0, D_GAMEPAD_AXIS_LEFT_X, 0.19f);
    begin();
    EXPECT_FLOAT_EQ(InputActions::getAxis("Horizontal"), 0.0f);
    end();
    Input::setGamepadAxisValue(0, D_GAMEPAD_AXIS_LEFT_X, 0.595f);
    begin();
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.5f, 1e-5);
    // no smoothing on an axis
    EXPECT_NEAR(InputActions::getAxisRaw("Horizontal"), 0.5f, 1e-5);
    end();
    Input::setGamepadAxisValue(0, D_GAMEPAD_AXIS_LEFT_X, 0.6f);
    begin();
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.50617f, 1e-4);
    end();
    Input::setGamepadAxisValue(0, D_GAMEPAD_AXIS_LEFT_X, -1.0f);
    begin();
    EXPECT_FLOAT_EQ(InputActions::getAxis("Horizontal"), -1.0f);
    EXPECT_TRUE(InputActions::getButton("Horizontal"));
    end();
}

TEST_F(InputActionsTest, VerticalStickIsInvertedSoUpIsPositive) {
    Input::addGamepad(0, "pad");
    Input::setGamepadAxisValue(0, D_GAMEPAD_AXIS_LEFT_Y, -1.0f);
    begin();
    EXPECT_FLOAT_EQ(InputActions::getAxis("Vertical"), 1.0f);
    end();
}

TEST_F(InputActionsTest, AnyGamepadTakesThePadPushedFurthest) {
    Input::addGamepad(0, "a");
    Input::addGamepad(4, "b");
    Input::setGamepadAxisValue(0, D_GAMEPAD_AXIS_LEFT_X, 0.4f);
    Input::setGamepadAxisValue(4, D_GAMEPAD_AXIS_LEFT_X, -1.0f);
    begin();
    EXPECT_FLOAT_EQ(InputActions::getAxis("Horizontal"), -1.0f);
    end();
    // gone: the remaining pad
    Input::removeGamepad(4);
    begin();
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), (0.4f - 0.19f) / 0.81f, 1e-5);
    end();
}

TEST_F(InputActionsTest, TriggersReadZeroToOne) {
    std::string error;
    ASSERT_TRUE(InputActions::loadJson(R"({"version": 1, "actions": [
        {"name": "Throttle", "type": "axis", "bindings": [{"kind": "gamepad_axis", "axis": "right_trigger", "dead": 0}]},
        {"name": "Brake", "type": "button", "press_point": 0.5, "bindings": [{"kind": "gamepad_axis", "axis": "left_trigger", "dead": 0}]}
    ]})", &error)) << error;
    Input::addGamepad(0, "pad");
    begin();
    EXPECT_FLOAT_EQ(InputActions::getAxis("Throttle"), 0.0f);
    end();
    Input::setGamepadAxisValue(0, D_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.0f);
    Input::setGamepadAxisValue(0, D_GAMEPAD_AXIS_LEFT_TRIGGER, -0.2f);
    begin();
    EXPECT_FLOAT_EQ(InputActions::getAxis("Throttle"), 0.5f);
    EXPECT_NEAR(InputActions::getAxis("Brake"), 0.4f, 1e-5);
    EXPECT_FALSE(InputActions::getButton("Brake"));
    end();
    Input::setGamepadAxisValue(0, D_GAMEPAD_AXIS_RIGHT_TRIGGER, 1.0f);
    Input::setGamepadAxisValue(0, D_GAMEPAD_AXIS_LEFT_TRIGGER, 0.2f);
    begin();
    EXPECT_FLOAT_EQ(InputActions::getAxis("Throttle"), 1.0f);
    // the press point
    EXPECT_TRUE(InputActions::getButton("Brake"));
    EXPECT_TRUE(InputActions::getButtonDown("Brake"));
    end();
}

TEST_F(InputActionsTest, BoundedSourcesAreSummedThenClampedDeltasAddedUnclamped) {
    Input::addGamepad(0, "pad");
    Input::addKeyPressed(D_KEY_RIGHT);
    for (int i = 0; i < 25; i++) frame();
    // keyboard 1 + stick 0.5 = 1
    Input::setGamepadAxisValue(0, D_GAMEPAD_AXIS_LEFT_X, 0.595f);
    begin();
    EXPECT_FLOAT_EQ(InputActions::getAxis("Horizontal"), 1.0f);
    end();
    // keyboard 1 + stick -1 = 0
    Input::setGamepadAxisValue(0, D_GAMEPAD_AXIS_LEFT_X, -1.0f);
    begin();
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.0f, 1e-6);
    end();
    // an on-screen stick (a bounded virtual source) adds the same way
    Input::setGamepadAxisValue(0, D_GAMEPAD_AXIS_LEFT_X, 0.0f);
    const uintptr_t stick = 0x1000;
    InputActions::setSourceValue(stick, "Horizontal", -0.5f);
    begin();
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.5f, 1e-5);
    end();
    InputActions::clearSource(stick);

    // mouse movement is not clamped
    Input::addMouseMovement(0, 0);
    Input::addMouseMovement(100, -40);
    begin();
    EXPECT_FLOAT_EQ(InputActions::getAxis("Mouse X"), 10.0f);
    EXPECT_FLOAT_EQ(InputActions::getAxis("Mouse Y"), 4.0f);   // canvas y down: up positive
    // a touch-area drag (delta source) adds on top, for this frame only
    InputActions::addSourceDelta(stick, "Mouse X", 25.0f);
    InputActions::addSourceDelta(stick, "Mouse X", 5.0f);
    EXPECT_FLOAT_EQ(InputActions::getAxis("Mouse X"), 40.0f);
    end();
    begin();
    EXPECT_FLOAT_EQ(InputActions::getAxis("Mouse X"), 0.0f);
    end();

    Input::setMouseScroll(0, 2);
    begin();
    EXPECT_NEAR(InputActions::getAxis("Mouse ScrollWheel"), 0.2f, 1e-6);
    end();
}

TEST_F(InputActionsTest, GetVectorClampsTheLengthAndLeavesDeltasOut) {
    const uintptr_t stick = 0x2000;
    InputActions::setSourceValue(stick, "Horizontal", 1.0f);
    InputActions::setSourceValue(stick, "Vertical", 1.0f);
    begin();
    const Vector2 v = InputActions::getVector("Horizontal", "Vertical");
    EXPECT_NEAR(v.length(), 1.0f, 1e-5);
    EXPECT_NEAR(v.x, std::sqrt(0.5f), 1e-5);
    InputActions::addSourceDelta(stick, "Horizontal", 50.0f);
    EXPECT_NEAR(InputActions::getVector("Horizontal", "Vertical").x, std::sqrt(0.5f), 1e-5);
    end();
}

TEST_F(InputActionsTest, VirtualButtonPressesAtThePressPointAndClearsWithItsOwner) {
    const uintptr_t button = 0x3000;
    InputActions::setSourceValue(button, "Jump", 1.0f);
    begin();
    EXPECT_TRUE(InputActions::getButton("Jump"));
    EXPECT_TRUE(InputActions::getButtonDown("Jump"));
    EXPECT_FLOAT_EQ(InputActions::getAxis("Jump"), 1.0f);
    end();
    // keyboard and the on-screen button together: still one press
    Input::addKeyPressed(D_KEY_SPACE);
    begin();
    EXPECT_FALSE(InputActions::getButtonDown("Jump"));
    EXPECT_FLOAT_EQ(InputActions::getAxis("Jump"), 1.0f);
    end();
    Input::releaseKeyPressed(D_KEY_SPACE);
    InputActions::clearSource(button);
    begin();
    EXPECT_TRUE(InputActions::getButtonUp("Jump"));
    EXPECT_FALSE(InputActions::getButton("Jump"));
    end();

    // a released script's sources go with it (Engine::removeSubscriptionsByTag("_<address>_"))
    const uintptr_t script = 0x7f001234;
    InputActions::setSourceValue(script, "Horizontal", 1.0f, InputSourceKind::Script);
    begin();
    EXPECT_FLOAT_EQ(InputActions::getAxis("Horizontal"), 1.0f);
    end();
    Engine::removeSubscriptionsByTag("_" + std::to_string(script) + "_");
    begin();
    EXPECT_FLOAT_EQ(InputActions::getAxis("Horizontal"), 0.0f);
    end();

    // named sources, cleared by kind
    InputActions::setSourceValue(InputActions::namedSource("mcp"), "Vertical", -1.0f, InputSourceKind::Agent);
    EXPECT_TRUE(InputActions::isNamedSource(InputActions::namedSource("mcp")));
    EXPECT_EQ(InputActions::namedSource("mcp"), InputActions::namedSource("mcp"));
    begin();
    EXPECT_FLOAT_EQ(InputActions::getAxis("Vertical"), -1.0f);
    end();
    InputActions::clearSources(InputSourceKind::Agent);
    begin();
    EXPECT_FLOAT_EQ(InputActions::getAxis("Vertical"), 0.0f);
    end();
}

TEST_F(InputActionsTest, ATapInsideOneFrameGivesDownAndUp) {
    Input::addKeyPressed(D_KEY_SPACE);
    Input::releaseKeyPressed(D_KEY_SPACE);
    begin();
    EXPECT_TRUE(InputActions::getButtonDown("Jump"));
    EXPECT_TRUE(InputActions::getButtonUp("Jump"));
    EXPECT_FALSE(InputActions::getButton("Jump"));
    // space is Jump and Submit
    EXPECT_EQ(InputActions::getFrameButtonDowns(), (std::vector<std::string>{"Jump", "Submit"}));
    EXPECT_EQ(InputActions::getFrameButtonUps(), (std::vector<std::string>{"Jump", "Submit"}));
    end();
}

TEST_F(InputActionsTest, AVirtualPressAndReleaseBetweenUpdatesGivesBothEdges) {
    // review fix: an on-screen button tapped between two frames was lost
    const uintptr_t button = 0x4000;
    InputActions::setSourceValue(button, "Jump", 1.0f);
    InputActions::setSourceValue(button, "Jump", 0.0f);
    begin();
    EXPECT_TRUE(InputActions::getButtonDown("Jump"));
    EXPECT_TRUE(InputActions::getButtonUp("Jump"));
    EXPECT_FALSE(InputActions::getButton("Jump"));
    EXPECT_EQ(InputActions::getFrameButtonDowns(), std::vector<std::string>{"Jump"});
    end();
    begin();
    EXPECT_FALSE(InputActions::getButtonDown("Jump"));
    EXPECT_FALSE(InputActions::getButtonUp("Jump"));
    end();

    // its owner going (a control destroyed) between the press and the frame: the same
    InputActions::setSourceValue(button, "Fire1", 1.0f);
    InputActions::clearSource(button);
    begin();
    EXPECT_TRUE(InputActions::getButtonDown("Fire1"));
    EXPECT_TRUE(InputActions::getButtonUp("Fire1"));
    end();

    // held, released and pressed again between two frames: an up and a down
    InputActions::setSourceValue(button, "Jump", 1.0f);
    frame();
    InputActions::setSourceValue(button, "Jump", 0.0f);
    InputActions::setSourceValue(button, "Jump", 1.0f);
    begin();
    EXPECT_TRUE(InputActions::getButtonDown("Jump"));
    EXPECT_TRUE(InputActions::getButtonUp("Jump"));
    EXPECT_TRUE(InputActions::getButton("Jump"));
    end();

    // a second button tapped while the first holds the action: no edges (one press)
    const uintptr_t other = 0x5000;
    InputActions::setSourceValue(other, "Jump", 1.0f);
    InputActions::setSourceValue(other, "Jump", 0.0f);
    begin();
    EXPECT_FALSE(InputActions::getButtonDown("Jump"));
    EXPECT_FALSE(InputActions::getButtonUp("Jump"));
    EXPECT_TRUE(InputActions::getButton("Jump"));
    end();
    InputActions::clearSource(button);
    frame();
}

TEST_F(InputActionsTest, AReleaseAndPressAgainWhileHeldGivesBothEdges) {
    // review fix: space held, released and pressed again inside one frame gave getKeyDown and
    // getKeyUp but no action edges
    Input::addKeyPressed(D_KEY_SPACE);
    frame();
    frame();
    Input::releaseKeyPressed(D_KEY_SPACE);
    Input::addKeyPressed(D_KEY_SPACE);
    begin();
    EXPECT_TRUE(Input::getKeyDown(D_KEY_SPACE));
    EXPECT_TRUE(Input::getKeyUp(D_KEY_SPACE));
    EXPECT_TRUE(InputActions::getButtonDown("Jump"));
    EXPECT_TRUE(InputActions::getButtonUp("Jump"));
    EXPECT_TRUE(InputActions::getButton("Jump"));
    end();
    begin();
    EXPECT_FALSE(InputActions::getButtonDown("Jump"));
    EXPECT_FALSE(InputActions::getButtonUp("Jump"));
    end();

    // from rest: down, up, down inside one frame gives both too (as getKeyDown / getKeyUp)
    Input::releaseKeyPressed(D_KEY_SPACE);
    frame();
    Input::addKeyPressed(D_KEY_SPACE);
    Input::releaseKeyPressed(D_KEY_SPACE);
    Input::addKeyPressed(D_KEY_SPACE);
    begin();
    EXPECT_TRUE(InputActions::getButtonDown("Jump"));
    EXPECT_TRUE(InputActions::getButtonUp("Jump"));
    EXPECT_TRUE(InputActions::getButton("Jump"));
    end();
}

TEST_F(InputActionsTest, ButtonEventsFireInBeginFrame) {
    std::vector<std::string> log;
    struct Listener {
        std::vector<std::string>* log;
        void down(const std::string& name) { log->push_back("down " + name); }
        void up(const std::string& name) { log->push_back("up " + name); }
    } listener{&log};
    // a test that ran the engine may have left the game events paused (Engine::pauseGameEvents)
    const bool downEnabled = Engine::onButtonDown.isEnabled(), upEnabled = Engine::onButtonUp.isEnabled();
    Engine::onButtonDown.setEnabled(true);
    Engine::onButtonUp.setEnabled(true);
    Engine::onButtonDown.add<Listener, &Listener::down>("InputTest", &listener);
    Engine::onButtonUp.add<Listener, &Listener::up>("InputTest", &listener);
    Input::addMousePressed(D_MOUSE_BUTTON_LEFT);
    frame();
    Input::releaseMousePressed(D_MOUSE_BUTTON_LEFT);
    frame();
    Engine::onButtonDown.remove("InputTest");
    Engine::onButtonUp.remove("InputTest");
    Engine::onButtonDown.setEnabled(downEnabled);
    Engine::onButtonUp.setEnabled(upEnabled);
    EXPECT_EQ(log, (std::vector<std::string>{"down Fire1", "up Fire1"}));
}

TEST_F(InputActionsTest, TimeScaleDoesNotMatterSmoothingIsUnscaled) {
    // the update gets the unscaled frame delta, whatever Engine.timeScale is
    Input::addKeyPressed(D_KEY_RIGHT);
    begin(0.1);
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.3f, 1e-5);
    end();
}

TEST_F(InputActionsTest, DisabledActionReadsZeroAndRestartsFromRest) {
    Input::addKeyPressed(D_KEY_RIGHT);
    for (int i = 0; i < 25; i++) frame();
    InputActions::setActionEnabled("Horizontal", false);
    begin();
    EXPECT_FALSE(InputActions::isActionEnabled("Horizontal"));
    EXPECT_FLOAT_EQ(InputActions::getAxis("Horizontal"), 0.0f);
    EXPECT_FALSE(InputActions::getButton("Horizontal"));
    // no edge for being disabled
    EXPECT_FALSE(InputActions::getButtonUp("Horizontal"));
    end();
    InputActions::setActionEnabled("Horizontal", true);
    begin();
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.05f, 1e-5);
    EXPECT_TRUE(InputActions::getButtonDown("Horizontal"));
    end();
}

TEST_F(InputActionsTest, UnknownNameLogsOnceAndReadsZero) {
    // Log reaches the editor's Out, or (another test's System, none) stdout / stderr
    const uint64_t mark = editor::Out::lastLogId();
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    begin();
    EXPECT_FLOAT_EQ(InputActions::getAxis("Fire4"), 0.0f);
    EXPECT_FALSE(InputActions::getButtonDown("Fire4"));
    EXPECT_FALSE(InputActions::getButton("Fire4"));
    end();
    fflush(stdout);
    const std::string logged = testing::internal::GetCapturedStderr() + testing::internal::GetCapturedStdout() + loggedSince(mark);
    EXPECT_EQ(count(logged, "Input action \"Fire4\" does not exist (Project Settings > Input)"), 1u) << logged;
    EXPECT_FALSE(InputActions::hasAction("Fire4"));
}

TEST_F(InputActionsTest, FocusedTextFieldAndGamepadNavigationSuppressTheirControls) {
    InputSuppression typing;
    typing.keyboard = true;
    Input::addKeyPressed(D_KEY_D);
    begin(frameTime, typing);
    EXPECT_FLOAT_EQ(InputActions::getAxis("Horizontal"), 0.0f);
    EXPECT_FALSE(InputActions::getButtonDown("Horizontal"));
    // raw queries still read the device
    EXPECT_TRUE(Input::isKeyPressed(D_KEY_D));
    end();
    // the mouse is not the keyboard
    Input::addMousePressed(D_MOUSE_BUTTON_LEFT);
    begin(frameTime, typing);
    EXPECT_TRUE(InputActions::getButton("Fire1"));
    end();
    Input::releaseMousePressed(D_MOUSE_BUTTON_LEFT);
    frame(frameTime, typing);

    // held before the field took focus: released, so nothing stays pressed
    Input::releaseKeyPressed(D_KEY_D);
    frame();
    Input::addKeyPressed(D_KEY_SPACE);
    begin();
    EXPECT_TRUE(InputActions::getButton("Jump"));
    end();
    begin(frameTime, typing);
    EXPECT_TRUE(InputActions::getButtonUp("Jump"));
    EXPECT_FALSE(InputActions::getButton("Jump"));
    end();
    Input::releaseKeyPressed(D_KEY_SPACE);
    frame();

    // a document with gamepad focus takes the d-pad, the left stick and A
    InputSuppression menu;
    menu.gamepadNavigation = true;
    Input::addGamepad(0, "pad");
    Input::setGamepadAxisValue(0, D_GAMEPAD_AXIS_LEFT_X, 1.0f);
    Input::addGamepadButtonPressed(0, D_GAMEPAD_BUTTON_A);
    Input::addGamepadButtonPressed(0, D_GAMEPAD_BUTTON_B);
    begin(frameTime, menu);
    EXPECT_FLOAT_EQ(InputActions::getAxis("Horizontal"), 0.0f);
    EXPECT_FALSE(InputActions::getButton("Fire1"));
    EXPECT_FALSE(InputActions::getButton("Submit"));
    EXPECT_TRUE(InputActions::getButton("Fire2"));
    EXPECT_TRUE(Input::isGamepadButtonPressed(0, D_GAMEPAD_BUTTON_A));
    end();
}

TEST_F(InputActionsTest, JsonLoadsAndRejectsWithTheBindingNamed) {
    std::string error;
    EXPECT_TRUE(InputActions::loadJson(R"({"version": 1, "actions": [
        {"name": "Dash", "type": "button", "description": "a quick move",
         "bindings": [{"kind": "buttons", "positive": ["left shift", "Joystick Button 5"]}]}]})", &error)) << error;
    ASSERT_EQ(InputActions::getActions().size(), 1u);
    EXPECT_EQ(names(InputActions::getActions()[0].bindings[0].positive),
              (std::vector<std::string>{"left_shift", "gamepad_right_bumper"}));
    // the bare list too
    EXPECT_TRUE(InputActions::loadJson(R"([{"name": "Jump", "bindings": []}])", &error)) << error;

    const std::vector<std::pair<std::string, std::string>> bad = {
        {R"([{"name": "Dash", "bindings": [{"kind": "buttons", "positive": ["lft"]}]}])", "action \"Dash\" binding 1: unknown control \"lft\""},
        {R"([{"name": "A"}, {"name": "A"}])", "two actions are named \"A\""},
        {R"([{"name": "  "}])", "has no name"},
        {R"([{"name": "A", "bindings": [{"kind": "gamepad_axis", "axis": "left_x", "dead": 1}]}])", "action \"A\" binding 1: dead must be below 1"},
        {R"([{"name": "A", "bindings": [{"kind": "buttons", "gravity": -1}]}])", "finite numbers >= 0"},
        {R"([{"name": "A", "bindings": [{"kind": "buttons", "positive": ["left_x"]}]}])", "is a gamepad axis: use a gamepad_axis binding"},
        {R"([{"name": "A", "bindings": [{"kind": "wheel"}]}])", "kind must be buttons, gamepad_axis or mouse_delta"},
        {R"([{"name": "A", "bindings": [{"kind": "buttons", "sensitivty": 2}]}])", "unknown field \"sensitivty\""},
        {R"([{"name": "A", "press_point": 0}])", "press_point must be above 0"},
        {R"([{"name": "A", "bindings": [{"kind": "buttons", "gamepad": 16}]}])", "gamepad must be \"any\" or 0-15"},
        {R"({"version": 2, "actions": []})", "is not supported"},
        {"not json", "not valid JSON"},
    };
    for (const auto& [json, expected] : bad) {
        error.clear();
        testing::internal::CaptureStderr();
        EXPECT_FALSE(InputActions::loadJson(json, &error)) << json;
        testing::internal::GetCapturedStderr();
        EXPECT_NE(error.find(expected), std::string::npos) << json << " -> " << error;
    }
    // a failed load changes nothing
    EXPECT_EQ(InputActions::getActionNames(), (std::vector<std::string>{"Jump"}));
}

TEST_F(InputActionsTest, JsonRoundTripsTheDefaults) {
    const std::string json = InputActions::actionsToJson(InputActions::defaults());
    std::vector<InputAction> parsed;
    std::string error;
    ASSERT_TRUE(InputActions::parseActionsJson(json, parsed, &error)) << error;
    EXPECT_EQ(InputActions::actionsToJson(parsed), json);
    // short numbers
    EXPECT_NE(json.find("\"dead\":0.001"), std::string::npos) << json;
    EXPECT_NE(json.find("\"dead\":0.19"), std::string::npos) << json;
    const nlohmann::json j = nlohmann::json::parse(json);
    EXPECT_EQ(j["version"], 1);
    EXPECT_EQ(j["actions"][0]["bindings"][1]["gamepad"], "any");
}

TEST_F(InputActionsTest, OverridesExportImportRoundTrip) {
    std::vector<InputBinding> bindings;
    std::string error;
    ASSERT_TRUE(InputActions::parseBindingsJson(R"([{"kind": "buttons", "positive": ["j", "gamepad_left_bumper"]}])", bindings, &error, "Jump")) << error;
    ASSERT_TRUE(InputActions::setBindings("Jump", bindings, &error)) << error;
    EXPECT_TRUE(InputActions::hasBindingOverride("Jump"));

    Input::addKeyPressed(D_KEY_SPACE);
    begin();
    EXPECT_FALSE(InputActions::getButton("Jump"));
    end();
    Input::releaseKeyPressed(D_KEY_SPACE);
    Input::addKeyPressed(D_KEY_J);
    begin();
    EXPECT_TRUE(InputActions::getButtonDown("Jump"));
    end();
    Input::releaseKeyPressed(D_KEY_J);
    frame();

    const std::string exported = InputActions::exportOverrides();
    const nlohmann::json j = nlohmann::json::parse(exported);
    EXPECT_EQ(j["version"], 1);
    ASSERT_TRUE(j["bindings"].contains("Jump"));
    EXPECT_FALSE(j["bindings"].contains("Fire1"));

    InputActions::resetBindings();
    EXPECT_FALSE(InputActions::hasBindingOverride("Jump"));
    EXPECT_EQ(InputActions::exportOverrides(), R"({"version":1,"bindings":{}})");

    ASSERT_TRUE(InputActions::importOverrides(exported, &error)) << error;
    EXPECT_EQ(InputActions::exportOverrides(), exported);
    EXPECT_EQ(names(InputActions::getBindings("Jump")[0].positive), (std::vector<std::string>{"j", "gamepad_left_bumper"}));

    // an action the game no longer has is skipped; an invalid binding fails and changes nothing
    testing::internal::CaptureStderr();
    EXPECT_TRUE(InputActions::importOverrides(R"({"version": 1, "bindings": {"Gone": []}})", &error));
    EXPECT_FALSE(InputActions::hasBindingOverride("Jump"));
    ASSERT_TRUE(InputActions::importOverrides(exported, &error));
    EXPECT_FALSE(InputActions::importOverrides(R"({"version": 1, "bindings": {"Jump": [{"kind": "buttons", "positive": ["nope"]}]}})", &error));
    testing::internal::GetCapturedStderr();
    EXPECT_NE(error.find("unknown control \"nope\""), std::string::npos) << error;
    EXPECT_TRUE(InputActions::hasBindingOverride("Jump"));

    // the definition changes (Project Settings during Play): the override of a name that stays stays
    InputActions::loadDefaults();
    EXPECT_TRUE(InputActions::hasBindingOverride("Jump"));
    InputActions::resetRuntimeState();
    EXPECT_FALSE(InputActions::hasBindingOverride("Jump"));
}

// ================================================================== control names

TEST(InputControlNamesTest, CanonicalNamesRoundTrip) {
    for (const std::string& name : InputControlNames::keyNames()) {
        InputControl control;
        ASSERT_TRUE(InputControlNames::parseButton(name, control)) << name;
        EXPECT_EQ(control.device, InputDevice::Key) << name;
        EXPECT_EQ(InputControlNames::name(control), name);
    }
    for (const std::string& name : InputControlNames::mouseButtonNames()) {
        InputControl control;
        ASSERT_TRUE(InputControlNames::parseButton(name, control)) << name;
        EXPECT_EQ(control.device, InputDevice::Mouse) << name;
        EXPECT_EQ(InputControlNames::name(control), name);
    }
    for (const std::string& name : InputControlNames::gamepadButtonNames()) {
        InputControl control;
        ASSERT_TRUE(InputControlNames::parseButton(name, control)) << name;
        EXPECT_EQ(control.device, InputDevice::GamepadButton) << name;
        EXPECT_EQ(InputControlNames::name(control), name);
    }
    for (const std::string& name : InputControlNames::gamepadAxisNames()) {
        InputControl control;
        ASSERT_TRUE(InputControlNames::parse(name, control)) << name;
        EXPECT_EQ(control.device, InputDevice::GamepadAxis) << name;
        EXPECT_EQ(InputControlNames::name(control), name);
    }
    // every key code has a name
    const int keys[] = {D_KEY_SPACE, D_KEY_APOSTROPHE, D_KEY_WORLD_2, D_KEY_KP_EQUAL, D_KEY_F25, D_KEY_MENU, D_KEY_PRINT_SCREEN};
    for (int key : keys) EXPECT_FALSE(InputControlNames::keyName(key).empty()) << key;
}

TEST(InputControlNamesTest, UnitySpellingsAreNormalised) {
    auto canonical = [](const std::string& name) {
        InputControl control;
        std::string error;
        if (!InputControlNames::parseButton(name, control, &error)) return "error: " + error;
        return InputControlNames::name(control);
    };
    EXPECT_EQ(canonical("left ctrl"), "left_control");
    EXPECT_EQ(canonical("LeftControl"), "left_control");
    EXPECT_EQ(canonical("right shift"), "right_shift");
    EXPECT_EQ(canonical("mouse 0"), "mouse_left");
    EXPECT_EQ(canonical("Mouse0"), "mouse_left");
    EXPECT_EQ(canonical("mouse 1"), "mouse_right");
    EXPECT_EQ(canonical("mouse 2"), "mouse_middle");
    EXPECT_EQ(canonical("mouse 3"), "mouse_4");
    EXPECT_EQ(canonical("mouse_4"), "mouse_4");
    EXPECT_EQ(canonical("joystick button 0"), "gamepad_a");
    EXPECT_EQ(canonical("JoystickButton3"), "gamepad_y");
    EXPECT_EQ(canonical("joystick button 8"), "gamepad_left_thumb");
    EXPECT_EQ(canonical("return"), "enter");
    EXPECT_EQ(canonical("Return"), "enter");
    EXPECT_EQ(canonical("[1]"), "kp_1");
    EXPECT_EQ(canonical("[+]"), "kp_add");
    EXPECT_EQ(canonical("Keypad7"), "kp_7");
    EXPECT_EQ(canonical("Alpha1"), "1");
    EXPECT_EQ(canonical("LeftArrow"), "left");
    EXPECT_EQ(canonical("up"), "up");
    EXPECT_EQ(canonical("page up"), "page_up");
    EXPECT_EQ(canonical("Space"), "space");
    EXPECT_EQ(canonical(" "), "space");
    EXPECT_EQ(canonical("A"), "a");
    EXPECT_EQ(canonical("BackQuote"), "grave_accent");
    EXPECT_EQ(canonical("left cmd"), "left_super");
    EXPECT_EQ(canonical("f12"), "f12");
    EXPECT_EQ(canonical("gamepad_dpad_up"), "gamepad_dpad_up");
    EXPECT_NE(canonical("joystick 1 button 0").find("names one joystick"), std::string::npos);
    EXPECT_NE(canonical("nope").find("unknown control"), std::string::npos);

    // send_input's own spellings
    EXPECT_EQ(InputControlNames::parseKey("ctrl"), D_KEY_LEFT_CONTROL);
    EXPECT_EQ(InputControlNames::parseKey("esc"), D_KEY_ESCAPE);
    EXPECT_EQ(InputControlNames::parseKey("numpad5"), D_KEY_KP_5);
    EXPECT_EQ(InputControlNames::parseKey("KEY_A"), D_KEY_A);
    EXPECT_EQ(InputControlNames::parseGamepadButton("a"), D_GAMEPAD_BUTTON_A);
    EXPECT_EQ(InputControlNames::parseGamepadButton("dpad_up"), D_GAMEPAD_BUTTON_DPAD_UP);
    EXPECT_EQ(InputControlNames::parseGamepadButton("rb"), D_GAMEPAD_BUTTON_RIGHT_BUMPER);
    EXPECT_EQ(InputControlNames::parseGamepadAxis("lt"), D_GAMEPAD_AXIS_LEFT_TRIGGER);
    EXPECT_EQ(InputControlNames::parseMouseButton("middle"), D_MOUSE_BUTTON_MIDDLE);
}
