// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animator's files (docs/specs/S21-animation.md AC9, R32, D13, D18): .controller, .mask and
// .montage round trips, unknown keys kept, newer versions refused, lenient warnings with JSON
// paths, mutated texts never crash, and .anim's root_motion extension.

#include <gtest/gtest.h>

#include "animation/AnimationClipIO.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/CompiledController.h"

#include <random>
#include <string>

using namespace jasperengine;
using namespace jasperengine::anim;

namespace {

    ControllerDef sampleController(){
        ControllerDef def;
        def.guid = "0123456789abcdef0123456789abcdef";
        def.parameters = {{"Speed", AnimatorParameterType::Float, 0.5f}, {"Jump", AnimatorParameterType::Trigger, 0.0f},
                          {"Grounded", AnimatorParameterType::Bool, 1.0f}, {"Combo", AnimatorParameterType::Int, 2.0f}};
        LayerDef base;
        base.name = "Base Layer";
        base.machine.defaultState = "Locomotion";
        StateDef locomotion;
        locomotion.name = "Locomotion";
        locomotion.position = Vector2(240, 120);
        BlendTreeDef tree;
        tree.type = BlendTreeType::FreeformCartesian2D;
        tree.parameter = "Speed";
        tree.parameterY = "Speed";
        for (int i = 0; i < 3; i++){
            BlendChildDef child;
            child.motion.clip.path = "anim/run" + std::to_string(i) + ".anim";
            child.motion.clip.guid = "ffffffffffffffffffffffffffffff0" + std::to_string(i);
            child.position = Vector2((float)i, 1.0f - (float)i);
            child.timeScale = i == 2 ? 1.5f : 1.0f;
            tree.children.push_back(child);
        }
        locomotion.motion.tree.push_back(tree);
        TransitionDef toJump;
        toJump.to = "Air/Jump";
        toJump.conditions = {{"Jump", AnimatorConditionMode::If, 0.0f}, {"Speed", AnimatorConditionMode::Greater, 0.25f}};
        toJump.interruptionSource = TransitionInterruptionSource::NextThenCurrent;
        locomotion.transitions.push_back(toJump);
        base.machine.states.push_back(locomotion);
        StateMachineDef air;
        air.name = "Air";
        air.position = Vector2(400, 200);
        StateDef jump;
        jump.name = "Jump";
        jump.motion.clip.path = "models/hero.gltf#Jump";
        jump.speed = 1.25f;
        jump.speedParameter = "Speed";
        jump.loop = AnimatorStateLoop::Once;
        jump.tag = "airborne";
        TransitionDef out;
        out.to = "Exit";
        out.hasExitTime = true;
        out.exitTime = 1.0f;
        jump.transitions.push_back(out);
        air.states.push_back(jump);
        TransitionDef back;
        back.to = "Locomotion";
        air.exitTransitions.push_back(back);
        base.machine.subMachines.push_back(air);
        TransitionDef hit;
        hit.to = "/Locomotion";
        hit.conditions = {{"Grounded", AnimatorConditionMode::IfNot, 0.0f}};
        hit.canTransitionToSelf = true;
        base.machine.anyState.push_back(hit);
        def.layers.push_back(base);
        LayerDef upper;
        upper.name = "Upper";
        upper.weight = 0.5f;
        upper.blending = AnimatorLayerBlending::Additive;
        upper.mask.path = "masks/upper.mask";
        upper.slot = "UpperBody";
        StateDef wave;
        wave.name = "Wave";
        wave.motion.clip.path = "anim/wave.anim";
        wave.additiveReference = AdditiveReference::Time;
        wave.additiveTime = 0.5f;
        upper.machine.states.push_back(wave);
        def.layers.push_back(upper);
        def.unknownKeys.push_back({"x_tool", "{\"a\":1}"});
        return def;
    }

}

TEST(AnimatorAssetIO, ControllerRoundTripIsStable){
    const std::string first = writeController(sampleController());
    ControllerParseResult parsed = parseController(first);
    ASSERT_TRUE(parsed.ok) << parsed.error.toString();
    const std::string second = writeController(parsed.data);
    EXPECT_EQ(first, second);
    ControllerParseResult again = parseController(second);
    ASSERT_TRUE(again.ok);
    EXPECT_EQ(writeController(again.data), second);

    const ControllerDef& def = parsed.data;
    ASSERT_EQ(def.parameters.size(), 4u);
    EXPECT_EQ(def.parameters[0].defaultValue, 0.5f);
    EXPECT_EQ(def.parameters[2].defaultValue, 1.0f);
    EXPECT_EQ(def.parameters[3].type, AnimatorParameterType::Int);
    ASSERT_EQ(def.layers.size(), 2u);
    EXPECT_EQ(def.layers[1].blending, AnimatorLayerBlending::Additive);
    EXPECT_EQ(def.layers[1].mask.path, "masks/upper.mask");
    EXPECT_EQ(def.layers[1].slot, "UpperBody");
    ASSERT_EQ(def.layers[0].machine.states.size(), 1u);
    const StateDef& locomotion = def.layers[0].machine.states[0];
    ASSERT_TRUE(locomotion.motion.isTree());
    EXPECT_EQ(locomotion.motion.tree[0].children.size(), 3u);
    EXPECT_EQ(locomotion.motion.tree[0].children[2].timeScale, 1.5f);
    EXPECT_EQ(locomotion.transitions[0].interruptionSource, TransitionInterruptionSource::NextThenCurrent);
    ASSERT_EQ(def.layers[0].machine.subMachines.size(), 1u);
    EXPECT_EQ(def.layers[0].machine.subMachines[0].exitTransitions.size(), 1u);
    EXPECT_EQ(def.layers[0].machine.subMachines[0].states[0].loop, AnimatorStateLoop::Once);
    EXPECT_TRUE(def.layers[0].machine.anyState[0].canTransitionToSelf);
    EXPECT_EQ(def.layers[1].machine.states[0].additiveReference, AdditiveReference::Time);
    ASSERT_EQ(def.unknownKeys.size(), 1u);
    EXPECT_EQ(def.unknownKeys[0].first, "x_tool");
    EXPECT_NE(second.find("\"x_tool\": {\"a\":1}"), std::string::npos) << "unknown keys are written back";
}

TEST(AnimatorAssetIO, ControllerCompilesAndResolvesTargets){
    ControllerParseResult parsed = parseController(writeController(sampleController()));
    ASSERT_TRUE(parsed.ok);
    std::shared_ptr<const CompiledController> compiled = compileController(parsed.data);
    EXPECT_EQ(compiled->findState("Base Layer.Air.Jump"), compiled->findState("Jump", 0));
    const int jump = compiled->findState("Jump");
    ASSERT_GE(jump, 0);
    EXPECT_EQ(compiled->states[(size_t)jump].fullPath, "Base Layer.Air.Jump");
    const int locomotion = compiled->findState("Locomotion");
    const CompiledState& state = compiled->states[(size_t)locomotion];
    ASSERT_EQ(state.transitionCount, 1u);
    const CompiledTransition& t = compiled->transitions[state.transitionBegin];
    EXPECT_EQ(t.kind, TransitionTargetKind::State);
    EXPECT_EQ(t.target, jump);
    EXPECT_EQ(compiled->clips.size(), 5u) << "three run clips, the jump, the wave";
    // the only diagnostics: none for a valid controller with every state reachable
    for (const ClipWarning& warning : compiled->diagnostics){
        ADD_FAILURE() << warning.path << ": " << warning.message;
    }
}

TEST(AnimatorAssetIO, ControllerDiagnostics){
    ControllerDef def;
    def.parameters = {{"Speed", AnimatorParameterType::Float, 0.0f}, {"Hit", AnimatorParameterType::Trigger, 0.0f}};
    LayerDef layer;
    layer.name = "Base Layer";
    StateDef idle;
    idle.name = "Idle";
    TransitionDef bad;
    bad.to = "Nowhere";
    bad.conditions = {{"Speed", AnimatorConditionMode::Greater, 0.1f}};
    TransitionDef never;
    never.to = "Idle";
    TransitionDef wrongMode;
    wrongMode.to = "Idle";
    wrongMode.conditions = {{"Hit", AnimatorConditionMode::Greater, 1.0f}, {"Missing", AnimatorConditionMode::If, 0.0f}};
    idle.transitions = {bad, never, wrongMode};
    StateDef lonely;
    lonely.name = "Lonely";
    layer.machine.states = {idle, lonely};
    def.layers.push_back(layer);
    std::shared_ptr<const CompiledController> compiled = compileController(def);
    auto has = [&](const std::string& text){
        for (const ClipWarning& warning : compiled->diagnostics){
            if (warning.message.find(text) != std::string::npos) return true;
        }
        return false;
    };
    EXPECT_TRUE(has("no state or state machine named \"Nowhere\""));
    EXPECT_TRUE(has("no condition and no exit time"));
    EXPECT_TRUE(has("does not apply to the trigger parameter \"Hit\""));
    EXPECT_TRUE(has("unknown parameter \"Missing\""));
    EXPECT_TRUE(has("\"Base Layer.Lonely\" is unreachable"));
}

TEST(AnimatorAssetIO, LenientReadsWarnWithJsonPaths){
    const std::string text = R"({"format": "jasperengine.controller", "version": 1,
        "parameters": [{"name": "Speed", "type": "float", "default": "fast"}, {"type": "bool"}],
        "layers": [{"name": "Base Layer", "weight": 1, "blending": "sideways",
                    "state_machine": {"states": [{"name": "Idle", "speed": "slow", "transitions": [{"conditions": []}]}]}}]})";
    ControllerParseResult parsed = parseController(text);
    ASSERT_TRUE(parsed.ok) << parsed.error.toString();
    auto warned = [&](const std::string& path){
        for (const ClipWarning& warning : parsed.warnings){
            if (warning.path == path) return true;
        }
        return false;
    };
    EXPECT_TRUE(warned("parameters[0].default"));
    EXPECT_TRUE(warned("parameters[1]"));
    EXPECT_TRUE(warned("layers[0].blending"));
    EXPECT_TRUE(warned("layers[0].state_machine.states[0].speed"));
    EXPECT_TRUE(warned("layers[0].state_machine.states[0].transitions[0]"));
    EXPECT_EQ(parsed.data.parameters.size(), 1u);
    EXPECT_EQ(parsed.data.layers[0].machine.states[0].speed, 1.0f);
}

TEST(AnimatorAssetIO, RefusesOtherFormatsNewerVersionsAndBadJson){
    EXPECT_FALSE(parseController("{\"format\": \"jasperengine.anim\", \"version\": 1}").ok);
    ControllerParseResult newer = parseController("{\"format\": \"jasperengine.controller\", \"version\": 2}");
    EXPECT_FALSE(newer.ok);
    EXPECT_NE(newer.error.message.find("newer JasperEngine"), std::string::npos) << newer.error.message;
    ControllerParseResult broken = parseController("{\"format\": \"jasperengine.controller\",\n  \"version\": 1,,}");
    EXPECT_FALSE(broken.ok);
    EXPECT_EQ(broken.error.line, 2);
    EXPECT_FALSE(parseMask("[]").ok);
    EXPECT_FALSE(parseMontage("{\"format\": \"jasperengine.montage\"}").ok);
}

TEST(AnimatorAssetIO, NamesAreMadeUnique){
    ControllerDef def;
    def.parameters = {{"A", AnimatorParameterType::Float, 0}, {"A", AnimatorParameterType::Bool, 0}};
    LayerDef layer;
    StateDef s1;
    s1.name = "Run";
    StateDef s2;
    s2.name = "Run";
    StateDef s3;
    s3.name = "Exit";
    StateDef s4;
    s4.name = "a/b.c";
    layer.machine.states = {s1, s2, s3, s4};
    def.layers = {layer, layer};
    std::vector<ClipWarning> warnings;
    normalizeController(def, &warnings);
    EXPECT_EQ(def.parameters[1].name, "A 1");
    EXPECT_EQ(def.layers[0].name, "Base Layer");
    EXPECT_EQ(def.layers[1].name, "New Layer");
    EXPECT_EQ(def.layers[0].machine.states[1].name, "Run 1");
    EXPECT_EQ(def.layers[0].machine.states[2].name, "Exit State");
    EXPECT_EQ(def.layers[0].machine.states[3].name, "a_b_c");
    EXPECT_FALSE(warnings.empty());
}

TEST(AnimatorAssetIO, MaskRoundTrip){
    AvatarMaskDef mask;
    mask.guid = "abcdefabcdefabcdefabcdefabcdef12";
    mask.bodyParts[(int)AvatarBodyPart::LeftLeg] = false;
    mask.bodyParts[(int)AvatarBodyPart::Root] = false;
    mask.paths = {{"Hero/Hips/Spine", 0.5f}, {"Hero/Hips", 2.0f}};
    std::vector<ClipWarning> warnings;
    normalizeMask(mask, &warnings);
    EXPECT_EQ(mask.paths[0].path, "Hero/Hips");
    EXPECT_EQ(mask.paths[0].weight, 1.0f) << "clamped";
    const std::string first = writeMask(mask);
    MaskParseResult parsed = parseMask(first);
    ASSERT_TRUE(parsed.ok) << parsed.error.toString();
    EXPECT_EQ(writeMask(parsed.data), first);
    EXPECT_FALSE(parsed.data.bodyParts[(int)AvatarBodyPart::LeftLeg]);
    EXPECT_TRUE(parsed.data.bodyParts[(int)AvatarBodyPart::Head]);
    EXPECT_EQ(animatorAssetKind(first), "mask");
    EXPECT_EQ(readAnimatorAssetGuid(first), mask.guid);
}

TEST(AnimatorAssetIO, MontageRoundTripAndSectionChain){
    const std::string text = R"({"format": "jasperengine.montage", "version": 1,
        "clip": {"path": "anim/attack.anim"}, "slot": "UpperBody", "rate": 1.5,
        "blend_in": {"time": 0.1, "ease": "quad_out"}, "blend_out": {"time": 0.2, "ease": "nope"},
        "sections": [{"name": "Windup", "start": 0}, {"name": "Hit", "start": 0.4}, {"name": "Recover", "start": 0.8, "next": null}],
        "events": [{"time": 0.45, "function": "onHit", "args": [1, "left", true]}, {"time": 0.1, "function": "1bad"}],
        "x_notes": "kept"})";
    MontageParseResult parsed = parseMontage(text);
    ASSERT_TRUE(parsed.ok) << parsed.error.toString();
    const MontageDef& m = parsed.data;
    EXPECT_EQ(m.slot, "UpperBody");
    EXPECT_EQ(m.rate, 1.5f);
    EXPECT_EQ(m.blendIn.ease, "quad_out");
    EXPECT_EQ(m.blendOut.ease, "linear");
    ASSERT_EQ(m.sections.size(), 3u);
    EXPECT_EQ(m.sections[0].next, "Hit") << "an absent next is the following section";
    EXPECT_EQ(m.sections[1].next, "Recover");
    EXPECT_EQ(m.sections[2].next, "");
    ASSERT_EQ(m.events.size(), 1u) << "the bad function name is dropped";
    EXPECT_EQ(m.events[0].args.size(), 3u);
    const std::string first = writeMontage(m);
    MontageParseResult again = parseMontage(first);
    ASSERT_TRUE(again.ok);
    EXPECT_EQ(writeMontage(again.data), first);
    EXPECT_NE(first.find("\"x_notes\": \"kept\""), std::string::npos);

    MontageDef empty;
    normalizeMontage(empty);
    ASSERT_EQ(empty.sections.size(), 1u);
    EXPECT_EQ(empty.sections[0].name, "Default");
}

TEST(AnimatorAssetIO, ClipRootMotionExtensionKeyIsKeptSilently){
    const std::string text = R"({"format": "jasperengine.anim", "version": 1, "wrap": "loop",
        "tracks": [{"path": "Hips", "channel": "position", "keys": [[0, [0, 0, 0]], [1, [0, 0, 2]]]}],
        "root_motion": {"bone": "Hips", "bake_rotation": true, "bake_position_y": false, "bake_position_xz": false}})";
    ClipParseResult parsed = parseClip(text);
    ASSERT_TRUE(parsed.ok()) << parsed.error.toString();
    for (const ClipWarning& warning : parsed.warnings){
        EXPECT_EQ(warning.message.find("unknown keys"), std::string::npos) << warning.message;
    }
    std::vector<ClipWarning> warnings;
    const ClipRootMotion rootMotion = readClipRootMotion(parsed.data.unknownKeys, &warnings);
    EXPECT_TRUE(rootMotion.present);
    EXPECT_EQ(rootMotion.bone, "Hips");
    EXPECT_TRUE(rootMotion.bakeRotation);
    EXPECT_FALSE(rootMotion.bakePositionY);
    EXPECT_TRUE(warnings.empty());
    const std::string written = writeClip(parsed.data);
    EXPECT_NE(written.find("\"root_motion\""), std::string::npos);
    EXPECT_EQ(readClipRootMotion(parseClip(written).data.unknownKeys).bone, "Hips");
    EXPECT_EQ(writeClipRootMotion(rootMotion), "{\"bone\": \"Hips\", \"bake_rotation\": true, \"bake_position_y\": false, \"bake_position_xz\": false}");
}

// AC9: mutated files are refused or loaded, never a crash or an out-of-bounds read (ASan runs it)
TEST(AnimatorAssetIO, FuzzNoCrash){
    const std::vector<std::string> seeds = {writeController(sampleController()), writeMask(AvatarMaskDef()), writeMontage(MontageDef())};
    std::mt19937 random(1234);
    const char alphabet[] = "{}[]\",:0123456789.-eE truefalsnul\\/\n";
    size_t loaded = 0;
    for (int i = 0; i < 20000; i++){
        std::string text = seeds[(size_t)i % seeds.size()];
        const int edits = 1 + (int)(random() % 8);
        for (int e = 0; e < edits && !text.empty(); e++){
            const size_t at = random() % text.size();
            switch (random() % 3){
                case 0: text[at] = alphabet[random() % (sizeof(alphabet) - 1)]; break;
                case 1: text.erase(at, 1 + random() % 8); break;
                default: text.insert(at, 1, alphabet[random() % (sizeof(alphabet) - 1)]); break;
            }
        }
        ControllerParseResult c = parseController(text);
        if (c.ok){
            loaded++;
            compileController(c.data);
            writeController(c.data);
        }
        MaskParseResult m = parseMask(text);
        if (m.ok) writeMask(m.data);
        MontageParseResult mo = parseMontage(text);
        if (mo.ok) writeMontage(mo.data);
    }
    EXPECT_GT(loaded, 0u) << "some mutations still parse";
}
