// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Acceptance tests 14 and 15: two-bone IK, moving the hips with planted feet, grounding and the
// skin's lowest point (spec 4.10)

#include "TestUtil.h"

#include "pose/PoseIK.h"

#include <gtest/gtest.h>

using namespace posetest;
using H = HumanBone;

namespace {

    const Limb LIMBS[] = {Limb::LeftArm, Limb::RightArm, Limb::LeftLeg, Limb::RightLeg};

    bool isLeg(Limb limb){ return limb == Limb::LeftLeg || limb == Limb::RightLeg; }

}

TEST(PoseIK, ReachableTargetsAreReached){
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::normal_distribution<double> normal(0.0, 1.0);
    for (const TestRig* t : allRigs()){
        const PoseRig& rig = *t->rig;
        const double hs = rig.humanScale();
        for (int i = 0; i < 40; i++){
            HumanPose pose = rig.fromBodyPose(randomBodyPose(rng, rig, 120.0, 0.2), nullptr, true, 0);
            for (Limb limb : LIMBS){
                H upper, lower, end;
                limbBones(limb, upper, lower, end);
                const std::vector<Vec3d> p = worldPositions(rig, pose);
                const Vec3d a = p[rig.boneIndex(upper)];
                const double lab = (p[rig.boneIndex(lower)] - a).length();
                const double lbc = (p[rig.boneIndex(end)] - p[rig.boneIndex(lower)]).length();
                const double reach = std::fabs(lab - lbc) + (lab + lbc - std::fabs(lab - lbc)) * (0.1 + 0.85 * unit(rng));
                const Vec3d target = a + Vec3d(normal(rng), normal(rng), normal(rng)).normalized() * reach;
                const bool keep = isLeg(limb);
                const std::vector<Quatd> rotBefore = worldRotations(rig, pose);
                const Quaternion localEndBefore = pose.rotations[boneIndex(end)];
                ASSERT_TRUE(solveLimbIK(rig, pose, limb, target.toVector3(), keep));
                const std::vector<Vec3d> q = worldPositions(rig, pose);
                ASSERT_LT((q[rig.boneIndex(end)] - target).length(), 1e-4 * hs) << t->label << " limb " << static_cast<int>(limb);
                // the upper bone did not move, the bone lengths are kept
                EXPECT_LT((q[rig.boneIndex(upper)] - a).length(), 1e-6);
                EXPECT_NEAR((q[rig.boneIndex(lower)] - q[rig.boneIndex(upper)]).length(), lab, 1e-5);
                if (keep){
                    // feet keep their world rotation
                    EXPECT_LT(angleBetween(worldRotations(rig, pose)[rig.boneIndex(end)], rotBefore[rig.boneIndex(end)]), 1e-5);
                }else{
                    // hands keep their local rotation
                    EXPECT_EQ(pose.rotations[boneIndex(end)], localEndBefore);
                }
            }
        }
    }
}

TEST(PoseIK, UnreachableTargetsClampOnTheTargetLine){
    for (const TestRig* t : allRigs()){
        const PoseRig& rig = *t->rig;
        const double hs = rig.humanScale();
        for (Limb limb : LIMBS){
            HumanPose pose = rig.neutralPose();
            H upper, lower, end;
            limbBones(limb, upper, lower, end);
            const std::vector<Vec3d> p = worldPositions(rig, pose);
            const Vec3d a = p[rig.boneIndex(upper)];
            const double full = (p[rig.boneIndex(lower)] - a).length() + (p[rig.boneIndex(end)] - p[rig.boneIndex(lower)]).length();
            const Vec3d direction = toModel(rig, Vec3d(0.3, -0.2, 0.8).normalized());
            ASSERT_TRUE(solveLimbIK(rig, pose, limb, (a + direction * (2.0 * full)).toVector3(), isLeg(limb)));
            const Vec3d e = worldPositions(rig, pose)[rig.boneIndex(end)] - a;
            EXPECT_NEAR(e.length(), full, 2e-4 * hs) << t->label;
            EXPECT_LT(e.normalized().cross(direction).length(), 1e-5) << t->label;
            EXPECT_GT(e.dot(direction), 0.0);
        }
    }
}

TEST(PoseIK, ThePoleDecidesTheBendSide){
    for (const TestRig* t : allRigs()){
        const PoseRig& rig = *t->rig;
        for (Limb limb : LIMBS){
            // the neutral limbs are straight: only the pole can pick the side
            HumanPose pose = rig.neutralPose();
            H upper, lower, end;
            limbBones(limb, upper, lower, end);
            const std::vector<Vec3d> p = worldPositions(rig, pose);
            const Vec3d a = toCanonical(rig, p[rig.boneIndex(upper)]);
            const Vec3d c = toCanonical(rig, p[rig.boneIndex(end)]);
            const Vec3d target = a + (c - a) * 0.7;
            ASSERT_TRUE(solveLimbIK(rig, pose, limb, toModel(rig, target).toVector3(), isLeg(limb)));
            const Vec3d b = toCanonical(rig, worldPositions(rig, pose)[rig.boneIndex(lower)]);
            if (isLeg(limb)){
                EXPECT_GT(b.z - a.z, 0.05) << t->label << ": knees bend forward";
            }else{
                EXPECT_LT(b.z - a.z, -0.05) << t->label << ": elbows bend backward";
            }
        }
    }
}

TEST(PoseIK, MovingTheHipsKeepsTheFeet){
    std::mt19937 rng(11);
    for (const TestRig* t : allRigs()){
        const PoseRig& rig = *t->rig;
        const double hs = rig.humanScale();
        for (int i = 0; i < 20; i++){
            BodyPose body = randomBodyPose(rng, rig, 30.0, 0.1);
            HumanPose pose = rig.fromBodyPose(body, nullptr, true, 0);
            const std::vector<Vec3d> before = worldPositions(rig, pose);
            const std::vector<Quatd> rotBefore = worldRotations(rig, pose);
            const Vec3d hips(rig.hipsWorldPosition(pose));
            const Vec3d moved = hips + toModel(rig, Vec3d(0.03, -0.12, 0.05) * hs);
            moveHipsKeepFeet(rig, pose, moved.toVector3());
            const std::vector<Vec3d> after = worldPositions(rig, pose);
            EXPECT_LT((Vec3d(rig.hipsWorldPosition(pose)) - moved).length(), 1e-6);
            for (H foot : {H::LeftFoot, H::RightFoot}){
                const int bone = rig.boneIndex(foot);
                ASSERT_LT((after[bone] - before[bone]).length(), 1e-4 * hs) << t->label << " " << humanBoneName(foot);
                EXPECT_LT(angleBetween(worldRotations(rig, pose)[bone], rotBefore[bone]), 1e-5);
            }
        }
    }
}

TEST(PoseGrounding, GroundedPosesTouchTheFloorAndLiftRaises){
    std::mt19937 rng(3);
    for (const TestRig* t : allRigs()){
        for (const PoseRig* rig : {t->rig.get(), t->skinned.get()}){
            const double hs = rig->humanScale();
            for (int i = 0; i < 30; i++){
                const BodyPose body = randomBodyPose(rng, *rig);
                const HumanPose grounded = rig->fromBodyPose(body, nullptr, true, 0.7f);   // lift ignored when grounded
                EXPECT_NEAR(lowestPoint(*rig, grounded), rig->floorHeight(), 1e-5) << t->label;
                const HumanPose lifted = rig->fromBodyPose(body, nullptr, false, 0.3f);
                EXPECT_NEAR(lowestPoint(*rig, lifted) - rig->floorHeight(), 0.3 * hs, 1e-5) << t->label;
                // ground() moves only along up
                HumanPose moved = lifted;
                ground(*rig, moved, 0.0f);
                EXPECT_NEAR(lowestPoint(*rig, moved), rig->floorHeight(), 1e-5);
                const Vec3d d = toCanonical(*rig, Vec3d(rig->hipsWorldPosition(moved)) - Vec3d(rig->hipsWorldPosition(lifted)));
                EXPECT_NEAR(d.x, 0.0, 1e-6);
                EXPECT_NEAR(d.z, 0.0, 1e-6);
                EXPECT_NEAR(d.y, -0.3 * hs, 1e-5);
            }
        }
    }
}

TEST(PoseGrounding, SkinBeatsTheSoleEstimateOnATiltedFoot){
    for (const TestRig* t : {&tposeRig(), &aposeRig(), &minimalRig(), &backwardRig()}){
        const PoseRig& sole = *t->rig;
        const PoseRig& skinned = *t->skinned;
        const PoseSkin& skin = *t->skin;
        // toes pointed down: the toe tip goes below the toes bone
        const BodyPose body = bodyWith({{"left_foot.point", 50}, {"right_foot.point", 50}});
        auto trueLowest = [&](const PoseRig& rig, const HumanPose& pose){
            std::vector<Quatd> rot;
            std::vector<Affine3d> world;
            rig.computeWorldPrecise(pose, rot, world);
            return skin.lowest(world);
        };
        const double skinFloor = skinned.floorHeight();
        const double withSkin = std::fabs(trueLowest(skinned, skinned.fromBodyPose(body, nullptr, true, 0)) - skinFloor);
        const double withSoles = std::fabs(trueLowest(sole, sole.fromBodyPose(body, nullptr, true, 0)) - skinFloor);
        EXPECT_LT(withSkin, 1e-5) << t->label;
        EXPECT_GT(withSoles, withSkin + 0.01) << t->label;
        // the skin argument of fromBodyPose overrides the rig's (sole) estimate
        // (the floor stays the rig's own: the lowest bone at rest for the sole rig)
        const double viaArgument = std::fabs(trueLowest(sole, sole.fromBodyPose(body, &skin, true, 0)) - sole.floorHeight());
        EXPECT_LT(viaArgument, 1e-5) << t->label;
        // the float Matrix4 path agrees
        std::vector<Matrix4> matrices;
        skinned.computeWorldMatrices(skinned.neutralPose(), matrices);
        EXPECT_NEAR(skin.lowest(matrices), skinFloor, 1e-5);
    }
}

TEST(PoseSkin, SampleStride){
    EXPECT_EQ(PoseSkin::sampleStride(0), 1u);
    EXPECT_EQ(PoseSkin::sampleStride(6000), 1u);
    EXPECT_EQ(PoseSkin::sampleStride(6001), 2u);
    EXPECT_EQ(PoseSkin::sampleStride(60000), 10u);
    for (size_t n : {6001u, 12345u, 100000u}){
        EXPECT_LE((n + PoseSkin::sampleStride(n) - 1) / PoseSkin::sampleStride(n), PoseSkin::MAX_VERTICES);
    }
    PoseSkin empty;
    EXPECT_TRUE(std::isinf(empty.lowest(std::vector<Matrix4>())));
}
