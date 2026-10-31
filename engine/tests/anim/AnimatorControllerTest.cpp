// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Animator Controllers (anim layers spec 14, test 13): the .controller format and validation
// (the GranjeroAnim port's dead end, unused parameters), transitions (fixed durations with
// numeric weights, triggers consumed, exit times, Any State self transitions, next_state
// interruption), 1D blend trees (weights and the synchronised normalized time), parameters
// (types, damping), callbacks (onStateEnter / onStateExit order, the Lua script method), layers
// and masks from the controller, and the Lua API.

#include <gtest/gtest.h>

#include "AnimTestEnvironment.h"

#include "Engine.h"
#include "Scene.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/AnimatorController.h"
#include "object/AnimationPlayer.h"
#include "pool/AnimationClipPool.h"
#include "pool/AnimatorControllerPool.h"
#include "script/LuaBinding.h"
#include "subsystem/ActionSystem.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <cmath>

using namespace jasperengine;

namespace {

    ClipKey key(float time, std::vector<float> value){
        ClipKey k;
        k.time = time;
        k.value = std::move(value);
        return k;
    }

    // Leg x goes 0 -> x over `length` seconds
    ClipData slide(float x, float length = 1.0f, ClipWrapMode wrap = ClipWrapMode::Loop){
        ClipData data;
        data.wrap = wrap;
        ClipTrackData track;
        track.path = "Leg";
        track.channel = ClipChannel::Position;
        track.keys = {key(0, {0, 0, 0}), key(length, {x, 0, 0})};
        data.tracks = {track};
        return data;
    }

    ClipData still(float x, float length = 1.0f, ClipWrapMode wrap = ClipWrapMode::Loop){
        ClipData data = slide(x, length, wrap);
        data.tracks[0].keys[0].value = {x, 0, 0};
        return data;
    }

    class Controllers : public ::testing::Test {
    protected:
        void SetUp() override {
            static int run = 0;
            prefix = "controllers/r" + std::to_string(run++) + "/";
            AnimationClipPool::clear();
            AnimatorControllerPool::clear();
            root = scene.createEntity();
            scene.setEntityName(root, "Granjero");
            scene.addComponent<Transform>(root, {});
            leg = scene.createEntity();
            scene.setEntityName(leg, "Leg");
            scene.addComponent<Transform>(leg, {});
            scene.addEntityChild(root, leg, false);
            animtest::takeWarnings();
        }
        void TearDown() override {
            AnimationClipPool::clear();
            AnimatorControllerPool::clear();
        }

        std::string clip(const std::string& name, const ClipData& data){
            animtest::writeAsset(prefix + name + ".anim", anim::writeClip(data));
            return prefix + name + ".anim";
        }

        // `text` with every "@" replaced by the test's prefix
        std::string controller(const std::string& name, std::string text){
            for (size_t at = text.find('@'); at != std::string::npos; at = text.find('@', at + prefix.size())) text.replace(at, 1, prefix);
            animtest::writeAsset(prefix + name + ".controller", text);
            return prefix + name + ".controller";
        }

        AnimationPlayer player(const std::string& controllerPath){
            AnimationPlayer object(&scene, root);
            object.setController(controllerPath);
            return object;
        }

        void step(double dt, int count = 1){
            for (int i = 0; i < count; i++) scene.getSystem<ActionSystem>()->update(dt);
        }

        float weight(AnimationPlayer& object, const std::string& state){
            return object.getStateInfo(state).weight;
        }

        float legX(){ return scene.getComponent<Transform>(leg).position.x; }

        Scene scene;
        std::string prefix;
        Entity root = NULL_ENTITY;
        Entity leg = NULL_ENTITY;
    };

    const char* GRANJERO = R"({"format": "jasperengine.controller", "version": 1,
      "parameters": [{"name": "Speed", "type": "float", "default": 0}, {"name": "Watering", "type": "bool"},
                     {"name": "Unused", "type": "int", "default": 3}],
      "layers": [{"name": "Base Layer", "default_state": "Idle",
        "states": [{"name": "Idle", "clip": "@idle.anim"}, {"name": "Running", "clip": "@running.anim"},
                   {"name": "Watering", "clip": "@watering.anim"}],
        "transitions": [
          {"from": "Idle", "to": "Running", "conditions": [{"parameter": "Speed", "mode": "greater", "threshold": 0.01}],
           "has_exit_time": false, "duration": 0.15, "fixed_duration": true},
          {"from": "Running", "to": "Idle", "conditions": [{"parameter": "Speed", "mode": "less", "threshold": 0.01}],
           "has_exit_time": false, "duration": 0.15},
          {"from": "Idle", "to": "Watering", "conditions": [{"parameter": "Watering", "mode": "if"}], "has_exit_time": false}]}]})";

}

TEST(ControllerIO, CanonicalRoundTripAndLeniency){
    const std::string text = R"({"format": "jasperengine.controller", "version": 1, "custom": [1, 2],
      "parameters": [{"name": "Speed", "type": "float", "default": 1.5}, {"name": "Speed", "type": "int"},
                     {"name": "Jump", "type": "trigger"}, {"name": "N", "type": "int", "default": 2.6}, {"name": "X", "type": "vector"}],
      "layers": [{"name": "Base Layer", "states": [
          {"name": "Move", "blend_tree": {"parameter": "Speed", "children": [{"clip": "run.anim", "threshold": 4}, {"clip": "walk.anim", "threshold": 0}]},
           "position": [10, 20], "foot_ik": true, "future": {"a": 1}},
          {"name": "any", "clip": "x.anim"}, {"name": "Jump", "clip": "jump.anim", "wrap": "once", "speed": 2}],
        "any_state": {"position": [1, 2]},
        "transitions": [{"from": "any", "to": "Jump", "conditions": [{"parameter": "Jump", "mode": "if"}], "can_transition_to_self": false},
                        {"from": "Jump", "to": "Move", "exit_time": 0.9, "interruption": "next_state"},
                        {"from": "Nowhere", "to": "Move"}]},
        {"name": "Upper", "weight": 0.5, "blend": "additive", "mask": "arm.mask", "states": []}]})";
    anim::ControllerParseResult parsed = anim::parseController(text);
    ASSERT_TRUE(parsed.ok()) << parsed.error.toString();
    const anim::ControllerData& data = *parsed.data;
    ASSERT_EQ(data.parameters.size(), 3u) << "the duplicate and the unknown type are dropped";
    EXPECT_EQ(data.parameters[2].defaultValue, 3.0f) << "an int default is rounded";
    ASSERT_EQ(data.layers.size(), 2u);
    const anim::ControllerLayer& base = data.layers[0];
    ASSERT_EQ(base.states.size(), 2u) << "\"any\" is reserved";
    EXPECT_EQ(base.states[0].blendTree.children[0].clip, "walk.anim") << "children sorted by threshold";
    EXPECT_TRUE(base.states[0].footIK);
    ASSERT_EQ(base.transitions.size(), 2u) << "a transition from an unknown state is dropped";
    EXPECT_FALSE(base.transitions[0].canTransitionToSelf);
    EXPECT_EQ(base.transitions[1].interruption, anim::TransitionInterruption::NextState);
    EXPECT_EQ(data.layers[1].blend, AnimationLayerBlend::Additive);
    bool warnedPath = false;
    for (const ClipWarning& warning : parsed.warnings) warnedPath |= warning.path == "layers[0].transitions[2].from";
    EXPECT_TRUE(warnedPath);

    const std::string canonical = anim::writeController(data);
    anim::ControllerParseResult again = anim::parseController(canonical);
    ASSERT_TRUE(again.ok()) << again.error.toString() << "\n" << canonical;
    EXPECT_EQ(anim::writeController(*again.data), canonical) << "canonical round trip";
    EXPECT_NE(canonical.find("\"future\": {\"a\":1}"), std::string::npos) << canonical;
    EXPECT_NE(canonical.find("\"custom\": [1,2]"), std::string::npos);

    EXPECT_EQ(anim::parseController("%YAML 1.1\n--- !u!91 &9100000\nAnimatorController:\n").error.message, anim::unityYamlControllerRefusal());
    EXPECT_FALSE(anim::parseController("{\"format\": \"jasperengine.controller\", \"version\": 9}").ok());
    anim::ControllerParseResult empty = anim::parseController("{\"format\": \"jasperengine.controller\", \"version\": 1}");
    ASSERT_TRUE(empty.ok());
    ASSERT_EQ(empty.data->layers.size(), 1u);
    EXPECT_EQ(empty.data->layers[0].name, anim::BASE_LAYER_NAME);
}

TEST(ControllerValidation, ReportsTheGranjeroPortsBugs){
    std::string text = GRANJERO;
    for (size_t at = text.find('@'); at != std::string::npos; at = text.find('@')) text.erase(at, 1);
    anim::ControllerParseResult parsed = anim::parseController(text);
    ASSERT_TRUE(parsed.ok());
    std::vector<ClipWarning> warnings = anim::validateController(*parsed.data, [](const std::string& clip){
        anim::ControllerClipInfo info;
        info.loads = clip != "missing.anim";
        info.loops = clip != "watering.anim";
        return info;
    });
    auto find = [&](const std::string& path, const std::string& text){
        for (const ClipWarning& warning : warnings){
            if (warning.path == path && warning.message.find(text) != std::string::npos) return true;
        }
        return false;
    };
    EXPECT_TRUE(find("layers[0].states[2]", "dead end")) << "Watering: no transition leaves it, its clip does not loop";
    EXPECT_TRUE(find("parameters[2]", "unused"));
    EXPECT_FALSE(find("parameters[0]", "unused"));

    anim::ControllerData data = *parsed.data;
    data.layers[0].transitions[0].conditions[0].mode = anim::ConditionMode::Equals;
    data.layers[0].transitions[2].conditions[0].parameter = "Nope";
    data.layers[0].transitions.push_back(anim::ControllerTransition());
    data.layers[0].transitions.back().from = "Running";
    data.layers[0].transitions.back().to = "Idle";
    data.layers[0].transitions.back().hasExitTime = false;
    anim::ControllerState lonely;
    lonely.name = "Lonely";
    lonely.clip = "missing.anim";
    data.layers[0].states.push_back(lonely);
    warnings = anim::validateController(data, [](const std::string& clip){
        anim::ControllerClipInfo info;
        info.loads = clip != "missing.anim";
        info.loops = true;
        info.error = "cannot read";
        return info;
    });
    EXPECT_TRUE(find("layers[0].transitions[0].conditions[0].mode", "does not fit the float parameter"));
    EXPECT_TRUE(find("layers[0].transitions[2].conditions[0].parameter", "no parameter named \"Nope\""));
    EXPECT_TRUE(find("layers[0].transitions[3]", "fires at once"));
    EXPECT_TRUE(find("layers[0].states[3]", "unreachable"));
    EXPECT_TRUE(find("layers[0].states[3].clip", "does not load"));
}

TEST_F(Controllers, FixedDurationTransitionsBlendLinearly){
    clip("idle", still(0));
    clip("running", still(10));
    clip("watering", still(5, 1.0f, ClipWrapMode::Once));
    AnimationPlayer object = player(controller("granjero", GRANJERO));
    std::vector<std::string> log;
    object.getOnStateEnter().add("test", std::function<void(int, const std::string&)>([&](int layer, const std::string& name){
        log.push_back("enter " + std::to_string(layer) + " " + name);
    }));
    object.getOnStateExit().add("test", std::function<void(int, const std::string&)>([&](int layer, const std::string& name){
        log.push_back("exit " + std::to_string(layer) + " " + name);
    }));
    step(0.1);
    EXPECT_EQ(object.getCurrentStateName(), "Idle");
    EXPECT_NEAR(legX(), 0.0f, 1e-5f);
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0], "enter 0 Idle");

    object.setFloat("Speed", 0.02f);
    step(0.05);     // the transition starts this update (x = 0)
    EXPECT_TRUE(object.isInTransition());
    EXPECT_EQ(object.getNextStateInfo().name, "Running");
    EXPECT_EQ(object.getCurrentStateInfo().fullName, "Base Layer.Idle");
    step(0.075);
    EXPECT_NEAR(weight(object, "Base Layer.Idle"), 0.5f, 1e-5f);
    EXPECT_NEAR(weight(object, "Base Layer.Running"), 0.5f, 1e-5f);
    EXPECT_NEAR(legX(), 5.0f, 1e-4f);
    step(0.075);
    EXPECT_FALSE(object.isInTransition()) << "exactly 0.15 s";
    EXPECT_EQ(object.getCurrentStateName(), "Running");
    EXPECT_NEAR(legX(), 10.0f, 1e-5f);
    EXPECT_EQ(weight(object, "Base Layer.Idle"), 0.0f);
    ASSERT_EQ(log.size(), 3u);
    EXPECT_EQ(log[1], "exit 0 Idle");
    EXPECT_EQ(log[2], "enter 0 Running");

    // the GranjeroAnim port's other bug: VictoryDance is a trigger set as a bool -> warned once
    animtest::takeWarnings();
    object.setBool("Speed", true);
    std::vector<std::string> warnings = animtest::takeWarnings();
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("'Speed' is a float, set as a bool"), std::string::npos) << warnings[0];
    EXPECT_EQ(object.getFloat("Speed"), 1.0f);
    object.setBool("Speed", false);
    EXPECT_TRUE(animtest::takeWarnings().empty()) << "once per parameter and type";
    object.getOnStateEnter().clear();
    object.getOnStateExit().clear();
}

TEST_F(Controllers, TriggersExitTimesAndAnyState){
    clip("a", still(1));
    clip("b", still(2));
    clip("c", still(3));
    AnimationPlayer object = player(controller("triggers", R"({"format": "jasperengine.controller", "version": 1,
      "parameters": [{"name": "Go", "type": "trigger"}, {"name": "Hit", "type": "trigger"}],
      "layers": [{"name": "Base Layer", "states": [{"name": "A", "clip": "@a.anim"}, {"name": "B", "clip": "@b.anim"}, {"name": "C", "clip": "@c.anim"}],
        "transitions": [
          {"from": "A", "to": "B", "conditions": [{"parameter": "Go", "mode": "if"}], "has_exit_time": false, "duration": 0},
          {"from": "B", "to": "A", "exit_time": 0.9, "duration": 0},
          {"from": "any", "to": "C", "conditions": [{"parameter": "Hit", "mode": "if"}], "has_exit_time": false, "duration": 0, "can_transition_to_self": false}]}]})"));
    step(0.1);
    EXPECT_EQ(object.getCurrentStateName(), "A");
    object.setTrigger("Go");
    EXPECT_TRUE(object.getBool("Go"));
    step(0.1);
    EXPECT_EQ(object.getCurrentStateName(), "B") << "a zero duration switches at once";
    EXPECT_FALSE(object.getBool("Go")) << "the transition consumed its trigger";

    // exit time 0.9 of a 1 s loop: not before 0.9, at the step that crosses it
    float elapsed = 0.0f;
    while (object.getCurrentStateName() == "B" && elapsed < 2.0f){
        step(0.05);
        elapsed += 0.05f;
    }
    EXPECT_NEAR(elapsed, 0.9f, 0.05f + 1e-4f);
    EXPECT_GE(elapsed, 0.9f - 1e-4f);
    EXPECT_EQ(object.getCurrentStateName(), "A");

    object.setTrigger("Hit");
    step(0.1);
    EXPECT_EQ(object.getCurrentStateName(), "C");
    EXPECT_NEAR(object.getCurrentStateInfo().normalizedTime, 0.0f, 1e-5f);
    step(0.3);
    object.setTrigger("Hit");
    step(0.1);
    EXPECT_EQ(object.getCurrentStateName(), "C");
    EXPECT_NEAR(object.getCurrentStateInfo().normalizedTime, 0.4f, 1e-4f) << "can_transition_to_self false: no restart";
    EXPECT_TRUE(object.getBool("Hit")) << "not consumed: no transition fired";
    object.resetTrigger("Hit");
    EXPECT_FALSE(object.getBool("Hit"));
}

TEST_F(Controllers, NextStateInterruption){
    clip("a", still(1));
    clip("b", still(2));
    clip("c", still(3));
    AnimationPlayer object = player(controller("interrupt", R"({"format": "jasperengine.controller", "version": 1,
      "parameters": [{"name": "Go", "type": "bool"}, {"name": "Skip", "type": "bool"}],
      "layers": [{"name": "Base Layer", "states": [{"name": "A", "clip": "@a.anim"}, {"name": "B", "clip": "@b.anim"}, {"name": "C", "clip": "@c.anim"}],
        "transitions": [
          {"from": "A", "to": "B", "conditions": [{"parameter": "Go", "mode": "if"}], "has_exit_time": false, "duration": 1, "interruption": "next_state"},
          {"from": "B", "to": "C", "conditions": [{"parameter": "Skip", "mode": "if"}], "has_exit_time": false, "duration": 1}]}]})"));
    step(0.1);
    object.setBool("Go", true);
    step(0.1);
    step(0.5);
    EXPECT_NEAR(weight(object, "Base Layer.B"), 0.5f, 1e-5f);
    object.setBool("Skip", true);
    step(0.1);      // B's transition takes over: A 0.5 and B 0.5 become its sources
    EXPECT_EQ(object.getNextStateInfo().name, "C");
    EXPECT_EQ(object.getCurrentStateName(), "B");
    step(0.5);
    EXPECT_NEAR(weight(object, "Base Layer.C"), 0.5f, 1e-5f);
    EXPECT_NEAR(weight(object, "Base Layer.A"), 0.25f, 1e-5f);
    EXPECT_NEAR(weight(object, "Base Layer.B"), 0.25f, 1e-5f);
    EXPECT_NEAR(legX(), 0.5f * 3 + 0.25f * 1 + 0.25f * 2, 1e-4f);
    step(0.5);
    EXPECT_EQ(object.getCurrentStateName(), "C");
    EXPECT_FALSE(object.isInTransition());
}

TEST_F(Controllers, BlendTreesWeightChildrenAndSyncTheirTime){
    clip("walk", slide(2, 1.0f));
    clip("run", slide(4, 0.5f));
    AnimationPlayer object = player(controller("tree", R"({"format": "jasperengine.controller", "version": 1,
      "parameters": [{"name": "Speed", "type": "float", "default": 1}],
      "layers": [{"name": "Base Layer", "states": [{"name": "Move", "blend_tree": {"parameter": "Speed", "children": [
          {"clip": "@walk.anim", "threshold": 0}, {"clip": "@run.anim", "threshold": 4}]}}]}]})"));
    step(0.0);
    step(0.2);
    EXPECT_NEAR(weight(object, "Base Layer.Move#0"), 0.75f, 1e-5f);
    EXPECT_NEAR(weight(object, "Base Layer.Move#1"), 0.25f, 1e-5f);
    // dτ/dt = 0.75 / 1 + 0.25 / 0.5 = 1.25
    EXPECT_NEAR(object.getCurrentStateInfo().normalizedTime, 0.25f, 1e-5f);
    EXPECT_NEAR(object.getCurrentStateInfo().length, 0.8f, 1e-5f);
    EXPECT_NEAR(object.getStateInfo("Base Layer.Move#0").time, 0.25f, 1e-5f);
    EXPECT_NEAR(object.getStateInfo("Base Layer.Move#1").time, 0.125f, 1e-5f);
    EXPECT_NEAR(legX(), 0.75f * 0.5f + 0.25f * 1.0f, 1e-4f);
    object.setFloat("Speed", 9.0f);
    step(0.0);
    EXPECT_EQ(weight(object, "Base Layer.Move#1"), 1.0f) << "clamped to the end child";
    // damping: SmoothDamp toward the value, once per update
    object.setFloat("Speed", 0.0f, 0.1f);
    step(0.05);
    const float damped = object.getFloat("Speed");
    EXPECT_GT(damped, 0.0f);
    EXPECT_LT(damped, 9.0f);
    step(0.05, 40);
    EXPECT_NEAR(object.getFloat("Speed"), 0.0f, 1e-3f);
    object.setFloat("Speed", 2.0f);
    EXPECT_EQ(object.getFloat("Speed"), 2.0f) << "a set without damping snaps";
}

TEST_F(Controllers, EventsFireFromTheHeaviestChildOnly){
    ClipData walk = slide(2, 1.0f);
    walk.events = {ClipEventData{0.5f, "onStep", {AnimationEvent::stringArg("walk")}}};
    ClipData run = slide(4, 1.0f);
    run.events = {ClipEventData{0.5f, "onStep", {AnimationEvent::stringArg("run")}}};
    clip("walk", walk);
    clip("run", run);
    AnimationPlayer object = player(controller("events", R"({"format": "jasperengine.controller", "version": 1,
      "parameters": [{"name": "Speed", "type": "float", "default": 1}],
      "layers": [{"name": "Base Layer", "states": [{"name": "Move", "blend_tree": {"parameter": "Speed", "children": [
          {"clip": "@walk.anim", "threshold": 0}, {"clip": "@run.anim", "threshold": 4}]}}]}]})"));
    std::vector<std::string> fired;
    object.getOnEvent().add("test", std::function<void(const AnimationEvent&)>([&](const AnimationEvent& e){ fired.push_back(e.getStringArg(0)); }));
    step(0.0);
    step(0.25, 4);
    ASSERT_EQ(fired.size(), 1u) << "no double footsteps";
    EXPECT_EQ(fired[0], "walk");
    object.getOnEvent().clear();
}

TEST_F(Controllers, LayersAndPlayFromScript){
    clip("idle", still(1));
    clip("wave", still(7));
    AnimationPlayer object = player(controller("layers", R"({"format": "jasperengine.controller", "version": 1,
      "layers": [{"name": "Base Layer", "states": [{"name": "Idle", "clip": "@idle.anim"}, {"name": "Other", "clip": "@idle.anim"}]},
                 {"name": "Holding", "weight": 0.5, "states": [{"name": "Hold", "clip": "@wave.anim"}]}]})"));
    step(0.1);
    EXPECT_EQ(object.getLayerCount(), 2);
    EXPECT_EQ(object.getLayerName(1), "Holding");
    EXPECT_EQ(object.getCurrentStateName(1), "Hold");
    EXPECT_NEAR(legX(), 4.0f, 1e-4f) << "lerp(1, 7, 0.5)";
    object.setLayerWeight("Holding", 1.0f);
    step(0.1);
    EXPECT_NEAR(legX(), 7.0f, 1e-4f);

    object.play("Other");
    EXPECT_TRUE(object.isPlaying("Other"));
    EXPECT_TRUE(object.isPlaying("Base Layer.Other"));
    object.play("Base Layer.Idle", 0, 0.5f);
    step(0.0);
    EXPECT_EQ(object.getCurrentStateName(), "Idle");
    EXPECT_NEAR(object.getCurrentStateInfo().normalizedTime, 0.5f, 1e-5f);
    object.crossFade("Other", 0.2f);
    step(0.1);
    EXPECT_TRUE(object.isInTransition());
    EXPECT_NEAR(weight(object, "Base Layer.Other"), 0.5f, 1e-5f);

    animtest::takeWarnings();
    object.queue("Idle");
    object.queue("Idle");
    EXPECT_EQ(animtest::takeWarnings().size(), 1u) << "queue is not available with a controller (once)";

    object.stop();
    EXPECT_FALSE(object.isPlaying());
    step(0.1);
    EXPECT_FALSE(object.isPlaying()) << "nothing plays until play() or an enable";
    object.setEnabled(false);
    step(0.1);
    object.setEnabled(true);
    step(0.1);
    EXPECT_EQ(object.getCurrentStateName(), "Idle") << "re-enabled: the default states again";
}

TEST_F(Controllers, LuaApiAndScriptMethods){
    clip("idle", still(0));
    clip("running", still(10));
    clip("watering", still(5, 1.0f, ClipWrapMode::Once));
    AnimationPlayer object = player(controller("granjero", GRANJERO));
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_TRUE(L != nullptr);
    ASSERT_EQ(luaL_dostring(L,
        "ctrlLog = {}\n"
        "CtrlScript = {}\n"
        "function CtrlScript:onStateEnter(layer, name) ctrlLog[#ctrlLog + 1] = 'enter:' .. layer .. ':' .. name end\n"
        "function CtrlScript:onStateExit(layer, name) ctrlLog[#ctrlLog + 1] = 'exit:' .. layer .. ':' .. name end\n"
        "ctrlInstance = setmetatable({}, {__index = CtrlScript})\n"), LUA_OK);
    lua_getglobal(L, "ctrlInstance");
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ScriptComponent scripts;
    ScriptEntry entry;
    entry.type = ScriptType::LUA;
    entry.enabled = true;
    entry.className = "CtrlScript";
    entry.instance = reinterpret_cast<void*>(static_cast<intptr_t>(ref));
    scripts.scripts.push_back(entry);
    scene.addComponent<ScriptComponent>(root, scripts);

    luabridge::setGlobal(L, object, "ctrlPlayer");
    step(0.1);
    ASSERT_EQ(luaL_dostring(L,
        "local p = ctrlPlayer\n"
        "p:setFloat('Speed', 2)\n"
        "ctrlResult = tostring(p:hasParameter('Speed')) .. '|' .. p:getParameterType('Watering') .. '|' .. tostring(p:getFloat('Speed')) .. '|' ..\n"
        "  p:getCurrentStateName() .. '|' .. tostring(p:getCurrentStateInfo(0).isValid) .. '|' .. tostring(p:isInTransition())"), LUA_OK)
        << lua_tostring(L, -1);
    step(0.1);
    step(0.2);
    ASSERT_EQ(luaL_dostring(L,
        "local p = ctrlPlayer\n"
        "ctrlResult = ctrlResult .. '|' .. p:getCurrentStateName() .. '|' .. p:getCurrentStateInfo().fullName .. '|' .. table.concat(ctrlLog, ',')"), LUA_OK)
        << lua_tostring(L, -1);
    lua_getglobal(L, "ctrlResult");
    const std::string result = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    EXPECT_EQ(result, "true|bool|2.0|Idle|true|false|Running|Base Layer.Running|enter:0:Idle,exit:0:Idle,enter:0:Running");
    luabridge::setGlobal(L, luabridge::LuaRef(L), "ctrlPlayer");
    scene.getComponent<ScriptComponent>(root).scripts.clear();
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

TEST_F(Controllers, MissingClipsAreEmptyStatesAndControllersReload){
    clip("idle", still(1));
    const std::string path = controller("reload", R"({"format": "jasperengine.controller", "version": 1,
      "layers": [{"name": "Base Layer", "states": [{"name": "Gone", "clip": "@missing.anim"}, {"name": "Idle", "clip": "@idle.anim"}]}]})");
    AnimationPlayer object = player(path);
    AnimationPlayerComponent& component = scene.getComponent<AnimationPlayerComponent>(root);
    AnimationPlayerClip legacy;
    legacy.clip = clip("legacy", still(9));
    component.clips.push_back(legacy);
    step(0.1);
    EXPECT_EQ(object.getCurrentStateName(), "Gone");
    EXPECT_NEAR(legX(), 0.0f, 1e-5f) << "an empty state writes nothing";
    bool ignored = false;
    for (const std::string& warning : animtest::takeWarnings()) ignored |= warning.find("clip entry and layers are ignored") != std::string::npos;
    EXPECT_TRUE(ignored);

    // an edited controller reloads: the layer keeps its current state by name
    controller("reload", R"({"format": "jasperengine.controller", "version": 1,
      "layers": [{"name": "Base Layer", "default_state": "Idle", "states": [{"name": "Idle", "clip": "@idle.anim"}]}]})");
    AnimatorControllerPool::invalidate(path);
    step(0.1);
    EXPECT_EQ(object.getCurrentStateName(), "Idle");
    EXPECT_NEAR(legX(), 1.0f, 1e-5f);
}
