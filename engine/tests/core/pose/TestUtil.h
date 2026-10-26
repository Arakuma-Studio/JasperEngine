// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Small helpers shared by the pose core tests

#ifndef POSE_TEST_UTIL_H
#define POSE_TEST_UTIL_H

#include "TestRigs.h"

#include "pose/PoseDofs.h"

#include <random>
#include <vector>

namespace posetest {

    inline std::vector<Vec3d> worldPositions(const PoseRig& rig, const HumanPose& pose){
        std::vector<Quatd> rot;
        std::vector<Affine3d> world;
        rig.computeWorldPrecise(pose, rot, world);
        std::vector<Vec3d> result;
        for (const Affine3d& w : world) result.push_back(w.t);
        return result;
    }

    inline std::vector<Quatd> worldRotations(const PoseRig& rig, const HumanPose& pose){
        std::vector<Quatd> rot;
        std::vector<Affine3d> world;
        rig.computeWorldPrecise(pose, rot, world);
        return rot;
    }

    inline Vec3d slotPosition(const PoseRig& rig, const HumanPose& pose, HumanBone slot){
        return worldPositions(rig, pose)[rig.boneIndex(slot)];
    }

    inline Vec3d toCanonical(const PoseRig& rig, const Vec3d& v){
        return Quatd(rig.characterRotation()).normalized().inverse() * v;
    }

    inline Vec3d toModel(const PoseRig& rig, const Vec3d& v){
        return Quatd(rig.characterRotation()).normalized() * v;
    }

    inline double degreesBetween(const Vec3d& a, const Vec3d& b){
        return radToDeg(angleBetween(a, b));
    }

    // random angles within the joint limits for the present slots, swings no longer than
    // `maxSwing` degrees (a longer swing has a shorter equivalent: see PoseRig.h)
    inline BodyPose randomBodyPose(std::mt19937& rng, const PoseRig& rig, double maxSwing = 170.0, double offsetRange = 0.5){
        BodyPose body;
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            const HumanBone bone = boneAt(s);
            if (!rig.has(bone)){
                continue;
            }
            for (int attempt = 0; attempt < 100; attempt++){
                double values[DOFS_PER_BONE];
                for (int k = 0; k < DOFS_PER_BONE; k++){
                    const DofInfo& dof = dofInfo(dofIndex(bone, k));
                    // keep away from the +-180 twist wrap
                    const double lo = std::max<double>(dof.minDegrees, -179.0);
                    const double hi = std::min<double>(dof.maxDegrees, 179.0);
                    values[k] = lo + (hi - lo) * unit(rng);
                }
                if (std::sqrt(values[0] * values[0] + values[1] * values[1]) <= maxSwing){
                    for (int k = 0; k < DOFS_PER_BONE; k++){
                        body.angles[dofIndex(bone, k)] = static_cast<float>(values[k]);
                    }
                    break;
                }
            }
        }
        body.offset = Vector3(static_cast<float>((unit(rng) * 2 - 1) * offsetRange), 0.0f, static_cast<float>((unit(rng) * 2 - 1) * offsetRange));
        return body;
    }

    inline Quaternion randomRotation(std::mt19937& rng, double maxRadians){
        std::normal_distribution<double> normal(0.0, 1.0);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const Vec3d axis(normal(rng), normal(rng), normal(rng));
        return axisAngleRad(axis, maxRadians * unit(rng)).toQuaternion();
    }

    inline double rotationError(const Quaternion& a, const Quaternion& b){
        return angleBetween(Quatd(a).normalized(), Quatd(b).normalized());
    }

    // aim child of the aimed bones (the neutral direction test and cross-rig comparisons)
    inline HumanBone firstPresent(const PoseRig& rig, std::initializer_list<HumanBone> bones){
        for (HumanBone b : bones){
            if (rig.has(b)) return b;
        }
        return HumanBone::Count;
    }

    inline HumanBone aimChildOf(const PoseRig& rig, HumanBone bone){
        using B = HumanBone;
        switch (bone){
            case B::Hips: return B::Spine;
            case B::Spine: return firstPresent(rig, {B::Chest, B::UpperChest, B::Neck, B::Head});
            case B::Chest: return firstPresent(rig, {B::UpperChest, B::Neck, B::Head});
            case B::UpperChest: return firstPresent(rig, {B::Neck, B::Head});
            case B::Neck: return B::Head;
            case B::LeftShoulder: return B::LeftUpperArm;
            case B::RightShoulder: return B::RightUpperArm;
            case B::LeftUpperArm: return B::LeftLowerArm;
            case B::RightUpperArm: return B::RightLowerArm;
            case B::LeftLowerArm: return B::LeftHand;
            case B::RightLowerArm: return B::RightHand;
            case B::LeftUpperLeg: return B::LeftLowerLeg;
            case B::RightUpperLeg: return B::RightLowerLeg;
            case B::LeftLowerLeg: return B::LeftFoot;
            case B::RightLowerLeg: return B::RightFoot;
            default: return B::Count;
        }
    }

    // canonical direction from a slot to its aim child
    inline Vec3d aimDirection(const PoseRig& rig, const HumanPose& pose, HumanBone bone){
        const std::vector<Vec3d> pos = worldPositions(rig, pose);
        const HumanBone child = aimChildOf(rig, bone);
        return toCanonical(rig, pos[rig.boneIndex(child)] - pos[rig.boneIndex(bone)]).normalized();
    }

    // a simple animation of `bodies` on `rig`
    inline PoseAnimationData makeAnimation(const std::vector<BodyPose>& bodies, bool loop, float hold = 0.2f){
        PoseAnimationData data;
        data.settings.loop = loop;
        for (size_t i = 0; i < bodies.size(); i++){
            PoseKey key;
            key.name = "pose" + std::to_string(i);
            key.hold = hold;
            key.body = bodies[i];
            data.poses.push_back(key);
        }
        return data;
    }

    inline BodyPose bodyWith(std::initializer_list<std::pair<const char*, float>> values){
        BodyPose body;
        for (const auto& v : values){
            int dof = -1;
            if (!dofFromName(v.first, dof)){
                throw std::runtime_error(std::string("unknown dof ") + v.first);
            }
            body.angles[dof] = v.second;
        }
        return body;
    }

}

#endif // POSE_TEST_UTIL_H
