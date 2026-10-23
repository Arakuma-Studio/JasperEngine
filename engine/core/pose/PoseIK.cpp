// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/PoseIK.h"

#include "pose/PoseMath.h"

#include <cmath>
#include <vector>

namespace jasperengine::pose {

    void limbBones(Limb limb, HumanBone& upper, HumanBone& lower, HumanBone& end){
        switch (limb){
            case Limb::LeftArm:  upper = HumanBone::LeftUpperArm;  lower = HumanBone::LeftLowerArm;  end = HumanBone::LeftHand; break;
            case Limb::RightArm: upper = HumanBone::RightUpperArm; lower = HumanBone::RightLowerArm; end = HumanBone::RightHand; break;
            case Limb::LeftLeg:  upper = HumanBone::LeftUpperLeg;  lower = HumanBone::LeftLowerLeg;  end = HumanBone::LeftFoot; break;
            case Limb::RightLeg: upper = HumanBone::RightUpperLeg; lower = HumanBone::RightLowerLeg; end = HumanBone::RightFoot; break;
        }
    }

    bool solveLimbIK(const PoseRig& rig, HumanPose& pose, Limb limb, const Vector3& targetModel, bool keepEndWorldRotation){
        if (!rig.valid()){
            return false;
        }
        HumanBone upper = HumanBone::Count, lower = HumanBone::Count, end = HumanBone::Count;
        limbBones(limb, upper, lower, end);
        const int ia = rig.boneIndex(upper);
        const int ib = rig.boneIndex(lower);
        const int ic = rig.boneIndex(end);

        std::vector<Quatd> rot;
        std::vector<Affine3d> world;
        rig.computeWorldPrecise(pose, rot, world);

        const Vec3d a = world[ia].t;
        const Vec3d b = world[ib].t;
        const Vec3d c = world[ic].t;
        const Vec3d target(targetModel);
        const double lab = (b - a).length();
        const double lbc = (c - b).length();
        if (lab < 1e-12 || lbc < 1e-12){
            return false;
        }

        const double margin = 1e-4 * rig.humanScale();
        const double dist = (target - a).length();
        const double minReach = std::fabs(lab - lbc) + margin;
        const double maxReach = std::max(minReach, lab + lbc - margin);
        const double reach = clampd(dist, minReach, maxReach);

        // bend axis: the limb plane, or the pole when the limb is straight
        const Vec3d ac = c - a;
        Vec3d axis = ac.cross(b - a);
        if (axis.length() < 1e-9 * lab * std::max(ac.length(), 1e-12)){
            const bool leg = limb == Limb::LeftLeg || limb == Limb::RightLeg;
            const Vec3d forward(rig.characterForward());
            axis = ac.cross(leg ? forward : -forward);
            if (axis.length() < 1e-12){
                axis = ac.cross(Vec3d(rig.characterLeft()));
            }
        }
        axis = axis.normalized();

        // B turns by the change of the interior angle, then A aims the end at the target
        const double current = angleBetween(a - b, c - b);
        const double wanted = std::acos(clampd((reach * reach - lab * lab - lbc * lbc) / (-2.0 * lab * lbc), -1.0, 1.0));
        const Quatd bendDelta = axisAngleRad(axis, wanted - current);
        const Vec3d bentEnd = b + bendDelta * (c - b);
        const Quatd aimDelta = dist > 1e-12 ? fromToRotation(bentEnd - a, target - a) : Quatd();

        auto parentRot = [&](int bone){
            const int parent = rig.skeleton().bones[bone].parent;
            return parent >= 0 ? rot[parent] : Quatd();
        };
        // world rotations: A' = aim * A, B' = aim * bend * B; A's parent is unchanged and B's
        // parent (A or a rest bone under it) turned with aim, so the locals are
        const Quatd localA = (parentRot(ia).inverse() * aimDelta * rot[ia]).normalized();
        const Quatd localB = (parentRot(ib).inverse() * bendDelta * rot[ib]).normalized();
        pose.rotations[boneIndex(upper)] = localA.toQuaternion();
        pose.rotations[boneIndex(lower)] = localB.toQuaternion();
        if (keepEndWorldRotation){
            // C's parent turned with aim * bend: undo it
            const Quatd localC = (parentRot(ic).inverse() * (aimDelta * bendDelta).inverse() * rot[ic]).normalized();
            pose.rotations[boneIndex(end)] = localC.toQuaternion();
        }
        return true;
    }

    void moveHipsKeepFeet(const PoseRig& rig, HumanPose& pose, const Vector3& newHipsWorldModel){
        if (!rig.valid()){
            return;
        }
        std::vector<Quaternion> rot;
        std::vector<Vector3> pos;
        rig.computeWorld(pose, rot, pos);
        const Vector3 leftFoot = pos[rig.boneIndex(HumanBone::LeftFoot)];
        const Vector3 rightFoot = pos[rig.boneIndex(HumanBone::RightFoot)];
        rig.setHipsWorldPosition(pose, newHipsWorldModel);
        solveLimbIK(rig, pose, Limb::LeftLeg, leftFoot, true);
        solveLimbIK(rig, pose, Limb::RightLeg, rightFoot, true);
    }

    float lowestPoint(const PoseRig& rig, const HumanPose& pose){
        return static_cast<float>(rig.lowestPointPrecise(pose, rig.skin()));
    }

    void ground(const PoseRig& rig, HumanPose& pose, float lift){
        rig.groundPose(pose, lift, rig.skin());
    }

}
