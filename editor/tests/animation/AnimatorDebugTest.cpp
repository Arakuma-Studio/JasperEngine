// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S77 R36, R37, R40, R49, AC17: makeSnapshot puts what the last update left in world space and mm;
// foot contact comes only from the Animator's legs; the field table and the live JSON hold the same
// keys; the overlay pass takes no snapshot and draws no line while every toggle is off.
// Mesh Space Rotation (docs/specs/mesh-space-rotation.md R29, R30, AC19, AC20): Torso Direction
// draws two axes per boundary bone of the selected Animator, records only while it draws, and
// nothing while off; the warning's rule on generic and humanoid rigs.

#include <gtest/gtest.h>

#include "Scene.h"
#include "TempDir.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/AnimatorDebug.h"
#include "animation/AnimatorInstance.h"
#include "animation/AvatarMaskWeights.h"
#include "animation/CompiledController.h"
#include "pose/HumanBones.h"
#include "component/AnimatorComponent.h"
#include "component/Transform.h"
#include "object/Animator.h"
#include "pool/AnimationClipPool.h"
#include "pool/AnimatorAssetPool.h"
#include "subsystem/AnimatorSystem.h"
#include "subsystem/RenderSystem.h"

#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::anim;
using Json = nlohmann::json;

namespace {

    ClipKey debugKey(float time, std::vector<float> value){
        ClipKey k;
        k.time = time;
        k.value = std::move(value);
        return k;
    }

    ClipTrackData debugTrack(const std::string& path, std::vector<ClipKey> keys){
        ClipTrackData t;
        t.path = path;
        t.channel = ClipChannel::Position;
        t.keys = std::move(keys);
        return t;
    }

    Quaternion debugTurn(float degrees, float x, float y, float z){
        const float half = degrees * 3.14159265358979f / 360.0f;
        return Quaternion(std::cos(half), x * std::sin(half), y * std::sin(half), z * std::sin(half));
    }

    // bones held at rotations for a second
    ClipData debugHoldRotations(const std::vector<std::pair<std::string, Quaternion>>& rotations){
        ClipData data;
        data.wrap = ClipWrapMode::Loop;
        for (const auto& r : rotations){
            const std::vector<float> q = {r.second.x, r.second.y, r.second.z, r.second.w};
            ClipTrackData t = debugTrack(r.first, {debugKey(0, q), debugKey(1, q)});
            t.channel = ClipChannel::Rotation;
            data.tracks.push_back(t);
        }
        return data;
    }

    // Hero (x = 10) -> Hips -> Thigh -> Shin -> Foot: a 0.9 m leg from (10.1, 1, 0) down
    class AnimatorDebugTest : public ::testing::Test {
    protected:
        Scene scene;
        test::TempDir directory;
        Entity root = NULL_ENTITY, hips = NULL_ENTITY, thigh = NULL_ENTITY, shin = NULL_ENTITY, foot = NULL_ENTITY;

        Entity node(Entity parent, const std::string& name, Vector3 position){
            const Entity entity = scene.createEntity();
            scene.setEntityName(entity, name);
            Transform transform;
            transform.position = position;
            scene.addComponent<Transform>(entity, transform);
            if (parent != NULL_ENTITY) scene.addEntityChild(parent, entity, false);
            return entity;
        }

        void SetUp() override {
            AnimationClipPool::clear();
            AnimatorAssetPool::clear();
            root = node(NULL_ENTITY, "Hero", Vector3(10, 0, 0));
            hips = node(root, "Hips", Vector3(0, 1, 0));
            thigh = node(hips, "Thigh", Vector3(0.1f, 0, 0));
            shin = node(thigh, "Shin", Vector3(0, -0.45f, 0));
            foot = node(shin, "Foot", Vector3(0, -0.45f, 0));
        }

        void TearDown() override {
            AnimationClipPool::clear();
            AnimatorAssetPool::clear();
        }

        void resolveTransforms(){
            auto transforms = scene.getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++){
                Transform& transform = transforms->getComponentFromIndex(i);
                scene.getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
                transform.needUpdate = false;
            }
        }

        // one state playing `data`, a Float parameter "Contact"
        AnimatorComponent& animator(const ClipData& data){
            const std::string clip = directory.write("clip.anim", anim::writeClip(data)).generic_string();
            ControllerDef controller;
            ParameterDef contact;
            contact.name = "Contact";
            contact.type = AnimatorParameterType::Float;
            controller.parameters = {contact};
            StateDef state;
            state.name = "Play";
            state.motion.clip.path = clip;
            LayerDef layer;
            layer.name = "Base Layer";
            layer.weight = 1.0f;
            layer.machine.defaultState = "Play";
            layer.machine.states = {state};
            controller.layers = {layer};
            const std::string file = directory.write("c.controller", anim::writeController(controller)).generic_string();
            scene.addComponent<AnimatorComponent>(root, {});
            AnimatorComponent& component = scene.getComponent<AnimatorComponent>(root);
            component.controller = file;
            component.needsLoad = true;
            return component;
        }

        ClipData holdHips(){
            ClipData data;
            data.wrap = ClipWrapMode::Loop;
            data.tracks = {debugTrack("Hips", {debugKey(0, {0, 1, 0}), debugKey(1, {0, 1, 0})})};
            return data;
        }

        AnimatorIKConstraint leg(const std::string& name, AnimatorIKType type){
            AnimatorIKConstraint c;
            c.name = name;
            c.type = type;
            c.bones = {"Hips/Thigh", "Hips/Thigh/Shin", "Hips/Thigh/Shin/Foot"};
            return c;
        }

        void step(float dt){
            resolveTransforms();
            scene.getSystem<AnimatorSystem>()->update(dt);
        }

        AnimatorDebugSnapshot snapshot(){
            resolveTransforms();
            const AnimatorComponent& component = scene.getComponent<AnimatorComponent>(root);
            return makeSnapshot(*component.runtime, component, animatorRoot(scene.getComponent<Transform>(root)));
        }
    };

    // a snapshot with every field the JSON can hold set (a Foot IK on the ground, a locked Foot Lock, root motion)
    AnimatorDebugSnapshot fullSnapshot(){
        AnimatorDebugSnapshot s;
        s.valid = true;
        AnimatorDebugIK footIK;
        footIK.type = AnimatorIKType::FootIK;
        footIK.leg = true;
        footIK.grounded = true;
        AnimatorDebugIK footLock;
        footLock.type = AnimatorIKType::FootLock;
        footLock.leg = true;
        footLock.locked = true;
        s.ik = {footIK, footLock};
        s.rootMotion.active = true;
        s.contactSource = "foot_goals";
        return s;
    }

}

TEST_F(AnimatorDebugTest, TwoBoneReachAndChainInWorldSpace){
    AnimatorComponent& component = animator(holdHips());
    AnimatorIKConstraint arm = leg("Leg", AnimatorIKType::TwoBone);
    arm.targetPosition = Vector3(10.1f, 0.5f, 0.3f);
    component.ik = {arm};
    step(0.0f);
    step(0.02f);
    AnimatorDebugSnapshot s = snapshot();
    ASSERT_TRUE(s.valid);
    ASSERT_EQ(s.ik.size(), 1u);
    EXPECT_EQ(s.ik[0].name, "Leg");
    EXPECT_FLOAT_EQ(s.ik[0].weight, 1.0f);
    EXPECT_TRUE(s.ik[0].reached);
    EXPECT_LT(s.ik[0].reachErrorMm, 1.0f);
    ASSERT_EQ(s.ik[0].chain.size(), 3u);
    EXPECT_NEAR(s.ik[0].chain[0].x, 10.1f, 1e-4f) << "under the Animator's Transform (x = 10)";
    EXPECT_NEAR(s.ik[0].chain[0].y, 1.0f, 1e-4f);
    EXPECT_NEAR(s.ik[0].chain[2].z, 0.3f, 1e-3f) << "the end on the target";
    EXPECT_STREQ(s.contactSource, "none") << "a Two Bone is no foot goal";

    // out of reach: 3 m from the thigh, a 0.9 m leg
    component.ik[0].targetPosition = Vector3(10.1f, -2.0f, 0.0f);
    step(0.02f);
    s = snapshot();
    EXPECT_FALSE(s.ik[0].reached);
    const AnimatorIKRun& run = scene.getComponent<AnimatorComponent>(root).runtime->ik[0];
    EXPECT_NEAR(s.ik[0].reachErrorMm, run.reachError * 1000.0f, 1e-3f) << "metres to mm here only";
    EXPECT_GT(s.ik[0].reachErrorMm, 2000.0f);
    EXPECT_EQ(animatorIKStateLabel(s.ik[0]), "Out of Reach");
    Json item = Json::object();
    addAnimatorDebugIKJson(s.ik[0], item);
    EXPECT_DOUBLE_EQ(item["reach_error_mm"].get<double>(), animatorDebugValue(s.ik[0].reachErrorMm));
    EXPECT_EQ(item["reached"], false);
}

TEST_F(AnimatorDebugTest, FootLockSlideAndDriftComeFromItsContact){
    AnimatorComponent& component = animator(holdHips());
    AnimatorIKConstraint lock = leg("Lock", AnimatorIKType::FootLock);
    lock.contactParameter = "Contact";
    component.ik = {lock};
    step(0.0f);
    Animator(&scene, root).setFloat("Contact", 1.0f);
    // the GameObject moves 1 cm a frame of 0.02 s: a foot the clip holds still slides at 0.5 m/s
    for (int i = 0; i < 6; i++){
        scene.getComponent<Transform>(root).position.x += 0.01f;
        scene.getComponent<Transform>(root).needUpdate = true;
        step(0.02f);
    }
    AnimatorDebugSnapshot s = snapshot();
    ASSERT_EQ(s.ik.size(), 1u);
    EXPECT_TRUE(s.ik[0].leg);
    EXPECT_TRUE(s.ik[0].contact);
    EXPECT_TRUE(s.ik[0].locked);
    EXPECT_NEAR(s.ik[0].slideMmS, 500.0f, 5.0f);
    EXPECT_STREQ(s.contactSource, "foot_goals");
    EXPECT_NEAR(s.slideMmS, 500.0f, 5.0f);
    EXPECT_TRUE(s.sliding);
    EXPECT_NEAR(s.ik[0].lockDriftMm, 50.0f, 1.0f) << "the clip carried the foot 5 cm from where it locked";
    EXPECT_EQ(animatorIKStateLabel(s.ik[0]), "Locked");

    Animator(&scene, root).setFloat("Contact", 0.0f);
    step(0.02f);
    s = snapshot();
    EXPECT_FALSE(s.ik[0].contact);
    EXPECT_FLOAT_EQ(s.slideMmS, 0.0f) << "a foot in the air does not slide";
    EXPECT_FALSE(s.sliding);
}

TEST_F(AnimatorDebugTest, RootMotionSpeedYawAndTotal){
    ClipData data;
    data.wrap = ClipWrapMode::Loop;
    data.tracks = {debugTrack("Hips", {debugKey(0, {0, 1, 0}), debugKey(1, {0, 1, 2})})};
    AnimatorComponent& component = animator(data);
    component.applyRootMotion = true;
    step(0.0f);
    for (int i = 0; i < 25; i++) step(0.02f);
    const AnimatorDebugSnapshot s = snapshot();
    ASSERT_TRUE(s.rootMotion.active);
    EXPECT_NEAR(s.rootMotion.speedMps, 2.0f, 1e-3f) << "2 m over the 1 s clip";
    EXPECT_NEAR(s.rootMotion.yawDegS, 0.0f, 1e-4f);
    EXPECT_NEAR(s.rootMotion.accumulatedPosition.z, 1.0f, 1e-3f) << "25 frames of 0.02 s";
    EXPECT_NEAR(scene.getComponent<Transform>(root).position.z, s.rootMotion.accumulatedPosition.z, 1e-4f);

    Json rootMotion = Json::object();
    addAnimatorDebugRootMotionJson(s, rootMotion);
    EXPECT_DOUBLE_EQ(rootMotion["speed_mps"].get<double>(), animatorDebugValue(s.rootMotion.speedMps));
    EXPECT_EQ(rootMotion["contact_source"], "none") << "no Foot IK or Foot Lock: no guess (R37)";
    EXPECT_FALSE(rootMotion.contains("slide_mm_s"));
    EXPECT_FALSE(rootMotion.contains("foot_speed_mps"));
}

TEST(AnimatorDebug, FootIKPelvisOffsetIsTheStacksDrop){
    AnimatorInstance instance;
    instance.controller = std::make_shared<CompiledController>();
    instance.bound = true;
    instance.evaluations = 1;
    AnimatorComponent component;
    component.ik.resize(2);
    instance.ik.resize(2);
    const float offsets[2] = {-0.03f, -0.05f};
    const float weights[2] = {1.0f, 0.5f};
    for (size_t i = 0; i < 2; i++){
        component.ik[i].type = AnimatorIKType::FootIK;
        AnimatorIKRun& run = instance.ik[i];
        run.boneCount = 3;
        run.bones[0] = 0;
        run.bones[1] = 1;
        run.bones[2] = 2;
        run.pelvis = 0;
        run.offset = offsets[i];
        run.solvedWeight = weights[i];
        run.reachError = 0.004f;
    }
    Transform root;
    root.worldScale = Vector3(2, 2, 2);
    const AnimatorDebugSnapshot s = makeSnapshot(instance, component, animatorRoot(root));
    ASSERT_EQ(s.ik.size(), 2u);
    EXPECT_NEAR(s.ik[0].pelvisOffsetMm, -60.0f, 1e-3f) << "the lowest weighted foot (3 cm), at scale 2";
    EXPECT_NEAR(s.ik[1].pelvisOffsetMm, -60.0f, 1e-3f) << "one drop for the stack";
    EXPECT_NEAR(s.ik[0].reachErrorMm, 4.0f, 1e-4f);
    EXPECT_EQ(animatorIKStateLabel(s.ik[0]), "No Ground");

    instance.evaluations = 0;
    EXPECT_FALSE(makeSnapshot(instance, component, animatorRoot(root)).valid) << "an Animator that never ran has nothing to show";
}

TEST(AnimatorDebug, FieldTableEqualsTheLiveJson){
    const AnimatorDebugSnapshot s = fullSnapshot();
    std::set<std::string> numbers;
    auto collect = [&](const Json& object){
        for (auto it = object.begin(); it != object.end(); ++it){
            if (it.value().is_number()) numbers.insert(it.key());
        }
    };
    for (const AnimatorDebugIK& ik : s.ik){
        Json item = Json::object();
        addAnimatorDebugIKJson(ik, item);
        collect(item);
    }
    Json rootMotion = Json::object();
    addAnimatorDebugRootMotionJson(s, rootMotion);
    collect(rootMotion);
    for (const AnimatorDebugField& field : kAnimatorDebugFields){
        EXPECT_TRUE(numbers.count(field.key)) << field.key << " is in the table and not in the JSON";
    }
    for (const std::string& key : numbers){
        EXPECT_NE(findAnimatorDebugField(key), nullptr) << key << " is in the JSON and not in the table";
    }
}

TEST(AnimatorDebug, FormatsAsTheInspectorShows){
    EXPECT_EQ(formatAnimatorDebugField("weight", 0.8f), "0.80");
    EXPECT_EQ(formatAnimatorDebugField("reach_error_mm", 4.2391), "4.2 mm");
    EXPECT_EQ(formatAnimatorDebugField("pelvis_offset_mm", -21.04), "-21.0 mm");
    EXPECT_EQ(formatAnimatorDebugField("pelvis_offset_mm", -0.001), "0.0 mm") << "no minus zero";
    EXPECT_EQ(formatAnimatorDebugField("speed_mps", 3.1249), "3.12 m/s");
    EXPECT_EQ(formatAnimatorDebugField("yaw_deg_s", 4.1), "4.1 deg/s");
    EXPECT_EQ(formatAnimatorDebugField("slide_mm_s", 280.4), "280 mm/s");
    EXPECT_DOUBLE_EQ(animatorDebugValue(-0.001), 0.0);
    EXPECT_FALSE(std::signbit(animatorDebugValue(-0.001)));
    EXPECT_DOUBLE_EQ(animatorDebugValue(3.14159), 3.14);
}

TEST(AnimatorDebug, FootTrailColours){
    EXPECT_EQ(footTrailColor(true, 0.0f), overlaycolor::planted) << "planted and still: green";
    const Vector4 slow = footTrailColor(true, kAnimatorSlideWarningMmS);
    const Vector4 fast = footTrailColor(true, 10.0f * kAnimatorSlideWarningMmS);
    EXPECT_FLOAT_EQ(slow.x, 1.0f);
    EXPECT_GT(slow.y, fast.y) << "yellow toward red as it slides faster";
    EXPECT_NEAR(fast.y, 0.2f, 1e-5f);
    EXPECT_LT(footTrailColor(false, 0.0f).w, 1.0f) << "in the air: dim";
}

TEST_F(AnimatorDebugTest, OverlayPassDoesNothingWhileOff){
    AnimatorComponent& component = animator(holdHips());
    AnimatorIKConstraint arm = leg("Leg", AnimatorIKType::TwoBone);
    arm.targetPosition = Vector3(10.1f, -2.0f, 0.0f);
    component.ik = {arm};
    step(0.0f);
    step(0.02f);
    resolveTransforms();

    AnimatorOverlayPass pass;
    int draws = 0;
    std::vector<AnimatorOverlayLine> drawn;
    const AnimatorOverlayPass::Draw draw = [&](Entity, const std::vector<AnimatorOverlayLine>& lines){
        draws++;
        drawn = lines;
    };
    pass.update(&scene, {root, hips}, AnimatorOverlayFlags(), draw);
    EXPECT_EQ(draws, 0);
    EXPECT_EQ(pass.snapshots(), 0u) << "off: no snapshot (R40)";
    EXPECT_EQ(pass.lines(), 0u) << "off: no line";

    AnimatorOverlayFlags ik;
    ik.ik = true;
    pass.update(&scene, {root, hips}, ik, draw);
    EXPECT_EQ(draws, 1) << "one Animator";
    EXPECT_EQ(pass.snapshots(), 1u);
    EXPECT_GT(pass.lines(), 0u);
    bool red = false, cyan = false;
    for (const AnimatorOverlayLine& line : drawn){
        red = red || line.color == overlaycolor::unreached;
        cyan = cyan || line.color == overlaycolor::chain;
    }
    EXPECT_TRUE(red) << "the target out of reach is red";
    EXPECT_TRUE(cyan) << "the chain is cyan";

    const uint64_t lines = pass.lines();
    pass.update(&scene, {root, hips}, AnimatorOverlayFlags(), draw);
    EXPECT_EQ(draws, 1);
    EXPECT_EQ(pass.snapshots(), 1u);
    EXPECT_EQ(pass.lines(), lines);
}

// AC20 (R30): the spec's AC4 setup, its Spine the one boundary bone. Off (or nothing selected)
// nothing is drawn and the runtime records nothing; on, the selected Animator gets the corrected
// axis at once and the ghost without the pass from the next update: two axes per boundary bone
TEST_F(AnimatorDebugTest, TorsoDirectionDrawsTwoAxesPerBoundaryBoneOfTheSelectedAnimator){
    const Entity spine = node(hips, "Spine", Vector3(0, 0.3f, 0));
    (void)spine;
    const std::string run = directory.write("run.anim", anim::writeClip(debugHoldRotations({{"Hips", debugTurn(90, 0, 1, 0)}}))).generic_string();
    const std::string attack = directory.write("attack.anim", anim::writeClip(debugHoldRotations(
        {{"Hips", debugTurn(-20, 0, 1, 0)}, {"Hips/Spine", debugTurn(40, 1, 0, 0)}}))).generic_string();
    AvatarMaskDef mask;
    mask.defaultWeight = 0.0f;
    mask.paths.push_back({"Hips/Spine", 1.0f, true});
    ControllerDef controller;
    for (const std::string& clip : {run, attack}){
        LayerDef layer;
        layer.name = clip == run ? "Base Layer" : "Upper Body";
        StateDef state;
        state.name = layer.name;
        state.motion.clip.path = clip;
        layer.machine.defaultState = state.name;
        layer.machine.states = {state};
        controller.layers.push_back(layer);
    }
    controller.layers[1].mask.path = directory.write("spine.mask", anim::writeMask(mask)).generic_string();
    controller.layers[1].meshSpaceRotation = true;
    scene.addComponent<AnimatorComponent>(root, {});
    AnimatorComponent& component = scene.getComponent<AnimatorComponent>(root);
    component.controller = directory.write("torso.controller", anim::writeController(controller)).generic_string();
    component.needsLoad = true;
    step(0.0f);
    step(0.02f);
    resolveTransforms();
    AnimatorInstance& instance = *component.runtime;
    ASSERT_TRUE(instance.layerMeshSpaceApplies(1));
    ASSERT_EQ(instance.layerBoundaryBones(1).size(), 1u);

    AnimatorOverlayPass pass;
    int draws = 0;
    std::vector<AnimatorOverlayLine> drawn;
    const AnimatorOverlayPass::Draw draw = [&](Entity, const std::vector<AnimatorOverlayLine>& lines){
        draws++;
        drawn = lines;
    };
    pass.update(&scene, {root, hips}, AnimatorOverlayFlags(), draw, {root});
    EXPECT_EQ(draws, 0);
    EXPECT_EQ(pass.lines(), 0u) << "off: no line";
    EXPECT_FALSE(instance.meshSpaceDebug()) << "off: nothing recorded";
    AnimatorOverlayFlags torso;
    torso.torsoDirection = true;
    pass.update(&scene, {root, hips}, torso, draw, {});
    EXPECT_EQ(draws, 0) << "only the selected Animators";
    EXPECT_FALSE(instance.meshSpaceDebug());

    pass.update(&scene, {root, hips}, torso, draw, {root});
    EXPECT_EQ(draws, 1);
    EXPECT_TRUE(instance.meshSpaceDebug()) << "recording while it draws the Animator";
    ASSERT_EQ(drawn.size(), kTorsoAxisLines) << "no update recorded the ghost yet: the corrected axis only";
    step(0.02f);
    pass.update(&scene, {root, hips}, torso, draw, {root});
    ASSERT_EQ(drawn.size(), 2 * kTorsoAxisLines) << "two axes per boundary bone";
    EXPECT_EQ(pass.snapshots(), 0u) << "Torso Direction takes no IK snapshot";
    // rest rotations are identity, so an axis is the bone's rig rotation turning +Z: the layer's
    // (hips -20 degrees, spine 40) with the pass, the base's hips under it without
    const Vector3 at(10.0f, 1.3f, 0.0f);
    const Vector3 corrected = (debugTurn(-20, 0, 1, 0) * debugTurn(40, 1, 0, 0)) * Vector3(0, 0, 1);
    const Vector3 ghost = (debugTurn(90, 0, 1, 0) * debugTurn(40, 1, 0, 0)) * Vector3(0, 0, 1);
    for (size_t k = 0; k < 2; k++){
        const Vector4& color = k == 0 ? overlaycolor::applied : overlaycolor::rejected;
        for (size_t i = 0; i < kTorsoAxisLines; i++) EXPECT_EQ(drawn[k * kTorsoAxisLines + i].color, color) << "axis " << k << ", line " << i;
        // its shaft
        const AnimatorOverlayLine& shaft = drawn[k * kTorsoAxisLines];
        const Vector3 expected = at + (k == 0 ? corrected : ghost) * kTorsoAxisLength;
        EXPECT_NEAR((shaft.a - at).length(), 0.0f, 1e-4f) << "from the spine, under the Animator's Transform";
        EXPECT_NEAR((shaft.b - expected).length(), 0.0f, 1e-4f) << (k == 0 ? "the corrected axis" : "the ghost");
    }

    const uint64_t lines = pass.lines();
    pass.update(&scene, {root, hips}, AnimatorOverlayFlags(), draw, {root});
    EXPECT_EQ(draws, 2);
    EXPECT_EQ(pass.lines(), lines);
    EXPECT_FALSE(instance.meshSpaceDebug()) << "off again: the recording stops";
}

// R29 on a bound instance made by hand: the warning names the humanoid's Spine, Chest or Upper
// Chest, or on a generic rig the bone under one the mask leaves out; an Additive layer, the option
// on, weight 0 or a boundary elsewhere on a humanoid give none
TEST(AnimatorMeshSpaceWarning, NamesTheTorsoWhereTheMaskStarts){
    ControllerDef def;
    def.layers.resize(2);
    def.layers[0].name = "Base Layer";
    def.layers[1].name = "Upper Body";
    AnimatorInstance instance;
    instance.controller = compileController(def);
    instance.bound = true;
    for (const char* name : {"B-hips", "B-spine", "B-chest"}){
        RigBone bone;
        bone.name = name;
        bone.parent = (int)instance.rig.bones.size() - 1;
        instance.rig.bones.push_back(bone);
        instance.rig.parents.push_back(bone.parent);
    }
    instance.layers.resize(2);
    instance.layers[1].weight = 1.0f;
    instance.layers[1].mask = {0.0f, 1.0f, 1.0f};
    instance.layers[1].boundaryBones = {1};
    EXPECT_EQ(meshSpaceBoundaryNames(instance, 1), std::vector<std::string>{"B-spine"});

    std::vector<MeshSpaceWarning> warnings = meshSpaceWarnings(def, instance);
    ASSERT_EQ(warnings.size(), 1u) << "a generic rig: the child of the hips the mask leaves out";
    EXPECT_EQ(warnings[0].layer, 1);
    EXPECT_EQ(warnings[0].message, "Layer 'Upper Body': the mask starts at B-spine, so the torso follows the base layer's hips. "
                                   "Turn on Mesh Space Rotation to keep this layer's direction.");
    instance.humanoid.valid = true;
    for (pose::HumanBone slot : {pose::HumanBone::Spine, pose::HumanBone::Chest, pose::HumanBone::UpperChest}){
        instance.humanoid.slotBones.fill(-1);
        instance.humanoid.slotBones[(size_t)pose::HumanBone::Hips] = 0;
        instance.humanoid.slotBones[(size_t)slot] = 1;
        warnings = meshSpaceWarnings(def, instance);
        ASSERT_EQ(warnings.size(), 1u) << pose::humanBoneName(slot);
        const char* label = slot == pose::HumanBone::Spine ? "Spine" : slot == pose::HumanBone::Chest ? "Chest" : "Upper Chest";
        EXPECT_EQ(warnings[0].message, std::string("Layer 'Upper Body': the mask starts at ") + label +
                                       ", so the torso follows the base layer's hips. Turn on Mesh Space Rotation to keep this layer's direction.");
    }
    instance.humanoid.slotBones.fill(-1);
    instance.humanoid.slotBones[(size_t)pose::HumanBone::Hips] = 0;
    instance.humanoid.slotBones[(size_t)pose::HumanBone::LeftUpperLeg] = 1;
    EXPECT_TRUE(meshSpaceWarnings(def, instance).empty()) << "a humanoid's boundary outside the torso";
    instance.humanoid.slotBones[(size_t)pose::HumanBone::LeftUpperLeg] = -1;
    instance.humanoid.slotBones[(size_t)pose::HumanBone::Spine] = 1;

    ControllerDef changed = def;
    changed.layers[1].meshSpaceRotation = true;
    EXPECT_TRUE(meshSpaceWarnings(changed, instance).empty()) << "the option on";
    instance.setLayerMeshSpaceRotation(1, true);
    EXPECT_TRUE(meshSpaceWarnings(def, instance).empty()) << "the runtime's value: a Lua override that turns it on";
    instance.setLayerMeshSpaceRotation(1, false);
    EXPECT_EQ(meshSpaceWarnings(def, instance).size(), 1u) << "the override off: the warning is back";
    changed = def;
    changed.layers[1].blending = AnimatorLayerBlending::Additive;
    EXPECT_TRUE(meshSpaceWarnings(changed, instance).empty()) << "an Additive layer";
    instance.layers[1].weight = 0.0f;
    EXPECT_TRUE(meshSpaceWarnings(def, instance).empty()) << "weight 0";
    instance.layers[1].weight = 1.0f;
    instance.bound = false;
    EXPECT_TRUE(meshSpaceWarnings(def, instance).empty()) << "not bound";
}

// R29's one rule: the warning shows only where the mask starts at a torso bone under a torso bone
// it leaves out (by humanoid slot, else by name), named at the first such start in rig order. A
// mask that takes the hips under B-root or Armature, or one from an arm, splits no torso
TEST(AnimatorMeshSpaceWarning, OnlyWhereTheMaskSplitsTheTorso){
    ControllerDef def;
    def.layers.resize(2);
    def.layers[0].name = "Base Layer";
    def.layers[1].name = "Upper Body";
    auto rigged = [&](const std::vector<std::pair<std::string, int>>& bones){
        AnimatorInstance instance;
        instance.controller = compileController(def);
        instance.bound = true;
        for (const auto& entry : bones){
            RigBone bone;
            bone.name = entry.first;
            bone.parent = entry.second;
            instance.rig.bones.push_back(bone);
            instance.rig.parents.push_back(entry.second);
        }
        instance.layers.resize(2);
        instance.layers[1].weight = 1.0f;
        return instance;
    };
    auto warned = [&](AnimatorInstance& instance, const std::vector<float>& mask){
        instance.layers[1].mask = mask;
        anim::maskBoundaryBones(mask.data(), instance.rig.parents.data(), mask.size(), -1, instance.layers[1].boundaryBones);
        const std::vector<MeshSpaceWarning> warnings = meshSpaceWarnings(def, instance);
        if (warnings.empty()) return std::string();
        const std::string& message = warnings[0].message;
        const size_t at = message.find("starts at ");
        return at == std::string::npos ? message : message.substr(at + 10, message.find(',', at) - at - 10);
    };

    // a B- rig without a humanoid: B-root > B-hips > B-thigh.L, B-hips > B-spine > B-chest > B-shoulder.L
    AnimatorInstance generic = rigged({{"B-root", -1}, {"B-hips", 0}, {"B-thigh.L", 1}, {"B-spine", 1}, {"B-chest", 3}, {"B-shoulder.L", 4}});
    EXPECT_EQ(warned(generic, {0, 1, 1, 1, 1, 1}), "") << "the whole body under B-root: the hips are in the mask";
    EXPECT_EQ(warned(generic, {0, 0, 0, 0, 0, 1}), "") << "an arm mask: the torso is not split";
    EXPECT_EQ(warned(generic, {0, 0, 0, 1, 1, 1}), "B-spine");
    EXPECT_EQ(warned(generic, {0, 0, 1, 1, 1, 1}), "B-spine") << "the first start that splits the torso, not the first start";

    // a humanoid: Armature > Hips > Spine > LeftShoulder
    AnimatorInstance human = rigged({{"Armature", -1}, {"Hips", 0}, {"Spine", 1}, {"LeftShoulder", 2}});
    human.humanoid.valid = true;
    human.humanoid.slotBones.fill(-1);
    human.humanoid.slotBones[(size_t)pose::HumanBone::Hips] = 1;
    human.humanoid.slotBones[(size_t)pose::HumanBone::Spine] = 2;
    human.humanoid.slotBones[(size_t)pose::HumanBone::LeftShoulder] = 3;
    EXPECT_EQ(warned(human, {0, 1, 1, 1}), "") << "the whole body under Armature";
    EXPECT_EQ(warned(human, {0, 0, 0, 1}), "") << "an arm mask";
    EXPECT_EQ(warned(human, {0, 0, 1, 1}), "Spine");
}
