// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Upstream's .controller and .mask files (anim-layers, version 1, the same header as S21's
// version 1 files) through S21's readers (docs/specs/S21-animation.md, "Upstream absorption"):
// the files play what upstream played, and a save keeps every state, transition and mask
// weight. These tests were written before the conversion existed: an upstream controller read
// as empty layers (a save wiped it) and an upstream mask let every bone through.

#include <gtest/gtest.h>

#include "AnimatorTestUtil.h"

#include <string>

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
