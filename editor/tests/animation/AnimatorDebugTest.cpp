// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S77 R36, R37, R40, R49, AC17: makeSnapshot puts what the last update left in world space and mm;
// foot contact comes only from the Animator's legs; the field table and the live JSON hold the same
// keys; the overlay pass takes no snapshot and draws no line while every toggle is off.

#include <gtest/gtest.h>

#include "Scene.h"
#include "TempDir.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/AnimatorDebug.h"
#include "animation/AnimatorInstance.h"
#include "animation/CompiledController.h"
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
