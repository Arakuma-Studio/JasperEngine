// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Upstream's Animator Controllers (anim layers spec 14, test 13) on S21 (docs/specs/S21-animation.md
// "Upstream absorption"): upstream's version 1 .controller files, read converted, played by an
// Animator that upstream's Animation Player API hands over to (AnimationPlayer::setController, the
// scene migration's shape). The format and validation (the GranjeroAnim port's dead end, unused
// parameters), transitions (fixed durations with numeric weights, triggers consumed, exit times,
// Any State self transitions, next state interruption), 1D blend trees, parameters (types, damping),
// callbacks (onStateEnter / onStateExit, the Lua script methods), layers, and the Lua API.
// Where S21's runtime keeps Unity's rule and upstream's did not, the test says so: a destination's
// onStateEnter comes when its transition starts (the source's onStateExit when it ends), an
// interrupted transition freezes the pose it showed, a blend tree lasts the weighted average of
// its children's lengths, a state's normalizedTime leaves out its cycle offset, a controller
// without layers stays without, and parameter names that are not Lua identifiers are a
// validation warning (not dropped).

#include <gtest/gtest.h>

#include "AnimTestEnvironment.h"

#include "Engine.h"
#include "Scene.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/AnimatorInstance.h"
#include "animation/CompiledController.h"
#include "animation/UpstreamAnimFormats.h"
#include "component/AnimatorComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/ScriptComponent.h"
#include "object/AnimationPlayer.h"
#include "object/Animator.h"
#include "object/Model.h"
#include "pool/AnimationClipPool.h"
#include "pool/AnimatorAssetPool.h"
#include "script/LuaBinding.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/AnimatorSystem.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <algorithm>
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
            AnimatorAssetPool::clear();
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
            AnimatorAssetPool::clear();
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

        // upstream's Animation Player with a controller: it hands over to the GameObject's Animator
        AnimationPlayer player(const std::string& controllerPath){
            AnimationPlayer object(&scene, root);
            object.setController(controllerPath);
            return object;
        }

        void step(double dt, int count = 1){
            for (int i = 0; i < count; i++){
                scene.getSystem<ActionSystem>()->update(dt);
                scene.getSystem<AnimatorSystem>()->update(dt);
            }
        }

        anim::AnimatorInstance& instance(){ return *scene.getComponent<AnimatorComponent>(root).runtime; }

        // a state's weight in its layer ("Layer.State" or "State"): the current state's 1 - the
        // transition's progress, the destination's the progress (both when it re-enters itself)
        float weight(const std::string& state){
            anim::AnimatorInstance& a = instance();
            float total = 0.0f;
            for (int l = 0; l < (int)a.layers.size(); l++){
                const float progress = a.transitionProgress(l);
                const anim::AnimatorInstance::StateInfo current = a.currentState(l);
                const anim::AnimatorInstance::StateInfo next = a.nextState(l);
                const bool snapshot = a.layers[(size_t)l].snapshot;
                if (current.state >= 0 && !snapshot && (current.name == state || current.fullPath == state)){
                    total += a.isInTransition(l) ? 1.0f - progress : 1.0f;
                }
                if (next.state >= 0 && (next.name == state || next.fullPath == state)) total += progress;
            }
            return total;
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

    bool hasWarning(const std::vector<ClipWarning>& warnings, const std::string& path, const std::string& text){
        for (const ClipWarning& warning : warnings){
            if (warning.path == path && warning.message.find(text) != std::string::npos) return true;
        }
        return false;
    }

}

TEST(ControllerIO, UpstreamControllerConvertsCanonicalAndLenient){
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
    ASSERT_TRUE(parsed.ok) << parsed.error.toString();
    EXPECT_EQ(parsed.dialect, anim::AnimatorFileDialect::UpstreamVersion1);
    const anim::ControllerDef& data = parsed.data;
    ASSERT_EQ(data.parameters.size(), 3u) << "the duplicate and the unknown type are dropped";
    EXPECT_EQ(data.parameters[2].defaultValue, 3.0f) << "an int default is rounded";
    ASSERT_EQ(data.layers.size(), 2u);
    const anim::StateMachineDef& base = data.layers[0].machine;
    ASSERT_EQ(base.states.size(), 2u) << "\"any\" is reserved";
    ASSERT_TRUE(base.states[0].motion.isTree());
    EXPECT_EQ(base.states[0].motion.tree[0].children[0].motion.clip.path, "walk.anim") << "children sorted by threshold";
    EXPECT_EQ(base.states[0].position, Vector2(10, 20));
    bool footIK = false;
    for (const auto& kept : base.states[0].unknownKeys) footIK = footIK || (kept.first == "foot_ik" && kept.second == "true");
    EXPECT_TRUE(footIK) << "upstream's pass-through flags are kept";
    EXPECT_EQ(base.states[1].loop, AnimatorStateLoop::Once);
    EXPECT_EQ(base.states[1].speed, 2.0f);
    EXPECT_EQ(base.anyStatePosition, Vector2(1, 2));
    ASSERT_EQ(base.anyState.size(), 1u);
    EXPECT_FALSE(base.anyState[0].canTransitionToSelf);
    EXPECT_TRUE(base.anyState[0].hasExitTime) << "upstream's default made explicit";
    ASSERT_EQ(base.states[1].transitions.size(), 1u) << "the transition from an unknown state is dropped";
    EXPECT_EQ(base.states[1].transitions[0].interruptionSource, TransitionInterruptionSource::Next);
    EXPECT_FLOAT_EQ(base.states[1].transitions[0].exitTime, 0.9f);
    EXPECT_EQ(data.layers[1].blending, AnimatorLayerBlending::Additive);
    EXPECT_EQ(data.layers[1].mask.path, "arm.mask");
    EXPECT_FLOAT_EQ(data.layers[1].weight, 0.5f);
    EXPECT_TRUE(hasWarning(parsed.warnings, "layers[0].transitions[2].from", ""));

    // saved as version 2: canonical, every upstream member kept
    const std::string canonical = anim::writeController(data);
    EXPECT_NE(canonical.find("\"version\": 2"), std::string::npos);
    anim::ControllerParseResult again = anim::parseController(canonical);
    ASSERT_TRUE(again.ok) << again.error.toString() << "\n" << canonical;
    EXPECT_EQ(again.dialect, anim::AnimatorFileDialect::Current);
    EXPECT_EQ(anim::writeController(again.data), canonical) << "canonical round trip";
    EXPECT_NE(canonical.find("\"future\": {\"a\":1}"), std::string::npos) << canonical;
    EXPECT_NE(canonical.find("\"custom\": [1,2]"), std::string::npos);

    EXPECT_EQ(anim::parseController("%YAML 1.1\n--- !u!91 &9100000\nAnimatorController:\n").error.message, anim::unityYamlControllerRefusal());
    EXPECT_FALSE(anim::parseController("{\"format\": \"jasperengine.controller\", \"version\": 9}").ok);
    anim::ControllerParseResult empty = anim::parseController("{\"format\": \"jasperengine.controller\", \"version\": 1}");
    ASSERT_TRUE(empty.ok);
    EXPECT_TRUE(empty.data.layers.empty()) << "S21: no layers stays no layers (upstream added an empty Base Layer: both play nothing)";
}

TEST(ControllerValidation, ReportsTheGranjeroPortsBugs){
    std::string text = GRANJERO;
    for (size_t at = text.find('@'); at != std::string::npos; at = text.find('@')) text.erase(at, 1);
    anim::ControllerParseResult parsed = anim::parseController(text);
    ASSERT_TRUE(parsed.ok);
    std::vector<ClipWarning> warnings = anim::validateController(parsed.data, [](const std::string& clip){
        anim::ControllerClipInfo info;
        info.loads = clip != "missing.anim";
        info.loops = clip != "watering.anim";
        return info;
    });
    EXPECT_TRUE(hasWarning(warnings, "layers[0].state_machine.states[2]", "dead end")) << "Watering: no way out, its clip does not loop";
    EXPECT_TRUE(hasWarning(warnings, "parameters[2]", "unused"));
    EXPECT_FALSE(hasWarning(warnings, "parameters[0]", "unused"));

    anim::ControllerDef data = parsed.data;
    anim::StateMachineDef& machine = data.layers[0].machine;
    machine.states[0].transitions[0].conditions[0].mode = AnimatorConditionMode::Equals;
    machine.states[0].transitions[1].conditions[0].parameter = "Nope";
    anim::TransitionDef never;
    never.to = "Idle";
    never.hasExitTime = false;
    machine.states[1].transitions.push_back(never);
    anim::StateDef lonely;
    lonely.name = "Lonely";
    lonely.motion.clip.path = "missing.anim";
    machine.states.push_back(lonely);
    warnings = anim::validateController(data, [](const std::string& clip){
        anim::ControllerClipInfo info;
        info.loads = clip != "missing.anim";
        info.loops = true;
        info.error = "cannot read";
        return info;
    });
    EXPECT_TRUE(hasWarning(warnings, "layers[0].state_machine.states[0].transitions[0].conditions[0].mode", "does not fit the float parameter"));
    bool unknown = false;
    for (const ClipWarning& warning : warnings) unknown = unknown || warning.message.find("\"Nope\"") != std::string::npos;
    EXPECT_TRUE(unknown) << "an unknown parameter";
    EXPECT_TRUE(hasWarning(warnings, "layers[0].state_machine.states[1].transitions[1]", "never taken"));
    bool unreachable = false;
    for (const ClipWarning& warning : warnings) unreachable = unreachable || warning.message.find("\"Base Layer.Lonely\" is unreachable") != std::string::npos;
    EXPECT_TRUE(unreachable);
    EXPECT_TRUE(hasWarning(warnings, "layers[0].state_machine.states[3].motion.clip", "does not load"));
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
    EXPECT_NEAR(weight("Base Layer.Idle"), 0.5f, 1e-5f);
    EXPECT_NEAR(weight("Base Layer.Running"), 0.5f, 1e-5f);
    EXPECT_NEAR(legX(), 5.0f, 1e-4f);
    step(0.075);
    EXPECT_FALSE(object.isInTransition()) << "exactly 0.15 s";
    EXPECT_EQ(object.getCurrentStateName(), "Running");
    EXPECT_NEAR(legX(), 10.0f, 1e-5f);
    EXPECT_EQ(weight("Base Layer.Idle"), 0.0f);
    // Unity's (and S21's) order: the destination enters when the transition starts, the source
    // exits when it ends (upstream gave both at the end, exit first)
    ASSERT_EQ(log.size(), 3u);
    EXPECT_EQ(log[1], "enter 0 Running");
    EXPECT_EQ(log[2], "exit 0 Idle");

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
    EXPECT_NEAR(weight("Base Layer.B"), 0.5f, 1e-5f);
    object.setBool("Skip", true);
    step(0.1);      // B's transition takes over from what the layer showed (A 0.5, B 0.5: x = 1.5)
    EXPECT_EQ(object.getNextStateInfo().name, "C");
    EXPECT_EQ(object.getCurrentStateName(), "B");
    step(0.5);
    EXPECT_NEAR(weight("Base Layer.C"), 0.5f, 1e-5f);
    // S21 (Unity) freezes the interrupted blend as one pose: lerp(1.5, 3, 0.5); upstream faded A
    // and B on at 0.25 each, the same number for still clips
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
    // weights 0.75 / 0.25; S21 (R6): the tree lasts the weighted average of its children's
    // lengths, 0.75 x 1 + 0.25 x 0.5 = 0.875 s (upstream averaged their rates: 0.8 s)
    EXPECT_NEAR(object.getCurrentStateInfo().length, 0.875f, 1e-5f);
    const float normalized = 0.2f / 0.875f;
    EXPECT_NEAR(object.getCurrentStateInfo().normalizedTime, normalized, 1e-5f);
    // the children at the same normalized time: walk at 0.2286 s of 1, run at 0.1143 s of 0.5
    EXPECT_NEAR(legX(), 0.75f * 2.0f * normalized + 0.25f * 4.0f * normalized, 1e-4f);
    object.setFloat("Speed", 9.0f);
    step(0.0);
    EXPECT_NEAR(legX(), 4.0f * std::fmod(normalized, 1.0f), 1e-4f) << "clamped to the end child";
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
    step(0.05);
    EXPECT_EQ(object.getFloat("Speed"), 2.0f) << "and stays: the damping ended";
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
    // a weight fade in the Animator's time (upstream's player fades)
    object.setLayerWeight("Holding", 0.0f, 0.4f);
    step(0.1);
    EXPECT_NEAR(object.getLayerWeight("Holding"), 0.75f, 1e-5f);
    object.setLayerWeight("Holding", 1.0f);

    object.play("Other");
    EXPECT_TRUE(object.isPlaying("Other"));
    EXPECT_TRUE(object.isPlaying("Base Layer.Other"));
    object.playInLayer("Base Layer.Idle", 0, 0.5f);
    step(0.0);
    EXPECT_EQ(object.getCurrentStateName(), "Idle");
    EXPECT_NEAR(object.getCurrentStateInfo().normalizedTime, 0.5f, 1e-5f);
    object.crossFade("Other", 0.2f);
    step(0.1);
    EXPECT_TRUE(object.isInTransition());
    EXPECT_NEAR(weight("Base Layer.Other"), 0.5f, 1e-5f);

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
    // (the destination enters as its transition starts: Unity's order)
    EXPECT_EQ(result, "true|bool|2.0|Idle|true|false|Running|Base Layer.Running|enter:0:Idle,enter:0:Running,exit:0:Idle");
    // GameObject:getAnimator() is the Animator now (upstream's alias of getAnimationPlayer for a
    // GameObject without one)
    ASSERT_EQ(luaL_dostring(L, "ctrlKind = tostring(GameObject(ctrlPlayer.scene, ctrlPlayer.entity):getAnimator():getLayerCount())"), LUA_OK)
        << lua_tostring(L, -1);
    lua_getglobal(L, "ctrlKind");
    EXPECT_STREQ(lua_tostring(L, -1), "1");
    lua_pop(L, 1);
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
    component.needsLoad = true;
    step(0.1);
    EXPECT_EQ(object.getCurrentStateName(), "Gone");
    EXPECT_NEAR(legX(), 0.0f, 1e-5f) << "an empty state writes the rest pose; the handing-over player's clip plays nothing";

    // an edited controller reloads: the layer keeps its current state by name
    controller("reload", R"({"format": "jasperengine.controller", "version": 1,
      "layers": [{"name": "Base Layer", "default_state": "Idle", "states": [{"name": "Idle", "clip": "@idle.anim"}]}]})");
    AnimatorAssetPool::invalidate(path);
    step(0.1);
    EXPECT_EQ(object.getCurrentStateName(), "Idle");
    EXPECT_NEAR(legX(), 1.0f, 1e-5f);
}

// review fixes: a transition into the playing state keeps its weight (no dip to the rest pose),
// start events fire when a state is entered through a transition, a trigger two layers read
// fires on both, a state whose clip failed counts as empty (1 s) so exit times go on
TEST_F(Controllers, ReentryStartEventsSharedTriggersAndFailedClips){
    ClipData hit = still(4);
    hit.events = {ClipEventData{0.0f, "onHit", {}}};
    clip("hit", hit);
    clip("idle", still(1));
    clip("upper", still(2));
    AnimationPlayer object = player(controller("reentry", R"({"format": "jasperengine.controller", "version": 1,
      "parameters": [{"name": "Hit", "type": "trigger"}],
      "layers": [{"name": "Base Layer", "states": [{"name": "Idle", "clip": "@idle.anim"}, {"name": "Hit", "clip": "@hit.anim"},
                                                   {"name": "Broken", "clip": "@missing.anim"}],
        "transitions": [{"from": "any", "to": "Hit", "conditions": [{"parameter": "Hit", "mode": "if"}], "has_exit_time": false, "duration": 0.2},
                        {"from": "Idle", "to": "Broken", "conditions": [{"parameter": "Hit", "mode": "if_not"}], "has_exit_time": true, "exit_time": 5},
                        {"from": "Broken", "to": "Idle", "exit_time": 0.5, "duration": 0}]},
        {"name": "Upper", "states": [{"name": "Rest", "clip": "@upper.anim"}, {"name": "Flinch", "clip": "@upper.anim"}],
         "transitions": [{"from": "Rest", "to": "Flinch", "conditions": [{"parameter": "Hit", "mode": "if"}], "has_exit_time": false, "duration": 0}]}]})"));
    std::vector<std::string> fired;
    object.getOnEvent().add("test", std::function<void(const AnimationEvent&)>([&](const AnimationEvent& e){ fired.push_back(e.functionName); }));
    step(0.1);
    object.setTrigger("Hit");
    step(0.1);
    EXPECT_EQ(object.getCurrentStateName(1), "Flinch") << "the trigger fired on both layers";
    EXPECT_TRUE(object.isInTransition(0));
    EXPECT_FALSE(object.getBool("Hit")) << "consumed after both layers took it";
    step(0.1);
    step(0.2);
    EXPECT_EQ(fired.size(), 1u) << "Hit's event at 0 fires once it has weight, through a 0.2 s transition";
    EXPECT_EQ(object.getCurrentStateName(0), "Hit");
    object.setTrigger("Hit");
    step(0.1);
    step(0.1);
    EXPECT_NEAR(weight("Base Layer.Hit"), 1.0f, 1e-5f) << "re-entering the playing state keeps its weight";

    // a failed clip: an empty state of 1 s whose exit time still comes
    object.play("Broken");
    step(0.0);
    EXPECT_EQ(object.getCurrentStateName(), "Broken");
    step(0.3, 2);
    EXPECT_EQ(object.getCurrentStateName(), "Idle");
    object.getOnEvent().clear();
}

TEST_F(Controllers, ReloadDuringATransitionKeepsItsDestination){
    clip("a", still(1));
    clip("b", still(2));
    const std::string text = R"({"format": "jasperengine.controller", "version": 1, "parameters": [{"name": "Go", "type": "bool"}],
      "layers": [{"name": "Base Layer", "states": [{"name": "A", "clip": "@a.anim"}, {"name": "B", "clip": "@b.anim"}],
        "transitions": [{"from": "A", "to": "B", "conditions": [{"parameter": "Go", "mode": "if"}], "has_exit_time": false, "duration": 1}]}]})";
    const std::string path = controller("mid", text);
    AnimationPlayer object = player(path);
    std::vector<std::string> log;
    object.getOnStateExit().add("test", std::function<void(int, const std::string&)>([&](int, const std::string& name){ log.push_back(name); }));
    step(0.1);
    object.setBool("Go", true);
    step(0.1);
    step(0.3);
    ASSERT_TRUE(object.isInTransition());
    controller("mid", text);
    AnimatorAssetPool::invalidate(path);
    step(0.1);
    EXPECT_EQ(object.getCurrentStateName(), "B");
    EXPECT_FALSE(object.isInTransition());
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0], "A");
    object.getOnStateExit().clear();
}

TEST(ControllerIO, ValidationDoesNotRepeatWarnings){
    anim::ControllerParseResult parsed = anim::parseController(R"({"format": "jasperengine.controller", "version": 1,
      "parameters": [{"name": "S", "type": "float"}],
      "layers": [{"name": "Base Layer", "states": [{"name": "M", "blend_tree": {"parameter": "S", "children": [{"clip": "a.anim", "threshold": 1}, {"clip": "b.anim", "threshold": 1}]}}]}]})");
    ASSERT_TRUE(parsed.ok);
    EXPECT_GE(parsed.warnings.size(), 1u) << "the duplicate threshold (upstream's reader)";
    const std::vector<ClipWarning> warnings = anim::validateController(parsed.data, nullptr);
    for (size_t i = 0; i < warnings.size(); i++){
        for (size_t j = i + 1; j < warnings.size(); j++){
            EXPECT_FALSE(warnings[i].path == warnings[j].path && warnings[i].message == warnings[j].message) << warnings[i].message;
        }
    }
}

// independent review: ordered interruption (current_state: only transitions listed before the
// running one), normalized durations, transition offset and cycle offset, speed parameters, int
// equals / not_equal, the callback order (clip events, onStateExit, onStateEnter), Model:playAnimation
// through a controller, Lua-safe parameter names, playInLayer / crossFadeInLayer
TEST_F(Controllers, OrderedInterruptionFromTheCurrentState){
    clip("a", still(1));
    clip("b", still(2));
    clip("c", still(3));
    clip("d", still(4));
    AnimationPlayer object = player(controller("ordered", R"({"format": "jasperengine.controller", "version": 1,
      "parameters": [{"name": "GoB", "type": "bool"}, {"name": "GoC", "type": "bool"}, {"name": "GoD", "type": "bool"}],
      "layers": [{"name": "Base Layer", "states": [{"name": "A", "clip": "@a.anim"}, {"name": "B", "clip": "@b.anim"},
                                                   {"name": "C", "clip": "@c.anim"}, {"name": "D", "clip": "@d.anim"}],
        "transitions": [
          {"from": "A", "to": "D", "conditions": [{"parameter": "GoD", "mode": "if"}], "has_exit_time": false, "duration": 1},
          {"from": "A", "to": "B", "conditions": [{"parameter": "GoB", "mode": "if"}], "has_exit_time": false, "duration": 1, "interruption": "current_state"},
          {"from": "A", "to": "C", "conditions": [{"parameter": "GoC", "mode": "if"}], "has_exit_time": false, "duration": 1}]}]})"));
    step(0.1);
    object.setBool("GoB", true);
    step(0.1);
    ASSERT_EQ(object.getNextStateInfo().name, "B");
    object.setBool("GoC", true);
    step(0.1);
    EXPECT_EQ(object.getNextStateInfo().name, "B") << "A->C is listed after A->B: it cannot interrupt";
    object.setBool("GoD", true);
    step(0.1);
    EXPECT_EQ(object.getNextStateInfo().name, "D") << "A->D is listed before A->B: it interrupts";
}

TEST_F(Controllers, DurationsOffsetsSpeedsAndIntConditions){
    clip("a", still(1, 2.0f));
    clip("b", slide(10, 1.0f));
    AnimationPlayer object = player(controller("timing", R"({"format": "jasperengine.controller", "version": 1,
      "parameters": [{"name": "N", "type": "int"}, {"name": "Rate", "type": "float", "default": 2}],
      "layers": [{"name": "Base Layer", "states": [{"name": "A", "clip": "@a.anim"},
                                                   {"name": "B", "clip": "@b.anim", "speed_parameter": "Rate", "cycle_offset": 0.25},
                                                   {"name": "C", "clip": "@b.anim"}],
        "transitions": [
          {"from": "A", "to": "B", "conditions": [{"parameter": "N", "mode": "equals", "threshold": 3}], "has_exit_time": false,
           "duration": 0.25, "fixed_duration": false},
          {"from": "B", "to": "C", "conditions": [{"parameter": "N", "mode": "not_equal", "threshold": 3}], "has_exit_time": false,
           "duration": 0, "offset": 0.5}]}]})"));
    step(0.1);
    object.setInteger("N", 2);
    step(0.1);
    EXPECT_FALSE(object.isInTransition()) << "2 != 3";
    object.setInteger("N", 3);
    step(0.1);
    ASSERT_TRUE(object.isInTransition());
    // S21 (Unity): the cycle offset shifts where the clip plays, normalizedTime starts at 0
    EXPECT_NEAR(object.getNextStateInfo().normalizedTime, 0.0f, 1e-5f);
    EXPECT_NEAR(legX(), 1.0f, 1e-4f) << "B (at 0.25 of its clip) has no weight yet";
    step(0.25);
    EXPECT_TRUE(object.isInTransition()) << "a normalized duration: 0.25 x A's 2 s = 0.5 s";
    EXPECT_NEAR(weight("Base Layer.B"), 0.5f, 1e-4f);
    // B at normalized 0.5 (Rate 2 for 0.25 s) + its cycle offset 0.25: x = 7.5; half of it over A's 1
    EXPECT_NEAR(legX(), 0.5f * 1.0f + 0.5f * 7.5f, 1e-3f);
    step(0.25);
    EXPECT_FALSE(object.isInTransition());
    // speed_parameter Rate 2: B (1 s) advances 2 per second
    const float before = object.getCurrentStateInfo().normalizedTime;
    step(0.1);
    EXPECT_NEAR(object.getCurrentStateInfo().normalizedTime - before, 0.2f, 1e-4f);
    EXPECT_NEAR(object.getCurrentStateInfo().speed, 2.0f, 1e-6f);
    object.setInteger("N", 4);
    step(0.1);
    EXPECT_EQ(object.getCurrentStateName(), "C");
    EXPECT_NEAR(object.getCurrentStateInfo().normalizedTime, 0.5f, 1e-5f) << "the transition's offset";
}

TEST_F(Controllers, CallbacksFollowTheStepsClipEvents){
    ClipData a = still(1, 1.0f);
    a.events = {ClipEventData{0.95f, "onLate", {}}};
    clip("a", a);
    clip("b", still(2));
    AnimationPlayer object = player(controller("order", R"({"format": "jasperengine.controller", "version": 1,
      "layers": [{"name": "Base Layer", "states": [{"name": "A", "clip": "@a.anim"}, {"name": "B", "clip": "@b.anim"}],
        "transitions": [{"from": "A", "to": "B", "exit_time": 0.9, "duration": 0}]}]})"));
    std::vector<std::string> log;
    object.getOnEvent().add("test", std::function<void(const AnimationEvent&)>([&](const AnimationEvent& e){ log.push_back(e.functionName); }));
    object.getOnStateExit().add("test", std::function<void(int, const std::string&)>([&](int, const std::string& n){ log.push_back("exit " + n); }));
    object.getOnStateEnter().add("test", std::function<void(int, const std::string&)>([&](int, const std::string& n){ log.push_back("enter " + n); }));
    step(0.0);
    log.clear();
    step(0.5);
    step(0.5);     // crosses 0.9 (exit) and 0.95 (the event) in one step
    // the step's clip events first (A plays its whole step), then the state notes: a 0 s
    // transition leaves A and enters B in this update
    ASSERT_EQ(log.size(), 3u);
    EXPECT_EQ(log[0], "onLate");
    EXPECT_EQ(log[1], "exit A");
    EXPECT_EQ(log[2], "enter B");
    object.getOnEvent().clear();
    object.getOnStateExit().clear();
    object.getOnStateEnter().clear();
}

TEST_F(Controllers, ModelPlayAnimationAndLayerForms){
    scene.addComponent<ModelComponent>(root, {});
    scene.addComponent<MeshComponent>(root, {});
    scene.getComponent<ModelComponent>(root).needUpdateModel = false;
    clip("a", still(1));
    clip("b", still(2));
    AnimationPlayer object = player(controller("model", R"({"format": "jasperengine.controller", "version": 1,
      "layers": [{"name": "Base Layer", "states": [{"name": "A", "clip": "@a.anim"}, {"name": "B", "clip": "@b.anim"}]}]})"));
    step(0.1);
    Model model(&scene, root);
    model.playAnimation("B", 0.2f);
    step(0.1);
    EXPECT_TRUE(object.isInTransition()) << "Model:playAnimation crossfades the controller";
    EXPECT_EQ(object.getNextStateInfo().name, "B");
    step(0.2);
    EXPECT_EQ(object.getCurrentStateName(), "B");
    model.stopAnimations(0.0f);
    EXPECT_FALSE(object.isPlaying());
    object.playInLayer("A", 0, 0.5f);
    step(0.0);
    EXPECT_EQ(object.getCurrentStateName(), "A");
    EXPECT_NEAR(object.getCurrentStateInfo().normalizedTime, 0.5f, 1e-5f);
    object.crossFadeInLayer("Base Layer.B", 0.4f, 0);
    step(0.1);
    EXPECT_NEAR(weight("Base Layer.B"), 0.25f, 1e-4f);
    object.play("A", 0.4f, PlayMode::StopAll);
    step(0.1);
    EXPECT_TRUE(object.isInTransition()) << "play with a fade crossfades with a controller too";
}

TEST(ControllerIO, ParameterNamesThatAreNotLuaIdentifiersAreWarned){
    // upstream's reader drops them; S21 keeps every parameter (scripts pass names as strings) and
    // its validation names them
    const anim::ControllerDef def = anim::upstream::toControllerDef(*anim::upstream::parseController(R"({"format": "jasperengine.controller", "version": 1,
      "parameters": [{"name": "Speed", "type": "float"}, {"name": "_ok2", "type": "trigger"}]})").data);
    ASSERT_EQ(def.parameters.size(), 2u);
    anim::ControllerParseResult parsed = anim::parseController(R"({"format": "jasperengine.controller", "version": 2,
      "parameters": [{"name": "Speed", "type": "float"}, {"name": "is running", "type": "bool"}, {"name": "end", "type": "bool"},
                     {"name": "2fast", "type": "bool"}, {"name": "_ok2", "type": "trigger"}], "layers": []})");
    ASSERT_TRUE(parsed.ok);
    ASSERT_EQ(parsed.data.parameters.size(), 5u);
    int named = 0;
    for (const ClipWarning& warning : anim::validateController(parsed.data, nullptr)) named += warning.message.find("not a Lua-safe name") != std::string::npos;
    EXPECT_EQ(named, 3);
}
