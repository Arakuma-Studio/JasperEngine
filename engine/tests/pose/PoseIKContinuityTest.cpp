// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Review round 2 regressions (reviewer probes 5, 9, 10): the IK solution moves continuously
// when the starting bend plane turns through the blend zone, when the starting bend grows
// through it, and when the target sweeps across the bend-plane normal

#include "TestUtil.h"

#include "pose/PoseIK.h"

#include <gtest/gtest.h>

using namespace posetest;
using H = HumanBone;

namespace {

    // bends the lower bone of `limb` by `degrees` in world space, in the plane at `phi` degrees
    // (about the lower bone) from the pole plane; phi = 0 bends toward the pole side
    void bendLower(const PoseRig& rig, HumanPose& pose, Limb limb, double degrees, double phi){
        H upper, lower, end;
        limbBones(limb, upper, lower, end);
        const std::vector<Quatd> rot = worldRotations(rig, pose);
        const std::vector<Vec3d> p = worldPositions(rig, pose);
        const int ib = rig.boneIndex(lower), ic = rig.boneIndex(end);
        const Vec3d dir = (p[ic] - p[ib]).normalized();
        const bool leg = limb == Limb::LeftLeg || limb == Limb::RightLeg;
        const Vec3d pole = leg ? Vec3d(rig.characterForward()) : -Vec3d(rig.characterForward());
        const Vec3d n0 = dir.cross(pole).normalized();
        const Vec3d axis = axisAngleRad(dir, degToRad(phi)) * n0;
        const Quatd turn = axisAngleRad(axis, degToRad(-degrees));
        const int parent = rig.skeleton().bones[ib].parent;
        const Quatd parentWorld = parent >= 0 ? rot[parent] : Quatd();
        pose.rotations[boneIndex(lower)] = (parentWorld.inverse() * turn * rot[ib]).normalized().toQuaternion();
    }

    Vec3d jointPosition(const PoseRig& rig, const HumanPose& pose, Limb limb){
        H upper, lower, end;
        limbBones(limb, upper, lower, end);
        return worldPositions(rig, pose)[rig.boneIndex(lower)];
    }

    double endError(const PoseRig& rig, const HumanPose& pose, Limb limb, const Vec3d& target){
        H upper, lower, end;
        limbBones(limb, upper, lower, end);
        return (worldPositions(rig, pose)[rig.boneIndex(end)] - target).length();
    }

}

TEST(PoseIKContinuity, StartingBendPlaneSweptThroughTheBlendZone){
    // probe 5 C: a knee bent 3/6/9/11 degrees (inside the blend zone of the near-straight rule)
    // in a plane swept 0..180 degrees from the pole plane; the solved knee must move
    // continuously (the old "opposite side -> pole" switch jumped 0.25 humanScale at 90 degrees)
    for (const TestRig* t : allRigs()){
        const PoseRig& rig = *t->rig;
        const double hs = rig.humanScale();
        for (Limb limb : {Limb::LeftLeg, Limb::RightArm}){
            H upper, lower, end;
            limbBones(limb, upper, lower, end);
            const HumanPose neutral = rig.neutralPose();
            const Vec3d a = worldPositions(rig, neutral)[rig.boneIndex(upper)];
            const bool leg = limb == Limb::LeftLeg;
            const Vec3d target = a + toModel(rig, leg ? Vec3d(0.0, -0.75, 0.1) : Vec3d(-0.05, -0.42, -0.1)) * hs;
            for (double degrees : {3.0, 6.0, 9.0, 11.0}){
                Vec3d previous;
                double worst = 0.0, worstAt = 0.0;
                for (int i = 0; i <= 360; i++){
                    const double phi = i * 0.5;
                    HumanPose pose = neutral;
                    bendLower(rig, pose, limb, degrees, phi);
                    ASSERT_TRUE(solveLimbIK(rig, pose, limb, target.toVector3(), leg));
                    ASSERT_LT(endError(rig, pose, limb, target), 1e-4 * hs);
                    const Vec3d joint = jointPosition(rig, pose, limb);
                    if (i > 0 && (joint - previous).length() / hs > worst){
                        worst = (joint - previous).length() / hs;
                        worstAt = phi;
                    }
                    previous = joint;
                }
                // at most 0.03 humanScale per degree of sweep (measured ~0.013; the jump was 0.25)
                EXPECT_LT(worst, 0.015) << t->label << " limb " << static_cast<int>(limb) << " bend " << degrees << " at phi " << worstAt;
            }
        }
    }
}

TEST(PoseIKContinuity, StartingBendGrowsThroughTheBlendZone){
    // probe 5 B: the knee's starting bend grows 0..15 degrees on the pole side, fixed target
    for (const TestRig* t : allRigs()){
        const PoseRig& rig = *t->rig;
        const double hs = rig.humanScale();
        const HumanPose neutral = rig.neutralPose();
        const Vec3d a = worldPositions(rig, neutral)[rig.boneIndex(H::LeftUpperLeg)];
        const Vec3d target = a + toModel(rig, Vec3d(0.05, -0.7, 0.15)) * hs;
        Vec3d previous;
        double worst = 0.0;
        for (int i = 0; i <= 300; i++){
            HumanPose pose = neutral;
            bendLower(rig, pose, Limb::LeftLeg, i * 0.05, 0.0);
            ASSERT_TRUE(solveLimbIK(rig, pose, Limb::LeftLeg, target.toVector3(), true));
            ASSERT_LT(endError(rig, pose, Limb::LeftLeg, target), 1e-4 * hs);
            const Vec3d knee = jointPosition(rig, pose, Limb::LeftLeg);
            if (i > 0) worst = std::max(worst, (knee - previous).length() / hs);
            previous = knee;
        }
        EXPECT_LT(worst, 1e-3) << t->label;
    }
}

TEST(PoseIKContinuity, TargetSweptAcrossTheBendNormal){
    // probes 9 and 10: a bent knee / elbow, target swept from down through lateral to up (it
    // crosses the bend-plane normal); the plane follows the limb by the shortest rotation, so the
    // joint moves continuously (measured <= 0.0062 humanScale per 0.25 degree, the spec's
    // B-then-fromTo order 0.0050)
    for (const TestRig* t : allRigs()){
        const PoseRig& rig = *t->rig;
        const double hs = rig.humanScale();
        for (Limb limb : {Limb::LeftLeg, Limb::LeftArm}){
            H upper, lower, end;
            limbBones(limb, upper, lower, end);
            HumanPose bent = rig.neutralPose();
            const float bend[3] = {60, 0, 0};
            bent.rotations[boneIndex(lower)] = rig.localFromAngles(lower, bend);
            const std::vector<Vec3d> p = worldPositions(rig, bent);
            const Vec3d a = p[rig.boneIndex(upper)];
            const double reach = (p[rig.boneIndex(end)] - a).length() * 0.9;
            Vec3d previous;
            double worst = 0.0, worstAt = 0.0;
            for (int i = 0; i <= 720; i++){
                const double angle = degToRad(i * 0.25);
                HumanPose pose = bent;
                const Vec3d target = a + toModel(rig, Vec3d(std::sin(angle), -std::cos(angle), 0.02).normalized()) * reach;
                ASSERT_TRUE(solveLimbIK(rig, pose, limb, target.toVector3(), true));
                const Vec3d joint = jointPosition(rig, pose, limb);
                if (i > 0 && (joint - previous).length() / hs > worst){
                    worst = (joint - previous).length() / hs;
                    worstAt = i * 0.25;
                }
                previous = joint;
            }
            EXPECT_LT(worst, 0.012) << t->label << " limb " << static_cast<int>(limb) << " at " << worstAt;
        }
    }
}
