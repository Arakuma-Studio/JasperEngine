// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Unity's Input Manager on S22's Input Actions (docs/specs/S22-gameplay.md "Upstream absorption"):
// the .inputactions additions (an action's description, a binding's gamepad and Input Manager
// smoothing, several parts per composite role) read and written byte for byte, their edit
// operations, the lossless conversion of the Input Manager's flat actions into the "Input
// Manager" map, and how that map lives with S22's maps (flat names, priorities, consumption,
// suppression while typing or navigating, virtual sources on S22 actions). Events go through
// Engine::system*, frames through Input::beginFrame(dt) as Engine::systemDraw runs them.

#include "Engine.h"
#include "Input.h"
#include "TestSystem.h"
#include "input/InputActionAssetJson.h"
#include "input/InputActions.h"
#include "input/InputManagerFormat.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    class InputManagerMapTest : public ::testing::Test {
    protected:
        void SetUp() override{
            test::TestSystem::get().setScreenSize(640, 360);
            Engine::setCanvasSize(640, 360);
            Engine::setScalingMode(Scaling::FITWIDTH);
            Engine::systemViewChanged();
            Input::reset();
            InputActions::loadDefaults();
            InputActions::resetRuntimeState();
        }

        void TearDown() override{
            for (int pad : {0, 1, 2})
                Engine::systemGamepadDisconnect(pad);
            Input::reset();
            InputActions::loadDefaults();
            InputActions::resetRuntimeState();
        }

        static void load(const std::string& json){
            ASSERT_TRUE(InputActions::loadFromText("test.inputactions", json));
        }

        static void frame(InputSuppression suppression = InputSuppression()){
            Input::beginFrame(1.0 / 60.0, suppression);
            Input::endFrame();
        }

        static void key(int key, bool down){
            if (down) Engine::systemKeyDown(key, false, 0);
            else Engine::systemKeyUp(key, false, 0);
        }
    };

    std::string roundTrip(const std::string& text){
        InputActionAssetDesc asset;
        std::string error;
        EXPECT_TRUE(InputActionAssetDesc::parse(text, asset, error)) << error;
        return asset.save();
    }

}

TEST_F(InputManagerMapTest, TheAssetKeepsTheInputManagerFields){
    const std::string text = R"({
  "version": 1,
  "maps": [
    {
      "name": "Input Manager",
      "consume": false,
      "actions": [
        {
          "name": "Fire/Alt",
          "description": "a name with a slash and a description",
          "type": "AXIS",
          "bindings": [
            {
              "id": "00000001",
              "composite": "axis1d",
              "parts": {
                "negative": [],
                "positive": [
                  {
                    "id": "00000002",
                    "path": "keyboard/left_control"
                  },
                  {
                    "id": "00000003",
                    "path": "mouse/left"
                  }
                ]
              },
              "gamepad": 3,
              "smoothing": {
                "gravity": 1000.0,
                "sensitivity": 1000.0,
                "dead": 0.0010000000474974513,
                "snap": true
              }
            }
          ]
        }
      ]
    }
  ]
}
)";
    EXPECT_EQ(roundTrip(text), text);
    InputActionAssetDesc asset;
    std::string error;
    ASSERT_TRUE(InputActionAssetDesc::parse(text, asset, error)) << error;
    const InputActionDesc* action = asset.findAction("Input Manager/Fire/Alt");
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->description, "a name with a slash and a description");
    const InputBindingDesc& binding = action->bindings[0];
    EXPECT_EQ(binding.gamepad, 3);
    ASSERT_TRUE(binding.smoothing.has_value());
    EXPECT_TRUE(binding.smoothing->snap);
    ASSERT_EQ(binding.parts.size(), 2u);
    EXPECT_EQ(binding.parts[0].first, "positive");
    EXPECT_EQ(binding.parts[1].second.path, "mouse/left");

    // bad values say where
    for (const char* bad : {R"({"version":1,"maps":[{"name":"M","actions":[{"name":"A","bindings":[{"path":"keyboard/a","gamepad":16}]}]}]})",
                            R"({"version":1,"maps":[{"name":"M","actions":[{"name":"A","bindings":[{"path":"keyboard/a","smoothing":{"dead":1}}]}]}]})",
                            R"({"version":1,"maps":[{"name":"M","actions":[{"name":"A","bindings":[{"path":"keyboard/a","smoothing":{"gravty":1}}]}]}]})",
                            R"({"version":1,"maps":[{"name":"M/N","actions":[]}]})"}){
        InputActionAssetDesc rejected;
        EXPECT_FALSE(InputActionAssetDesc::parse(bad, rejected, error)) << bad;
    }
}

TEST_F(InputManagerMapTest, EditOperationsReachTheNewFields){
    InputActionAssetDesc asset;
    applyInputActionsOp(asset, InputJson::parse(R"({"op":"add_input_manager"})"));
    ASSERT_NE(asset.findMap(InputManagerFormat::MAP_NAME), nullptr);
    EXPECT_EQ(InputManagerFormat::actionsToJson(InputManagerFormat::fromMap(*asset.findMap(InputManagerFormat::MAP_NAME))),
              InputManagerFormat::actionsToJson(InputManagerFormat::defaults()));
    EXPECT_THROW(applyInputActionsOp(asset, InputJson::parse(R"({"op":"add_input_manager"})")), std::invalid_argument);

    applyInputActionsOp(asset, InputJson::parse(R"({"op":"add_action","map":"Input Manager","name":"Dash/Air","description":"in the air"})"));
    applyInputActionsOp(asset, InputJson::parse(R"({"op":"add_composite","action":"Input Manager/Dash/Air","composite":"axis1d",
        "parts":{"negative":[],"positive":["keyboard/left_shift"]},"smoothing":{"gravity":0,"sensitivity":0,"dead":0},"gamepad":1})"));
    InputBindingDesc composite = asset.findAction("Input Manager/Dash/Air")->bindings[0];
    EXPECT_EQ(composite.gamepad, 1);
    ASSERT_EQ(composite.parts.size(), 1u);
    applyInputActionsOp(asset, InputJson::parse(R"({"op":"add_part","binding":")" + composite.id + R"(","part":"positive","path":"gamepad/right_bumper"})"));
    applyInputActionsOp(asset, InputJson::parse(R"({"op":"add_part","binding":")" + composite.id + R"(","part":"negative","path":"keyboard/z","index":0})"));
    composite = asset.findAction("Input Manager/Dash/Air")->bindings[0];
    ASSERT_EQ(composite.parts.size(), 3u);
    // in role order: the negative one first
    EXPECT_EQ(composite.parts[0].first, "negative");
    EXPECT_EQ(composite.parts[2].second.path, "gamepad/right_bumper");
    // a part may go (its role reads 0 when it was the last)
    applyInputActionsOp(asset, InputJson::parse(R"({"op":"remove","binding":")" + composite.parts[0].second.id + R"("})"));
    EXPECT_EQ(asset.findAction("Input Manager/Dash/Air")->bindings[0].parts.size(), 2u);
    applyInputActionsOp(asset, InputJson::parse(R"({"op":"set","binding":")" + composite.id + R"(","values":{"gravity":3,"snap":true}})"));
    composite = asset.findAction("Input Manager/Dash/Air")->bindings[0];
    EXPECT_FLOAT_EQ(composite.smoothing->gravity, 3.0f);
    EXPECT_TRUE(composite.smoothing->snap);
    applyInputActionsOp(asset, InputJson::parse(R"({"op":"set","binding":")" + composite.id + R"(","values":{"smoothing":null,"gamepad":null}})"));
    composite = asset.findAction("Input Manager/Dash/Air")->bindings[0];
    EXPECT_FALSE(composite.smoothing.has_value());
    EXPECT_EQ(composite.gamepad, -1);
    EXPECT_THROW(applyInputActionsOp(asset, InputJson::parse(R"({"op":"set","binding":")" + composite.id + R"(","values":{"dead":1}})")),
                 std::invalid_argument);
    EXPECT_THROW(applyInputActionsOp(asset, InputJson::parse(R"({"op":"add_map","name":"A/B"})")), std::invalid_argument);
}

TEST_F(InputManagerMapTest, TheFlatActionsConvertLosslessly){
    // every kind of the Input Manager, values off their defaults, names YAML would read as others
    const std::string json = R"({"version":1,"actions":[
      {"name":"1","type":"button","description":"true","press_point":0.25,"bindings":[
        {"kind":"buttons","positive":["0","left_shift","gamepad_x"],"gamepad":2,"gravity":2.5,"sensitivity":7,"dead":0.05,"snap":true,"invert":true},
        {"kind":"gamepad_axis","axis":"right_trigger","gamepad":"any","sensitivity":2,"dead":0.1,"invert":true}]},
      {"name":"Look/Y","type":"axis","bindings":[
        {"kind":"gamepad_axis","axis":"right_y","gamepad":5,"sensitivity":1,"dead":0.19},
        {"kind":"gamepad_axis","axis":"left_y","gamepad":"any","sensitivity":1,"dead":0.19,"invert":true},
        {"kind":"mouse_delta","axis":"y","sensitivity":0.25},
        {"kind":"mouse_delta","axis":"wheel","sensitivity":0.1,"invert":true},
        {"kind":"buttons","positive":[],"negative":["down"],"gravity":0,"sensitivity":0,"dead":0}]},
      {"name":"Move","type":"vector2","hold_time":0.5,"deadzone":0.2,"bindings":[
        {"kind":"binding","binding":{"composite":"dpad2d","parts":{"up":{"path":"keyboard/w"},"down":{"path":"keyboard/s"},"left":{"path":"keyboard/a"},"right":{"path":"keyboard/d"}},"groups":["Keyboard&Mouse"]}},
        {"kind":"binding","binding":{"path":"gamepad/left_stick","requiredMods":["ctrl"]}}]}]})";
    std::vector<InputManagerAction> actions;
    std::string error;
    ASSERT_TRUE(InputManagerFormat::parseActionsJson(json, actions, &error)) << error;
    const std::string canonical = InputManagerFormat::actionsToJson(actions);

    // flat -> the map -> the asset's text -> the map -> flat: the same
    InputActionAssetDesc asset;
    asset.maps.push_back(InputManagerFormat::toMap(actions));
    InputActionAssetDesc reread;
    ASSERT_TRUE(InputActionAssetDesc::parse(asset.save(), reread, error)) << error;
    EXPECT_EQ(InputManagerFormat::actionsToJson(InputManagerFormat::fromMap(*reread.findMap(InputManagerFormat::MAP_NAME))), canonical);
    // and the same actions give the same asset (ids from the names)
    InputActionAssetDesc again;
    again.maps.push_back(InputManagerFormat::toMap(actions));
    EXPECT_EQ(again.save(), asset.save());

    // what the map holds: S22 bindings with the Input Manager's numbers
    const InputActionMapDesc& map = *reread.findMap(InputManagerFormat::MAP_NAME);
    EXPECT_FALSE(map.consume);
    const InputBindingDesc& buttons = map.actions[0].bindings[0];
    EXPECT_EQ(buttons.composite, InputCompositeType::AXIS_1D);
    EXPECT_EQ(buttons.gamepad, 2);
    EXPECT_TRUE(buttons.invert);
    ASSERT_EQ(buttons.parts.size(), 3u);
    EXPECT_EQ(buttons.parts[0].second.path, "keyboard/0");
    EXPECT_EQ(buttons.parts[2].second.path, "gamepad/x");
    // the action's y is up-positive: the Input Manager's right_y without invert is inverted here
    EXPECT_EQ(map.actions[1].bindings[0].path, "gamepad/right_y");
    EXPECT_TRUE(map.actions[1].bindings[0].invert);
    EXPECT_FALSE(map.actions[1].bindings[1].invert);
    EXPECT_FLOAT_EQ(*map.actions[1].bindings[0].deadzone, 0.19f);
    EXPECT_EQ(map.actions[1].bindings[2].path, "mouse/delta/y");
    EXPECT_FLOAT_EQ(map.actions[1].bindings[2].scale, 0.25f);
    EXPECT_EQ(map.actions[1].bindings[3].path, "mouse/scroll/y");
    EXPECT_EQ(map.actions[2].type, InputActionType::VECTOR2);
    EXPECT_FLOAT_EQ(map.actions[2].holdTime, 0.5f);
    EXPECT_EQ(map.actions[2].bindings[1].requiredMods, D_MODIFIER_CONTROL);
}

TEST_F(InputManagerMapTest, FlatNamesFindTheInputManagerMapFirstThenByPriority){
    load(R"({"version":1,"maps":[
      {"name":"Gameplay","priority":10,"actions":[{"name":"Jump","bindings":[{"path":"keyboard/j"}]},{"name":"Crouch","bindings":[{"path":"keyboard/c"}]}]},
      {"name":"Vehicle","priority":20,"actions":[{"name":"Crouch","bindings":[{"path":"keyboard/v"}]}]},
      {"name":"Input Manager","priority":0,"consume":false,"actions":[{"name":"Jump","type":"BUTTON","bindings":[{"path":"keyboard/space"}]}]}]})");
    EXPECT_FALSE(InputActions::usesBuiltInDefaults());
    EXPECT_EQ(InputActions::find("Jump"), InputActions::get("Input Manager/Jump"));
    EXPECT_EQ(InputActions::find("Crouch"), InputActions::get("Vehicle/Crouch"));
    EXPECT_EQ(InputActions::find("Gameplay/Jump"), InputActions::get("Gameplay/Jump"));
    EXPECT_EQ(InputActions::getActionNames(), (std::vector<std::string>{"Jump", "Gameplay/Jump", "Gameplay/Crouch", "Vehicle/Crouch"}));
    InputActions::setMapPriority("Gameplay", 30);
    EXPECT_EQ(InputActions::find("Crouch"), InputActions::get("Gameplay/Crouch"));

    key(D_KEY_C, true);
    frame();
    EXPECT_TRUE(InputActions::getButton("Crouch"));
    EXPECT_TRUE(InputActions::getButtonDown("Gameplay/Crouch"));
    key(D_KEY_C, false);
    // an asset without the map has no defaults: an unknown name reads 0
    load(R"({"version":1,"maps":[{"name":"Gameplay","actions":[{"name":"Jump","bindings":[{"path":"keyboard/j"}]}]}]})");
    EXPECT_FALSE(InputActions::hasAction("Horizontal"));
    EXPECT_TRUE(InputActions::hasAction("Jump"));
}

TEST_F(InputManagerMapTest, TheInputManagerMapTakesNoControlFromLowerMaps){
    // its consume is off: a lower S22 map still gets the key
    load(R"({"version":1,"maps":[
      {"name":"Input Manager","priority":50,"consume":false,"actions":[{"name":"Jump","type":"BUTTON","bindings":[{"path":"keyboard/space"}]}]},
      {"name":"Gameplay","priority":0,"actions":[{"name":"Jump","bindings":[{"path":"keyboard/space"}]}]}]})");
    key(D_KEY_SPACE, true);
    frame();
    EXPECT_TRUE(InputActions::getButton("Jump"));
    EXPECT_TRUE(InputActions::get("Gameplay/Jump")->isPressed());
    EXPECT_EQ(InputActions::get("Gameplay/Jump")->getConsumedBy(), "");
    key(D_KEY_SPACE, false);
}

TEST_F(InputManagerMapTest, SmoothingAndRolePartsOnS22Actions){
    // an S22 AXIS with an Input Manager entry: several keys per side, gravity and sensitivity
    load(R"({"version":1,"maps":[{"name":"Gameplay","actions":[{"name":"Steer","type":"AXIS","bindings":[
      {"composite":"axis1d","parts":{"negative":[{"path":"keyboard/left"},{"path":"keyboard/a"}],"positive":[{"path":"keyboard/right"},{"path":"keyboard/d"}]},
       "smoothing":{"gravity":3,"sensitivity":3,"dead":0.001,"snap":true}}]}]}]})");
    InputAction* steer = InputActions::get("Gameplay/Steer");
    ASSERT_NE(steer, nullptr);
    key(D_KEY_D, true);
    frame();
    EXPECT_NEAR(steer->readValue(), 0.05f, 1e-5f);
    EXPECT_NEAR(InputActions::getAxis("Steer"), 0.05f, 1e-5f);
    EXPECT_FLOAT_EQ(InputActions::getAxisRaw("Steer"), 1.0f);
    // held at once for the Input Manager (a key holds the button), at the press point for S22
    EXPECT_TRUE(InputActions::getButton("Steer"));
    EXPECT_FALSE(steer->isPressed());
    // either key of a side
    key(D_KEY_RIGHT, true);
    for (int i = 0; i < 20; i++) frame();
    EXPECT_FLOAT_EQ(steer->readValue(), 1.0f);
    EXPECT_TRUE(steer->isPressed());
    key(D_KEY_D, false);
    key(D_KEY_RIGHT, false);
    // snap: the other side starts from 0
    key(D_KEY_A, true);
    frame();
    EXPECT_NEAR(steer->readValue(), -0.05f, 1e-5f);
    key(D_KEY_A, false);
    frame();
    // gravity takes it back
    EXPECT_NEAR(steer->readValue(), 0.0f, 1e-5f);
}

TEST_F(InputManagerMapTest, ABindingReadsItsOwnGamepad){
    load(R"({"version":1,"maps":[{"name":"Gameplay","actions":[{"name":"Fire","type":"BUTTON","bindings":[
      {"path":"gamepad/a","gamepad":1}]}]}]})");
    Engine::systemGamepadConnect(0, "pad 0");
    Engine::systemGamepadConnect(1, "pad 1");
    Engine::systemGamepadButtonDown(0, D_GAMEPAD_BUTTON_A);
    frame();
    EXPECT_FALSE(InputActions::get("Gameplay/Fire")->isPressed());
    Engine::systemGamepadButtonDown(1, D_GAMEPAD_BUTTON_A);
    frame();
    EXPECT_TRUE(InputActions::get("Gameplay/Fire")->isPressed());
    Engine::systemGamepadButtonUp(0, D_GAMEPAD_BUTTON_A);
    Engine::systemGamepadButtonUp(1, D_GAMEPAD_BUTTON_A);
}

TEST_F(InputManagerMapTest, TypingAndMenusSuppressOnlyMapsThatIgnoreTheUI){
    load(R"({"version":1,"maps":[
      {"name":"Gameplay","actions":[{"name":"Walk","type":"BUTTON","bindings":[{"path":"keyboard/w"}]},
                                    {"name":"Confirm","type":"BUTTON","bindings":[{"path":"gamepad/a"}]}]},
      {"name":"Debug","priority":-5,"ignoreWhenUIConsumes":false,"consume":false,"actions":[{"name":"Walk","type":"BUTTON","bindings":[{"path":"keyboard/w"}]}]}]})");
    InputSuppression typing;
    typing.keyboard = true;
    key(D_KEY_W, true);
    frame(typing);
    EXPECT_FALSE(InputActions::get("Gameplay/Walk")->isPressed());
    EXPECT_TRUE(InputActions::get("Debug/Walk")->isPressed());
    // the field loses the focus: the held key counts again
    frame();
    EXPECT_TRUE(InputActions::get("Gameplay/Walk")->isPressed());
    key(D_KEY_W, false);

    InputSuppression menu;
    menu.gamepadNavigation = true;
    Engine::systemGamepadConnect(0, "pad");
    Engine::systemGamepadButtonDown(0, D_GAMEPAD_BUTTON_A);
    frame(menu);
    EXPECT_FALSE(InputActions::get("Gameplay/Confirm")->isPressed());
    frame();
    EXPECT_TRUE(InputActions::get("Gameplay/Confirm")->isPressed());
    Engine::systemGamepadButtonUp(0, D_GAMEPAD_BUTTON_A);
}

TEST_F(InputManagerMapTest, VirtualSourcesFeedS22Actions){
    load(R"({"version":1,"maps":[{"name":"Gameplay","actions":[
      {"name":"Throttle","type":"AXIS","bindings":[{"path":"keyboard/w"}]},
      {"name":"Fire","type":"BUTTON","bindings":[{"path":"keyboard/f"}]},
      {"name":"Look","type":"AXIS","bindings":[{"path":"mouse/delta/x","scale":0.1}]}]}]})");
    InputAction* throttle = InputActions::get("Gameplay/Throttle");
    InputAction* fire = InputActions::get("Gameplay/Fire");
    const uintptr_t stick = 0x1100, button = 0x2200, area = 0x3300;
    // a bounded source sums with the bindings (counted at once by getAxis, by the action at the evaluation)
    InputActions::setSourceValue(stick, "Gameplay/Throttle", 0.5f);
    EXPECT_NEAR(InputActions::getAxis("Throttle"), 0.5f, 1e-5f);
    frame();
    EXPECT_NEAR(throttle->readValue(), 0.5f, 1e-5f);
    key(D_KEY_W, true);
    frame();
    EXPECT_FLOAT_EQ(throttle->readValue(), 1.0f);
    key(D_KEY_W, false);
    // an on-screen button tapped between two frames: pressed and released in the window
    InputActions::setSourceValue(button, "Fire", 1.0f);
    InputActions::setSourceValue(button, "Fire", 0.0f);
    frame();
    EXPECT_TRUE(fire->wasPressedThisFrame());
    EXPECT_TRUE(fire->wasReleasedThisFrame());
    EXPECT_FALSE(fire->isPressed());
    // held by the button, released when its owner clears its sources
    InputActions::setSourceValue(button, "Fire", 1.0f);
    frame();
    EXPECT_TRUE(fire->isPressed());
    InputActions::clearSource(button);
    frame();
    EXPECT_FALSE(fire->isPressed());
    EXPECT_TRUE(fire->wasReleasedThisFrame());
    // a touch area's drag: a delta for the frame, unclamped, on top of the bindings
    InputActions::addSourceDelta(area, "Look", 30.0f);
    frame();
    EXPECT_FLOAT_EQ(InputActions::get("Gameplay/Look")->readValue(), 30.0f);
    frame();
    EXPECT_FLOAT_EQ(InputActions::get("Gameplay/Look")->readValue(), 0.0f);
    EXPECT_TRUE(InputActions::isMouseMotionBound("Look"));
    EXPECT_FALSE(InputActions::isMouseMotionBound("Fire"));
    InputActions::clearSource(stick);
}

TEST_F(InputManagerMapTest, FlatRebindingReplacesS22Bindings){
    load(R"({"version":1,"maps":[{"name":"Gameplay","actions":[{"name":"Jump","type":"BUTTON","bindings":[{"id":"aaaa0001","path":"keyboard/space"}]}]}]})");
    // an S22 binding has the binding kind in the flat shape
    std::vector<InputManagerBinding> current = InputActions::getBindings("Gameplay/Jump");
    ASSERT_EQ(current.size(), 1u);
    EXPECT_EQ(current[0].kind, InputManagerBindingKind::Binding);
    EXPECT_EQ(InputManagerFormat::bindingsToJson(current), R"([{"kind":"binding","binding":{"path":"keyboard/space"}}])");
    // its per-binding override (S22 rebinding) shows there
    InputActions::setOverride("aaaa0001", "keyboard/enter");
    EXPECT_EQ(InputManagerFormat::bindingsToJson(InputActions::getBindings("Jump")), R"([{"kind":"binding","binding":{"path":"keyboard/enter"}}])");
    InputActions::removeOverride("aaaa0001");

    std::vector<InputManagerBinding> replacement;
    std::string error;
    ASSERT_TRUE(InputManagerFormat::parseBindingsJson(R"([{"kind":"buttons","positive":["k"],"gravity":0,"sensitivity":0,"dead":0}])",
                                                      replacement, &error, "Jump")) << error;
    ASSERT_TRUE(InputActions::setBindings("Jump", replacement, &error)) << error;
    key(D_KEY_K, true);
    frame();
    EXPECT_TRUE(InputActions::get("Gameplay/Jump")->isPressed());
    key(D_KEY_K, false);
    EXPECT_NE(InputActions::exportOverrides().find("\"Gameplay/Jump\""), std::string::npos);
    InputActions::resetBindings("Jump");
    EXPECT_FALSE(InputActions::hasBindingOverride("Jump"));
}

TEST_F(InputManagerMapTest, TouchesHaveUnityFieldsAndTaps){
    Engine::systemTouchStart(3, 100, 100);
    frame();
    ASSERT_EQ(Input::getTouchCount(), 1u);
    const Touch touch = Input::getTouch(0);
    EXPECT_EQ(touch.fingerId, 3);
    EXPECT_EQ(touch.pointer, 3);
    EXPECT_EQ(touch.phase, TouchPhase::Began);
    EXPECT_EQ(touch.tapCount, 1);
    Engine::systemTouchEnd(3, 100, 100);
    frame();
    Engine::systemTouchStart(3, 102, 101);
    frame();
    EXPECT_EQ(Input::getTouch(0).tapCount, 2);
    Engine::systemTouchMove(3, 110, 101);
    frame();
    EXPECT_EQ(Input::getTouch(0).phase, TouchPhase::Moved);
    EXPECT_FLOAT_EQ(Input::getTouch(0).deltaPosition.x, 8.0f);
    EXPECT_FLOAT_EQ(Input::getTouch(0).delta.x, 8.0f);
    EXPECT_NEAR(Input::getTouch(0).deltaTime, 1.0f / 60.0f, 1e-5f);
    Engine::systemTouchEnd(3, 110, 101);
    frame();
}
