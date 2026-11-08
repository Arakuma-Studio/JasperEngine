// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Input Actions (docs/specs/S22-gameplay.md R8-R18, AC4, AC5): assets loaded from text, events
// through Engine::system*, each frame latched with Input::beginFrame() and evaluated with
// InputActions::evaluate(), as Engine::systemDraw does. Rebinding overrides go to a fake store.

#include "Engine.h"
#include "Input.h"
#include "TestSystem.h"
#include "input/InputActionAssetJson.h"
#include "input/InputActions.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    class FakeStore : public InputOverrideStore {
    public:
        std::map<std::string, std::string> values;
        std::string read(const std::string& key) override{
            auto it = values.find(key);
            return it != values.end() ? it->second : std::string();
        }
        void write(const std::string& key, const std::string& value) override{
            values[key] = value;
        }
        void remove(const std::string& key) override{
            values.erase(key);
        }
    };

    // the asset excerpt of the spec (§6), with ids for every part
    const char* GAMEPLAY = R"({"version":1,"maps":[
 {"name":"Gameplay","priority":0,"actions":[
  {"name":"Move","type":"VECTOR2","bindings":[
   {"id":"3f9a12c0","composite":"dpad2d","parts":{"up":{"id":"3f9a12c1","path":"keyboard/w"},"down":{"id":"3f9a12c2","path":"keyboard/s"},
    "left":{"id":"3f9a12c3","path":"keyboard/a"},"right":{"id":"3f9a12c4","path":"keyboard/d"}},"groups":["Keyboard&Mouse"]},
   {"id":"8b01aa47","path":"gamepad/left_stick","groups":["Gamepad"]}]},
  {"name":"Jump","type":"BUTTON","bindings":[{"id":"c2d4e611","path":"keyboard/space","groups":["Keyboard&Mouse"]},{"id":"77e0b9d2","path":"gamepad/a","groups":["Gamepad"]}]}]},
 {"name":"Menu","priority":100,"enabled":false,"exclusive":true,"actions":[]}]})";

    class InputActionsTest : public ::testing::Test {
    protected:
        std::shared_ptr<FakeStore> store = std::make_shared<FakeStore>();

        void SetUp() override{
            test::TestSystem::get().setScreenSize(640, 360);
            Engine::setCanvasSize(640, 360);
            Engine::setScalingMode(Scaling::FITWIDTH);
            Engine::systemViewChanged();
            Input::resetFrameState();
            InputActions::setOverrideStore(store);
            InputActions::setDefaultDeadzone(0.15f);
        }

        void TearDown() override{
            InputActions::unload();
            InputActions::setOverrideStore(nullptr);
            for (int pad : {0, 1})
                Engine::systemGamepadDisconnect(pad);
            Input::resetFrameState();
        }

        static void load(const char* json, const char* path = "test.inputactions"){
            ASSERT_TRUE(InputActions::loadFromText(path, json));
        }

        static void frame(){
            Input::beginFrame();
            InputActions::evaluate(1.0f / 60.0f);
        }

        static void key(int key, bool down, int mods = 0){
            if (down) Engine::systemKeyDown(key, false, mods);
            else Engine::systemKeyUp(key, false, mods);
        }

        static Vector2 move(){
            return InputActions::get("Gameplay/Move")->readVector2();
        }
    };

    void expectNear(Vector2 actual, Vector2 expected, float tolerance = 1e-4f){
        EXPECT_NEAR(actual.x, expected.x, tolerance);
        EXPECT_NEAR(actual.y, expected.y, tolerance);
    }

}

TEST_F(InputActionsTest, Dpad2dNormalized){
    load(GAMEPLAY);
    key(D_KEY_W, true);
    frame();
    expectNear(move(), Vector2(0, 1));
    key(D_KEY_D, true);
    frame();
    expectNear(move(), Vector2(0.70710677f, 0.70710677f));
    // opposite parts cancel
    key(D_KEY_S, true);
    frame();
    expectNear(move(), Vector2(1, 0));
    key(D_KEY_W, false);
    key(D_KEY_S, false);
    key(D_KEY_D, false);
    frame();
    expectNear(move(), Vector2(0, 0));
}

TEST_F(InputActionsTest, RadialDeadzone){
    load(GAMEPLAY);
    Engine::systemGamepadConnect(0, "Pad");
    Engine::systemGamepadAxisMove(0, D_GAMEPAD_AXIS_LEFT_X, 0.1f);
    frame();
    expectNear(move(), Vector2(0, 0));
    EXPECT_EQ(InputActions::get("Gameplay/Move")->getPhase(), InputActionPhase::WAITING);

    // rescaled from the deadzone: (0.575 - 0.15) / 0.85 = 0.5
    Engine::systemGamepadAxisMove(0, D_GAMEPAD_AXIS_LEFT_X, 0.575f);
    frame();
    expectNear(move(), Vector2(0.5f, 0));
    // the stick's y is up-positive (the device's is down-positive)
    Engine::systemGamepadAxisMove(0, D_GAMEPAD_AXIS_LEFT_X, 0.0f);
    Engine::systemGamepadAxisMove(0, D_GAMEPAD_AXIS_LEFT_Y, -1.0f);
    frame();
    expectNear(move(), Vector2(0, 1));
    // radial: a diagonal under the deadzone in length is 0 although each axis alone is not tiny
    Engine::systemGamepadAxisMove(0, D_GAMEPAD_AXIS_LEFT_X, 0.1f);
    Engine::systemGamepadAxisMove(0, D_GAMEPAD_AXIS_LEFT_Y, 0.1f);
    frame();
    expectNear(move(), Vector2(0, 0));
}

TEST_F(InputActionsTest, PriorityConsumption){
    load(R"({"version":1,"maps":[
     {"name":"Low","actions":[{"name":"Jump","bindings":[{"path":"keyboard/space"}]}]},
     {"name":"High","priority":10,"actions":[{"name":"Fire","bindings":[{"path":"keyboard/space"}]}]}]})");
    key(D_KEY_SPACE, true);
    frame();
    EXPECT_TRUE(InputActions::get("High/Fire")->isPressed());
    EXPECT_FALSE(InputActions::get("Low/Jump")->isPressed());
    EXPECT_EQ(InputActions::get("Low/Jump")->getConsumedBy(), "High");

    // a map that does not consume leaves the control to the lower ones
    key(D_KEY_SPACE, false);
    frame();
    ASSERT_TRUE(InputActions::reloadText(R"({"version":1,"maps":[
     {"name":"Low","actions":[{"name":"Jump","bindings":[{"path":"keyboard/space"}]}]},
     {"name":"High","priority":10,"consume":false,"actions":[{"name":"Fire","bindings":[{"path":"keyboard/space"}]}]}]})"));
    key(D_KEY_SPACE, true);
    frame();
    EXPECT_TRUE(InputActions::get("High/Fire")->isPressed());
    EXPECT_TRUE(InputActions::get("Low/Jump")->isPressed());

    // priority changed at runtime
    InputActions::setMapPriority("Low", 20);
    InputActions::setMapPriority("High", 0);
    ASSERT_TRUE(InputActions::reloadText(R"({"version":1,"maps":[
     {"name":"Low","actions":[{"name":"Jump","bindings":[{"path":"keyboard/space"}]}]},
     {"name":"High","priority":10,"actions":[{"name":"Fire","bindings":[{"path":"keyboard/space"}]}]}]})"));
    frame();
    // the maps keep their runtime priority across the edit: Low is first now and takes the key
    EXPECT_EQ(InputActions::getMapPriority("Low"), 20);
    EXPECT_TRUE(InputActions::get("Low/Jump")->isPressed());
    EXPECT_FALSE(InputActions::get("High/Fire")->isPressed());
    EXPECT_EQ(InputActions::get("High/Fire")->getConsumedBy(), "Low");
}

TEST_F(InputActionsTest, ExclusiveBlocksLower){
    load(GAMEPLAY);
    key(D_KEY_W, true);
    frame();
    expectNear(move(), Vector2(0, 1));
    InputActions::enableMap("Menu", true);
    frame();
    expectNear(move(), Vector2(0, 0));
    EXPECT_EQ(InputActions::get("Gameplay/Move")->getConsumedBy(), "Menu");
    EXPECT_TRUE(InputActions::get("Gameplay/Move")->wasReleasedThisFrame() || InputActions::get("Gameplay/Move")->getPhase() == InputActionPhase::CANCELED);
    InputActions::enableMap("Menu", false);
    frame();
    expectNear(move(), Vector2(0, 1));
}

TEST_F(InputActionsTest, ModifierBindingSuppressesPlain){
    load(R"({"version":1,"maps":[{"name":"Editor","actions":[
     {"name":"Save","bindings":[{"path":"keyboard/s","requiredMods":["ctrl"]}]},
     {"name":"Back","bindings":[{"path":"keyboard/s"}]}]}]})");
    key(D_KEY_LEFT_CONTROL, true);
    key(D_KEY_S, true);
    frame();
    EXPECT_TRUE(InputActions::get("Editor/Save")->isPressed());
    EXPECT_FALSE(InputActions::get("Editor/Back")->isPressed());
    key(D_KEY_S, false);
    key(D_KEY_LEFT_CONTROL, false);
    frame();

    key(D_KEY_S, true);
    frame();
    EXPECT_FALSE(InputActions::get("Editor/Save")->isPressed());
    EXPECT_TRUE(InputActions::get("Editor/Back")->isPressed());
    key(D_KEY_S, false);
    frame();

    // the modifiers of the key event itself (send_input's mods) count too, for a tap
    key(D_KEY_S, true, D_MODIFIER_CONTROL);
    key(D_KEY_S, false, D_MODIFIER_CONTROL);
    frame();
    EXPECT_TRUE(InputActions::get("Editor/Save")->wasPressedThisFrame());
    EXPECT_FALSE(InputActions::get("Editor/Back")->wasPressedThisFrame());
}

TEST_F(InputActionsTest, ForbiddenMods){
    load(R"({"version":1,"maps":[{"name":"Gameplay","actions":[
     {"name":"Walk","bindings":[{"path":"keyboard/w","forbiddenMods":["shift"]}]}]}]})");
    key(D_KEY_LEFT_SHIFT, true);
    key(D_KEY_W, true);
    frame();
    EXPECT_FALSE(InputActions::get("Gameplay/Walk")->isPressed());
    key(D_KEY_LEFT_SHIFT, false);
    frame();
    EXPECT_TRUE(InputActions::get("Gameplay/Walk")->isPressed());
}

TEST_F(InputActionsTest, UIConsumedIgnored){
    load(R"({"version":1,"maps":[
     {"name":"Game","actions":[{"name":"Jump","bindings":[{"path":"keyboard/space"}]}]},
     {"name":"Always","consume":false,"ignoreWhenUIConsumes":false,"actions":[{"name":"Jump","bindings":[{"path":"keyboard/space"}]}]}]})");
    // what Engine::systemKeyDown does when a document took the key
    Engine::systemKeyDown(D_KEY_SPACE, false, 0);
    Input::setPressConsumedByUI(Input::keyControl(D_KEY_SPACE));
    frame();
    EXPECT_FALSE(InputActions::get("Game/Jump")->isPressed());
    EXPECT_FALSE(InputActions::get("Game/Jump")->wasPressedThisFrame());
    EXPECT_TRUE(InputActions::get("Always/Jump")->isPressed());
    // raw Input is unaffected
    EXPECT_TRUE(Input::getKeyDown(D_KEY_SPACE));
    // the held key stays the UI's until it comes up
    frame();
    EXPECT_FALSE(InputActions::get("Game/Jump")->isPressed());
    Engine::systemKeyUp(D_KEY_SPACE, false, 0);
    frame();
    Engine::systemKeyDown(D_KEY_SPACE, false, 0);
    frame();
    EXPECT_TRUE(InputActions::get("Game/Jump")->isPressed());
}

TEST_F(InputActionsTest, GamepadFilter){
    load(R"({"version":1,"maps":[
     {"name":"P1","actions":[{"name":"Jump","bindings":[{"path":"gamepad/a"}]},{"name":"Steer","type":"AXIS","bindings":[{"path":"gamepad/left_x"}]}]},
     {"name":"P2","actions":[{"name":"Jump","bindings":[{"path":"gamepad/a"}]}]},
     {"name":"Any","consume":false,"priority":5,"actions":[{"name":"Jump","bindings":[{"path":"gamepad/a"}]},{"name":"Steer","type":"AXIS","bindings":[{"path":"gamepad/left_x"}]}]}]})");
    Engine::systemGamepadConnect(0, "Pad A");
    Engine::systemGamepadConnect(1, "Pad B");
    InputActions::setGamepad("P1", 0);
    InputActions::getMap("P2").setGamepad(1);
    EXPECT_EQ(InputActions::getGamepad("P2"), 1);

    Engine::systemGamepadButtonDown(1, D_GAMEPAD_BUTTON_A);
    frame();
    EXPECT_FALSE(InputActions::get("P1/Jump")->isPressed());
    EXPECT_TRUE(InputActions::get("P2/Jump")->isPressed());
    EXPECT_TRUE(InputActions::get("Any/Jump")->isPressed());

    // any pad: the axis of largest magnitude
    Engine::systemGamepadAxisMove(0, D_GAMEPAD_AXIS_LEFT_X, 0.4f);
    Engine::systemGamepadAxisMove(1, D_GAMEPAD_AXIS_LEFT_X, -0.9f);
    frame();
    EXPECT_LT(InputActions::get("Any/Steer")->readValue(), -0.8f);
    EXPECT_GT(InputActions::get("P1/Steer")->readValue(), 0.2f);
}

// every action event goes at Stop (unload) and with a new session, through InputAction::forEachEvent
TEST_F(InputActionsTest, UnloadAndANewSessionClearEveryActionEvent){
    for (bool unload : {true, false}){
        load(GAMEPLAY);
        InputAction* jump = InputActions::get("Gameplay/Jump");
        ASSERT_NE(jump, nullptr);
        auto ignore = std::function<void(InputActionEvent&)>([](InputActionEvent&){});
        jump->onStarted.add("test", ignore);
        jump->onPerformed.add("test", ignore);
        jump->onCanceled.add("test", ignore);
        if (unload) InputActions::unload();
        else InputActions::resetSession();
        EXPECT_EQ(jump->onStarted.size(), 0u) << unload;
        EXPECT_EQ(jump->onPerformed.size(), 0u) << unload;
        EXPECT_EQ(jump->onCanceled.size(), 0u) << unload;
    }
}

TEST_F(InputActionsTest, SameWindowTapPerformsAndCancels){
    load(GAMEPLAY);
    InputAction* jump = InputActions::get("Gameplay/Jump");
    std::vector<std::string> events;
    jump->onStarted.add("test", std::function<void(InputActionEvent&)>([&](InputActionEvent& e){ events.push_back("started"); }));
    jump->onPerformed.add("test", std::function<void(InputActionEvent&)>([&](InputActionEvent& e){ events.push_back("performed"); }));
    jump->onCanceled.add("test", std::function<void(InputActionEvent&)>([&](InputActionEvent& e){ events.push_back("canceled"); }));

    key(D_KEY_SPACE, true);
    key(D_KEY_SPACE, false);
    frame();
    EXPECT_TRUE(jump->wasPressedThisFrame());
    EXPECT_TRUE(jump->wasReleasedThisFrame());
    EXPECT_FALSE(jump->isPressed());
    EXPECT_EQ(events, std::vector<std::string>({"started", "performed", "canceled"}));

    frame();
    EXPECT_FALSE(jump->wasPressedThisFrame());
    EXPECT_FALSE(jump->wasReleasedThisFrame());
    EXPECT_EQ(events.size(), 3u);

    // a held press: started and performed, then canceled on release
    events.clear();
    key(D_KEY_SPACE, true);
    frame();
    EXPECT_EQ(events, std::vector<std::string>({"started", "performed"}));
    frame();
    key(D_KEY_SPACE, false);
    frame();
    EXPECT_EQ(events, std::vector<std::string>({"started", "performed", "canceled"}));
}

TEST_F(InputActionsTest, CallbackConsume){
    load(R"({"version":1,"maps":[
     {"name":"Hud","priority":10,"consume":false,"actions":[{"name":"Click","bindings":[{"path":"mouse/left"}]}]},
     {"name":"World","actions":[{"name":"Shoot","bindings":[{"path":"mouse/left"}]}]}]})");
    bool consume = true;
    int performed = 0;
    InputActions::get("Hud/Click")->onPerformed.add("test", std::function<void(InputActionEvent&)>([&](InputActionEvent& e){
        performed++;
        if (consume) e.consume();
    }));
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, 10, 10, 0);
    frame();
    EXPECT_EQ(performed, 1);
    EXPECT_TRUE(InputActions::get("Hud/Click")->isPressed());
    EXPECT_FALSE(InputActions::get("World/Shoot")->isPressed());
    EXPECT_FALSE(InputActions::get("World/Shoot")->wasPressedThisFrame());
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, 10, 10, 0);
    frame();

    consume = false;
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, 10, 10, 0);
    frame();
    EXPECT_TRUE(InputActions::get("World/Shoot")->isPressed());
}

TEST_F(InputActionsTest, FixedStepSeesActionEdgeOnce){
    load(GAMEPLAY);
    InputAction* jump = InputActions::get("Gameplay/Jump");
    key(D_KEY_SPACE, true);
    key(D_KEY_SPACE, false);
    frame();
    int seen = 0;
    for (int step = 0; step < 3; step++){
        Input::beginFixedStep();
        if (jump->wasPressedThisFrame()) seen++;
        Input::endFixedStep();
        InputActions::endFixedStep();
    }
    EXPECT_EQ(seen, 1);
}

TEST_F(InputActionsTest, HoldTimePerformsAfterHolding){
    load(R"({"version":1,"maps":[{"name":"Gameplay","actions":[{"name":"Charge","holdTime":0.1,"bindings":[{"path":"keyboard/e"}]}]}]})");
    InputAction* charge = InputActions::get("Gameplay/Charge");
    int performed = 0;
    charge->onPerformed.add("test", std::function<void(InputActionEvent&)>([&](InputActionEvent&){ performed++; }));
    key(D_KEY_E, true);
    frame();
    EXPECT_EQ(charge->getPhase(), InputActionPhase::STARTED);
    for (int i = 0; i < 3; i++)
        frame();
    EXPECT_EQ(performed, 0);
    for (int i = 0; i < 4; i++)
        frame();
    EXPECT_EQ(performed, 1);
    EXPECT_EQ(charge->getPhase(), InputActionPhase::PERFORMED);
    EXPECT_GE(charge->getHeldTime(), 0.1f);
    // a tap shorter than the hold starts and cancels, never performs
    key(D_KEY_E, false);
    frame();
    key(D_KEY_E, true);
    key(D_KEY_E, false);
    frame();
    EXPECT_EQ(performed, 1);
}

TEST_F(InputActionsTest, VirtualAndInject){
    load(R"({"version":1,"maps":[{"name":"Gameplay","actions":[
     {"name":"Move","type":"VECTOR2","bindings":[{"path":"virtual/stick"}]},
     {"name":"Jump","bindings":[{"path":"keyboard/space"}]}]}]})");
    InputActions::setVirtual("stick", Vector2(0.3f, -0.4f));
    frame();
    expectNear(move(), Vector2(0.3f, -0.4f));
    InputActions::setVirtual("stick", Vector2(0, 0));
    frame();
    expectNear(move(), Vector2(0, 0));

    // two windows of a forced value, then back to the bindings
    InputActions::inject("Gameplay/Jump", Vector2(1, 0), 2);
    frame();
    EXPECT_TRUE(InputActions::get("Gameplay/Jump")->wasPressedThisFrame());
    frame();
    EXPECT_TRUE(InputActions::get("Gameplay/Jump")->isPressed());
    frame();
    EXPECT_FALSE(InputActions::get("Gameplay/Jump")->isPressed());
    EXPECT_TRUE(InputActions::get("Gameplay/Jump")->wasReleasedThisFrame());
    // 0 windows: a tap
    InputActions::inject("Gameplay/Jump", Vector2(1, 0), 0);
    frame();
    EXPECT_TRUE(InputActions::get("Gameplay/Jump")->wasPressedThisFrame());
    EXPECT_TRUE(InputActions::get("Gameplay/Jump")->wasReleasedThisFrame());
}

TEST_F(InputActionsTest, DisplayNames){
    load(R"({"version":1,"maps":[{"name":"Gameplay","actions":[
     {"name":"Save","bindings":[{"id":"00000001","path":"keyboard/s","requiredMods":["ctrl"]}]},
     {"name":"Jump","bindings":[{"id":"00000002","path":"keyboard/space"},{"id":"00000003","path":"gamepad/a"}]},
     {"name":"Look","type":"VECTOR2","bindings":[{"id":"00000004","path":"gamepad/left_stick"}]},
     {"name":"Move","type":"VECTOR2","bindings":[{"id":"00000005","composite":"dpad2d","parts":{"up":{"path":"keyboard/w"},"down":{"path":"keyboard/s"},"left":{"path":"keyboard/a"},"right":{"path":"keyboard/d"}}}]}]}]})");
    EXPECT_EQ(InputActions::getBindingDisplayName("00000001"), "Ctrl+S");
    EXPECT_EQ(InputActions::getBindingDisplayName("00000002"), "Space");
    EXPECT_EQ(InputActions::getBindingDisplayName("00000004"), "Left Stick");
    EXPECT_EQ(InputActions::getBindingDisplayName("00000005"), "W/A/S/D");
    EXPECT_EQ(InputActions::getBindingDisplayName("00000003"), "A");
    Engine::systemGamepadConnect(0, "Sony DualSense Wireless Controller");
    EXPECT_EQ(InputActions::getBindingDisplayName("00000003"), "Cross");
    EXPECT_EQ(InputActions::getBinding("00000003").getDisplayName(), "Cross");
}

TEST_F(InputActionsTest, ControlBoundForShortcutRouting){
    load(GAMEPLAY);
    EXPECT_TRUE(InputActions::isControlBound("keyboard/space"));
    EXPECT_TRUE(InputActions::isControlBound("keyboard/w"));
    EXPECT_TRUE(InputActions::isControlBound("Keyboard/W"));
    EXPECT_FALSE(InputActions::isControlBound("keyboard/f"));
    // a disabled map's bindings do not take a shortcut
    ASSERT_TRUE(InputActions::reloadText(R"({"version":1,"maps":[{"name":"Menu","enabled":false,"actions":[{"name":"Open","bindings":[{"path":"keyboard/f"}]}]}]})"));
    EXPECT_FALSE(InputActions::isControlBound("keyboard/f"));
}

// ---- rebinding and persistence (AC5)

TEST_F(InputActionsTest, RebindCapturesNextControl){
    load(GAMEPLAY);
    std::vector<InputRebindResult> results;
    ASSERT_TRUE(InputActions::startRebind("Gameplay/Jump", "c2d4e611", InputRebindOptions(),
                                          [&](const InputRebindResult& r){ results.push_back(r); }));
    EXPECT_TRUE(InputActions::isRebinding());
    frame();
    EXPECT_TRUE(results.empty());

    // the maps are suspended meanwhile: the old binding does nothing
    key(D_KEY_J, true);
    frame();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].status, InputRebindStatus::COMPLETED);
    EXPECT_EQ(results[0].path, "keyboard/j");
    EXPECT_EQ(results[0].bindingId, "c2d4e611");
    EXPECT_FALSE(InputActions::isRebinding());
    EXPECT_EQ(InputActions::getBindingPath("c2d4e611"), "keyboard/j");
    // an override, not an asset edit
    EXPECT_EQ(InputActions::getAsset().findBinding("c2d4e611")->path, "keyboard/space");
    key(D_KEY_J, false);
    frame();

    key(D_KEY_J, true);
    frame();
    EXPECT_TRUE(InputActions::get("Gameplay/Jump")->isPressed());
    key(D_KEY_J, false);
    key(D_KEY_SPACE, true);
    frame();
    EXPECT_FALSE(InputActions::get("Gameplay/Jump")->isPressed());
    key(D_KEY_SPACE, false);

    // groups and exclusions: a key is refused when only the gamepad is asked for
    results.clear();
    InputRebindOptions gamepadOnly;
    gamepadOnly.groups = {"Gamepad"};
    ASSERT_TRUE(InputActions::startRebind("Gameplay/Jump", "77e0b9d2", gamepadOnly, [&](const InputRebindResult& r){ results.push_back(r); }));
    key(D_KEY_K, true);
    frame();
    EXPECT_TRUE(results.empty());
    Engine::systemGamepadConnect(0, "Pad");
    Engine::systemGamepadButtonDown(0, D_GAMEPAD_BUTTON_Y);
    frame();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].path, "gamepad/y");

    // a stick binding gets a stick, from an axis crossing 0.5
    results.clear();
    ASSERT_TRUE(InputActions::startRebind("Gameplay/Move", "8b01aa47", gamepadOnly, [&](const InputRebindResult& r){ results.push_back(r); }));
    Engine::systemGamepadAxisMove(0, D_GAMEPAD_AXIS_RIGHT_X, 0.3f);
    frame();
    EXPECT_TRUE(results.empty());
    Engine::systemGamepadAxisMove(0, D_GAMEPAD_AXIS_RIGHT_X, 0.8f);
    frame();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].path, "gamepad/right_stick");

    // conflicts: another binding in the same group on the same path
    InputActions::setOverride("77e0b9d2", "keyboard/w");
    const std::vector<InputBindingInfo> conflicts = InputActions::getConflicts("Gameplay/Jump", "c2d4e611");
    EXPECT_TRUE(conflicts.empty());
    InputActions::setOverride("c2d4e611", "keyboard/w");
    const std::vector<InputBindingInfo> withW = InputActions::getConflicts("Gameplay/Jump", "c2d4e611");
    ASSERT_EQ(withW.size(), 1u);
    EXPECT_EQ(withW[0].id, "3f9a12c1");
    EXPECT_EQ(withW[0].action, "Gameplay/Move");
}

TEST_F(InputActionsTest, RebindCancelKey){
    load(GAMEPLAY);
    std::vector<InputRebindResult> results;
    ASSERT_TRUE(InputActions::startRebind("Gameplay/Jump", "c2d4e611", InputRebindOptions(), [&](const InputRebindResult& r){ results.push_back(r); }));
    key(D_KEY_ESCAPE, true);
    frame();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].status, InputRebindStatus::CANCELED);
    EXPECT_EQ(InputActions::getBindingPath("c2d4e611"), "keyboard/space");
    key(D_KEY_ESCAPE, false);

    // a timeout
    results.clear();
    InputRebindOptions options;
    options.timeout = 0.05f;
    options.exclude = {"mouse/*"};
    ASSERT_TRUE(InputActions::startRebind("Gameplay/Jump", "c2d4e611", options, [&](const InputRebindResult& r){ results.push_back(r); }));
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, 5, 5, 0);
    for (int i = 0; i < 5 && results.empty(); i++)
        frame();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].status, InputRebindStatus::TIMED_OUT);

    // cancelRebind reports the cancel
    results.clear();
    ASSERT_TRUE(InputActions::startRebind("Gameplay/Jump", "c2d4e611", InputRebindOptions(), [&](const InputRebindResult& r){ results.push_back(r); }));
    InputActions::cancelRebind();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].status, InputRebindStatus::CANCELED);
}

TEST_F(InputActionsTest, OverridesRoundTrip){
    load(GAMEPLAY, "input/player.inputactions");
    const std::string key = InputActions::getOverridesKey();
    // no GUID is known for the path in a unit test: the path is the key
    EXPECT_EQ(key, "jasperengine.input.input/player.inputactions");
    InputActions::setOverride("c2d4e611", "keyboard/enter");
    ASSERT_TRUE(InputActions::saveOverrides());
    ASSERT_TRUE(store->values.count(key));
    EXPECT_NE(store->values[key].find("\"c2d4e611\":\"keyboard/enter\""), std::string::npos);
    EXPECT_NE(store->values[key].find("\"version\":1"), std::string::npos);

    InputActions::unload();
    load(GAMEPLAY, "input/player.inputactions");
    EXPECT_EQ(InputActions::getBindingPath("c2d4e611"), "keyboard/enter");
    EXPECT_TRUE(InputActions::getBinding("c2d4e611").hasOverride());

    InputActions::resetOverrides();
    EXPECT_EQ(InputActions::getBindingPath("c2d4e611"), "keyboard/space");
    EXPECT_FALSE(store->values.count(key));
}

TEST_F(InputActionsTest, BindingIdStableAcrossReorder){
    load(GAMEPLAY);
    InputActions::setOverride("77e0b9d2", "gamepad/b");
    InputActionAssetDesc asset = InputActions::getAsset();
    applyInputActionsOp(asset, InputJson::parse(R"({"op":"move","binding":"77e0b9d2","index":0})"));
    applyInputActionsOp(asset, InputJson::parse(R"({"op":"move","action":"Gameplay/Jump","index":0})"));
    EXPECT_EQ(asset.findAction("Gameplay/Jump")->bindings[0].id, "77e0b9d2");
    ASSERT_TRUE(InputActions::reloadText(asset.save()));
    EXPECT_EQ(InputActions::getBindingPath("77e0b9d2"), "gamepad/b");
    // ids survive a save and a parse
    InputActionAssetDesc again;
    std::string error;
    ASSERT_TRUE(InputActionAssetDesc::parse(asset.save(), again, error)) << error;
    EXPECT_EQ(again.save(), asset.save());
    EXPECT_NE(again.findBinding("3f9a12c4"), nullptr);
}

TEST_F(InputActionsTest, UnknownOverrideIgnored){
    store->values["jasperengine.input.test.inputactions"] =
        R"({"version":1,"overrides":{"deadbeef":"keyboard/q","c2d4e611":"keyboard/x"}})";
    load(GAMEPLAY);
    EXPECT_EQ(InputActions::getBindingPath("c2d4e611"), "keyboard/x");
    key(D_KEY_Q, true);
    frame();
    EXPECT_FALSE(InputActions::get("Gameplay/Jump")->isPressed());
    // kept for a build that has the binding
    ASSERT_TRUE(InputActions::saveOverrides());
    EXPECT_NE(store->values["jasperengine.input.test.inputactions"].find("deadbeef"), std::string::npos);
}

TEST_F(InputActionsTest, UnknownKeysPreservedOnSave){
    const char* text = R"({"version":1,"author":"tools","maps":[
     {"name":"Gameplay","color":"#ff0000","actions":[
      {"name":"Jump","type":"BUTTON","icon":"jump.png","bindings":[
       {"id":"c2d4e611","path":"keyboard/space","note":{"why":"default"}},
       {"id":"3f9a12c0","composite":"axis1d","parts":{"negative":{"id":"3f9a12c1","path":"keyboard/q","x":1},"positive":{"id":"3f9a12c2","path":"keyboard/e"}},"extra":[1,2]}]}]}],
     "trailer":true})";
    InputActionAssetDesc asset;
    std::string error;
    ASSERT_TRUE(InputActionAssetDesc::parse(text, asset, error)) << error;
    const InputJson saved = InputJson::parse(asset.save());
    EXPECT_EQ(saved["author"], "tools");
    EXPECT_EQ(saved["trailer"], true);
    EXPECT_EQ(saved["maps"][0]["color"], "#ff0000");
    EXPECT_EQ(saved["maps"][0]["actions"][0]["icon"], "jump.png");
    EXPECT_EQ(saved["maps"][0]["actions"][0]["bindings"][0]["note"]["why"], "default");
    EXPECT_EQ(saved["maps"][0]["actions"][0]["bindings"][1]["extra"], InputJson::parse("[1,2]"));
    EXPECT_EQ(saved["maps"][0]["actions"][0]["bindings"][1]["parts"]["negative"]["x"], 1);
    // an edit keeps them too
    applyInputActionsOp(asset, InputJson::parse(R"({"op":"rename","action":"Gameplay/Jump","name":"Hop"})"));
    const InputJson edited = InputJson::parse(asset.save());
    EXPECT_EQ(edited["maps"][0]["actions"][0]["icon"], "jump.png");
    EXPECT_EQ(edited["maps"][0]["actions"][0]["name"], "Hop");
    // the same model writes the same bytes
    InputActionAssetDesc reparsed;
    ASSERT_TRUE(InputActionAssetDesc::parse(asset.save(), reparsed, error));
    EXPECT_EQ(reparsed.save(), asset.save());
}

TEST_F(InputActionsTest, ScriptsKeepTheirActionAcrossALiveEdit){
    load(GAMEPLAY);
    InputAction* jump = InputActions::get("Gameplay/Jump");
    int performed = 0;
    jump->onPerformed.add("test", std::function<void(InputActionEvent&)>([&](InputActionEvent&){ performed++; }));
    InputActions::enableMap("Menu", true);
    InputActionAssetDesc asset = InputActions::getAsset();
    applyInputActionsOp(asset, InputJson::parse(R"({"op":"add_binding","action":"Gameplay/Jump","path":"keyboard/j"})"));
    ASSERT_TRUE(InputActions::reloadText(asset.save()));
    EXPECT_EQ(InputActions::get("Gameplay/Jump"), jump);
    // the map kept its runtime state
    EXPECT_TRUE(InputActions::isMapEnabled("Menu"));
    InputActions::enableMap("Menu", false);
    key(D_KEY_J, true);
    frame();
    EXPECT_EQ(performed, 1);
}
