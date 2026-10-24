// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Review round 1 regressions: the IK bend side on rigs whose bone frames do not aim at their
// children (spec 4.10 near-straight rule), and grounding with the lowest skin vertex outside the
// hips subtree

#include "TestUtil.h"

#include "pose/PoseIK.h"

#include <gtest/gtest.h>

using namespace posetest;
using H = HumanBone;

namespace {

    // canonical offset of a limb's middle joint from its upper joint
    Vec3d jointOffset(const PoseRig& rig, const HumanPose& pose, Limb limb){
        H upper, lower, end;
        limbBones(limb, upper, lower, end);
        const std::vector<Vec3d> p = worldPositions(rig, pose);
        return toCanonical(rig, p[rig.boneIndex(lower)] - p[rig.boneIndex(upper)]);
    }

    // knees forward after lowering the hips with planted feet, elbows back after pulling the
    // hands to 70% of the reach, both from the neutral pose; returns the number of wrong sides
    int wrongBendSides(const PoseRig& rig, std::string* detail = nullptr){
        int wrong = 0;
        const double hs = rig.humanScale();
        HumanPose pose = rig.neutralPose();
        moveHipsKeepFeet(rig, pose, (Vec3d(rig.hipsWorldPosition(pose)) + toModel(rig, Vec3d(0, -0.1, 0)) * hs).toVector3());
        for (Limb limb : {Limb::LeftLeg, Limb::RightLeg}){
            const Vec3d d = jointOffset(rig, pose, limb);
            if (d.z < 0.02 * hs){
                wrong++;
                if (detail) *detail += " knee " + std::to_string(d.z);
            }
        }
        HumanPose arms = rig.neutralPose();
        for (Limb limb : {Limb::LeftArm, Limb::RightArm}){
            H upper, lower, end;
            limbBones(limb, upper, lower, end);
            const std::vector<Vec3d> w = worldPositions(rig, arms);
            const Vec3d a = w[rig.boneIndex(upper)], c = w[rig.boneIndex(end)];
            solveLimbIK(rig, arms, limb, (a + (c - a) * 0.7).toVector3(), false);
            const Vec3d d = jointOffset(rig, arms, limb);
            if (d.z > -0.02 * hs){
                wrong++;
                if (detail) *detail += " elbow " + std::to_string(d.z);
            }
        }
        return wrong;
    }

    // the T-pose rig turned `degrees` about up (the root bone turned): the character's left is no
    // longer the model's x axis
    const TestRig& turnedRig(double degrees){
        static std::map<double, TestRig> rigs;
        auto it = rigs.find(degrees);
        if (it == rigs.end()){
            Skeleton skeleton = tposeRig().skeleton;
            const Quatd turn = axisAngleRad(Vec3d(0, 1, 0), degToRad(degrees));
            for (SkeletonBone& bone : skeleton.bones){
                if (bone.parent < 0){
                    bone.position = (turn * Vec3d(bone.position)).toVector3();
                    bone.rotation = (turn * Quatd(bone.rotation)).normalized().toQuaternion();
                }
            }
            it = rigs.emplace(degrees, finish("tpose-turned-" + std::to_string(static_cast<int>(degrees)), skeleton, tposeRig().expected)).first;
        }
        return it->second;
    }

}

TEST(PoseIKBend, KneesForwardElbowsBackFromNeutralOnEveryRig){
    for (const TestRig* t : allRigs()){
        std::string detail;
        EXPECT_EQ(wrongBendSides(*t->rig, &detail), 0) << t->label << detail;
        EXPECT_EQ(wrongBendSides(*t->skinned, &detail), 0) << t->label << " (skinned)" << detail;
    }
}

TEST(PoseIKBend, RandomisedHumanoidsBendTheRightWay){
    // reviewer probe 1c: T-pose layouts with perturbed knee/elbow rest offsets and random
    // (non-aimed) bone frames, fixed seed
    std::mt19937 rng(1);
    std::uniform_real_distribution<double> U(-1, 1);
    int wrong = 0, total = 0, built = 0;
    for (int trial = 0; trial < 200; trial++){
        SkeletonBuilder b;
        const Vec3d hips(0, 0.95, 0), spine(0, 1.1, 0), chest(0, 1.3, 0), head(0, 1.55, 0), top(0, 1.75, 0);
        b.add("Hips", "", hips, aim(dirTo(hips, spine), U(rng)));
        b.add("Spine", "Hips", spine, aim(dirTo(spine, chest), U(rng)));
        b.add("Chest", "Spine", chest, aim(dirTo(chest, head), U(rng)));
        b.add("Head", "Chest", head, aim(dirTo(head, top), U(rng)));
        const Vec3d ua(0.17, 1.42, 0), la(0.45 + 0.01 * U(rng), 1.42 + 0.01 * U(rng), 0.02 * U(rng)), ha(0.72, 1.42, 0.01 * U(rng)), ht(0.8, 1.42, 0);
        const Vec3d ul(0.09, 0.9, 0), ll(0.09 + 0.01 * U(rng), 0.5, 0.02 * U(rng)), ft(0.09, 0.08, -0.02), tt(0.09, 0.02, 0.15);
        std::vector<SidedBone> bones = {
            {"LeftUpperArm", "RightUpperArm", "Chest", "Chest", ua, dirTo(ua, la), 3 * U(rng)},
            {"LeftLowerArm", "RightLowerArm", "LeftUpperArm", "RightUpperArm", la, dirTo(la, ha), 3 * U(rng)},
            {"LeftHand", "RightHand", "LeftLowerArm", "RightLowerArm", ha, dirTo(ha, ht), U(rng)},
            {"LeftUpperLeg", "RightUpperLeg", "Hips", "Hips", ul, dirTo(ul, ll), 3 * U(rng)},
            {"LeftLowerLeg", "RightLowerLeg", "LeftUpperLeg", "RightUpperLeg", ll, dirTo(ll, ft), 3 * U(rng)},
            {"LeftFoot", "RightFoot", "LeftLowerLeg", "RightLowerLeg", ft, dirTo(ft, tt), U(rng)},
        };
        for (SidedBone& sb : bones){
            sb.aimDir = Vec3d(U(rng), U(rng), U(rng)).normalized();
        }
        addSided(b, bones);
        PoseRig rig;
        std::string error;
        if (!rig.build(b.skeleton, autoDetectHumanoid(b.skeleton), &error)){
            ADD_FAILURE() << "trial " << trial << ": " << error;
            continue;
        }
        built++;
        total += 4;
        wrong += wrongBendSides(rig);
    }
    EXPECT_EQ(built, 200);
    EXPECT_EQ(wrong, 0) << "wrong bend side: " << wrong << " / " << total;

    // and fully random rest rotations
    std::mt19937 rng2(77);
    for (int trial = 0; trial < 50; trial++){
        const Skeleton skeleton = randomHumanoid(rng2);
        PoseRig rig;
        std::string error;
        ASSERT_TRUE(rig.build(skeleton, autoDetectHumanoid(skeleton), &error)) << error;
        std::string detail;
        EXPECT_EQ(wrongBendSides(rig, &detail), 0) << "trial " << trial << detail;
    }
}

TEST(PoseIKBend, NearlyStraightLimbsTakeThePoleSide){
    // from a knee bent a little either way, lowering the hips bends the knee forward; bent
    // clearly backward (beyond the blend range) the limb keeps its own plane
    const PoseRig& rig = *tposeRig().rig;
    const double hs = rig.humanScale();
    for (double degrees : {-20.0, -4.0, -1.0, 0.0, 1.0, 4.0, 8.0, 20.0}){
        HumanPose pose = rig.neutralPose();
        const float angles[3] = {static_cast<float>(degrees), 0, 0};   // left_lower_leg.bend (outside the limits when < 0)
        pose.rotations[boneIndex(H::LeftLowerLeg)] = rig.localFromAngles(H::LeftLowerLeg, angles);
        ground(rig, pose, 0.0f);
        moveHipsKeepFeet(rig, pose, (Vec3d(rig.hipsWorldPosition(pose)) + toModel(rig, Vec3d(0, -0.1, 0)) * hs).toVector3());
        const Vec3d d = jointOffset(rig, pose, Limb::LeftLeg);
        if (degrees > -10.0){
            EXPECT_GT(d.z, 0.02 * hs) << degrees;
        }else{
            EXPECT_LT(d.z, -0.02 * hs) << degrees;
        }
    }
}

TEST(PoseIKBend, BlendedPlaneStillReachesTheTarget){
    // the blend range (bends of 1 to 6 degrees) must not cost accuracy
    const PoseRig& rig = *aposeRig().rig;
    const double hs = rig.humanScale();
    for (double degrees : {1.0, 2.0, 3.0, 4.5, 6.0}){
        HumanPose pose = rig.neutralPose();
        const float angles[3] = {static_cast<float>(degrees), 0, 0};
        pose.rotations[boneIndex(H::RightLowerLeg)] = rig.localFromAngles(H::RightLowerLeg, angles);
        const std::vector<Vec3d> p = worldPositions(rig, pose);
        const Vec3d a = p[rig.boneIndex(H::RightUpperLeg)];
        const Vec3d target = a + toModel(rig, Vec3d(0.05, -0.6, 0.2)) * hs;
        ASSERT_TRUE(solveLimbIK(rig, pose, Limb::RightLeg, target.toVector3(), true));
        EXPECT_LT((worldPositions(rig, pose)[rig.boneIndex(H::RightFoot)] - target).length(), 1e-4 * hs) << degrees;
        EXPECT_GT(jointOffset(rig, pose, Limb::RightLeg).z, 0.0) << degrees;
    }
}

TEST(PoseIKBend, TargetOppositeTheLimbTakesThePoleSide){
    // spec 4.10 singular case: the target (nearly) exactly opposite the current C - A direction
    // (a hanging leg's foot pulled overhead, an arm folded back through its shoulder). The
    // shortest rotation from C - A to T - A is then a half turn about an axis that the approach
    // direction, or fromToRotation's fixed model-space axis, picks; that used to decide the bend
    // side, so it depended on how the model is turned. Within 1 degree of the half turn the pole
    // plane decides: knees forward, elbows back, on every rig (also turned about up), from
    // straight, slightly and clearly bent limbs, for every approach direction
    std::vector<const TestRig*> rigs = allRigs();
    rigs.push_back(&turnedRig(90.0));
    rigs.push_back(&turnedRig(-37.0));
    int cases = 0;
    for (const TestRig* t : rigs){
        const PoseRig& rig = *t->rig;
        const double hs = rig.humanScale();
        for (Limb limb : {Limb::LeftLeg, Limb::RightLeg, Limb::LeftArm, Limb::RightArm}){
            const bool leg = limb == Limb::LeftLeg || limb == Limb::RightLeg;
            H upper, lower, end;
            limbBones(limb, upper, lower, end);
            for (float degrees : {0.0f, 3.0f, 30.0f}){
                HumanPose start = rig.neutralPose();
                const float angles[3] = {degrees, 0, 0};   // <lower>.bend
                start.rotations[boneIndex(lower)] = rig.localFromAngles(lower, angles);
                const std::vector<Vec3d> p = worldPositions(rig, start);
                const Vec3d a = p[rig.boneIndex(upper)], c = p[rig.boneIndex(end)];
                const double reach = ((p[rig.boneIndex(lower)] - a).length() + (c - p[rig.boneIndex(lower)]).length()) * 0.8;
                const Vec3d opposite = -(c - a).normalized();
                Vec3d tilt = opposite.cross(Vec3d(rig.characterLeft()));
                if (tilt.length() < 0.1){
                    tilt = opposite.cross(Vec3d(rig.characterUp()));
                }
                tilt = tilt.normalized();
                for (double offAxis : {0.0, 0.5, 0.9}){   // degrees away from exactly opposite
                    for (int azimuth = 0; azimuth < 360; azimuth += 45){
                        const Vec3d axis = axisAngleRad(opposite, degToRad(azimuth)) * tilt;
                        const Vec3d aim = (axisAngleRad(axis, degToRad(offAxis)) * opposite).normalized();
                        const Vec3d target = a + aim * reach;
                        HumanPose pose = start;
                        ASSERT_TRUE(solveLimbIK(rig, pose, limb, target.toVector3(), false));
                        const std::vector<Vec3d> q = worldPositions(rig, pose);
                        const std::string where = t->label + " " + humanBoneName(lower) + " bend " + std::to_string(degrees) +
                            " off " + std::to_string(offAxis) + " azimuth " + std::to_string(azimuth);
                        EXPECT_LT((q[rig.boneIndex(end)] - target).length(), 1e-4 * hs) << where;
                        // the joint's side: its offset off the A - T line, canonical forward component
                        Vec3d d = q[rig.boneIndex(lower)] - a;
                        d = d - aim * d.dot(aim);
                        const double forward = toCanonical(rig, d).z;
                        if (leg){
                            EXPECT_GT(forward, 0.05 * hs) << where << " " << forward / hs;
                        }else{
                            EXPECT_LT(forward, -0.05 * hs) << where << " " << forward / hs;
                        }
                        cases++;
                    }
                }
            }
        }
    }
    EXPECT_EQ(cases, 7 * 4 * 3 * 3 * 8);
}

TEST(PoseGrounding, LowestVertexOutsideTheHipsSubtree){
    const TestRig& t = tposeRig();
    const int armature = findSkeletonBone(t.skeleton, "Armature");
    const int leftFoot = findSkeletonBone(t.skeleton, "mixamorig:LeftFoot");
    ASSERT_GE(armature, 0);
    ASSERT_GE(leftFoot, 0);
    const BodyPose body = bodyWith({{"left_lower_leg.bend", 60}, {"spine.bend", 20}});

    {   // a vertex on the root only: grounding cannot move it, so the hips stay put
        auto skin = std::make_shared<PoseSkin>(*t.skin);
        PoseSkinVertex v;
        v.position = Vector3(0, -0.3f, 0);
        v.bones = {{armature, -1, -1, -1}};
        v.weights = {{1, 0, 0, 0}};
        skin->vertices.push_back(v);
        PoseRig rig;
        rig.setSkin(skin);
        std::string error;
        ASSERT_TRUE(rig.build(t.skeleton, t.expected, &error)) << error;
        EXPECT_NEAR(rig.floorHeight(), -0.3, 1e-6);
        const HumanPose pose = rig.fromBodyPose(body, nullptr, false, 0.2f);
        EXPECT_NEAR(rig.hipsWorldPosition(pose).y, rig.neutralHipsWorldPosition().y, 1e-6);
        EXPECT_NEAR(rig.neutralHipsWorldPosition().y, 0.95, 1e-5);
        EXPECT_NEAR(lowestPoint(rig, pose), rig.floorHeight(), 1e-6);
    }
    {   // half on the root, half on the foot: it moves at half the hips' speed (secant steps)
        auto skin = std::make_shared<PoseSkin>(*t.skin);
        PoseSkinVertex v;
        v.position = Vector3(0.09f, -0.2f, 0);
        v.bones = {{armature, leftFoot, -1, -1}};
        v.weights = {{0.5f, 0.5f, 0, 0}};
        skin->vertices.push_back(v);
        PoseRig rig;
        rig.setSkin(skin);
        std::string error;
        ASSERT_TRUE(rig.build(t.skeleton, t.expected, &error)) << error;
        EXPECT_NEAR(rig.floorHeight(), -0.2, 1e-6);
        for (float lift : {0.0f, 0.1f, 0.3f}){
            const HumanPose pose = rig.fromBodyPose(BodyPose(), nullptr, false, lift);
            EXPECT_NEAR(lowestPoint(rig, pose), rig.floorHeight() + lift * rig.humanScale(), 1e-6) << lift;
        }
    }
}
