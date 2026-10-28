// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The 22 humanoid slots the pose tool animates (spec: pose animation phase 1, section 4.1).
// The order is a contract shared by every pose file, test and tool: never reorder.

#ifndef POSE_HUMAN_BONES_H
#define POSE_HUMAN_BONES_H

#include "Export.h"

#include <cstdint>
#include <string>

namespace jasperengine::pose {

    enum class HumanBone : uint8_t {
        Hips = 0, Spine, Chest, UpperChest, Neck, Head,
        LeftShoulder, LeftUpperArm, LeftLowerArm, LeftHand,
        RightShoulder, RightUpperArm, RightLowerArm, RightHand,
        LeftUpperLeg, LeftLowerLeg, LeftFoot, LeftToes,
        RightUpperLeg, RightLowerLeg, RightFoot, RightToes,
        Count
    };

    constexpr int HUMAN_BONE_COUNT = static_cast<int>(HumanBone::Count);

    // Body-part groups (the order of PoseAnimationSettings::weights)
    enum class BodyGroup : uint8_t { Spine = 0, Head, Arms, Legs, Count };

    enum class BodySide : uint8_t { Center = 0, Left, Right };

    struct HumanBoneInfo {
        const char* name;      // snake_case, e.g. "left_upper_arm"
        HumanBone parent;      // anatomical parent slot; HumanBone::Count for Hips
        HumanBone mirror;      // left/right counterpart; itself for centre slots
        BodyGroup group;
        BodySide side;
        float depth;           // 0 = leads the motion, 1 = trails the most (overlap, looseness)
        bool required;         // a rig without it is not a valid humanoid
    };

    JASPERENGINE_API const HumanBoneInfo& humanBoneInfo(HumanBone bone);
    JASPERENGINE_API const char* humanBoneName(HumanBone bone);
    // name -> slot (exact snake_case name); false when unknown
    JASPERENGINE_API bool humanBoneFromName(const std::string& name, HumanBone& bone);
    JASPERENGINE_API const char* bodyGroupName(BodyGroup group);   // "spine", "head", "arms", "legs"

    inline int boneIndex(HumanBone bone){ return static_cast<int>(bone); }
    inline HumanBone boneAt(int index){ return static_cast<HumanBone>(index); }

}

#endif // POSE_HUMAN_BONES_H
