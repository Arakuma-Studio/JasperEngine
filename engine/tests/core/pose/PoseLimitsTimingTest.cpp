// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Review round 1 regressions: swings longer than 180 degrees and the joint limits (spec 4.5),
// auto timing with clamped durations and step snapping (4.8), the 3ds Max biped neck (4.6)

#include "TestUtil.h"

#include "pose/PoseSampler.h"
#include "pose/PoseTween.h"

#include <gtest/gtest.h>

using namespace posetest;
using H = HumanBone;

TEST(PoseLimits, SwingIsCappedBelow180){
    for (float v : {120.0f, 130.0f, 150.0f}){
        BodyPose body = bodyWith({{"left_upper_arm.raise", v}, {"left_upper_arm.forward", v}});
        const std::vector<int> clamped = clampToLimits(body);
        const float raise = body.angles[dofIndex(H::LeftUpperArm, 0)];
        const float forward = body.angles[dofIndex(H::LeftUpperArm, 1)];
        const double swing = std::sqrt(static_cast<double>(raise) * raise + static_cast<double>(forward) * forward);
        if (v * std::sqrt(2.0) <= MAX_SWING_DEGREES){
            EXPECT_TRUE(clamped.empty()) << v;
            EXPECT_EQ(raise, v);
        }else{
            EXPECT_EQ(clamped, (std::vector<int>{dofIndex(H::LeftUpperArm, 0), dofIndex(H::LeftUpperArm, 1)})) << v;
            EXPECT_NEAR(swing, MAX_SWING_DEGREES, 1e-4) << v;
            EXPECT_NEAR(raise, forward, 1e-5);   // the direction is kept
        }
    }
    // per-DOF clamping first, then the cap; ranges all contain 0 so the result stays inside
    BodyPose body = bodyWith({{"right_upper_leg.forward", 125}, {"right_upper_leg.out", 70}, {"hips.lean", 170}, {"hips.side", 170}});
    clampToLimits(body);
    float leg[3] = {body.angles[dofIndex(H::RightUpperLeg, 0)], body.angles[dofIndex(H::RightUpperLeg, 1)], 0};
    EXPECT_TRUE(boneWithinLimits(H::RightUpperLeg, leg));
    float hips[3] = {body.angles[0], body.angles[1], 0};
    EXPECT_TRUE(boneWithinLimits(H::Hips, hips));
    EXPECT_NEAR(std::hypot(hips[0], hips[1]), MAX_SWING_DEGREES, 1e-4);
    EXPECT_TRUE(clampToLimits(body).empty());
}

TEST(PoseLimits, WithinLimitsMatchesTheSwingCap){
    // boneWithinLimits accepts exactly what clampBoneToLimits leaves unchanged: a swing between
    // the cap and 180 (inside every per-DOF hips range) is not within limits, and every clamp
    // result is (the float rounding of the scaled pair never lands above the cap)
    for (float length : {179.6f, 179.9f, 180.0f}){
        float a[3] = {length, 0.0f, 0.0f};
        EXPECT_FALSE(boneWithinLimits(H::Hips, a)) << length;
        EXPECT_EQ(clampBoneToLimits(H::Hips, a), 3u) << length;
        EXPECT_TRUE(boneWithinLimits(H::Hips, a)) << length;
    }
    int checked = 0;
    for (H bone : {H::Hips, H::LeftUpperArm, H::RightUpperLeg}){
        for (int k = 0; k < 360; k++){
            const double theta = degToRad(k * 1.0 + 0.37);
            for (double length : {170.0, 179.4, 179.5, 179.50001, 179.7, 200.0, 400.0}){
                float a[3] = {static_cast<float>(length * std::cos(theta)), static_cast<float>(length * std::sin(theta)), 10.0f};
                const bool within = boneWithinLimits(bone, a);
                float b[3] = {a[0], a[1], a[2]};
                const unsigned changed = clampBoneToLimits(bone, b);
                EXPECT_EQ(within, changed == 0u) << humanBoneName(bone) << " " << k << " " << length;
                EXPECT_TRUE(boneWithinLimits(bone, b)) << humanBoneName(bone) << " " << k << " " << length;
                EXPECT_EQ(clampBoneToLimits(bone, b), 0u) << humanBoneName(bone) << " " << k << " " << length;
                checked++;
            }
        }
    }
    EXPECT_EQ(checked, 3 * 360 * 7);
}

TEST(PoseLimits, FramesKeepKeysInsideTheLimits){
    // reviewer probe 3: raise = forward = 120 / 130 / 150 (130 and 150 are capped to a 179.5-degree
    // swing by the key clamp); the frames' limit pass must not turn the key into another pose
    for (const TestRig* t : allRigs()){
        const PoseRig& rig = *t->rig;
        for (float v : {120.0f, 127.27922f, 130.0f, 150.0f}){
            const BodyPose body = bodyWith({{"left_upper_arm.raise", v}, {"left_upper_arm.forward", v}});
            PoseAnimationData data = makeAnimation({body}, false, 0.5f);
            const SamplerInput input = makeSamplerInput(data, realiseKeys(data, rig), &rig);
            const std::vector<HumanPose> f = frames(input, 30);
            const HumanPose& key = input.keys[0];
            for (const HumanPose& frame : f){
                EXPECT_LT(radToDeg(rotationError(key.rotations[boneIndex(H::LeftUpperArm)], frame.rotations[boneIndex(H::LeftUpperArm)])), 1e-3)
                    << t->label << " " << v;
            }
        }
    }
    // a frame swinging beyond 180 degrees inside the limits (long form) is kept, not clamped
    const PoseRig& rig = *tposeRig().rig;
    const float longSwing[3] = {150.0f, 150.0f, 0.0f};   // 212 degrees, each value inside the limits
    HumanPose pose = rig.neutralPose();
    pose.rotations[boneIndex(H::LeftUpperArm)] = rig.localFromAngles(H::LeftUpperArm, longSwing);
    PoseAnimationData data = makeAnimation({BodyPose()}, false, 0.2f);
    SamplerInput input = makeSamplerInput(data, {pose}, &rig);
    const std::vector<HumanPose> f = frames(input, 30);
    EXPECT_LT(rotationError(f[0].rotations[boneIndex(H::LeftUpperArm)], pose.rotations[boneIndex(H::LeftUpperArm)]), 1e-6);
    // ... while a rotation outside the limits either way is clamped
    const float outside[3] = {-60.0f, 0.0f, 0.0f};
    pose.rotations[boneIndex(H::LeftUpperArm)] = rig.localFromAngles(H::LeftUpperArm, outside);
    input = makeSamplerInput(data, {pose}, &rig);
    float back[3];
    rig.anglesFromLocal(H::LeftUpperArm, frames(input, 30)[0].rotations[boneIndex(H::LeftUpperArm)], back);
    EXPECT_NEAR(back[0], -40.0f, 1e-3);
}

TEST(PoseAutoTiming, ClampedDurationsKeepTheLength){
    const PoseRig& rig = *tposeRig().rig;
    // a long base duration: the big move's 1.6x snap exceeds 1.0 s and is clamped
    PoseAnimationData data = makeAnimation({bodyWith({{"left_upper_arm.raise", 0}}), bodyWith({{"left_upper_arm.raise", 170}, {"spine.bend", 30}}),
                                            bodyWith({{"left_upper_arm.raise", 165}, {"spine.bend", 30}})}, true, 0.3f);
    data.settings.transition.duration = 0.9f;
    const double before = buildTimeline(data).length;
    autoTiming(data, realiseKeys(data, rig), rig);
    EXPECT_NEAR(buildTimeline(data).length, before, 1e-5);
    bool clampedOne = false;
    for (const PoseKey& key : data.poses){
        EXPECT_LE(key.transition.duration, 1.0f);
        EXPECT_GE(key.transition.duration, 0.02f);
        clampedOne = clampedOne || key.transition.duration == 1.0f;
    }
    EXPECT_TRUE(clampedOne);

    // step snapping: whole steps, the total within half a step of the old length
    for (float stepRate : {8.0f, 12.0f, 24.0f}){
        PoseAnimationData stepped = makeAnimation({bodyWith({{"left_upper_arm.raise", 0}}), bodyWith({{"left_upper_arm.raise", 120}}),
                                                   bodyWith({{"left_upper_arm.raise", 110}}), bodyWith({{"spine.bend", 20}})}, true, 0.23f);
        stepped.settings.stepRate = stepRate;
        const double old = buildTimeline(stepped).length;
        autoTiming(stepped, realiseKeys(stepped, rig), rig);
        const double step = 1.0 / stepRate;
        EXPECT_LE(std::fabs(buildTimeline(stepped).length - old), 0.5 * step + 1e-6) << stepRate;
        for (const PoseKey& key : stepped.poses){
            EXPECT_NEAR(std::round(key.hold / step) * step, key.hold, 1e-5);
            EXPECT_NEAR(std::round(key.transition.duration / step) * step, key.transition.duration, 1e-5);
            EXPECT_GE(key.hold, step - 1e-6);
        }
    }
    // a length on the grid is kept exactly
    PoseAnimationData exact = makeAnimation({bodyWith({{"left_upper_arm.raise", 0}}), bodyWith({{"left_upper_arm.raise", 120}}),
                                             bodyWith({{"spine.bend", 20}})}, true, 0.25f);
    exact.settings.stepRate = 12.0f;
    exact.settings.transition.duration = 1.0f / 6.0f;
    const double old = buildTimeline(exact).length;
    autoTiming(exact, realiseKeys(exact, rig), rig);
    EXPECT_NEAR(buildTimeline(exact).length, old, 1e-5);
}

TEST(PoseHumanoidMap, RealBipedClaviclesUnderTheNeck){
    std::vector<std::pair<std::string, std::string>> bones = {
        {"Bip01", ""}, {"Bip01 Pelvis", "Bip01"}, {"Bip01 Spine", "Bip01 Pelvis"}, {"Bip01 Spine1", "Bip01 Spine"},
        {"Bip01 Neck", "Bip01 Spine1"}, {"Bip01 Head", "Bip01 Neck"}, {"Bip01 HeadNub", "Bip01 Head"}};
    for (const char* s : {"L", "R"}){
        const std::string x = std::string("Bip01 ") + s + " ";
        bones.push_back({x + "Clavicle", "Bip01 Neck"});
        bones.push_back({x + "UpperArm", x + "Clavicle"});
        bones.push_back({x + "Forearm", x + "UpperArm"});
        bones.push_back({x + "Hand", x + "Forearm"});
        bones.push_back({x + "Finger0", x + "Hand"});
        bones.push_back({x + "Thigh", "Bip01 Pelvis"});
        bones.push_back({x + "Calf", x + "Thigh"});
        bones.push_back({x + "Foot", x + "Calf"});
        bones.push_back({x + "Toe0", x + "Foot"});
        bones.push_back({x + "Toe0Nub", x + "Toe0"});
    }
    const Skeleton skeleton = skeletonFromList(bones);
    std::vector<std::string> notes;
    const HumanoidMap map = autoDetectHumanoid(skeleton, &notes);
    EXPECT_EQ(map.bones[boneIndex(H::Neck)], "");
    EXPECT_EQ(map.bones[boneIndex(H::Head)], "Bip01 Head");
    EXPECT_EQ(map.bones[boneIndex(H::Spine)], "Bip01 Spine");
    EXPECT_EQ(map.bones[boneIndex(H::Chest)], "Bip01 Spine1");
    EXPECT_EQ(map.bones[boneIndex(H::LeftShoulder)], "Bip01 L Clavicle");
    EXPECT_EQ(map.bones[boneIndex(H::RightUpperArm)], "Bip01 R UpperArm");
    std::string error;
    EXPECT_TRUE(validateHumanoid(skeleton, map, &error)) << error;
    bool note = false;
    for (const std::string& n : notes) note = note || n.find("neck: 'Bip01 Neck' left unmapped") == 0;
    EXPECT_TRUE(note);
    // the map with the neck is invalid, as before
    HumanoidMap withNeck = map;
    withNeck.bones[boneIndex(H::Neck)] = "Bip01 Neck";
    EXPECT_FALSE(validateHumanoid(skeleton, withNeck, &error));
}

TEST(PoseLimits, TwistedKeysAtTheSwingCapSurviveTheFrames){
    // reviewer probe 8: keys whose swing is capped (179.5 degrees) with twists 0/20/45/90 come
    // back from frames() unchanged; at exactly 180 the twist is undefined and the frames' limit
    // pass altered up to 45 of 75 keys by up to 139 degrees
    int cases = 0;
    for (H bone : {H::LeftUpperArm, H::RightUpperLeg, H::Hips}){
        for (float twist : {0.0f, 20.0f, 45.0f, 90.0f}){
            for (const TestRig* t : allRigs()){
                const PoseRig& rig = *t->rig;
                for (int k = 0; k < 72; k++){
                    const double theta = degToRad(k * 5.0);
                    float a[3] = {static_cast<float>(300.0 * std::cos(theta)), static_cast<float>(300.0 * std::sin(theta)), twist};
                    clampBoneToLimits(bone, a);
                    if (std::hypot(a[0], a[1]) < MAX_SWING_DEGREES - 1e-3){
                        continue;   // the limits stop this direction before the cap
                    }
                    BodyPose body;
                    for (int j = 0; j < 3; j++) body.angles[dofIndex(bone, j)] = a[j];
                    PoseAnimationData data = makeAnimation({body}, false, 0.2f);
                    data.settings.humanize.enabled = true;
                    data.settings.humanize.life = 0.0f;
                    data.settings.humanize.movingHolds = 0.0f;
                    const SamplerInput input = makeSamplerInput(data, realiseKeys(data, rig), &rig);
                    const std::vector<HumanPose> f = frames(input, 30);
                    cases++;
                    EXPECT_LT(radToDeg(rotationError(input.keys[0].rotations[boneIndex(bone)], f[0].rotations[boneIndex(bone)])), 0.1)
                        << t->label << " " << humanBoneName(bone) << " direction " << k * 5 << " twist " << twist;
                }
            }
        }
    }
    EXPECT_GT(cases, 500);
}
