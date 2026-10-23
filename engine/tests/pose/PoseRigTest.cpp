// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Acceptance tests 3-9: character frame, neutral pose, joint-angle round trips, semantics,
// cross-rig equivalence, mirror and joint limits (spec 4.3-4.5, 4.7)

#include "TestUtil.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace posetest;
using H = HumanBone;

namespace {

    const H AIMED[] = {H::Hips, H::Spine, H::Chest, H::UpperChest, H::Neck, H::LeftShoulder, H::RightShoulder,
                       H::LeftUpperArm, H::RightUpperArm, H::LeftLowerArm, H::RightLowerArm,
                       H::LeftUpperLeg, H::RightUpperLeg, H::LeftLowerLeg, H::RightLowerLeg};

}

TEST(PoseRig, CharacterFrame){
    for (const TestRig* t : allRigs()){
        const PoseRig& rig = *t->rig;
        const Vec3d forward(rig.characterForward());
        const Vec3d up(rig.characterUp());
        const double expectedZ = t == &backwardRig() ? -1.0 : 1.0;
        EXPECT_NEAR(forward.x, 0.0, 1e-6) << t->label;
        EXPECT_NEAR(forward.y, 0.0, 1e-6) << t->label;
        EXPECT_NEAR(forward.z, expectedZ, 1e-6) << t->label;
        EXPECT_NEAR(up.y, 1.0, 1e-6) << t->label;
        EXPECT_NEAR(Vec3d(rig.characterLeft()).x, expectedZ, 1e-6) << t->label;
    }
}

TEST(PoseRig, ScaleAndFloor){
    const PoseRig& rig = *tposeRig().rig;
    // sole estimate: the lowest foot/toes bone (toes at 0.02); the neutral hips rise slightly
    // above the rest 0.95 (the rest knees are bent 1 cm, the neutral legs are straight)
    EXPECT_NEAR(rig.floorHeight(), 0.02, 1e-6);
    EXPECT_NEAR(rig.humanScale(), (rig.neutralHipsWorldPosition().y - rig.floorHeight()) / 0.95, 1e-6);
    EXPECT_NEAR(rig.humanScale(), (0.95 - 0.02) / 0.95, 0.01);
    EXPECT_NEAR(rig.soleOffset(H::LeftFoot), 0.06, 1e-6);
    EXPECT_NEAR(rig.soleOffset(H::LeftToes), 0.0, 1e-6);
    // with the skin the floor is the lowest vertex
    EXPECT_NEAR(tposeRig().skinned->floorHeight(), 0.0, 1e-6);
    EXPECT_NEAR(tposeRig().skinned->humanScale(), tposeRig().skinned->neutralHipsWorldPosition().y / 0.95, 1e-6);
    EXPECT_NEAR(tposeRig().skinned->humanScale(), 1.0, 0.01);
    EXPECT_NEAR(tposeRig().skinned->soleOffset(H::LeftToes), 0.02, 1e-6);
    // setSkin after build recomputes the same values
    PoseRig late;
    std::string error;
    ASSERT_TRUE(late.build(tposeRig().skeleton, tposeRig().expected, &error)) << error;
    late.setSkin(tposeRig().skin);
    EXPECT_NEAR(late.floorHeight(), 0.0, 1e-6);
    EXPECT_NEAR(late.humanScale(), tposeRig().skinned->humanScale(), 1e-6);
}

TEST(PoseRig, RestPoseReproducesTheSkeleton){
    for (const TestRig* t : allRigs()){
        const PoseRig& rig = *t->rig;
        std::vector<Quatd> rot;
        std::vector<Affine3d> world;
        rig.computeWorldPrecise(rig.restPose(), rot, world);
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            if (!rig.has(boneAt(s))) continue;
            EXPECT_LT(rotationError(rig.restPose().rotations[s], t->skeleton.bones[rig.boneIndex(boneAt(s))].rotation), 1e-6);
        }
        std::vector<Matrix4> matrices;
        rig.computeWorldMatrices(rig.restPose(), matrices);
        ASSERT_EQ(matrices.size(), world.size());
        for (size_t i = 0; i < world.size(); i++){
            EXPECT_NEAR(matrices[i].get(3, 1), world[i].t.y, 1e-5);
            EXPECT_NEAR(matrices[i].get(0, 0), world[i].m[0][0], 1e-5);
        }
    }
}

TEST(PoseRig, NeutralDirectionsAndZeroAngles){
    for (const TestRig* t : allRigs()){
        const PoseRig& rig = *t->rig;
        const HumanPose& neutral = rig.neutralPose();
        for (H bone : AIMED){
            if (!rig.has(bone)) continue;
            const Vec3d d(boneDofInfo(bone).neutralDirection);
            EXPECT_LT(degreesBetween(aimDirection(rig, neutral, bone), d), 0.5) << t->label << " " << humanBoneName(bone);
        }
        const BodyPose body = rig.toBodyPose(neutral);
        for (int d = 0; d < DOF_COUNT; d++){
            EXPECT_LT(std::fabs(body.angles[d]), 1e-3) << t->label << " " << dofName(d);
        }
        EXPECT_LT(std::fabs(body.offset.x), 1e-5);
        EXPECT_LT(std::fabs(body.offset.z), 1e-5);
        // the neutral pose is grounded
        EXPECT_NEAR(rig.lowestPointPrecise(neutral, nullptr), rig.floorHeight(), 1e-6) << t->label;
        // carried bones keep their rest rotation relative to their effective parent
        std::array<Quatd, HUMAN_BONE_COUNT> neutralWorld, restWorld;
        rig.slotWorldCanonical(neutral, neutralWorld);
        rig.slotWorldCanonical(rig.restPose(), restWorld);
        for (H bone : {H::Head, H::LeftHand, H::RightFoot, H::LeftToes}){
            if (!rig.has(bone)) continue;
            const int p = boneIndex(rig.effectiveParent(bone));
            const Quatd neutralRel = neutralWorld[p].inverse() * neutralWorld[boneIndex(bone)];
            const Quatd restRel = restWorld[p].inverse() * restWorld[boneIndex(bone)];
            EXPECT_LT(angleBetween(neutralRel, restRel), 1e-6) << t->label << " " << humanBoneName(bone);
        }
    }
    // T-pose and A-pose neutral: the same directions
    for (H bone : AIMED){
        EXPECT_LT(degreesBetween(aimDirection(*tposeRig().rig, tposeRig().rig->neutralPose(), bone),
                                 aimDirection(*aposeRig().rig, aposeRig().rig->neutralPose(), bone)), 0.5) << humanBoneName(bone);
    }
}

TEST(PoseRig, BodyHumanBodyRoundTrip){
    std::mt19937 rng(1234);
    for (const TestRig* t : allRigs()){
        const PoseRig& rig = *t->rig;
        double worst = 0.0;
        for (int i = 0; i < 200; i++){
            const BodyPose body = randomBodyPose(rng, rig);
            const HumanPose human = rig.fromBodyPose(body, nullptr, true, 0.0f);
            const BodyPose back = rig.toBodyPose(human);
            for (int d = 0; d < DOF_COUNT; d++){
                if (!rig.has(dofBone(d))) continue;
                const double error = std::fabs(back.angles[d] - body.angles[d]);
                worst = std::max(worst, error);
                ASSERT_LT(error, 1e-3) << t->label << " " << dofName(d) << " " << body.angles[d] << " -> " << back.angles[d];
            }
            EXPECT_NEAR(back.offset.x, body.offset.x, 1e-4);
            EXPECT_NEAR(back.offset.z, body.offset.z, 1e-4);
            EXPECT_EQ(back.offset.y, 0.0f);
        }
        RecordProperty(t->label + "_worst_degrees", std::to_string(worst));
    }
}

TEST(PoseRig, HumanBodyHumanRoundTrip){
    std::mt19937 rng(99);
    for (const TestRig* t : allRigs()){
        const PoseRig& rig = *t->rig;
        const double hs = rig.humanScale();
        for (int i = 0; i < 200; i++){
            HumanPose human = rig.neutralPose();
            for (int s = 0; s < HUMAN_BONE_COUNT; s++){
                if (rig.has(boneAt(s))){
                    human.rotations[s] = (Quatd(randomRotation(rng, 2.0)) * Quatd(human.rotations[s])).normalized().toQuaternion();
                }
            }
            std::uniform_real_distribution<double> offset(-0.3, 0.3);
            rig.setHipsWorldPosition(human, (Vec3d(rig.hipsWorldPosition(human)) + Vec3d(offset(rng), offset(rng), offset(rng))).toVector3());
            const HumanPose back = rig.fromBodyPose(rig.toBodyPose(human), nullptr, true, 0.0f);
            for (int s = 0; s < HUMAN_BONE_COUNT; s++){
                if (!rig.has(boneAt(s))) continue;
                ASSERT_LT(rotationError(back.rotations[s], human.rotations[s]), 1e-4) << t->label << " " << humanBoneName(boneAt(s));
            }
            const Vec3d a = toCanonical(rig, Vec3d(rig.hipsWorldPosition(human)));
            const Vec3d b = toCanonical(rig, Vec3d(rig.hipsWorldPosition(back)));
            EXPECT_NEAR(a.x, b.x, 1e-4 * hs);
            EXPECT_NEAR(a.z, b.z, 1e-4 * hs);
            // vertical comes from grounding
            EXPECT_NEAR(rig.lowestPointPrecise(back, nullptr), rig.floorHeight(), 1e-5);
        }
    }
}

TEST(PoseRig, SingleSlotConversions){
    const PoseRig& rig = *aposeRig().rig;
    const float angles[3] = {40.0f, -20.0f, 15.0f};
    const Quaternion local = rig.localFromAngles(H::LeftUpperArm, angles);
    float back[3];
    rig.anglesFromLocal(H::LeftUpperArm, local, back);
    for (int k = 0; k < 3; k++) EXPECT_NEAR(back[k], angles[k], 1e-4);
    // unmapped slots: identity and zeros
    const PoseRig& minimal = *minimalRig().rig;
    EXPECT_EQ(minimal.localFromAngles(H::Chest, angles), Quaternion());
    minimal.anglesFromLocal(H::Chest, local, back);
    EXPECT_EQ(back[0], 0.0f);
    EXPECT_EQ(minimal.boneIndex(H::Chest), -1);
    EXPECT_FALSE(minimal.has(H::Neck));
    EXPECT_EQ(minimal.effectiveParent(H::Head), H::Spine);
    EXPECT_EQ(minimal.effectiveParent(H::LeftUpperArm), H::Spine);
}

TEST(PoseRig, Semantics){
    const PoseRig& rig = *tposeRig().rig;
    const double hs = rig.humanScale();
    const HumanPose neutral = rig.fromBodyPose(BodyPose(), nullptr, true, 0.0f);
    const std::vector<Vec3d> n = worldPositions(rig, neutral);
    auto at = [&](const std::vector<Vec3d>& p, H bone){ return p[rig.boneIndex(bone)]; };

    {   // left_upper_arm.raise = 90: the hand at shoulder height, to the left
        const std::vector<Vec3d> p = worldPositions(rig, rig.fromBodyPose(bodyWith({{"left_upper_arm.raise", 90}}), nullptr, true, 0));
        const Vec3d arm = at(p, H::LeftUpperArm), hand = at(p, H::LeftHand);
        EXPECT_NEAR(hand.y, arm.y, 1e-4 * hs);
        EXPECT_GT(hand.x - arm.x, 0.5);
        EXPECT_NEAR(hand.z, arm.z, 1e-4 * hs);
    }
    {   // left_upper_leg.forward = 90: the foot forward at hip height
        const std::vector<Vec3d> p = worldPositions(rig, rig.fromBodyPose(bodyWith({{"left_upper_leg.forward", 90}}), nullptr, true, 0));
        const Vec3d hip = at(p, H::LeftUpperLeg), foot = at(p, H::LeftFoot);
        EXPECT_NEAR(foot.y, hip.y, 1e-4 * hs);
        EXPECT_GT(foot.z - hip.z, 0.7);
    }
    {   // left_lower_leg.bend = 90: the foot moves back
        const std::vector<Vec3d> p = worldPositions(rig, rig.fromBodyPose(bodyWith({{"left_lower_leg.bend", 90}}), nullptr, true, 0));
        const Vec3d knee = at(p, H::LeftLowerLeg), foot = at(p, H::LeftFoot);
        EXPECT_LT(foot.z - knee.z, -0.35);
        EXPECT_NEAR(foot.y, knee.y, 1e-4 * hs);
    }
    {   // hips.turn = 90: forward points to +X
        const std::vector<Vec3d> p = worldPositions(rig, rig.fromBodyPose(bodyWith({{"hips.turn", 90}}), nullptr, true, 0));
        Vec3d left = at(p, H::LeftUpperLeg) - at(p, H::RightUpperLeg);
        left.y = 0;
        const Vec3d forward = left.normalized().cross(Vec3d(0, 1, 0));
        EXPECT_NEAR(forward.x, 1.0, 1e-5);
        // the character's right side now faces +Z
        EXPECT_GT(at(p, H::RightHand).z, 0.1);
    }
    {   // head.bend = 30: the head (its tip) moves forward
        const HumanPose pose = rig.fromBodyPose(bodyWith({{"head.bend", 30}}), nullptr, true, 0);
        const int top = findSkeletonBone(rig.skeleton(), "mixamorig:HeadTop_End");
        EXPECT_GT(worldPositions(rig, pose)[top].z, n[top].z + 0.05);
    }
    {   // signs of a few more DOFs
        auto tip = [&](const char* dof, float value, H bone){
            const std::vector<Vec3d> p = worldPositions(rig, rig.fromBodyPose(bodyWith({{dof, value}}), nullptr, true, 0));
            return toCanonical(rig, at(p, bone) - at(n, bone));
        };
        EXPECT_GT(tip("spine.bend", 30, H::Head).z, 0.1);            // forward
        EXPECT_GT(tip("spine.side", 20, H::Head).x, 0.05);           // toward the left
        EXPECT_GT(tip("left_lower_arm.bend", 90, H::LeftHand).z, 0.2);   // elbow flex brings the hand forward
        EXPECT_LT(tip("right_upper_arm.raise", 90, H::RightHand).x, -0.4);   // the right hand goes out to the right
        EXPECT_GT(tip("left_upper_arm.forward", 90, H::LeftHand).z, 0.4);    // swing forward
    }
}

TEST(PoseRig, CrossRigSameDirections){
    std::mt19937 rng(7);
    const PoseRig& a = *tposeRig().rig;
    const PoseRig& b = *aposeRig().rig;
    const int handTipA = findSkeletonBone(a.skeleton(), "mixamorig:LeftHandMiddle1");
    const int handTipB = findSkeletonBone(b.skeleton(), "f_middle.01.L");
    for (int i = 0; i < 100; i++){
        const BodyPose body = randomBodyPose(rng, a);
        const HumanPose pa = a.fromBodyPose(body, nullptr, true, 0);
        const HumanPose pb = b.fromBodyPose(body, nullptr, true, 0);
        for (H bone : AIMED){
            ASSERT_LT(degreesBetween(aimDirection(a, pa, bone), aimDirection(b, pb, bone)), 1.0) << humanBoneName(bone);
        }
        const std::vector<Vec3d> wa = worldPositions(a, pa), wb = worldPositions(b, pb);
        const Vec3d ha = toCanonical(a, wa[handTipA] - wa[a.boneIndex(H::LeftHand)]);
        const Vec3d hb = toCanonical(b, wb[handTipB] - wb[b.boneIndex(H::LeftHand)]);
        ASSERT_LT(degreesBetween(ha, hb), 1.0);
    }
    // also between the T-pose and the backward rig (same layout, turned): positions match in
    // canonical space
    const PoseRig& c = *backwardRig().rig;
    for (int i = 0; i < 20; i++){
        const BodyPose body = randomBodyPose(rng, a);
        const std::vector<Vec3d> wa = worldPositions(a, a.fromBodyPose(body, nullptr, true, 0));
        const std::vector<Vec3d> wc = worldPositions(c, c.fromBodyPose(body, nullptr, true, 0));
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            const Vec3d pa = toCanonical(a, wa[a.boneIndex(boneAt(s))]);
            const Vec3d pc = toCanonical(c, wc[c.boneIndex(boneAt(s))]);
            ASSERT_LT((pa - pc).length(), 1e-4) << humanBoneName(boneAt(s));
        }
    }
}

TEST(PoseRig, MirrorTwiceIsIdentityAndReflects){
    std::mt19937 rng(42);
    const PoseRig& rig = *tposeRig().rig;
    const double hs = rig.humanScale();
    for (int i = 0; i < 100; i++){
        const BodyPose body = randomBodyPose(rng, rig);
        const BodyPose twice = mirrorBodyPose(mirrorBodyPose(body));
        for (int d = 0; d < DOF_COUNT; d++){
            ASSERT_EQ(twice.angles[d], body.angles[d]);
        }
        ASSERT_EQ(twice.offset.x, body.offset.x);
        ASSERT_EQ(twice.offset.z, body.offset.z);

        const std::vector<Vec3d> p = worldPositions(rig, rig.fromBodyPose(body, nullptr, true, 0));
        const std::vector<Vec3d> m = worldPositions(rig, rig.fromBodyPose(mirrorBodyPose(body), nullptr, true, 0));
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            const H bone = boneAt(s);
            const Vec3d reflected = mirrorPos(p[rig.boneIndex(humanBoneInfo(bone).mirror)]);
            ASSERT_LT((m[rig.boneIndex(bone)] - reflected).length(), 1e-4 * hs) << humanBoneName(bone);
        }
    }
    // the centre/side rules on named DOFs
    const BodyPose mirrored = mirrorBodyPose(bodyWith({{"left_upper_arm.raise", 30}, {"spine.bend", 10}, {"spine.side", 5}, {"hips.turn", 20}}));
    int dof = -1;
    dofFromName("right_upper_arm.raise", dof); EXPECT_EQ(mirrored.angles[dof], 30.0f);
    dofFromName("left_upper_arm.raise", dof);  EXPECT_EQ(mirrored.angles[dof], 0.0f);
    dofFromName("spine.bend", dof);            EXPECT_EQ(mirrored.angles[dof], 10.0f);
    dofFromName("spine.side", dof);            EXPECT_EQ(mirrored.angles[dof], -5.0f);
    dofFromName("hips.turn", dof);             EXPECT_EQ(mirrored.angles[dof], -20.0f);
}

TEST(PoseRig, JointLimitsClampAndReport){
    BodyPose body = bodyWith({{"left_lower_arm.bend", -20}, {"left_upper_leg.forward", 150}, {"head.twist", -70},
                              {"right_toes.side", 6}, {"spine.bend", 10}});
    const BodyPose before = body;
    const std::vector<int> clamped = clampToLimits(body);
    int a, b, c, inRange, edge;
    dofFromName("left_lower_arm.bend", a);
    dofFromName("left_upper_leg.forward", b);
    dofFromName("head.twist", c);
    dofFromName("spine.bend", inRange);
    dofFromName("right_toes.side", edge);
    std::vector<int> expected{a, b, c};
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(clamped, expected);
    EXPECT_EQ(body.angles[a], 0.0f);
    EXPECT_EQ(body.angles[b], 125.0f);
    EXPECT_EQ(body.angles[c], -55.0f);
    EXPECT_EQ(body.angles[inRange], 10.0f);
    EXPECT_EQ(body.angles[edge], 6.0f);
    for (int d = 0; d < DOF_COUNT; d++){
        if (d != a && d != b && d != c){ EXPECT_EQ(body.angles[d], before.angles[d]); }
    }
    EXPECT_TRUE(clampToLimits(body).empty());
    // every table row
    for (int d = 0; d < DOF_COUNT; d++){
        BodyPose low, high;
        low.angles[d] = -1000.0f;
        high.angles[d] = 1000.0f;
        clampToLimits(low);
        clampToLimits(high);
        EXPECT_EQ(low.angles[d], dofInfo(d).minDegrees);
        EXPECT_EQ(high.angles[d], dofInfo(d).maxDegrees);
    }
}
