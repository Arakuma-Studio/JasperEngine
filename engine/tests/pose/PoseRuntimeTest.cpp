// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Pose Animator runtime (spec: pose animation phase 2, section 4.2) on the synthetic rigs:
// playback sampling between frames, stepped playback, the one-shot end, goToPose reaching the
// key pose, the weight blend; and resolveHumanoid's sidecar overrides (section 3.1)

#include "TestUtil.h"

#include "pose/ModelRig.h"
#include "pose/PoseAnimatorRuntime.h"
#include "pose/PoseSampler.h"

#include <gtest/gtest.h>

using namespace posetest;
using H = HumanBone;

namespace {

    PoseAnimationData armsAnimation(bool loop){
        PoseAnimationData data = makeAnimation({
            bodyWith({{"left_upper_arm.raise", 0}}),
            bodyWith({{"left_upper_arm.raise", 80}, {"right_upper_arm.raise", 40}, {"spine.bend", 15}}),
            bodyWith({{"left_upper_arm.forward", 60}, {"left_lower_arm.bend", 70}}),
        }, loop, 0.2f);
        return data;
    }

    double maxDifference(const HumanPose& a, const HumanPose& b){
        double worst = 0.0;
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            worst = std::max(worst, rotationError(a.rotations[s], b.rotations[s]));
        }
        worst = std::max(worst, (double)(a.hipsPosition - b.hipsPosition).length());
        return worst;
    }

    std::shared_ptr<PoseAnimatorRuntime> loaded(const PoseAnimationData& data, const TestRig& rig){
        auto runtime = std::make_shared<PoseAnimatorRuntime>();
        runtime->data = data;
        runtime->dataLoaded = true;
        std::string error;
        EXPECT_TRUE(runtime->build(rig.skinned, &error)) << error;
        return runtime;
    }

}

TEST(PoseRuntime, BuildRealisesKeysAndFrames){
    for (const TestRig* rig : allRigs()){
        PoseAnimationData data = armsAnimation(true);
        auto runtime = loaded(data, *rig);
        ASSERT_TRUE(runtime->isLoaded()) << rig->label;
        EXPECT_EQ(runtime->keys.size(), 3u);
        const PoseTimeline timeline = buildTimeline(data);
        EXPECT_NEAR(runtime->length, timeline.length, 1e-6);
        // count + 1 frames, the last a copy of the first (loop)
        const size_t count = std::max<size_t>(1, (size_t)std::lround(timeline.length * data.settings.frameRate));
        EXPECT_EQ(runtime->frames.size(), count + 1);
        EXPECT_LT(maxDifference(runtime->frames.front(), runtime->frames.back()), 1e-6);
        // the keys are fromBodyPose of the key bodies (joint limits on, grounded)
        HumanPose expected = rig->skinned->fromBodyPose(data.poses[1].body, nullptr, true, 0.0f);
        EXPECT_LT(maxDifference(runtime->keys[1], expected), 1e-5) << rig->label;
    }
}

TEST(PoseRuntime, SamplingBetweenFrames){
    PoseAnimationData data = armsAnimation(true);
    auto runtime = loaded(data, tposeRig());
    const size_t last = runtime->frames.size() - 1;
    const float frameTime = runtime->length / (float)last;

    HumanPose out;
    // on a frame: that frame
    runtime->samplePlayback(3 * frameTime, out);
    EXPECT_LT(maxDifference(out, runtime->frames[3]), 1e-5);
    // between two frames: the slerp (hips lerp) at the fraction
    runtime->samplePlayback(3.25f * frameTime, out);
    const HumanPose& a = runtime->frames[3];
    const HumanPose& b = runtime->frames[4];
    for (int s = 0; s < HUMAN_BONE_COUNT; s++){
        EXPECT_LT(rotationError(out.rotations[s], slerpShortest(a.rotations[s], b.rotations[s], 0.25f)), 1e-4) << s;
    }
    const Vector3 hips = a.hipsPosition + (b.hipsPosition - a.hipsPosition) * 0.25f;
    EXPECT_LT((out.hipsPosition - hips).length(), 1e-5);
    // a loop wraps
    HumanPose wrapped;
    runtime->samplePlayback(runtime->length + 3.25f * frameTime, wrapped);
    EXPECT_LT(maxDifference(out, wrapped), 1e-4);
}

TEST(PoseRuntime, SteppedPlaybackShowsThePreviousFrame){
    PoseAnimationData data = armsAnimation(true);
    data.settings.stepRate = 12;
    auto runtime = loaded(data, tposeRig());
    ASSERT_TRUE(runtime->stepped());
    const size_t last = runtime->frames.size() - 1;
    const float frameTime = runtime->length / (float)last;
    HumanPose out;
    runtime->samplePlayback(5.9f * frameTime, out);
    EXPECT_LT(maxDifference(out, runtime->frames[5]), 1e-6);
}

TEST(PoseRuntime, OneShotEndsOnceAndClamps){
    float time = 0.0f;
    bool playing = true;
    int finished = 0;
    for (int i = 0; i < 200; i++){
        if (advancePlayback(time, playing, 1.0f / 60.0f, 1.0f, 1.0f, false)) finished++;
    }
    EXPECT_EQ(finished, 1);
    EXPECT_FALSE(playing);
    EXPECT_FLOAT_EQ(time, 1.0f);

    // backwards ends at 0
    time = 0.5f;
    playing = true;
    finished = 0;
    for (int i = 0; i < 100; i++){
        if (advancePlayback(time, playing, 1.0f / 60.0f, -1.0f, 1.0f, false)) finished++;
    }
    EXPECT_EQ(finished, 1);
    EXPECT_FLOAT_EQ(time, 0.0f);

    // a loop wraps and never ends
    time = 0.9f;
    playing = true;
    EXPECT_FALSE(advancePlayback(time, playing, 0.2f, 1.0f, 1.0f, true));
    EXPECT_TRUE(playing);
    EXPECT_NEAR(time, 0.1f, 1e-5);

    // the end of a one-shot shows its last frame
    PoseAnimationData data = armsAnimation(false);
    auto runtime = loaded(data, aposeRig());
    HumanPose out;
    runtime->samplePlayback(runtime->length + 1.0f, out);
    EXPECT_LT(maxDifference(out, runtime->frames.back()), 1e-6);
}

TEST(PoseRuntime, GoToPoseReachesTheKeyPose){
    for (const TestRig* rig : allRigs()){
        PoseAnimationData data = armsAnimation(true);
        auto runtime = loaded(data, *rig);
        runtime->samplePlayback(0.0f, runtime->displayed);
        runtime->hasDisplayed = true;

        PoseGoTo request;
        request.key = 1;
        request.duration = 0.15f;
        ASSERT_TRUE(runtime->startTransition(request)) << rig->label;
        HumanPose out = runtime->displayed;
        // moving, not there yet after a third of the duration
        runtime->sampleTransition(0.05f, out);
        EXPECT_GT(maxDifference(out, runtime->keys[1]), 1e-3) << rig->label;
        for (int i = 0; i < 600 && !runtime->transitionArrived; i++){
            runtime->sampleTransition(1.0f / 60.0f, out);
        }
        EXPECT_TRUE(runtime->transitionArrived) << rig->label;
        EXPECT_LT(maxDifference(out, runtime->keys[1]), 1e-7) << rig->label;
        // then holds it
        runtime->sampleTransition(1.0f, out);
        EXPECT_LT(maxDifference(out, runtime->keys[1]), 1e-7) << rig->label;
        // near the settle time it is already within the spring's tolerance
        HumanPose late;
        runtime->transitionPose(runtime->transitionSettle * 0.999f, late);
        EXPECT_LT(maxDifference(late, runtime->keys[1]), 2e-3) << rig->label;
    }

    PoseAnimatorRuntime empty;
    PoseGoTo request;
    request.key = 0;
    EXPECT_FALSE(empty.startTransition(request));
}

TEST(PoseRuntime, GoToPoseOvershootsWithASpring){
    PoseAnimationData data = armsAnimation(true);
    data.settings.overlap = 0.0f;
    data.settings.looseness = 0.0f;
    auto runtime = loaded(data, tposeRig());
    runtime->displayed = runtime->keys[0];
    runtime->hasDisplayed = true;
    PoseGoTo request;
    request.key = 1;
    request.duration = 0.2f;
    request.overshoot = 0.5f;
    request.anticipation = 0.0f;
    ASSERT_TRUE(runtime->startTransition(request));
    const int arm = boneIndex(H::LeftUpperArm);
    const double total = rotationError(runtime->keys[0].rotations[arm], runtime->keys[1].rotations[arm]);
    double peak = 0.0;
    for (float t = 0.0f; t < 1.0f; t += 0.005f){
        HumanPose out;
        runtime->transitionPose(t, out);
        peak = std::max(peak, rotationError(runtime->keys[0].rotations[arm], out.rotations[arm]));
    }
    EXPECT_GT(peak, total * 1.3);
}

TEST(PoseRuntime, WeightBlend){
    const Quaternion rest;   // what the bone holds (the rest or a clip pose)
    const Quaternion pose = Quaternion(0.7071068f, 0.7071068f, 0.0f, 0.0f);   // 90 degrees about X
    const Vector3 restPosition(0, 1, 0);
    const Vector3 posePosition(0, 0.8f, 0.2f);

    // weight 1: the pose; weight 0: the rest pose
    {
        PoseBlendBone blend;
        EXPECT_LT(rotationError(blend.blendRotation(rest, pose, 1.0f), pose), 1e-6);
        EXPECT_LT((blend.blendPosition(restPosition, posePosition, 1.0f) - posePosition).length(), 1e-6);
    }
    {
        PoseBlendBone blend;
        EXPECT_LT(rotationError(blend.blendRotation(rest, pose, 0.0f), rest), 1e-6);
        EXPECT_LT((blend.blendPosition(restPosition, posePosition, 0.0f) - restPosition).length(), 1e-6);
    }
    // weight 0.5: half way, and it stays there frame after frame (what it wrote is not the base)
    {
        PoseBlendBone blend;
        Quaternion current = rest;
        Vector3 position = restPosition;
        for (int frame = 0; frame < 10; frame++){
            current = blend.blendRotation(current, pose, 0.5f);
            position = blend.blendPosition(position, posePosition, 0.5f);
        }
        EXPECT_LT(rotationError(current, slerpShortest(rest, pose, 0.5f)), 1e-5);
        EXPECT_NEAR(rotationError(rest, current), M_PI / 4.0, 1e-4);
        EXPECT_LT((position - (restPosition + posePosition) * 0.5f).length(), 1e-6);

        // back to weight 0 gives the base back; a clip writing a new value becomes the base
        current = blend.blendRotation(current, pose, 0.0f);
        EXPECT_LT(rotationError(current, rest), 1e-6);
        const Quaternion clip(0.9238795f, 0.0f, 0.3826834f, 0.0f);
        current = blend.blendRotation(clip, pose, 0.5f);
        EXPECT_LT(rotationError(current, slerpShortest(clip, pose, 0.5f)), 1e-5);
    }
}

TEST(PoseRuntime, WeightBlendOnARig){
    // the realised pose blended over the rest pose of the A-pose rig: 0 = rest, 1 = pose,
    // 0.5 = the per-bone slerp half way
    const TestRig& rig = aposeRig();
    PoseAnimationData data = armsAnimation(true);
    auto runtime = loaded(data, rig);
    const HumanPose& rest = rig.skinned->restPose();
    const HumanPose& target = runtime->keys[1];
    for (float weight : {0.0f, 0.5f, 1.0f}){
        std::array<PoseBlendBone, HUMAN_BONE_COUNT> blend;
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            if (!rig.skinned->has(boneAt(s))) continue;
            const Quaternion out = blend[s].blendRotation(rest.rotations[s], target.rotations[s], weight);
            EXPECT_LT(rotationError(out, slerpShortest(rest.rotations[s], target.rotations[s], weight)), 1e-5) << s;
            if (weight == 0.0f){
                EXPECT_LT(rotationError(out, rest.rotations[s]), 1e-6);
            }
            if (weight == 1.0f){
                EXPECT_LT(rotationError(out, target.rotations[s]), 1e-6);
            }
        }
    }
}

TEST(PoseRuntime, ResolveHumanoidOverrides){
    const Skeleton skeleton = skeletonFromList(heroFixtureBones());
    HumanoidMap map;
    std::string error;
    std::vector<std::string> notes;

    // no overrides: auto-detect
    ASSERT_TRUE(resolveHumanoid(skeleton, HumanoidOverrides(), map, &error)) << error;
    EXPECT_EQ(map.bones[boneIndex(H::LeftToes)], "mixamorig:LeftToeBase");
    EXPECT_EQ(map.bones[boneIndex(H::UpperChest)], "mixamorig:Spine2");

    // "" unmaps an optional slot
    HumanoidOverrides overrides;
    overrides.bones[boneIndex(H::LeftToes)] = std::string();
    ASSERT_TRUE(resolveHumanoid(skeleton, overrides, map, &error, &notes)) << error;
    EXPECT_EQ(map.bones[boneIndex(H::LeftToes)], "");
    EXPECT_FALSE(notes.empty());

    // a bone forced to another slot leaves the slot auto-detect gave it
    overrides = HumanoidOverrides();
    overrides.bones[boneIndex(H::Chest)] = std::string("mixamorig:Spine2");
    ASSERT_TRUE(resolveHumanoid(skeleton, overrides, map, &error)) << error;
    EXPECT_EQ(map.bones[boneIndex(H::Chest)], "mixamorig:Spine2");
    EXPECT_EQ(map.bones[boneIndex(H::UpperChest)], "");

    // an unknown bone fails, naming the slot
    overrides = HumanoidOverrides();
    overrides.bones[boneIndex(H::LeftHand)] = std::string("NoSuchBone");
    EXPECT_FALSE(resolveHumanoid(skeleton, overrides, map, &error));
    EXPECT_NE(error.find("left_hand"), std::string::npos) << error;

    // unmapping a required slot fails validation
    overrides = HumanoidOverrides();
    overrides.bones[boneIndex(H::LeftHand)] = std::string();
    EXPECT_FALSE(resolveHumanoid(skeleton, overrides, map, &error));
    EXPECT_NE(error.find("left_hand"), std::string::npos) << error;
}

TEST(PoseRuntime, RigCacheSharesRigs){
    PoseRigCache cache;
    const TestRig& rig = tposeRig();
    int built = 0;
    auto skinFactory = [&](){ built++; return rig.skin; };
    ModelRigInfo a, b, c;
    std::string error;
    ASSERT_TRUE(cache.get("models/hero.gltf", "", rig.skeleton, HumanoidOverrides(), skinFactory, a, &error)) << error;
    ASSERT_TRUE(cache.get("models/hero.gltf", "", rig.skeleton, HumanoidOverrides(), skinFactory, b, &error)) << error;
    EXPECT_EQ(a.rig.get(), b.rig.get());
    EXPECT_EQ(built, 1);
    // a new sidecar stamp builds again
    HumanoidOverrides overrides;
    overrides.bones[boneIndex(H::LeftToes)] = std::string();
    ASSERT_TRUE(cache.get("models/hero.gltf", "{}", rig.skeleton, overrides, skinFactory, c, &error)) << error;
    EXPECT_NE(a.rig.get(), c.rig.get());
    EXPECT_FALSE(c.rig->has(H::LeftToes));
    EXPECT_EQ(cache.size(), 1u);
}

TEST(PoseRuntime, RigCacheFollowsTheModelData){
    PoseRigCache cache;
    const TestRig& rig = tposeRig();
    auto skinFactory = [&](){ return rig.skin; };
    std::string error;
    auto loadA = std::make_shared<int>(1);   // stands for a ModelComponent's loaded gltfModel
    ModelRigInfo a, again, b;
    ASSERT_TRUE(cache.get("models/hero.gltf", "", loadA, rig.skeleton, HumanoidOverrides(), skinFactory, a, &error)) << error;
    ASSERT_TRUE(cache.get("models/hero.gltf", "", loadA, rig.skeleton, HumanoidOverrides(), skinFactory, again, &error)) << error;
    EXPECT_EQ(a.rig.get(), again.rig.get());
    // the file reloaded: new data, a new rig
    auto loadB = std::make_shared<int>(2);
    ASSERT_TRUE(cache.get("models/hero.gltf", "", loadB, rig.skeleton, HumanoidOverrides(), skinFactory, b, &error)) << error;
    EXPECT_NE(a.rig.get(), b.rig.get());
    EXPECT_EQ(cache.size(), 1u);
    // unloaded: the entry goes with the data
    loadB.reset();
    ModelRigInfo other;
    ASSERT_TRUE(cache.get("models/robot.gltf", "", rig.skeleton, HumanoidOverrides(), skinFactory, other, &error)) << error;
    EXPECT_EQ(cache.size(), 1u);
    cache.clear();
    EXPECT_EQ(cache.size(), 0u);
}

TEST(PoseRuntime, PausedTransitionFreezes){
    PoseAnimationData data = armsAnimation(true);
    auto runtime = loaded(data, tposeRig());
    runtime->displayed = runtime->keys[0];
    runtime->hasDisplayed = true;
    PoseGoTo request;
    request.key = 1;
    request.duration = 0.2f;
    ASSERT_TRUE(runtime->startTransition(request));
    HumanPose out = runtime->displayed;
    runtime->sampleTransition(0.05f, out);
    const float time = runtime->transitionTime;
    // what PoseAnimator::pause does; the system then advances it by 0
    runtime->transitionPaused = true;
    HumanPose frozen = out;
    runtime->sampleTransition(0.0f, frozen);
    EXPECT_FLOAT_EQ(runtime->transitionTime, time);
    EXPECT_LT(maxDifference(frozen, out), 1e-7);
    // a new goToPose runs again
    ASSERT_TRUE(runtime->startTransition(request));
    EXPECT_FALSE(runtime->transitionPaused);
}
