// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Acceptance tests 11 and 12: the sampler, the frames and auto timing (spec 4.8)

#include "TestUtil.h"

#include "pose/PoseSampler.h"
#include "pose/PoseTween.h"

#include <gtest/gtest.h>

using namespace posetest;
using H = HumanBone;

namespace {

    std::vector<BodyPose> threeBodies(){
        return {
            bodyWith({{"left_upper_arm.raise", 60}, {"right_upper_arm.raise", 20}, {"spine.bend", 10}, {"left_lower_arm.bend", 30}}),
            bodyWith({{"left_upper_arm.raise", 10}, {"right_upper_arm.raise", 80}, {"spine.bend", -10}, {"left_hand.bend", 40},
                      {"left_upper_leg.forward", 40}, {"left_lower_leg.bend", 60}}),
            bodyWith({{"left_upper_arm.forward", 70}, {"hips.turn", 30}, {"head.side", 20}, {"right_lower_leg.bend", 30}}),
        };
    }

    SamplerInput inputFor(const PoseAnimationData& data, const PoseRig& rig){
        return makeSamplerInput(data, realiseKeys(data, rig), &rig);
    }

    double maxRotationDifference(const HumanPose& a, const HumanPose& b){
        double worst = 0.0;
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            worst = std::max(worst, rotationError(a.rotations[s], b.rotations[s]));
        }
        return worst;
    }

}

TEST(PoseSampler, Timeline){
    PoseAnimationData data = makeAnimation(threeBodies(), true, 0.2f);
    data.poses[1].customTransition = true;
    data.poses[1].transition.duration = 0.5f;
    PoseTimeline t = buildTimeline(data);
    ASSERT_EQ(t.segmentCount(), 3u);
    EXPECT_NEAR(t.start[0], 0.2, 1e-6);
    EXPECT_NEAR(t.start[1], 0.2 + 0.12 + 0.2, 1e-6);
    EXPECT_NEAR(t.start[2], 0.52 + 0.5 + 0.2, 1e-6);
    EXPECT_NEAR(t.length, 3 * 0.2 + 0.12 + 0.5 + 0.12, 1e-6);
    data.settings.loop = false;
    t = buildTimeline(data);
    ASSERT_EQ(t.segmentCount(), 2u);
    EXPECT_NEAR(t.length, 3 * 0.2 + 0.12 + 0.5, 1e-6);
    // one pose: its hold, at least one frame
    PoseAnimationData one = makeAnimation({BodyPose()}, true, 0.0f);
    EXPECT_NEAR(buildTimeline(one).length, 1.0 / 30, 1e-9);
    one.poses[0].hold = 0.7f;
    EXPECT_NEAR(buildTimeline(one).length, 0.7, 1e-6);
}

TEST(PoseSampler, TelescopesToTheLastKey){
    const PoseRig& rig = *tposeRig().rig;
    PoseAnimationData data = makeAnimation(threeBodies(), false, 0.3f);
    data.settings.transition.curve = TransitionCurve::Linear;
    const SamplerInput input = inputFor(data, rig);
    HumanPose out;
    sample(input, static_cast<float>(input.timeline.length), out);
    EXPECT_LT(maxRotationDifference(out, input.keys.back()), 2e-6);
    EXPECT_LT((Vec3d(out.hipsPosition) - Vec3d(input.keys.back().hipsPosition)).length(), 1e-6);
    // and the first key at t = 0
    sample(input, 0.0f, out);
    EXPECT_LT(maxRotationDifference(out, input.keys.front()), 2e-6);
    // the middle of the hold of key 1 (Linear: every earlier ease is exactly 1)
    sample(input, static_cast<float>(input.timeline.start[0] + 0.12 + 0.2), out);
    EXPECT_LT(maxRotationDifference(out, input.keys[1]), 2e-6);
}

TEST(PoseSampler, LoopSeamAndLife){
    const PoseRig& rig = *tposeRig().rig;
    PoseAnimationData data = makeAnimation(threeBodies(), true, 0.15f);
    data.settings.humanize.enabled = true;
    data.settings.humanize.life = 2.0f;
    data.settings.humanize.movingHolds = 0.2f;
    data.settings.overlap = 0.1f;
    data.settings.looseness = 0.5f;
    data.settings.weights = {1.0f, 1.0f, 1.2f, 0.5f};
    const SamplerInput input = inputFor(data, rig);
    HumanPose a, b;
    sample(input, 0.0f, a);
    sample(input, static_cast<float>(input.timeline.length), b);
    EXPECT_LT(maxRotationDifference(a, b), 1e-5);
    EXPECT_LT((Vec3d(a.hipsPosition) - Vec3d(b.hipsPosition)).length(), 1e-6);

    // life loops seamlessly and sways
    const float length = static_cast<float>(input.timeline.length);
    for (int bone = 1; bone < HUMAN_BONE_COUNT; bone++){
        EXPECT_LT(rotationError(lifeRotation(bone, 0.0f, length, 2.0f, 7), lifeRotation(bone, length, length, 2.0f, 7)), 1e-6);
    }
    double sway = 0.0;
    for (int i = 0; i < 50; i++){
        sway = std::max(sway, rotationError(lifeRotation(5, length * i / 50.0f, length, 2.0f, 7), Quaternion()));
    }
    EXPECT_GT(sway, degToRad(0.5));
    EXPECT_LT(sway, degToRad(2.0 * 1.83 / 1.83 * 3.0));
    // the seed changes the phases; hash01 is deterministic and in [0, 1)
    EXPECT_GT(rotationError(lifeRotation(5, 0.3f, length, 2.0f, 7), lifeRotation(5, 0.3f, length, 2.0f, 8)), 1e-4);
    EXPECT_EQ(hash01(1, 2, 3, 4), hash01(1, 2, 3, 4));
    for (int i = 0; i < 100; i++){
        const double h = hash01(static_cast<uint32_t>(i), i % 22, i % 3, 1 + i % 3);
        EXPECT_GE(h, 0.0);
        EXPECT_LT(h, 1.0);
    }
    // legs and hips get no life
    PoseAnimationData still = makeAnimation({BodyPose(), BodyPose()}, true, 0.5f);
    still.settings.humanize.enabled = true;
    still.settings.humanize.movingHolds = 0.0f;
    still.settings.humanize.life = 3.0f;
    const SamplerInput stillInput = inputFor(still, rig);
    HumanPose s;
    sample(stillInput, 0.37f, s);
    EXPECT_LT(rotationError(s.rotations[boneIndex(H::LeftUpperLeg)], stillInput.keys[0].rotations[boneIndex(H::LeftUpperLeg)]), 1e-6);
    EXPECT_LT(rotationError(s.rotations[0], stillInput.keys[0].rotations[0]), 1e-6);
    EXPECT_GT(rotationError(s.rotations[boneIndex(H::Head)], stillInput.keys[0].rotations[boneIndex(H::Head)]), 1e-4);
}

TEST(PoseSampler, OverlapDelaysDeepBones){
    const PoseRig& rig = *tposeRig().rig;
    PoseAnimationData data = makeAnimation(threeBodies(), false, 0.3f);
    data.settings.transition.duration = 0.3f;
    data.settings.looseness = 0.0f;
    data.settings.overlap = 0.0f;
    const SamplerInput plain = inputFor(data, rig);
    data.settings.overlap = 0.1f;
    const SamplerInput delayed = inputFor(data, rig);
    const H hand = H::LeftHand;   // arms weight 1, depth 1: 0.1 s late
    const H upperArm = H::LeftUpperArm;   // depth 0.6: 0.06 s late
    for (double t = 0.35; t < 0.9; t += 0.05){
        HumanPose a, b;
        sample(delayed, static_cast<float>(t), a);
        sample(plain, static_cast<float>(t - 0.1), b);
        EXPECT_LT(rotationError(a.rotations[boneIndex(hand)], b.rotations[boneIndex(hand)]), 1e-5) << t;
        sample(plain, static_cast<float>(t - 0.06), b);
        EXPECT_LT(rotationError(a.rotations[boneIndex(upperArm)], b.rotations[boneIndex(upperArm)]), 1e-5) << t;
        // the hips (depth 0) and the legs (weight 0) are not delayed
        sample(plain, static_cast<float>(t), b);
        EXPECT_LT(rotationError(a.rotations[0], b.rotations[0]), 1e-6);
        EXPECT_LT(rotationError(a.rotations[boneIndex(H::LeftLowerLeg)], b.rotations[boneIndex(H::LeftLowerLeg)]), 1e-6);
    }
    // looseness: deep bones arrive later
    data.settings.overlap = 0.0f;
    data.settings.looseness = 1.0f;
    const SamplerInput loose = inputFor(data, rig);
    HumanPose a, b;
    const float arrival = static_cast<float>(plain.timeline.start[0] + 0.3);
    sample(loose, arrival, a);
    sample(plain, arrival, b);
    EXPECT_GT(rotationError(a.rotations[boneIndex(hand)], plain.keys[1].rotations[boneIndex(hand)]), 1e-3);
    EXPECT_LT(rotationError(b.rotations[boneIndex(hand)], plain.keys[1].rotations[boneIndex(hand)]), 1e-5);
}

TEST(PoseSampler, StepRateQuantises){
    const PoseRig& rig = *tposeRig().rig;
    PoseAnimationData data = makeAnimation(threeBodies(), true, 0.2f);
    data.settings.stepRate = 12.0f;
    const SamplerInput input = inputFor(data, rig);
    for (double t = 0.0; t < input.timeline.length; t += 0.013){
        const double stepped = std::floor(t * 12.0 + 1e-4) / 12.0;
        HumanPose a, b;
        sample(input, static_cast<float>(t), a);
        sample(input, static_cast<float>(stepped + 0.001), b);
        EXPECT_LT(maxRotationDifference(a, b), 1e-6) << t;
    }
}

TEST(PoseSampler, MovingHoldsDriftDuringAHold){
    const PoseRig& rig = *tposeRig().rig;
    PoseAnimationData data = makeAnimation(threeBodies(), false, 0.5f);
    const SamplerInput still = inputFor(data, rig);
    data.settings.humanize.enabled = true;
    data.settings.humanize.life = 0.0f;
    data.settings.humanize.inbetweens = 0.0f;
    data.settings.humanize.movingHolds = 0.2f;
    const SamplerInput drifting = inputFor(data, rig);
    HumanPose a, b;
    sample(still, 0.05f, a);
    sample(still, 0.45f, b);
    EXPECT_LT(rotationError(a.rotations[boneIndex(H::LeftUpperArm)], b.rotations[boneIndex(H::LeftUpperArm)]), 1e-6);
    sample(drifting, 0.05f, a);
    sample(drifting, 0.45f, b);
    EXPECT_GT(rotationError(a.rotations[boneIndex(H::LeftUpperArm)], b.rotations[boneIndex(H::LeftUpperArm)]), degToRad(3.0));
}

TEST(PoseSampler, FramesCountAndLoopSeam){
    const PoseRig& rig = *tposeRig().rig;
    for (bool loop : {true, false}){
        PoseAnimationData data = makeAnimation(threeBodies(), loop, 0.2f);
        data.settings.humanize.enabled = true;
        data.settings.humanize.life = 1.0f;
        const SamplerInput input = inputFor(data, rig);
        for (int fps : {24, 30, 60}){
            const std::vector<HumanPose> f = frames(input, fps);
            const int count = std::max(1, static_cast<int>(std::lround(input.timeline.length * fps)));
            ASSERT_EQ(f.size(), static_cast<size_t>(count + 1));
            if (loop){
                for (int s = 0; s < HUMAN_BONE_COUNT; s++){
                    EXPECT_EQ(f.back().rotations[s], f.front().rotations[s]);
                }
            }
        }
    }
}

TEST(PoseSampler, InbetweensCapThePerFrameRotation){
    const PoseRig& rig = *tposeRig().rig;
    PoseAnimationData data = makeAnimation({bodyWith({{"left_upper_arm.raise", 0}, {"left_lower_arm.bend", 0}}),
                                            bodyWith({{"left_upper_arm.raise", 160}, {"left_lower_arm.bend", 140}})}, true, 0.6f);
    data.settings.transition.curve = TransitionCurve::Linear;
    data.settings.transition.duration = 0.02f;
    data.settings.overlap = 0.0f;
    data.settings.humanize.enabled = true;
    data.settings.humanize.life = 0.0f;
    data.settings.humanize.movingHolds = 0.0f;
    data.settings.humanize.torsoFollow = 0.0f;
    data.settings.humanize.headFollow = 0.0f;
    data.settings.humanize.inbetweens = 0.0f;
    data.settings.humanize.jointLimits = false;
    const int fps = 30;
    auto worstStep = [&](const std::vector<HumanPose>& f){
        double worst = 0.0;
        for (size_t i = 1; i < f.size(); i++) worst = std::max(worst, maxRotationDifference(f[i - 1], f[i]));
        return worst;
    };
    const double uncapped = worstStep(frames(inputFor(data, rig), fps));
    EXPECT_GT(radToDeg(uncapped), 100.0);
    data.settings.humanize.inbetweens = 1.0f;
    const std::vector<HumanPose> capped = frames(inputFor(data, rig), fps);
    const double maxDeg = 12.0 * 30 / fps;
    EXPECT_LT(radToDeg(worstStep(capped)), maxDeg + 1e-3);
    data.settings.humanize.inbetweens = 0.5f;
    EXPECT_LT(radToDeg(worstStep(frames(inputFor(data, rig), 60))), (180.0 + (12.0 - 180.0) * 0.5) * 30 / 60 + 1e-3);
}

TEST(PoseSampler, OnePoseIsStatic){
    const PoseRig& rig = *tposeRig().rig;
    PoseAnimationData data = makeAnimation({bodyWith({{"spine.bend", 20}})}, true, 0.5f);
    data.settings.humanize.enabled = true;
    data.settings.humanize.life = 3.0f;
    const SamplerInput input = inputFor(data, rig);
    HumanPose out;
    for (float t : {0.0f, 0.13f, 0.5f, 3.0f}){
        sample(input, t, out);
        EXPECT_LT(maxRotationDifference(out, input.keys[0]), 1e-7);
    }
    const std::vector<HumanPose> f = frames(input, 30);
    EXPECT_EQ(f.size(), 16u);
    for (const HumanPose& p : f) EXPECT_LT(maxRotationDifference(p, input.keys[0]), 1e-7);
    // empty: no frames, out untouched
    const SamplerInput empty = makeSamplerInput(PoseAnimationData(), {}, &rig);
    EXPECT_TRUE(frames(empty, 30).empty());
}

TEST(PoseSampler, FramesRespectJointLimits){
    const PoseRig& rig = *tposeRig().rig;
    // a big overshoot past the elbow limit
    PoseAnimationData data = makeAnimation({bodyWith({{"left_lower_arm.bend", 0}}), bodyWith({{"left_lower_arm.bend", 150}})}, false, 0.3f);
    data.settings.transition.overshoot = 0.9f;
    data.settings.humanize.jointLimits = false;
    int dof = -1;
    dofFromName("left_lower_arm.bend", dof);
    double peak = 0.0;
    for (const HumanPose& p : frames(inputFor(data, rig), 60)) peak = std::max<double>(peak, rig.toBodyPose(p).angles[dof]);
    EXPECT_GT(peak, 155.5);
    data.settings.humanize.jointLimits = true;
    for (const HumanPose& p : frames(inputFor(data, rig), 60)){
        const BodyPose body = rig.toBodyPose(p);
        for (int d = 0; d < DOF_COUNT; d++){
            EXPECT_LE(body.angles[d], dofInfo(d).maxDegrees + 1e-3f) << dofName(d);
            EXPECT_GE(body.angles[d], dofInfo(d).minDegrees - 1e-3f) << dofName(d);
        }
    }
}

TEST(PoseAutoTiming, PreservesLengthAndScalesWithMotion){
    const PoseRig& rig = *tposeRig().rig;
    // segment 0 is a big move, segment 1 small, segment 2 (loop back) big
    PoseAnimationData data = makeAnimation({bodyWith({{"left_upper_arm.raise", 0}}), bodyWith({{"left_upper_arm.raise", 150}, {"spine.bend", 30}}),
                                            bodyWith({{"left_upper_arm.raise", 140}, {"spine.bend", 30}})}, true, 0.2f);
    const std::vector<HumanPose> keys = realiseKeys(data, rig);
    const double before = buildTimeline(data).length;
    autoTiming(data, keys, rig.humanScale());
    EXPECT_NEAR(buildTimeline(data).length, before, 1e-5);
    for (const PoseKey& key : data.poses) EXPECT_TRUE(key.customTransition);
    EXPECT_GT(data.poses[0].transition.duration, data.poses[1].transition.duration);   // larger motion: longer snap
    EXPECT_GT(data.poses[1].hold, data.poses[2].hold);                                  // after a big move: a longer hold
    // the rig form (hips travel in model space: the T-pose rig's hips parent is scaled 0.01)
    PoseAnimationData viaRig = makeAnimation({bodyWith({{"left_upper_arm.raise", 0}}), bodyWith({{"left_upper_arm.raise", 150}, {"spine.bend", 30}}),
                                              bodyWith({{"left_upper_arm.raise", 140}, {"spine.bend", 30}})}, true, 0.2f);
    autoTiming(viaRig, keys, rig);
    EXPECT_NEAR(buildTimeline(viaRig).length, before, 1e-5);
    EXPECT_GT(viaRig.poses[0].transition.duration, viaRig.poses[1].transition.duration);
}

TEST(PoseAutoTiming, SnapsToTheStepGrid){
    const PoseRig& rig = *aposeRig().rig;
    PoseAnimationData data = makeAnimation(threeBodies(), true, 0.25f);
    applyFeelPreset(*findFeelPreset("Stop-motion"), data.settings);
    autoTiming(data, realiseKeys(data, rig), rig);
    const double step = 1.0 / 12.0;
    for (const PoseKey& key : data.poses){
        EXPECT_NEAR(std::fmod(key.hold + 1e-6, step), 0.0, 1e-5) << key.hold;
        EXPECT_NEAR(std::fmod(key.transition.duration + 1e-6, step), 0.0, 1e-5) << key.transition.duration;
        EXPECT_GE(key.hold, step - 1e-6);
        EXPECT_GE(key.transition.duration, step - 1e-6);
    }
}

TEST(PoseAutoTiming, ZeroMotionIsANoOp){
    const PoseRig& rig = *tposeRig().rig;
    PoseAnimationData data = makeAnimation({BodyPose(), BodyPose(), BodyPose()}, true, 0.2f);
    data.poses[1].hold = 0.4f;
    const PoseAnimationData before = data;
    autoTiming(data, realiseKeys(data, rig), rig.humanScale());
    for (size_t i = 0; i < data.poses.size(); i++){
        EXPECT_EQ(data.poses[i].hold, before.poses[i].hold);
        EXPECT_EQ(data.poses[i].customTransition, before.poses[i].customTransition);
        EXPECT_EQ(data.poses[i].transition.duration, before.poses[i].transition.duration);
    }
}
