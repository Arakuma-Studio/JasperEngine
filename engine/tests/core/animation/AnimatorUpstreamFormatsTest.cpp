// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Upstream's .controller and .mask files (anim-layers, version 1, the same header as S21's
// version 1 files) through S21's readers (docs/specs/S21-animation.md, "Upstream absorption"):
// the files play what upstream played, and a save keeps every state, transition and mask
// weight. These tests were written before the conversion existed: an upstream controller read
// as empty layers (a save wiped it) and an upstream mask let every bone through.

#include <gtest/gtest.h>

#include "AnimatorTestUtil.h"

#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::anim;
using namespace animatortest;

namespace {

    class AnimatorUpstreamFiles : public ::testing::Test, public AnimatorFixture {
    protected:
        // `text` with every "@" replaced by the test's prefix, written as an asset
        std::string upstreamFile(const std::string& name, std::string text){
            for (size_t at = text.find('@'); at != std::string::npos; at = text.find('@', at + prefix.size())) text.replace(at, 1, prefix);
            animtest::writeAsset(prefix + name, text);
            return prefix + name;
        }
    };

    // upstream's GranjeroAnim port shape: flat states and transitions, "blend", string masks,
    // has_exit_time left out (upstream's default: true)
    const char* UPSTREAM_CONTROLLER = R"({"format": "jasperengine.controller", "version": 1,
      "parameters": [{"name": "Speed", "type": "float", "default": 0}, {"name": "Jump", "type": "trigger"}],
      "layers": [{"name": "Base Layer", "default_state": "Idle",
        "states": [{"name": "Idle", "clip": "@idle.anim"}, {"name": "Running", "clip": "@running.anim"}],
        "transitions": [
          {"from": "Idle", "to": "Running", "conditions": [{"parameter": "Speed", "mode": "greater", "threshold": 0.5}],
           "has_exit_time": false, "duration": 0},
          {"from": "Running", "to": "Idle", "conditions": [{"parameter": "Speed", "mode": "less", "threshold": 0.5}],
           "has_exit_time": false, "duration": 0}]}]})";

}

TEST_F(AnimatorUpstreamFiles, UpstreamControllerPlaysItsStates){
    clip("idle", hold("Hips", Vector3(1, 1, 0)));
    clip("running", hold("Hips", Vector3(3, 1, 0)));
    animator(upstreamFile("granjero.controller", UPSTREAM_CONTROLLER));
    step(0.0);
    step(0.1);
    EXPECT_NEAR(position(hips).x, 1.0f, 1e-6f) << "the default state Idle plays";
    Animator object(&scene, root);
    object.setFloat("Speed", 1.0f);
    step(0.1);
    EXPECT_NEAR(position(hips).x, 3.0f, 1e-6f) << "Idle -> Running on Speed > 0.5";
    EXPECT_EQ(object.getCurrentState(0).name, "Running");
}

TEST_F(AnimatorUpstreamFiles, UpstreamControllerSurvivesASave){
    std::string text = UPSTREAM_CONTROLLER;
    for (size_t at = text.find('@'); at != std::string::npos; at = text.find('@')) text.erase(at, 1);
    const ControllerParseResult first = parseController(text);
    ASSERT_TRUE(first.ok) << first.error.toString();
    // what an editor writes back after opening it
    const ControllerParseResult saved = parseController(writeController(first.data));
    ASSERT_TRUE(saved.ok) << saved.error.toString();
    ASSERT_EQ(saved.data.layers.size(), 1u);
    const StateMachineDef& machine = saved.data.layers[0].machine;
    ASSERT_EQ(machine.states.size(), 2u) << "a save must not wipe the states";
    EXPECT_EQ(machine.defaultState, "Idle");
    EXPECT_EQ(machine.states[0].motion.clip.path, "idle.anim");
    ASSERT_EQ(machine.states[0].transitions.size(), 1u);
    EXPECT_EQ(machine.states[0].transitions[0].to, "Running");
    ASSERT_EQ(machine.states[0].transitions[0].conditions.size(), 1u);
    EXPECT_EQ(machine.states[0].transitions[0].conditions[0].mode, AnimatorConditionMode::Greater);
    EXPECT_FLOAT_EQ(machine.states[0].transitions[0].conditions[0].threshold, 0.5f);
    ASSERT_EQ(machine.states[1].transitions.size(), 1u);
    ASSERT_EQ(saved.data.parameters.size(), 2u);
    EXPECT_EQ(saved.data.parameters[1].type, AnimatorParameterType::Trigger);
}

TEST_F(AnimatorUpstreamFiles, UpstreamMaskMasksBones){
    // a base layer and an upper override layer through upstream's mask: nothing (default 0) but
    // the spine and what is below it (children: true)
    clip("base", hold("Hips", Vector3(1, 1, 0)));
    ClipData upper;
    upper.wrap = ClipWrapMode::Loop;
    upper.tracks = {track("Hips", ClipChannel::Position, {key(0, {5, 5, 5}), key(1, {5, 5, 5})}),
                    track("Hips/Spine", ClipChannel::Position, {key(0, {0, 2, 0}), key(1, {0, 2, 0})}),
                    track("Hips/Spine/Head", ClipChannel::Position, {key(0, {0, 3, 0}), key(1, {0, 3, 0})})};
    clip("upper", upper);
    const std::string mask = upstreamFile("spine.mask", R"({"format": "jasperengine.mask", "version": 1, "default": 0,
      "transforms": [{"path": "Hips/Spine", "weight": 1, "children": true}]})");

    ControllerDef def = controller({}, {state("Base", prefix + "base.anim")});
    LayerDef layer;
    layer.name = "Upper";
    layer.weight = 1.0f;
    layer.mask.path = mask;
    layer.machine.states.push_back(state("Upper", prefix + "upper.anim"));
    def.layers.push_back(layer);
    animator(controllerFile("masked", def));
    step(0.0);
    step(0.1);
    EXPECT_NEAR(position(hips).x, 1.0f, 1e-6f) << "default 0: the hips keep the base layer's";
    EXPECT_NEAR(position(spine).y, 2.0f, 1e-6f) << "the listed path";
    EXPECT_NEAR(position(head).y, 3.0f, 1e-6f) << "children: true covers the head";
}

TEST_F(AnimatorUpstreamFiles, UpstreamMaskSurvivesASave){
    const MaskParseResult first = parseMask(R"({"format": "jasperengine.mask", "version": 1, "default": 0,
      "transforms": [{"path": "Hips/Spine", "weight": 0.5, "children": true}]})");
    ASSERT_TRUE(first.ok) << first.error.toString();
    clip("base", hold("Hips/Spine/Head", Vector3(0, 1, 0)));
    clip("upper", hold("Hips/Spine/Head", Vector3(0, 3, 0)));
    // the saved mask, played: the head (below the spine) takes half of the upper layer
    animtest::writeAsset(prefix + "saved.mask", writeMask(first.data));
    ControllerDef def = controller({}, {state("Base", prefix + "base.anim")});
    LayerDef layer;
    layer.name = "Upper";
    layer.weight = 1.0f;
    layer.mask.path = prefix + "saved.mask";
    layer.machine.states.push_back(state("Upper", prefix + "upper.anim"));
    def.layers.push_back(layer);
    animator(controllerFile("saved", def));
    step(0.0);
    step(0.1);
    EXPECT_NEAR(position(head).y, 2.0f, 1e-5f) << "lerp(1, 3, 0.5) after a save";
    EXPECT_NEAR(position(hips).y, 1.0f, 1e-6f);
}

// ---- the conversion in detail (written once the conversion existed) ---------------------------

namespace {

    std::string readAssetText(const std::string& relative){
        std::ifstream file(animtest::assetDir() + "/" + relative, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    // a fuller upstream controller: a blend tree, exit times, an Any State transition on a
    // trigger, a second layer with an upstream mask, upstream-only members (mirror, foot_ik,
    // automate_thresholds) and members nobody knows (editor_note)
    const char* UPSTREAM_FULL = R"({"format": "jasperengine.controller", "version": 1, "editor_note": "root",
      "parameters": [{"name": "Speed", "type": "float"}, {"name": "Reset", "type": "trigger"}],
      "layers": [
        {"name": "Base Layer", "default_state": "Idle", "any_state": {"position": [40, 30]}, "editor_note": "layer",
         "states": [
           {"name": "Idle", "clip": "@idle.anim", "position": [200, 100], "mirror": true, "foot_ik": true},
           {"name": "Move", "position": [400, 100], "speed": 1.5, "wrap": "loop",
            "blend_tree": {"parameter": "Speed", "automate_thresholds": false,
                           "children": [{"clip": "@idle.anim", "threshold": 0}, {"clip": "@running.anim", "threshold": 1, "speed": 2}]}}],
         "transitions": [
           {"from": "Idle", "to": "Move", "conditions": [{"parameter": "Speed", "mode": "greater", "threshold": 0.1}],
            "has_exit_time": false, "duration": 0.2, "interruption": "next_state"},
           {"from": "Move", "to": "Idle", "conditions": [{"parameter": "Speed", "mode": "less", "threshold": 0.1}],
            "exit_time": 0.5, "duration": 0.1, "editor_note": "transition"},
           {"from": "any", "to": "Idle", "conditions": [{"parameter": "Reset", "mode": "if"}], "has_exit_time": false, "duration": 0}]},
        {"name": "Upper", "weight": 0.5, "blend": "override", "mask": "@spine.mask",
         "states": [{"name": "Wave", "clip": "@wave.anim"}]}]})";

    const char* UPSTREAM_SPINE_MASK = R"({"format": "jasperengine.mask", "version": 1, "default": 0,
      "transforms": [{"path": "Hips/Spine"}]})";

}

TEST_F(AnimatorUpstreamFiles, UpstreamControllerConvertsMemberByMember){
    const std::string text = upstreamFile("full.controller", UPSTREAM_FULL);
    const ControllerParseResult read = parseController(readAssetText(text));
    ASSERT_TRUE(read.ok) << read.error.toString();
    EXPECT_EQ(read.version, 1);
    EXPECT_EQ(read.dialect, AnimatorFileDialect::UpstreamVersion1);
    EXPECT_TRUE(read.converted());
    const ControllerDef& def = read.data;
    ASSERT_EQ(def.layers.size(), 2u);
    const StateMachineDef& base = def.layers[0].machine;
    EXPECT_EQ(base.defaultState, "Idle");
    EXPECT_EQ(base.anyStatePosition.x, 40.0f);
    ASSERT_EQ(base.states.size(), 2u);
    EXPECT_EQ(base.states[0].position.x, 200.0f);
    // Idle -> Move: no exit time, 0.2 s, interruption from the next state
    ASSERT_EQ(base.states[0].transitions.size(), 1u);
    const TransitionDef& go = base.states[0].transitions[0];
    EXPECT_EQ(go.to, "Move");
    EXPECT_FALSE(go.hasExitTime);
    EXPECT_FLOAT_EQ(go.duration, 0.2f);
    EXPECT_EQ(go.interruptionSource, TransitionInterruptionSource::Next);
    // Move -> Idle: upstream's default has_exit_time (true) made explicit
    ASSERT_EQ(base.states[1].transitions.size(), 1u);
    EXPECT_TRUE(base.states[1].transitions[0].hasExitTime);
    EXPECT_FLOAT_EQ(base.states[1].transitions[0].exitTime, 0.5f);
    // the "any" transition: Any State's, upstream's can_transition_to_self default (true) kept
    ASSERT_EQ(base.anyState.size(), 1u);
    EXPECT_EQ(base.anyState[0].to, "Idle");
    EXPECT_TRUE(base.anyState[0].canTransitionToSelf);
    // the blend tree: child speed -> time scale, the state's wrap -> loop
    ASSERT_TRUE(base.states[1].motion.isTree());
    const BlendTreeDef& tree = base.states[1].motion.tree[0];
    EXPECT_EQ(tree.type, BlendTreeType::Simple1D);
    EXPECT_EQ(tree.parameter, "Speed");
    ASSERT_EQ(tree.children.size(), 2u);
    EXPECT_FLOAT_EQ(tree.children[1].threshold, 1.0f);
    EXPECT_FLOAT_EQ(tree.children[1].timeScale, 2.0f);
    EXPECT_FLOAT_EQ(base.states[1].speed, 1.5f);
    EXPECT_EQ(base.states[1].loop, AnimatorStateLoop::Repeat);
    // the second layer: its weight, blending and mask
    EXPECT_FLOAT_EQ(def.layers[1].weight, 0.5f);
    EXPECT_EQ(def.layers[1].blending, AnimatorLayerBlending::Override);
    EXPECT_EQ(def.layers[1].mask.path, prefix + "spine.mask");
    ASSERT_EQ(def.parameters.size(), 2u);
    EXPECT_EQ(def.parameters[1].type, AnimatorParameterType::Trigger);

    // saved: version 2, and every member upstream wrote is still in the file
    const std::string saved = writeController(def);
    EXPECT_NE(saved.find("\"version\": 2"), std::string::npos);
    for (const char* kept : {"\"mirror\": true", "\"foot_ik\": true", "\"automate_thresholds\": false", "\"editor_note\": \"root\"",
                             "\"editor_note\": \"layer\"", "\"editor_note\": \"transition\""}){
        EXPECT_NE(saved.find(kept), std::string::npos) << kept << "\n" << saved;
    }
    // and it is a fixpoint: read as the current version, written to the same text
    const ControllerParseResult again = parseController(saved);
    ASSERT_TRUE(again.ok) << again.error.toString();
    EXPECT_EQ(again.dialect, AnimatorFileDialect::Current);
    EXPECT_FALSE(again.converted());
    EXPECT_EQ(writeController(again.data), saved);
}

TEST_F(AnimatorUpstreamFiles, AConvertedControllerPlaysLikeTheUpstreamFile){
    clip("idle", hold("Hips", Vector3(1, 1, 0)));
    clip("running", slide("Hips", 4.0f));
    clip("wave", hold("Hips/Spine", Vector3(0, 2, 0)));
    upstreamFile("spine.mask", UPSTREAM_SPINE_MASK);
    const std::string upstreamPath = upstreamFile("play.controller", UPSTREAM_FULL);

    auto play = [&](const std::string& path){
        std::vector<Vector3> samples;
        animator(path);
        Animator object(&scene, root);
        auto sample = [&](double dt, int count){
            for (int i = 0; i < count; i++){
                step(dt);
                samples.push_back(position(hips));
                samples.push_back(position(spine));
                samples.push_back(position(head));
            }
        };
        sample(0.0, 1);
        sample(0.1, 3);
        object.setFloat("Speed", 0.5f);
        sample(0.05, 12);
        object.setFloat("Speed", 0.0f);
        sample(0.05, 20);
        object.setFloat("Speed", 1.0f);
        sample(0.05, 6);
        object.setTrigger("Reset");
        sample(0.05, 6);
        return samples;
    };
    const std::vector<Vector3> upstream = play(upstreamPath);

    // the file an editor saves after opening it (version 2), played from the start
    const ControllerParseResult read = parseController(readAssetText(upstreamPath));
    ASSERT_TRUE(read.ok) << read.error.toString();
    animtest::writeAsset(prefix + "saved.controller", writeController(read.data));
    const std::vector<Vector3> saved = play(prefix + "saved.controller");

    ASSERT_EQ(saved.size(), upstream.size());
    bool moved = false;
    for (size_t i = 0; i < saved.size(); i++){
        EXPECT_NEAR(saved[i].x, upstream[i].x, 1e-6f) << i;
        EXPECT_NEAR(saved[i].y, upstream[i].y, 1e-6f) << i;
        EXPECT_NEAR(saved[i].z, upstream[i].z, 1e-6f) << i;
        moved = moved || std::abs(upstream[i].x - 1.0f) > 1e-3f;
    }
    EXPECT_TRUE(moved) << "the sequence reached the running clip";
    // the upper layer through its mask: the spine takes half of the wave (layer weight 0.5)
    EXPECT_NEAR(upstream[upstream.size() - 2].y, 0.3f + (2.0f - 0.3f) * 0.5f, 1e-5f) << "spine";
}

TEST_F(AnimatorUpstreamFiles, UpstreamHumanoidMaskConvertsPartByPart){
    const MaskParseResult read = parseMask(R"({"format": "jasperengine.mask", "version": 1, "default": 0,
      "humanoid": {"root": 1, "right_arm": 1, "right_fingers": 0.5, "head": 0},
      "transforms": [{"path": "Hips/Spine", "weight": 0.25}, {"path": "Hips/Leg", "weight": 1, "children": false}],
      "preview": {"model": "models/hero.gltf"}})");
    ASSERT_TRUE(read.ok) << read.error.toString();
    EXPECT_EQ(read.dialect, AnimatorFileDialect::UpstreamVersion1);
    const AvatarMaskDef& mask = read.data;
    EXPECT_EQ(mask.defaultWeight, 0.0f);
    EXPECT_EQ(mask.partWeight(AvatarBodyPart::Root), 1.0f);
    EXPECT_TRUE(mask.rootHierarchy) << "upstream's root part covered the root and what is above the hips";
    EXPECT_EQ(mask.partWeight(AvatarBodyPart::RightArm), 1.0f);
    EXPECT_EQ(mask.partWeight(AvatarBodyPart::RightHand), 1.0f) << "upstream's hand belonged to the arm";
    EXPECT_EQ(mask.partWeight(AvatarBodyPart::RightFingers), 0.5f);
    EXPECT_EQ(mask.partWeight(AvatarBodyPart::Head), 0.0f);
    EXPECT_EQ(mask.partWeight(AvatarBodyPart::LeftArm), 0.0f) << "a part upstream did not set weighs the default";
    ASSERT_EQ(mask.paths.size(), 2u);
    const int spinePath = mask.findPath("Hips/Spine");
    ASSERT_GE(spinePath, 0);
    EXPECT_TRUE(mask.paths[(size_t)spinePath].children) << "upstream's children default: true";
    EXPECT_FLOAT_EQ(mask.paths[(size_t)spinePath].weight, 0.25f);
    EXPECT_FALSE(mask.paths[(size_t)mask.findPath("Hips/Leg")].children);
    EXPECT_EQ(mask.previewModel, "models/hero.gltf");

    const std::string saved = writeMask(mask);
    EXPECT_NE(saved.find("\"version\": 2"), std::string::npos);
    const MaskParseResult again = parseMask(saved);
    ASSERT_TRUE(again.ok) << again.error.toString();
    EXPECT_EQ(again.dialect, AnimatorFileDialect::Current);
    EXPECT_EQ(again.data.paths, mask.paths);
    for (int part = 0; part < AVATAR_BODY_PART_COUNT; part++){
        EXPECT_EQ(again.data.partWeight((AvatarBodyPart)part), mask.partWeight((AvatarBodyPart)part)) << part;
    }
    EXPECT_EQ(writeMask(again.data), saved) << "a fixpoint";
}

TEST_F(AnimatorUpstreamFiles, S21VersionOneMasksAndControllersAreReadAsS21s){
    // S21's version 1 mask: bool parts, exact paths; the version 2 form weighs the bones the same
    const MaskParseResult mask = parseMask(R"({"format": "jasperengine.mask", "version": 1,
      "body_parts": {"head": false, "left_hand": false}, "paths": [{"path": "Hips/Spine", "weight": 0.5}]})");
    ASSERT_TRUE(mask.ok) << mask.error.toString();
    EXPECT_EQ(mask.dialect, AnimatorFileDialect::S21Version1);
    EXPECT_EQ(mask.data.defaultWeight, 1.0f) << "the body's";
    EXPECT_EQ(mask.data.partWeight(AvatarBodyPart::Head), 0.0f);
    EXPECT_EQ(mask.data.partWeight(AvatarBodyPart::LeftHand), 0.0f);
    EXPECT_EQ(mask.data.partWeight(AvatarBodyPart::LeftFingers), 0.0f) << "a hand's part also below the hand";
    EXPECT_EQ(mask.data.partWeight(AvatarBodyPart::RightFingers), 1.0f);
    EXPECT_FALSE(mask.data.rootHierarchy);
    ASSERT_EQ(mask.data.paths.size(), 1u);
    EXPECT_FALSE(mask.data.paths[0].children) << "S21's version 1 paths were exact";

    // S21's version 1 controller (nested state machines) is S21's, whatever else it holds: an
    // upstream member next to S21's is kept (written back), not read
    ControllerDef def = controller({}, {state("Idle", "idle.anim")});
    std::string text = writeController(def);
    const size_t version = text.find("\"version\": 2");
    ASSERT_NE(version, std::string::npos);
    text.replace(version, 12, "\"version\": 1");
    const size_t layerName = text.find("\"name\": \"Base Layer\"");
    ASSERT_NE(layerName, std::string::npos);
    text.insert(layerName, "\"states\": [{\"name\": \"Ghost\"}], ");
    const ControllerParseResult read = parseController(text);
    ASSERT_TRUE(read.ok) << read.error.toString();
    EXPECT_EQ(read.dialect, AnimatorFileDialect::S21Version1);
    ASSERT_EQ(read.data.layers[0].machine.states.size(), 1u);
    EXPECT_EQ(read.data.layers[0].machine.states[0].name, "Idle");
    const std::string saved = writeController(read.data);
    EXPECT_NE(saved.find("Ghost"), std::string::npos) << "the member nobody reads is still in the file";
    EXPECT_NE(saved.find("\"version\": 2"), std::string::npos);
}
