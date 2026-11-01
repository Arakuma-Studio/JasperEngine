// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Upstream's humanoid clips and Avatars on S21's Animator (docs/specs/S21-animation.md §12,
// "Retargeting"): one retarget path for the Animator and the Animation Player, so the same clip
// with the same options gives the same bones on both. Written red first: the Animator ignored
// humanoid tracks (.anim v2, an Animation Type: Humanoid model's clips), a state's Mirror, Foot IK
// and Retarget (upstream's controllers, kept as unknown keys), the Avatar's twist corrections, a
// slot the target lacks (S21 dropped its rotation instead of folding it) and the fingers.

#include <gtest/gtest.h>

#include "AnimatorTestUtil.h"

#include "animation/AnimationClipIO.h"
#include "animation/ClipBinding.h"
#include "animation/HumanoidClip.h"
#include "component/ModelComponent.h"
#include "object/AnimationPlayer.h"
#include "object/Model.h"
#include "pool/AnimationClipPool.h"
#include "pose/HumanoidMapIO.h"
#include "pose/PoseMath.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/MeshSystem.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::anim;
using namespace animatortest;

namespace {

    constexpr float kDt = 1.0f / 30.0f;

    std::string readFixture(const std::string& name){
        std::ifstream file(std::string(JASPER_TEST_REPO_DIR "/tools/fixtures/pose-animation/assets/models/") + name, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    double degreesBetween(const Quaternion& a, const Quaternion& b){
        return pose::radToDeg(pose::angleBetween(pose::Quatd(a).normalized(), pose::Quatd(b).normalized()));
    }

    // The same model twice (or two models): the Animator on one, the Animation Player on the other
    class AnimatorHumanoid : public ::testing::Test, public AnimatorFixture {
    protected:
        std::vector<std::unique_ptr<Model>> models;
        std::string hero, robot, tpose;

        void SetUp() override{
            hero = prefix + "hero_animated.gltf";
            robot = prefix + "robot_apose.gltf";
            tpose = prefix + "hero_tpose.gltf";
            animtest::writeAsset(hero, readFixture("hero_animated.gltf"));
            animtest::writeAsset(robot, readFixture("robot_apose.gltf"));
            animtest::writeAsset(tpose, readFixture("hero_tpose.gltf"));
        }

        Entity load(const std::string& path, float x){
            models.push_back(std::make_unique<Model>(&scene));
            const Entity entity = models.back()->getEntity();
            EXPECT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(entity, path, false, false, false));
            scene.getComponent<ModelComponent>(entity).needUpdateModel = false;
            scene.getComponent<ModelComponent>(entity).filename = path;
            scene.getComponent<Transform>(entity).position = Vector3(x, 0, 0);
            return entity;
        }

        // both players advance: the Animation Player (ActionSystem) and the Animator
        void run(int frames){
            scene.getSystem<ActionSystem>()->update(0.0);
            step(0.0);
            for (int i = 0; i < frames; i++){
                scene.getSystem<ActionSystem>()->update(kDt);
                step(kDt);
            }
        }

        // a player on `entity` with one entry
        void player(Entity entity, const std::string& clip, bool retarget, bool mirror = false, bool footIK = false){
            AnimationPlayer object(&scene, entity);
            AnimationClipOptions options;
            options.retarget = retarget;
            options.mirror = mirror;
            options.footIK = footIK;
            ASSERT_TRUE(object.addClip(clip, "Clip", options));
            object.setPlayAutomatically(true);
        }

        Entity findByName(Entity root, const std::string& name){
            std::vector<Entity> stack = {root};
            while (!stack.empty()){
                const Entity e = stack.back();
                stack.pop_back();
                if (scene.getEntityNameRef(e) == name) return e;
                auto transforms = scene.getComponentArray<Transform>();
                for (size_t i = 0; i < transforms->size(); i++){
                    if (transforms->getComponentFromIndex(i).parent == e) stack.push_back(transforms->getEntity(i));
                }
            }
            return NULL_ENTITY;
        }

        // the worst difference between the same-named bones of two copies of one model
        struct Difference{ double degrees = 0.0; double meters = 0.0; std::string bone; };
        Difference compare(Entity a, Entity b, const std::vector<std::string>& bones){
            Difference worst;
            for (const std::string& name : bones){
                const Entity ea = findByName(a, name);
                const Entity eb = findByName(b, name);
                EXPECT_NE(ea, NULL_ENTITY) << name;
                EXPECT_NE(eb, NULL_ENTITY) << name;
                if (ea == NULL_ENTITY || eb == NULL_ENTITY) continue;
                const double d = degreesBetween(rotation(ea), rotation(eb));
                const double m = (pose::Vec3d(position(ea)) - pose::Vec3d(position(eb))).length();
                if (d > worst.degrees){ worst.degrees = d; worst.bone = name; }
                worst.meters = std::max(worst.meters, m);
            }
            return worst;
        }

        // how far `entity`'s bones are from their rest (the clip moves them)
        double motion(Entity entity, const std::string& bone){
            const Entity e = findByName(entity, bone);
            if (e == NULL_ENTITY || !restOf.count(e)) return 0.0;
            return degreesBetween(rotation(e), restOf[e]);
        }

        // every bone's rotation now (before playing: the rest)
        void rememberRest(){
            auto transforms = scene.getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++) restOf[transforms->getEntity(i)] = transforms->getComponentFromIndex(i).rotation;
        }

        std::string humanoidWalk(){
            std::shared_ptr<const HumanoidSource> source;
            std::string error;
            EXPECT_TRUE(HumanoidSourceCache::get(hero, source, &error)) << error;
            std::shared_ptr<const AnimationClip> walk = AnimationClipPool::getOrLoad(hero + "#Walk", &error);
            EXPECT_TRUE(walk) << error;
            ClipData converted;
            std::vector<ClipWarning> warnings;
            EXPECT_TRUE(convertToHumanoid(walk->toData(), *source, converted, warnings, &error)) << error;
            EXPECT_TRUE(converted.isHumanoid());
            const std::string path = prefix + "walk_h.anim";
            animtest::writeAsset(path, writeClip(converted));
            return path;
        }

        std::map<Entity, Quaternion> restOf;
    };

    const std::vector<std::string> ROBOT_BONES = {"pelvis", "spine", "spine.001", "neck", "head", "shoulder.L", "upper_arm.L",
                                                  "forearm.L", "hand.L", "shoulder.R", "upper_arm.R", "forearm.R", "hand.R",
                                                  "thigh.L", "shin.L", "foot.L", "thigh.R", "shin.R", "foot.R"};
    const std::vector<std::string> HERO_FINGERS = {"mixamorig:LeftHandIndex1", "mixamorig:LeftHandIndex2", "mixamorig:LeftHandThumb1",
                                                   "mixamorig:RightHandIndex1", "mixamorig:RightHandMiddle1"};

}

// a humanoid clip (.anim version 2, upstream's Convert to Humanoid Clip) plays on the Animator as
// the Animation Player plays it
TEST_F(AnimatorHumanoid, HumanoidClipPlaysLikeThePlayer){
    const std::string walk = humanoidWalk();
    const Entity a = load(robot, -1);
    const Entity b = load(robot, 1);
    rememberRest();
    animator(controllerFile("walk", controller({}, {state("Walk", walk)})), a);
    player(b, walk, false);
    run(12);
    EXPECT_GT(motion(b, "thigh.L"), 1.0) << "the player walks";
    EXPECT_GT(motion(a, "thigh.L"), 1.0) << "the Animator walks the humanoid clip";
    const Difference d = compare(a, b, ROBOT_BONES);
    EXPECT_LT(d.degrees, 0.05) << d.bone;
    EXPECT_LT(d.meters, 1e-4);
}

// a generic clip of another humanoid: the Animator's retargeting (Auto) and a Retarget entry give
// the same bones, with the hero's upper chest (Spine2) folded into the robot, which has none
TEST_F(AnimatorHumanoid, RetargetedClipPlaysLikeARetargetEntry){
    const Entity a = load(robot, -1);
    const Entity b = load(robot, 1);
    rememberRest();
    animator(controllerFile("walk", controller({}, {state("Walk", hero + "#Walk")})), a);
    player(b, hero + "#Walk", true);
    run(12);
    EXPECT_GT(motion(a, "thigh.L"), 1.0);
    const Difference d = compare(a, b, ROBOT_BONES);
    EXPECT_LT(d.degrees, 0.05) << d.bone;
    EXPECT_LT(d.meters, 1e-4);
}

// the fingers (upstream's extra bones) retarget too: a curl made on the hero plays on the T-pose
// hero (another file) on the Animator as on a Retarget entry
TEST_F(AnimatorHumanoid, FingersRetargetLikeARetargetEntry){
    const Entity heroEntity = load(hero, -3);
    std::string indexPath, thumbPath, hipsPath;
    ASSERT_TRUE(relativePath(&scene, heroEntity, findByName(heroEntity, "mixamorig:LeftHandIndex1"), indexPath));
    ASSERT_TRUE(relativePath(&scene, heroEntity, findByName(heroEntity, "mixamorig:LeftHandThumb1"), thumbPath));
    ASSERT_TRUE(relativePath(&scene, heroEntity, findByName(heroEntity, "mixamorig:Hips"), hipsPath));
    const Quaternion indexRest = rotation(findByName(heroEntity, "mixamorig:LeftHandIndex1"));
    const Quaternion thumbRest = rotation(findByName(heroEntity, "mixamorig:LeftHandThumb1"));
    ClipData data;
    data.previewModel = hero;
    data.tracks.push_back(track(indexPath, ClipChannel::Rotation, {key(0, {indexRest.x, indexRest.y, indexRest.z, indexRest.w}),
        key(1, {0.0f, 0.0f, 0.0f, 1.0f})}));
    data.tracks.back().keys[1].value = {0.4f, 0.0f, 0.0f, 0.9165f};
    data.tracks.push_back(track(thumbPath, ClipChannel::Rotation, {key(0, {thumbRest.x, thumbRest.y, thumbRest.z, thumbRest.w}),
        key(1, {0.0f, 0.3f, 0.0f, 0.9539f})}));
    const std::string curl = clip("curl", data);

    const Entity a = load(tpose, -1);
    const Entity b = load(tpose, 1);
    rememberRest();
    animator(controllerFile("curl", controller({}, {state("Curl", curl)})), a);
    player(b, curl, true);
    run(15);
    EXPECT_GT(motion(b, "mixamorig:LeftHandIndex1"), 1.0) << "the Retarget entry curls the finger";
    EXPECT_GT(motion(a, "mixamorig:LeftHandIndex1"), 1.0) << "the Animator curls it too";
    const Difference d = compare(a, b, HERO_FINGERS);
    EXPECT_LT(d.degrees, 0.05) << d.bone;
}

// upstream's controller (version 1, its dialect) with a state's Mirror and Foot IK: the Animator
// plays it as upstream's Animation Player played the state (an entry with Mirror and Foot IK)
TEST_F(AnimatorHumanoid, UpstreamStateMirrorAndFootIKPlayLikeThePlayer){
    const std::string walk = humanoidWalk();
    const std::string text = std::string(R"({"format": "jasperengine.controller", "version": 1, "parameters": [],
      "layers": [{"name": "Base Layer", "default_state": "Walk",
        "states": [{"name": "Walk", "clip": ")") + walk + R"(", "mirror": true, "foot_ik": true}], "transitions": []}]})";
    animtest::writeAsset(prefix + "mirrored.controller", text);
    const Entity a = load(robot, -1);
    const Entity b = load(robot, 1);
    const Entity c = load(robot, 3);
    animator(prefix + "mirrored.controller", a);
    player(b, walk, false, true, true);
    player(c, walk, false);
    run(12);
    const Difference mirrored = compare(b, c, {"thigh.L", "thigh.R"});
    EXPECT_GT(mirrored.degrees, 1.0) << "Mirror changes the legs";
    const Difference d = compare(a, b, ROBOT_BONES);
    EXPECT_LT(d.degrees, 0.05) << d.bone;
    EXPECT_LT(d.meters, 1e-4);
}

// a migrated upstream controller (the Animator's Retarget Off, as upstream's player played it)
// whose state has upstream's Retarget: the state still retargets, as upstream's did
TEST_F(AnimatorHumanoid, UpstreamStateRetargetOnAMigratedAnimator){
    const std::string text = std::string(R"({"format": "jasperengine.controller", "version": 1, "parameters": [],
      "layers": [{"name": "Base Layer", "default_state": "Walk",
        "states": [{"name": "Walk", "clip": ")") + hero + R"(#Walk", "retarget": true}], "transitions": []}]})";
    animtest::writeAsset(prefix + "retarget.controller", text);
    const Entity a = load(robot, -1);
    const Entity b = load(robot, 1);
    rememberRest();
    animator(prefix + "retarget.controller", a).retarget = AnimatorRetarget::Off;
    player(b, hero + "#Walk", true);
    run(12);
    EXPECT_GT(motion(a, "thigh.L"), 1.0) << "the state's Retarget plays the hero's walk on the robot";
    const Difference d = compare(a, b, ROBOT_BONES);
    EXPECT_LT(d.degrees, 0.05) << d.bone;
}

// the Avatar's twist corrections (.humanoid version 2) apply on the Animator as on the player
TEST_F(AnimatorHumanoid, AvatarTwistAppliesLikeThePlayer){
    const std::string walk = humanoidWalk();
    animtest::writeAsset(robot + ".humanoid", R"({"format": "jasperengine.humanoid", "version": 2, "bones": {},
      "twist": {"left_lower_arm": 40, "right_upper_arm": -30}})");
    AnimationClipPool::invalidateModel(robot);
    const Entity a = load(robot, -1);
    const Entity b = load(robot, 1);
    animator(controllerFile("walk", controller({}, {state("Walk", walk)})), a);
    player(b, walk, false);
    run(12);
    const Difference d = compare(a, b, ROBOT_BONES);
    EXPECT_LT(d.degrees, 0.05) << d.bone;
}

// an Animation Type: Humanoid model serves its clips as humanoid clips: the Animator plays them
TEST_F(AnimatorHumanoid, HumanoidModelClipsPlayLikeThePlayer){
    animtest::writeAsset(hero + ".humanoid", R"({"format": "jasperengine.humanoid", "version": 2, "animation_type": "humanoid", "bones": {}})");
    AnimationClipPool::invalidateModel(hero);
    const Entity a = load(robot, -1);
    const Entity b = load(robot, 1);
    rememberRest();
    animator(controllerFile("walk", controller({}, {state("Walk", hero + "#Walk")})), a);
    player(b, hero + "#Walk", false);
    run(12);
    EXPECT_GT(motion(a, "thigh.L"), 1.0);
    const Difference d = compare(a, b, ROBOT_BONES);
    EXPECT_LT(d.degrees, 0.05) << d.bone;
    EXPECT_LT(d.meters, 1e-4);
}

// ---- upstream's anim-retarget files load and convert losslessly ----

// upstream's controller: a state's Mirror, Foot IK and Retarget and a blend child's Mirror become
// S21's fields (not unknown keys), a save writes them, the saved file reads back the same; an S21
// version 2 file of the earlier absorption (the flags kept as unknown keys) reads them as fields
TEST_F(AnimatorHumanoid, UpstreamControllerHumanoidFlagsConvertLosslessly){
    const std::string text = R"({"format": "jasperengine.controller", "version": 1, "parameters": [{"name": "Speed", "type": "float", "default": 0}],
      "layers": [{"name": "Base Layer", "default_state": "Walk",
        "states": [{"name": "Walk", "clip": "hero.gltf#Walk", "mirror": true, "foot_ik": true, "retarget": true, "editor_note": "kept"},
                   {"name": "Move", "blend_tree": {"parameter": "Speed", "children": [
                     {"clip": "hero.gltf#Idle", "threshold": 0}, {"clip": "hero.gltf#Walk", "threshold": 1, "mirror": true}]}}],
        "transitions": []}]})";
    ControllerParseResult parsed = parseController(text);
    ASSERT_TRUE(parsed.ok) << parsed.error.message;
    EXPECT_EQ(parsed.dialect, AnimatorFileDialect::UpstreamVersion1);
    const StateDef& walk = parsed.data.layers[0].machine.states[0];
    EXPECT_TRUE(walk.mirror);
    EXPECT_TRUE(walk.footIK);
    EXPECT_TRUE(walk.retarget);
    for (const auto& key : walk.unknownKeys) EXPECT_TRUE(key.first != "mirror" && key.first != "foot_ik" && key.first != "retarget") << key.first;
    ASSERT_EQ(walk.unknownKeys.size(), 1u) << "upstream's own extra member is kept";
    const StateDef& move = parsed.data.layers[0].machine.states[1];
    ASSERT_TRUE(move.motion.isTree());
    ASSERT_EQ(move.motion.tree[0].children.size(), 2u);
    EXPECT_FALSE(move.motion.tree[0].children[0].mirror);
    EXPECT_TRUE(move.motion.tree[0].children[1].mirror);
    EXPECT_TRUE(move.motion.tree[0].children[1].unknownKeys.empty());

    const std::string saved = writeController(parsed.data);
    ControllerParseResult again = parseController(saved);
    ASSERT_TRUE(again.ok) << again.error.message;
    EXPECT_EQ(again.dialect, AnimatorFileDialect::Current);
    EXPECT_EQ(writeController(again.data), saved) << "a second save writes the same text";
    EXPECT_TRUE(again.data.layers[0].machine.states[0].mirror && again.data.layers[0].machine.states[0].footIK &&
                again.data.layers[0].machine.states[0].retarget);
    EXPECT_TRUE(again.data.layers[0].machine.states[1].motion.tree[0].children[1].mirror);

    // the flags as unknown keys (what the earlier absorption wrote): S21 fields now, same text
    ControllerDef old = parsed.data;
    old.layers[0].machine.states[0].mirror = old.layers[0].machine.states[0].footIK = old.layers[0].machine.states[0].retarget = false;
    old.layers[0].machine.states[0].unknownKeys.insert(old.layers[0].machine.states[0].unknownKeys.begin(),
                                                       {{"mirror", "true"}, {"foot_ik", "true"}, {"retarget", "true"}});
    old.layers[0].machine.states[1].motion.tree[0].children[1].mirror = false;
    old.layers[0].machine.states[1].motion.tree[0].children[1].unknownKeys.push_back({"mirror", "true"});
    ControllerParseResult earlier = parseController(writeController(old));
    ASSERT_TRUE(earlier.ok) << earlier.error.message;
    EXPECT_TRUE(earlier.data.layers[0].machine.states[0].mirror && earlier.data.layers[0].machine.states[0].footIK &&
                earlier.data.layers[0].machine.states[0].retarget);
    EXPECT_TRUE(earlier.data.layers[0].machine.states[1].motion.tree[0].children[1].mirror);
    EXPECT_EQ(writeController(earlier.data), saved);
}

// upstream's .anim version 2 (humanoid tracks, angle keys, the humanoid header, a generic track)
// and .humanoid version 2 (Animation Type, extra bones, twist) read, write and read again the
// same; the clip plays on the Animator, the Avatar's extra-bone override is the Animator's avatar's
TEST_F(AnimatorHumanoid, UpstreamHumanoidFilesRoundTripAndPlay){
    const std::string animText = R"({"format": "jasperengine.anim", "version": 2, "frame_rate": 30, "wrap": "loop",
      "humanoid": {"source_model": "models/hero.gltf", "human_scale": 0.982},
      "tracks": [
        {"path": "Hat", "channel": "rotation", "keys": [[0, [0, 0, 0, 1]]]},
        {"channel": "humanoid", "bone": "hips", "keys": [[0, [0, 0, 0, 1]], [0.5, [0.02, 0, 0, 0.9998]]]},
        {"channel": "humanoid", "bone": "left_upper_leg", "keys": [[0, {"angles": [30, 5, 0]}], [1, {"angles": [-20, 0, 10]}]]},
        {"channel": "humanoid", "bone": "left_index_proximal", "keys": [[0, [0.2588, 0, 0, 0.9659]]]},
        {"channel": "humanoid_position", "bone": "hips", "keys": [[0, [0, -0.02, 0]], [0.5, [0, 0.01, 0.15]]]},
        {"channel": "humanoid_position", "bone": "left_foot", "keys": [[0, [0.09, -0.9, 0.1]]]}]})";
    ClipParseResult parsed = parseClip(animText);
    ASSERT_TRUE(parsed.ok()) << parsed.error.toString();
    ASSERT_TRUE(parsed.data->isHumanoid());
    const std::string saved = writeClip(*parsed.data);
    EXPECT_NE(saved.find("\"version\": 2"), std::string::npos);
    ClipParseResult again = parseClip(saved);
    ASSERT_TRUE(again.ok()) << again.error.toString();
    EXPECT_EQ(writeClip(*again.data), saved) << "a second save writes the same text";
    ASSERT_EQ(again.data->tracks.size(), parsed.data->tracks.size());
    EXPECT_EQ(again.data->humanoid.sourceModel, "models/hero.gltf");

    const std::string humanoidText = "{\n  \"format\": \"jasperengine.humanoid\",\n  \"version\": 2,\n  \"animation_type\": \"humanoid\",\n"
                                     "  \"bones\": {},\n  \"extra_bones\": {\"left_index_proximal\": \"\"},\n  \"twist\": {\"left_lower_arm\": 15}\n}\n";
    pose::HumanoidOverrides overrides;
    std::vector<std::string> warnings;
    std::string error;
    ASSERT_TRUE(pose::parseHumanoidOverrides(humanoidText, overrides, warnings, &error)) << error;
    EXPECT_TRUE(overrides.isHumanoid());
    ASSERT_TRUE(overrides.extraBones[(size_t)pose::extraIndex(pose::HumanExtraBone::LeftIndexProximal)].has_value());
    pose::HumanoidOverrides reread;
    ASSERT_TRUE(pose::parseHumanoidOverrides(pose::writeHumanoidOverrides(overrides), reread, warnings, &error)) << error;
    EXPECT_TRUE(reread == overrides);

    // the hero's Avatar unmaps its left index (and is Humanoid): the Animator's avatar follows it
    animtest::writeAsset(hero + ".humanoid", humanoidText);
    AnimationClipPool::invalidateModel(hero);
    animtest::writeAsset(prefix + "upstream_h.anim", saved);
    const Entity a = load(hero, -1);
    rememberRest();
    animator(controllerFile("h", controller({}, {state("H", prefix + "upstream_h.anim")})), a);
    run(6);
    ASSERT_TRUE(instance(a).humanoid.valid) << instance(a).humanoid.error;
    EXPECT_LT(instance(a).humanoid.extraBones[(size_t)pose::extraIndex(pose::HumanExtraBone::LeftIndexProximal)], 0)
        << "the sidecar's extra_bones \"\" unmaps the bone";
    EXPECT_NEAR(instance(a).humanoid.rig->twist(pose::HumanBone::LeftLowerArm), 15.0f, 1e-4f);
    ASSERT_TRUE(instance(a).clips[0].retargeted) << "a humanoid clip plays through the humanoid path";
    EXPECT_GT(motion(a, "mixamorig:LeftUpLeg"), 1.0);
    EXPECT_LT(motion(a, "mixamorig:LeftHandIndex1"), 1e-3) << "an unmapped finger is left alone";
}

// one path: an Animator state with Mirror on the model's own clip (no retargeting needed) plays
// it mirrored through the avatar, as a Retarget + Mirror entry does
TEST_F(AnimatorHumanoid, StateMirrorOnTheModelsOwnClip){
    anim::ControllerDef def = controller({}, {state("Walk", hero + "#Walk")});
    def.layers[0].machine.states[0].mirror = true;
    const Entity a = load(hero, -1);
    const Entity b = load(hero, 1);
    const Entity c = load(hero, 3);
    animator(controllerFile("mirror", def), a);
    player(b, hero + "#Walk", true, true);
    player(c, hero + "#Walk", false);
    run(10);
    ASSERT_TRUE(instance(a).clips[0].retargeted);
    const std::vector<std::string> legs = {"mixamorig:LeftUpLeg", "mixamorig:RightUpLeg", "mixamorig:LeftLeg", "mixamorig:RightLeg"};
    EXPECT_GT(compare(a, c, legs).degrees, 1.0) << "mirrored, unlike the plain walk";
    const Difference d = compare(a, b, legs);
    EXPECT_LT(d.degrees, 0.05) << d.bone;
}
