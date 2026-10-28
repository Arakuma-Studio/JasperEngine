// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Acceptance test 13: head and torso follow-through (spec 4.9)

#include "TestUtil.h"

#include "pose/PoseSampler.h"
#include "pose/PoseSecondary.h"

#include <gtest/gtest.h>

using namespace posetest;
using H = HumanBone;

namespace {

    HumanizeSettings followSettings(){
        HumanizeSettings h;
        h.enabled = true;
        h.torsoFollow = 0.4f;
        h.headFollow = 0.5f;
        h.springiness = 0.5f;
        return h;
    }

    const int FPS = 30;

}

TEST(PoseFollowThrough, AcceleratingForwardLeansTheTorsoBack){
    std::vector<Vector3> hips;
    for (int f = 0; f <= 30; f++){
        const double t = f / static_cast<double>(FPS);
        hips.push_back(Vector3(0, 0, static_cast<float>(0.5 * 6.0 * t * t)));   // 6 humanScale/s^2 forward
    }
    const std::vector<FollowTilt> tilts = followThroughTilts(hips, FPS, false, followSettings());
    for (int f = 5; f < 29; f++){
        // steady state: lean = 300 * amount * (-acc / omega^2) = -1.49 degrees for the torso
        EXPECT_LT(tilts[f].torsoLean, -1.0f) << f;
        EXPECT_LT(tilts[f].headLean, -1.0f) << f;
        EXPECT_NEAR(tilts[f].torsoSide, 0.0f, 1e-6f);
    }
    // sideways acceleration to the left tilts away (to the right)
    std::vector<Vector3> side;
    for (int f = 0; f <= 30; f++){
        const double t = f / static_cast<double>(FPS);
        side.push_back(Vector3(static_cast<float>(0.5 * 6.0 * t * t), 0, 0));
    }
    EXPECT_LT(followThroughTilts(side, FPS, false, followSettings())[15].torsoSide, -1.0f);
}

TEST(PoseFollowThrough, DropTiltsTheChinUpAndLandingLeansForward){
    std::vector<Vector3> hips;
    const int land = 15;
    for (int f = 0; f <= 40; f++){
        const double t = std::min(f, land) / static_cast<double>(FPS);
        hips.push_back(Vector3(0, static_cast<float>(-0.5 * 9.81 * t * t), 0));   // falls, then lands and stops
    }
    const std::vector<FollowTilt> tilts = followThroughTilts(hips, FPS, false, followSettings());
    for (int f = 4; f < land; f++){
        EXPECT_LT(tilts[f].headLean, -1.0f) << f;   // chin up
        EXPECT_LT(tilts[f].torsoLean, -1.0f) << f;
    }
    float peak = -100.0f;
    for (int f = land; f < land + 8; f++) peak = std::max(peak, tilts[f].torsoLean);
    EXPECT_GT(peak, 1.0f);   // landing leans forward
    for (const FollowTilt& t : tilts){
        EXPECT_LE(std::fabs(t.torsoLean), 35.0f);
        EXPECT_LE(std::fabs(t.headLean), 35.0f);
    }
}

TEST(PoseFollowThrough, LoopSteadyState){
    // two identical cycles of a bobbing, swaying hips path in one loop
    const int period = 24;
    std::vector<Vector3> hips;
    for (int f = 0; f <= 2 * period; f++){
        const double phase = 2.0 * POSE_PI * f / period;
        hips.push_back(Vector3(static_cast<float>(0.03 * std::sin(phase)), static_cast<float>(0.05 * std::sin(2 * phase)),
                               static_cast<float>(0.04 * std::cos(phase))));
    }
    HumanizeSettings h = followSettings();
    h.springiness = 0.3f;
    const std::vector<FollowTilt> tilts = followThroughTilts(hips, FPS, true, h);
    ASSERT_EQ(tilts.size(), hips.size());
    EXPECT_EQ(tilts.back().torsoLean, tilts.front().torsoLean);
    EXPECT_EQ(tilts.back().headSide, tilts.front().headSide);
    double motion = 0.0;
    for (int f = 0; f < period; f++){
        EXPECT_NEAR(tilts[f].torsoLean, tilts[f + period].torsoLean, 0.02) << f;
        EXPECT_NEAR(tilts[f].headLean, tilts[f + period].headLean, 0.02) << f;
        EXPECT_NEAR(tilts[f].torsoSide, tilts[f + period].torsoSide, 0.02) << f;
        motion = std::max(motion, std::fabs(static_cast<double>(tilts[f].torsoLean)));
    }
    EXPECT_GT(motion, 0.5);
}

TEST(PoseFollowThrough, AppliedToFramesLeansTheChain){
    for (const TestRig* t : {&tposeRig(), &minimalRig()}){
        const PoseRig& rig = *t->rig;
        // the hips accelerate forward over the frames
        std::vector<HumanPose> frames;
        for (int f = 0; f <= 20; f++){
            HumanPose pose = rig.neutralPose();
            const double tt = f / static_cast<double>(FPS);
            const Vec3d hips = Vec3d(rig.neutralHipsWorldPosition()) + toModel(rig, Vec3d(0, 0, 0.5 * 10.0 * tt * tt * rig.humanScale()));
            rig.setHipsWorldPosition(pose, hips.toVector3());
            frames.push_back(pose);
        }
        const std::vector<HumanPose> original = frames;
        applyFollowThrough(frames, rig, FPS, false, followSettings());
        const int f = 12;
        const Vec3d headBefore = toCanonical(rig, slotPosition(rig, original[f], H::Head) - slotPosition(rig, original[f], H::Hips));
        const Vec3d headAfter = toCanonical(rig, slotPosition(rig, frames[f], H::Head) - slotPosition(rig, frames[f], H::Hips));
        EXPECT_LT(headAfter.z, headBefore.z - 0.01) << t->label;   // the torso leans back
        // only the spine and head chains changed
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            const H bone = boneAt(s);
            const bool chain = bone == H::Spine || bone == H::Chest || bone == H::UpperChest || bone == H::Neck || bone == H::Head;
            if (!chain){
                EXPECT_EQ(frames[f].rotations[s], original[f].rotations[s]) << humanBoneName(bone);
            }
        }
        // the torso tilt is shared by the present chain bones and sums to the full tilt
        const std::vector<Vector3> hipsPath = [&](){
            std::vector<Vector3> p;
            for (const HumanPose& pose : original){
                p.push_back((toCanonical(rig, Vec3d(rig.hipsWorldPosition(pose))) / rig.humanScale()).toVector3());
            }
            return p;
        }();
        const FollowTilt tilt = followThroughTilts(hipsPath, FPS, false, followSettings())[f];
        std::array<Quatd, HUMAN_BONE_COUNT> before, after;
        rig.slotWorldCanonical(original[f], before);
        rig.slotWorldCanonical(frames[f], after);
        const H lastTorso = rig.has(H::UpperChest) ? H::UpperChest : (rig.has(H::Chest) ? H::Chest : H::Spine);
        const Quatd torsoDelta = after[boneIndex(lastTorso)] * before[boneIndex(lastTorso)].inverse();
        EXPECT_NEAR(radToDeg(angleBetween(torsoDelta, Quatd())), std::fabs(tilt.torsoLean), 1e-3) << t->label;
    }
}

TEST(PoseFollowThrough, FramesUseItWithHumanize){
    const PoseRig& rig = *tposeRig().rig;
    PoseAnimationData data = makeAnimation({BodyPose(), bodyWith({{"left_upper_leg.forward", 60}, {"left_lower_leg.bend", 100}})}, true, 0.1f);
    data.poses[1].body.offset = Vector3(0, 0, 0.4f);
    data.settings.humanize.enabled = true;
    data.settings.humanize.inbetweens = 0.0f;
    data.settings.humanize.life = 0.0f;
    data.settings.humanize.movingHolds = 0.0f;
    const SamplerInput with = makeSamplerInput(data, realiseKeys(data, rig), &rig);
    data.settings.humanize.torsoFollow = 0.0f;
    data.settings.humanize.headFollow = 0.0f;
    const SamplerInput without = makeSamplerInput(data, realiseKeys(data, rig), &rig);
    const std::vector<HumanPose> a = frames(with, 30), b = frames(without, 30);
    ASSERT_EQ(a.size(), b.size());
    double difference = 0.0;
    for (size_t i = 0; i < a.size(); i++){
        difference = std::max(difference, rotationError(a[i].rotations[boneIndex(H::Spine)], b[i].rotations[boneIndex(H::Spine)]));
    }
    EXPECT_GT(difference, degToRad(0.5));
    EXPECT_EQ(a.back().rotations[boneIndex(H::Head)], a.front().rotations[boneIndex(H::Head)]);
}
