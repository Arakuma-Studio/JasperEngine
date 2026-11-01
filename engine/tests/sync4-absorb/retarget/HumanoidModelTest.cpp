// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SYNC4-ABSORB(retarget): parked here (not built) until the retarget absorption: it needs upstream's
// AvatarMask / AvatarMaskPool layers (converted onto S21 and deleted); its place is engine/tests/core/animation/

// Humanoid retargeting on the pose-animation fixture models (anim-retarget spec 14, tests 1, 2,
// 11, 12): the glTF skeleton equals the loaded model's, a converted clip played on its own rig
// reproduces the source, Animation Type: Humanoid models serve humanoid clips, the robot plays
// the hero's walk with matching joint angles, "@humanoid" without a source model fails, and a
// masked humanoid layer over a generic walk.

#include <gtest/gtest.h>

#include "../../core/animation/AnimTestEnvironment.h"

#include "Scene.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/AvatarMask.h"
#include "animation/ClipBinding.h"
#include "animation/HumanoidClip.h"
#include "animation/KeyframeSampling.h"
#include "animation/ModelSubClips.h"
#include "component/BoneComponent.h"
#include "component/ModelComponent.h"
#include "object/AnimationPlayer.h"
#include "object/Model.h"
#include "pool/AnimationClipPool.h"
#include "pool/AvatarMaskPool.h"
#include "pose/ModelRig.h"
#include "pose/PoseMath.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/MeshSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::pose;

namespace {

    std::string readFile(const std::string& path){
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    double degreesBetween(const Quaternion& a, const Quaternion& b){
        return radToDeg(angleBetween(Quatd(a).normalized(), Quatd(b).normalized()));
    }

    class HumanoidModel : public ::testing::Test {
    protected:
        void SetUp() override {
            static int run = 0;
            dir = "models/humanoid" + std::to_string(run++) + "/";
            hero = dir + "hero_animated.gltf";
            robot = dir + "robot_apose.gltf";
            animtest::writeAsset(hero, readFile(std::string(ANIM_POSE_FIXTURE_MODELS_DIR) + "/hero_animated.gltf"));
            animtest::writeAsset(robot, readFile(std::string(ANIM_POSE_FIXTURE_MODELS_DIR) + "/robot_apose.gltf"));
            AnimationClipPool::clear();
            AvatarMaskPool::clear();
        }
        void TearDown() override {
            AnimationClipPool::clear();
            AvatarMaskPool::clear();
        }

        Entity load(const std::string& path){
            // the handle owns its entity: kept for the test
            models.push_back(std::make_unique<Model>(&scene));
            const Entity entity = models.back()->getEntity();
            EXPECT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(entity, path, false, false, false));
            scene.getComponent<ModelComponent>(entity).needUpdateModel = false;
            return entity;
        }

        void step(double dt, int count = 1){
            for (int i = 0; i < count; i++) scene.getSystem<ActionSystem>()->update(dt);
        }

        Scene scene;
        std::vector<std::unique_ptr<Model>> models;
        std::string dir;
        std::string hero;
        std::string robot;
    };

}

// ---- test 1: the glTF skeleton ----

TEST_F(HumanoidModel, GltfSkeletonEqualsTheLoadedModel){
    const Entity entity = load(hero);
    Skeleton fromScene;
    std::vector<Entity> bones;
    std::vector<Matrix4> sceneBridges;
    ASSERT_TRUE(buildSkeleton(&scene, entity, fromScene, bones, &sceneBridges));

    Skeleton fromGltf;
    std::vector<std::string> paths;
    std::vector<Matrix4> gltfBridges;
    std::string error;
    ASSERT_TRUE(anim::readModelFile(hero, [&](const tinygltf::Model& gltf){
        return buildSkeletonFromGltf(gltf, fromGltf, paths, &gltfBridges);
    }, &error)) << error;

    ASSERT_EQ(fromGltf.bones.size(), fromScene.bones.size());
    for (size_t i = 0; i < fromScene.bones.size(); i++){
        const SkeletonBone& a = fromScene.bones[i];
        const SkeletonBone& b = fromGltf.bones[i];
        EXPECT_EQ(a.name, b.name);
        EXPECT_EQ(a.parent, b.parent) << a.name;
        EXPECT_LT((Vec3d(a.position) - Vec3d(b.position)).length(), 1e-6) << a.name;
        EXPECT_LT((Vec3d(a.scale) - Vec3d(b.scale)).length(), 1e-6) << a.name;
        EXPECT_LT(std::fabs(std::fabs(Quatd(a.rotation).dot(Quatd(b.rotation))) - 1.0), 1e-6) << a.name;
        for (int r = 0; r < 4; r++){
            for (int c = 0; c < 4; c++) EXPECT_NEAR(sceneBridges[i].get(c, r), gltfBridges[i].get(c, r), 1e-6);
        }
        // the path from the model root resolves to the same bone entity
        EXPECT_EQ(anim::resolvePath(&scene, entity, paths[i]), bones[i]) << paths[i];
    }
}

// ---- test 2: round trip on the source rig ----

TEST_F(HumanoidModel, ConvertedWalkReproducesTheSourceOnItsRig){
    const Entity entity = load(hero);
    std::shared_ptr<const anim::HumanoidSource> source;
    std::string error;
    ASSERT_TRUE(anim::HumanoidSourceCache::get(hero, source, &error)) << error;
    std::shared_ptr<const AnimationClip> walk = AnimationClipPool::getOrLoad(hero + "#Walk", &error);
    ASSERT_TRUE(walk) << error;

    // the walk with a finger curl added (the fixture's clips do not move the fingers)
    ClipData generic = walk->toData();
    const int finger = source->extraBones[(size_t)extraIndex(HumanExtraBone::LeftIndexProximal)];
    ASSERT_GE(finger, 0);
    ClipTrackData curl;
    curl.path = source->bonePaths[(size_t)finger];
    curl.channel = ClipChannel::Rotation;
    for (int k = 0; k <= 2; k++){
        ClipKey key;
        key.time = k * 0.5f;
        const Quatd q = Quatd(source->restRotation[(size_t)finger]) * axisAngleRad(Vec3d(0, 0, 1), degToRad(20.0 * k));
        key.value = {(float)q.x, (float)q.y, (float)q.z, (float)q.w};
        curl.keys.push_back(key);
    }
    generic.tracks.push_back(curl);
    anim::normalize(generic);
    std::shared_ptr<const AnimationClip> genericClip = AnimationClip::build(generic);

    ClipData converted;
    std::vector<ClipWarning> warnings;
    ASSERT_TRUE(anim::convertToHumanoid(generic, *source, converted, warnings, &error)) << error;
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(converted);
    ASSERT_TRUE(clip->isHumanoid());

    const anim::HumanoidSlots slots = anim::resolveHumanoidSlots(&scene, entity);
    ASSERT_TRUE(slots.target) << slots.error;
    anim::HumanoidBinding binding;
    ASSERT_TRUE(anim::bindHumanoidClip(*clip, slots.target, binding));
    const float scale = slots.target->rig->humanScale();
    for (float t : {0.0f, 0.25f, 0.5f, 0.75f}){
        anim::sampleHumanoidBinding(*clip, binding, t, nullptr);
        std::vector<anim::TrackSample> samples;
        anim::sampleClipTracks(*genericClip, t, samples);
        for (const anim::HumanoidOutput& output : binding.outputs){
            std::string path;
            ASSERT_TRUE(anim::relativePath(&scene, entity, output.entity, path));
            const int track = genericClip->findTrack(path, output.channel);
            if (output.channel == ClipChannel::Position){
                ASSERT_GE(track, 0);
                EXPECT_LT((Vec3d(output.position) - Vec3d(samples[(size_t)track].vector)).length(), 1e-4 * scale) << path;
                continue;
            }
            const Quaternion expected = track >= 0 ? samples[(size_t)track].rotation
                                                   : scene.getComponent<BoneComponent>(output.entity).bindRotation;
            EXPECT_LT(degreesBetween(output.rotation, expected), output.kind == anim::HumanoidOutput::Kind::Extra ? 0.05 : 1e-3)
                << path << " t " << t;
        }
    }
}

// ---- Mirror of a one-sided finger curl (review H1) ----

TEST_F(HumanoidModel, MirroredRightHandCurlDrivesTheLeftHand){
    const Entity entity = load(hero);
    std::shared_ptr<const anim::HumanoidSource> source;
    std::string error;
    ASSERT_TRUE(anim::HumanoidSourceCache::get(hero, source, &error)) << error;
    const int right = source->extraBones[(size_t)extraIndex(HumanExtraBone::RightIndexProximal)];
    ASSERT_GE(right, 0);
    // a right index curl only: 35 degrees about the bone's own Z
    ClipData generic;
    ClipTrackData curl;
    curl.path = source->bonePaths[(size_t)right];
    curl.channel = ClipChannel::Rotation;
    ClipKey key;
    const Quatd curled = Quatd(source->restRotation[(size_t)right]) * axisAngleRad(Vec3d(0, 0, 1), degToRad(35.0));
    key.value = {(float)curled.x, (float)curled.y, (float)curled.z, (float)curled.w};
    curl.keys.push_back(key);
    generic.tracks.push_back(curl);
    anim::normalize(generic);
    ClipData converted;
    std::vector<ClipWarning> warnings;
    ASSERT_TRUE(anim::convertToHumanoid(generic, *source, converted, warnings, &error)) << error;
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(converted);

    const anim::HumanoidSlots slots = anim::resolveHumanoidSlots(&scene, entity);
    ASSERT_TRUE(slots.target) << slots.error;
    const int leftIndex = extraIndex(HumanExtraBone::LeftIndexProximal);
    const int rightIndex = extraIndex(HumanExtraBone::RightIndexProximal);
    auto outputOf = [](const anim::HumanoidBinding& binding, int x) -> const anim::HumanoidOutput* {
        for (const anim::HumanoidOutput& output : binding.outputs){
            if (output.kind == anim::HumanoidOutput::Kind::Extra && output.index == x) return &output;
        }
        return nullptr;
    };
    const Quaternion leftRest = scene.getComponent<BoneComponent>(slots.target->extraEntities[(size_t)leftIndex]).bindRotation;
    const Quaternion rightRest = scene.getComponent<BoneComponent>(slots.target->extraEntities[(size_t)rightIndex]).bindRotation;
    for (bool mirror : {false, true}){
        anim::HumanoidBinding binding;
        ASSERT_TRUE(anim::bindHumanoidClip(*clip, slots.target, binding));
        binding.mirror = mirror;
        // a stale value in the scratch (an earlier sample) must not leak into a bone without a value
        binding.extraLocal.fill(Quaternion(0.5f, 0.5f, 0.5f, 0.5f));
        anim::sampleHumanoidBinding(*clip, binding, 0.0f, nullptr);
        const anim::HumanoidOutput* left = outputOf(binding, leftIndex);
        const anim::HumanoidOutput* rightOut = outputOf(binding, rightIndex);
        ASSERT_TRUE(left && rightOut) << "both index fingers are outputs, mirrored or not";
        // the live one curls; the other hand's index is not the clip's (it writes nothing there)
        const anim::HumanoidOutput* curledOut = mirror ? left : rightOut;
        const anim::HumanoidOutput* otherOut = mirror ? rightOut : left;
        EXPECT_TRUE(anim::isLive(binding, *curledOut)) << "mirror " << mirror;
        EXPECT_FALSE(anim::isLive(binding, *otherOut)) << "mirror " << mirror;
        EXPECT_NEAR(degreesBetween(curledOut->rotation, mirror ? leftRest : rightRest), 35.0, 0.05) << "mirror " << mirror;
    }
}

// ---- a one-sided hand clip on an Override layer over an idle (review M-1) ----

TEST_F(HumanoidModel, OneSidedHandClipLeavesTheOtherHandToTheIdle){
    const Entity entity = load(hero);
    std::shared_ptr<const anim::HumanoidSource> source;
    std::string error;
    ASSERT_TRUE(anim::HumanoidSourceCache::get(hero, source, &error)) << error;
    const HumanExtraBone curledBones[] = {HumanExtraBone::LeftIndexProximal, HumanExtraBone::LeftMiddleProximal,
                                          HumanExtraBone::RightIndexProximal, HumanExtraBone::RightMiddleProximal};
    auto turned = [&](HumanExtraBone bone, double degrees){
        const int b = source->extraBones[(size_t)extraIndex(bone)];
        return (Quatd(source->restRotation[(size_t)b]) * axisAngleRad(Vec3d(0, 0, 1), degToRad(degrees))).normalized().toQuaternion();
    };
    // the idle (generic, base layer): both hands' index and middle fingers curled 25 degrees
    ClipData idle;
    idle.wrap = ClipWrapMode::Loop;
    for (HumanExtraBone bone : curledBones){
        const int b = source->extraBones[(size_t)extraIndex(bone)];
        ASSERT_GE(b, 0);
        ClipTrackData track;
        track.path = source->bonePaths[(size_t)b];
        track.channel = ClipChannel::Rotation;
        ClipKey key;
        const Quaternion q = turned(bone, 25.0);
        key.value = {q.x, q.y, q.z, q.w};
        track.keys = {key};
        idle.tracks.push_back(track);
    }
    animtest::writeAsset(dir + "idle.anim", anim::writeClip(idle));
    // the point (humanoid, Override layer without a mask): the right index alone, 50 degrees
    ClipData point;
    point.wrap = ClipWrapMode::Loop;
    {
        const int b = source->extraBones[(size_t)extraIndex(HumanExtraBone::RightIndexProximal)];
        ClipTrackData track;
        track.path = source->bonePaths[(size_t)b];
        track.channel = ClipChannel::Rotation;
        ClipKey key;
        const Quaternion q = turned(HumanExtraBone::RightIndexProximal, 50.0);
        key.value = {q.x, q.y, q.z, q.w};
        track.keys = {key};
        point.tracks.push_back(track);
    }
    ClipData converted;
    std::vector<ClipWarning> warnings;
    ASSERT_TRUE(anim::convertToHumanoid(point, *source, converted, warnings, &error)) << error;
    // the point's fingers only (a hand-pose clip): no body slots
    converted.tracks.erase(std::remove_if(converted.tracks.begin(), converted.tracks.end(), [](const ClipTrackData& t){
        return !(t.channel == ClipChannel::Humanoid && t.humanBone >= HUMAN_BONE_COUNT);
    }), converted.tracks.end());
    ASSERT_EQ(converted.tracks.size(), 1u);
    animtest::writeAsset(dir + "point.anim", anim::writeClip(converted));

    scene.addComponent<AnimationPlayerComponent>(entity, AnimationPlayerComponent());
    AnimationPlayerComponent& component = scene.getComponent<AnimationPlayerComponent>(entity);
    AnimationPlayerLayer base;
    base.name = "Base Layer";
    AnimationPlayerLayer hand;
    hand.name = "Hand";
    component.layers = {base, hand};
    AnimationPlayerClip idleEntry;
    idleEntry.clip = dir + "idle.anim";
    AnimationPlayerClip pointEntry;
    pointEntry.clip = dir + "point.anim";
    pointEntry.layer = 1;
    component.clips = {idleEntry, pointEntry};
    component.playAutomatically = false;
    AnimationPlayer player(&scene, entity);
    player.play("idle");
    player.play("point");
    step(0.0);
    step(0.1);
    ASSERT_TRUE(player.isLoaded()) << player.getStateInfo("point").error;
    ASSERT_TRUE(player.getState("point").isHumanoid());

    const anim::HumanoidSlots slots = anim::resolveHumanoidSlots(&scene, entity);
    ASSERT_TRUE(slots.target);
    auto local = [&](HumanExtraBone bone){
        return scene.getComponent<Transform>(slots.target->extraEntities[(size_t)extraIndex(bone)]).rotation;
    };
    // unmirrored: the right index points, the left hand is the idle's
    EXPECT_NEAR(degreesBetween(local(HumanExtraBone::RightIndexProximal), turned(HumanExtraBone::RightIndexProximal, 0.0)), 50.0, 0.05);
    EXPECT_LT(degreesBetween(local(HumanExtraBone::LeftIndexProximal), turned(HumanExtraBone::LeftIndexProximal, 25.0)), 0.05)
        << "the left index keeps the idle's curl (not snapped to rest)";
    EXPECT_LT(degreesBetween(local(HumanExtraBone::LeftMiddleProximal), turned(HumanExtraBone::LeftMiddleProximal, 25.0)), 0.05);
    EXPECT_LT(degreesBetween(local(HumanExtraBone::RightMiddleProximal), turned(HumanExtraBone::RightMiddleProximal, 25.0)), 0.05);

    // mirrored: the left index points (by 50 degrees from its rest), the right hand is the idle's
    player.getState("point").setMirror(true);
    step(0.1);
    const Quaternion leftRest = scene.getComponent<BoneComponent>(slots.target->extraEntities[(size_t)extraIndex(HumanExtraBone::LeftIndexProximal)]).bindRotation;
    EXPECT_NEAR(degreesBetween(local(HumanExtraBone::LeftIndexProximal), leftRest), 50.0, 0.05) << "the mirrored point drives the left hand";
    EXPECT_LT(degreesBetween(local(HumanExtraBone::RightIndexProximal), turned(HumanExtraBone::RightIndexProximal, 25.0)), 0.05)
        << "the right index is the idle's again";
    EXPECT_LT(degreesBetween(local(HumanExtraBone::LeftMiddleProximal), turned(HumanExtraBone::LeftMiddleProximal, 25.0)), 0.05);
}

// ---- a bad hands-and-head override (review H2, L-6) ----

TEST_F(HumanoidModel, BadExtraOverrideKeepsTheRigWithOneWarningPerBuild){
    // the left index forced onto the hips bone (a body slot's): the rig stays, fingers auto-detect
    animtest::writeAsset(hero + ".humanoid", "{\"format\": \"jasperengine.humanoid\", \"version\": 2, \"bones\": {}, "
                                            "\"extra_bones\": {\"left_index_proximal\": \"mixamorig:Hips\"}}");
    const Entity entity = load(hero);
    animtest::takeWarnings();
    const anim::HumanoidSlots first = anim::resolveHumanoidSlots(&scene, entity);
    ASSERT_TRUE(first.target) << first.error;
    EXPECT_NE(first.target->extra.bones[(size_t)extraIndex(HumanExtraBone::LeftIndexProximal)], "mixamorig:Hips");
    EXPECT_GE(first.target->extra.mappedCount(0, HUMAN_FINGER_BONE_COUNT), 12) << "auto-detected fingers";
    for (int i = 0; i < 3; i++) EXPECT_TRUE(anim::resolveHumanoidSlots(&scene, entity).target);
    size_t warned = 0;
    for (const std::string& line : animtest::takeWarnings()) warned += line.find("auto-detected instead") != std::string::npos ? 1 : 0;
    EXPECT_EQ(warned, 1u) << "one warning per rig build (the cached rig says nothing)";
}

// ---- dropped scale tracks of mapped bones (review L-2) ----

TEST_F(HumanoidModel, ScaleOfMappedBonesAtTheNodeRestIsDroppedQuietly){
    std::shared_ptr<const anim::HumanoidSource> source;
    std::string error;
    ASSERT_TRUE(anim::HumanoidSourceCache::get(hero, source, &error)) << error;
    const int hips = source->rig->boneIndex(HumanBone::Hips);
    ASSERT_GE(hips, 0);
    ASSERT_EQ(source->restScale.size(), source->bonePaths.size());
    auto convert = [&](const Vector3& scale){
        ClipData generic;
        ClipTrackData track;
        track.path = source->bonePaths[(size_t)hips];
        track.channel = ClipChannel::Scale;
        ClipKey key;
        key.value = {scale.x, scale.y, scale.z};
        track.keys = {key};
        generic.tracks.push_back(track);
        ClipData converted;
        std::vector<ClipWarning> warnings;
        EXPECT_TRUE(anim::convertToHumanoid(generic, *source, converted, warnings, &error)) << error;
        EXPECT_LT(converted.findTrack(source->bonePaths[(size_t)hips], ClipChannel::Scale), 0) << "never copied onto a same-named bone";
        bool warned = false;
        for (const ClipWarning& warning : warnings) warned = warned || warning.message.find("(scale)") != std::string::npos;
        return warned;
    };
    // the node's own rest scale (not the skeleton's, which folds in Mixamo's 0.01 Armature)
    EXPECT_FALSE(convert(source->restScale[(size_t)hips]));
    EXPECT_TRUE(convert(Vector3(2, 2, 2)));
}

// ---- test 11: the pool ----

TEST_F(HumanoidModel, HumanoidModelsServeHumanoidClips){
    std::string error;
    std::shared_ptr<const AnimationClip> generic = AnimationClipPool::getOrLoad(hero + "#Walk", &error);
    ASSERT_TRUE(generic) << error;
    EXPECT_FALSE(generic->isHumanoid()) << "no sidecar: generic, as before";

    // Animation Type: Humanoid in the sidecar: the editor's writer invalidates the model
    animtest::writeAsset(hero + ".humanoid", "{\"format\": \"jasperengine.humanoid\", \"version\": 2, \"animation_type\": \"humanoid\", \"bones\": {}}");
    AnimationClipPool::invalidateModel(hero);
    std::shared_ptr<const AnimationClip> walk = AnimationClipPool::getOrLoad(hero + "#Walk", &error);
    ASSERT_TRUE(walk) << error;
    EXPECT_TRUE(walk->isHumanoid());
    EXPECT_EQ(walk->getHumanoidInfo().sourceModel, hero);
    // "@humanoid" of a humanoid clip is the clip
    std::shared_ptr<const AnimationClip> again = AnimationClipPool::getOrLoad(hero + "#Walk@humanoid", &error);
    ASSERT_TRUE(again) << error;
    EXPECT_TRUE(again->isHumanoid());

    // the robot plays it: the left upper leg's angles match the hero's (0.1 degree), no NaN
    const Entity heroEntity = load(hero);
    const Entity robotEntity = load(robot);
    const anim::HumanoidSlots heroSlots = anim::resolveHumanoidSlots(&scene, heroEntity);
    const anim::HumanoidSlots robotSlots = anim::resolveHumanoidSlots(&scene, robotEntity);
    ASSERT_TRUE(heroSlots.target) << heroSlots.error;
    ASSERT_TRUE(robotSlots.target) << robotSlots.error;
    anim::HumanoidBinding heroBinding, robotBinding;
    ASSERT_TRUE(anim::bindHumanoidClip(*walk, heroSlots.target, heroBinding));
    ASSERT_TRUE(anim::bindHumanoidClip(*walk, robotSlots.target, robotBinding));
    anim::sampleHumanoidBinding(*walk, heroBinding, 0.5f, nullptr);
    anim::sampleHumanoidBinding(*walk, robotBinding, 0.5f, nullptr);
    for (HumanBone bone : {HumanBone::LeftUpperLeg, HumanBone::RightUpperLeg, HumanBone::LeftLowerLeg, HumanBone::LeftUpperArm}){
        float expected[3], actual[3];
        heroSlots.target->rig->anglesFromLocal(bone, heroBinding.pose.rotations[(size_t)boneIndex(bone)], expected);
        robotSlots.target->rig->anglesFromLocal(bone, robotBinding.pose.rotations[(size_t)boneIndex(bone)], actual);
        for (int k = 0; k < 3; k++){
            EXPECT_TRUE(std::isfinite(actual[k]));
            EXPECT_NEAR(actual[k], expected[k], 0.1) << humanBoneName(bone) << " dof " << k;
        }
    }
    for (const anim::HumanoidOutput& output : robotBinding.outputs){
        EXPECT_TRUE(std::isfinite(output.rotation.w) && std::isfinite(output.position.y));
    }
    // the hips at the robot's height: its neutral hips plus the scaled offset
    const float heroHeight = heroSlots.target->rig->hipsWorldPosition(heroBinding.pose).y - heroSlots.target->rig->floorHeight();
    const float robotHeight = robotSlots.target->rig->hipsWorldPosition(robotBinding.pose).y - robotSlots.target->rig->floorHeight();
    EXPECT_NEAR(robotHeight / robotSlots.target->rig->humanScale(), heroHeight / heroSlots.target->rig->humanScale(), 1e-3);
}

TEST_F(HumanoidModel, RetargetWithoutASourceModelFails){
    ClipData data;
    ClipTrackData track;
    track.path = "mixamorig:Hips";
    track.channel = ClipChannel::Rotation;
    ClipKey key;
    key.value = {0, 0, 0, 1};
    track.keys.push_back(key);
    data.tracks.push_back(track);
    animtest::writeAsset(dir + "nosource.anim", anim::writeClip(data));
    std::string error;
    EXPECT_FALSE(AnimationClipPool::getOrLoad(dir + "nosource.anim@humanoid", &error));
    EXPECT_EQ(error, "retarget: the clip names no source model (set its Preview Model)");

    // with a preview model it converts, and an edit of the file drops the conversion
    data.previewModel = hero;
    animtest::writeAsset(dir + "withsource.anim", anim::writeClip(data));
    std::shared_ptr<const AnimationClip> converted = AnimationClipPool::getOrLoad(dir + "withsource.anim@humanoid", &error);
    ASSERT_TRUE(converted) << error;
    EXPECT_TRUE(converted->isHumanoid());
    const uint64_t generation = AnimationClipPool::getGeneration(dir + "withsource.anim@humanoid");
    AnimationClipPool::invalidate(dir + "withsource.anim");
    EXPECT_NE(AnimationClipPool::getGeneration(dir + "withsource.anim@humanoid"), generation);
    EXPECT_FALSE(AnimationClipPool::get(dir + "withsource.anim@humanoid"));
    // and an edit of its source model's sidecar too
    ASSERT_TRUE(AnimationClipPool::getOrLoad(dir + "withsource.anim@humanoid", &error)) << error;
    AnimationClipPool::invalidateModel(hero);
    EXPECT_FALSE(AnimationClipPool::get(dir + "withsource.anim@humanoid"));
}

// ---- the player: a retargeted entry, test 12 (layers) ----

TEST_F(HumanoidModel, RetargetedEntryPlaysOnTheRobot){
    const Entity entity = load(robot);
    AnimationPlayer player(&scene, entity);
    AnimationClipOptions options;
    options.retarget = true;
    ASSERT_TRUE(player.addClip(hero + "#Walk", "", options));
    player.setPlayAutomatically(true);
    step(0.0);
    step(0.5);
    ASSERT_TRUE(player.isLoaded()) << player.getStateInfo("Walk").error;
    AnimationStateInfo info = player.getStateInfo("Walk");
    EXPECT_TRUE(info.humanoid);
    EXPECT_TRUE(info.retarget);
    EXPECT_GE(info.humanoidBones, 20u);
    EXPECT_TRUE(player.getState("Walk").isHumanoid());

    // the bones show the realised walk
    const anim::HumanoidSlots slots = anim::resolveHumanoidSlots(&scene, entity);
    ASSERT_TRUE(slots.target);
    std::shared_ptr<const AnimationClip> clip = AnimationClipPool::getOrLoad(hero + "#Walk@humanoid");
    ASSERT_TRUE(clip);
    anim::HumanoidBinding binding;
    ASSERT_TRUE(anim::bindHumanoidClip(*clip, slots.target, binding));
    anim::sampleHumanoidBinding(*clip, binding, player.getState("Walk").getTime(), nullptr);
    for (const anim::HumanoidOutput& output : binding.outputs){
        const Transform& transform = scene.getComponent<Transform>(output.entity);
        if (output.channel == ClipChannel::Rotation){
            EXPECT_LT(degreesBetween(transform.rotation, output.rotation), 0.05) << scene.getEntityName(output.entity);
        }else{
            EXPECT_LT((Vec3d(transform.position) - Vec3d(output.position)).length(), 1e-4);
        }
    }
    // Lua's state.mirror: the left and right legs swap
    player.getState("Walk").setMirror(true);
    EXPECT_TRUE(player.getState("Walk").getMirror());
    const Entity leftThigh = slots.target->slotEntities[(size_t)boneIndex(HumanBone::LeftUpperLeg)];
    const Quaternion before = scene.getComponent<Transform>(leftThigh).rotation;
    step(0.0);
    EXPECT_GT(degreesBetween(scene.getComponent<Transform>(leftThigh).rotation, before), 1.0);
}

TEST_F(HumanoidModel, MaskedHumanoidLayerOverAGenericWalk){
    const Entity entity = load(hero);
    anim::AvatarMaskData maskData;
    maskData.defaultWeight = 0.0f;
    maskData.hasHumanoid = true;
    maskData.parts[(size_t)anim::AvatarMaskPart::RightArm] = 1.0f;
    animtest::writeAsset(dir + "rightarm.mask", anim::writeAvatarMask(maskData));

    scene.addComponent<AnimationPlayerComponent>(entity, AnimationPlayerComponent());
    AnimationPlayerComponent& component = scene.getComponent<AnimationPlayerComponent>(entity);
    AnimationPlayerLayer base;
    base.name = "Base Layer";
    AnimationPlayerLayer wave;
    wave.name = "Wave";
    wave.mask = dir + "rightarm.mask";
    component.layers = {base, wave};
    AnimationPlayerClip walkEntry;
    walkEntry.clip = hero + "#Walk";
    AnimationPlayerClip waveEntry;
    waveEntry.clip = hero + "#Wave 2";
    waveEntry.retarget = true;
    waveEntry.layer = 1;
    component.clips = {walkEntry, waveEntry};
    component.playAutomatically = false;
    AnimationPlayer player(&scene, entity);
    player.play("Walk");
    player.play("Wave 2");
    step(0.0);
    step(0.3);
    ASSERT_TRUE(player.isLoaded()) << player.getStateInfo("Wave 2").error;

    const anim::HumanoidSlots slots = anim::resolveHumanoidSlots(&scene, entity);
    ASSERT_TRUE(slots.target);
    std::shared_ptr<const AnimationClip> clip = AnimationClipPool::getOrLoad(hero + "#Wave 2@humanoid");
    ASSERT_TRUE(clip);
    anim::HumanoidBinding binding;
    ASSERT_TRUE(anim::bindHumanoidClip(*clip, slots.target, binding));
    anim::sampleHumanoidBinding(*clip, binding, player.getState("Wave 2").getTime(), nullptr);
    const Entity rightForearm = slots.target->slotEntities[(size_t)boneIndex(HumanBone::RightLowerArm)];
    const Entity leftThigh = slots.target->slotEntities[(size_t)boneIndex(HumanBone::LeftUpperLeg)];
    bool foundArm = false;
    Quaternion waveThigh;
    bool foundThigh = false;
    for (const anim::HumanoidOutput& output : binding.outputs){
        if (output.entity == rightForearm){
            foundArm = true;
            EXPECT_LT(degreesBetween(scene.getComponent<Transform>(rightForearm).rotation, output.rotation), 0.05);
        }
        if (output.entity == leftThigh){
            foundThigh = true;
            waveThigh = output.rotation;
        }
    }
    ASSERT_TRUE(foundArm && foundThigh);
    // the left thigh (masked out of the wave layer) is the walk's, which moves it: the walk's
    // sample at its state time, not the wave's realised thigh
    std::shared_ptr<const AnimationClip> walk = AnimationClipPool::getOrLoad(hero + "#Walk");
    ASSERT_TRUE(walk);
    std::string thighPath;
    ASSERT_TRUE(anim::relativePath(&scene, entity, leftThigh, thighPath));
    const int track = walk->findTrack(thighPath, ClipChannel::Rotation);
    ASSERT_GE(track, 0) << "the walk animates " << thighPath;
    std::vector<anim::TrackSample> samples;
    anim::sampleClipTracks(*walk, player.getState("Walk").getTime(), samples);
    const Quaternion walked = samples[(size_t)track].rotation;
    EXPECT_GT(degreesBetween(walked, scene.getComponent<BoneComponent>(leftThigh).bindRotation), 1.0) << "the walk moves the thigh";
    EXPECT_GT(degreesBetween(walked, waveThigh), 1.0) << "the wave's thigh differs (the mask is what keeps the walk's)";
    EXPECT_LT(degreesBetween(scene.getComponent<Transform>(leftThigh).rotation, walked), 0.05);
}

// ---- spec 13: performance (host, Release) ----

TEST_F(HumanoidModel, PerformanceBudget){
    std::shared_ptr<const anim::HumanoidSource> source;
    std::string error;
    ASSERT_TRUE(anim::HumanoidSourceCache::get(hero, source, &error)) << error;
    std::shared_ptr<const AnimationClip> walk = AnimationClipPool::getOrLoad(hero + "#Walk", &error);
    ASSERT_TRUE(walk);
    // a 3 s clip: the walk's keys repeated
    ClipData data = walk->toData();
    for (ClipTrackData& track : data.tracks){
        std::vector<ClipKey> keys = track.keys;
        for (int r = 1; r < 3; r++){
            for (ClipKey key : keys){
                key.time += r * 1.0f + 1e-3f;
                track.keys.push_back(key);
            }
        }
    }
    anim::normalize(data);
    ClipData converted;
    std::vector<ClipWarning> warnings;
    auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(anim::convertToHumanoid(data, *source, converted, warnings, &error));
    const double convertMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("convert a 3 s clip: %.2f ms\n", convertMs);
    EXPECT_LT(convertMs, 5.0);

    // 30 characters with one humanoid state each and Foot IK: the realisation per frame
    const Entity entity = load(hero);
    const anim::HumanoidSlots slots = anim::resolveHumanoidSlots(&scene, entity);
    ASSERT_TRUE(slots.target);
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(converted);
    std::vector<anim::HumanoidBinding> bindings(30);
    std::vector<std::vector<anim::KeyCursor>> cursors(30, std::vector<anim::KeyCursor>(clip->getTracks().size()));
    for (anim::HumanoidBinding& binding : bindings){
        ASSERT_TRUE(anim::bindHumanoidClip(*clip, slots.target, binding));
        binding.footIK = true;
    }
    double best = 1e9;
    for (int round = 0; round < 8; round++){
        start = std::chrono::steady_clock::now();
        for (int frame = 0; frame < 30; frame++){
            for (size_t i = 0; i < bindings.size(); i++){
                anim::sampleHumanoidBinding(*clip, bindings[i], std::fmod(frame / 30.0f + i * 0.1f, 3.0f), &cursors[i]);
            }
        }
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 30.0);
    }
    std::printf("30 humanoid states with Foot IK: %.3f ms per frame\n", best);
#ifdef NDEBUG
    EXPECT_LT(best, 0.3);
#endif
}
