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
