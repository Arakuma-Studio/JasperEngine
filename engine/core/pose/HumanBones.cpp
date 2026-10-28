// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/HumanBones.h"

namespace jasperengine::pose {

    namespace {

        using B = HumanBone;
        using G = BodyGroup;
        using S = BodySide;

        // spec: pose animation phase 1, table 4.1
        const HumanBoneInfo BONES[HUMAN_BONE_COUNT] = {
            {"hips",            B::Count,         B::Hips,          G::Spine, S::Center, 0.0f,  true},
            {"spine",           B::Hips,          B::Spine,         G::Spine, S::Center, 0.2f,  true},
            {"chest",           B::Spine,         B::Chest,         G::Spine, S::Center, 0.3f,  false},
            {"upper_chest",     B::Chest,         B::UpperChest,    G::Spine, S::Center, 0.4f,  false},
            {"neck",            B::UpperChest,    B::Neck,          G::Head,  S::Center, 0.55f, false},
            {"head",            B::Neck,          B::Head,          G::Head,  S::Center, 0.7f,  true},
            {"left_shoulder",   B::UpperChest,    B::RightShoulder, G::Arms,  S::Left,   0.45f, false},
            {"left_upper_arm",  B::LeftShoulder,  B::RightUpperArm, G::Arms,  S::Left,   0.6f,  true},
            {"left_lower_arm",  B::LeftUpperArm,  B::RightLowerArm, G::Arms,  S::Left,   0.8f,  true},
            {"left_hand",       B::LeftLowerArm,  B::RightHand,     G::Arms,  S::Left,   1.0f,  true},
            {"right_shoulder",  B::UpperChest,    B::LeftShoulder,  G::Arms,  S::Right,  0.45f, false},
            {"right_upper_arm", B::RightShoulder, B::LeftUpperArm,  G::Arms,  S::Right,  0.6f,  true},
            {"right_lower_arm", B::RightUpperArm, B::LeftLowerArm,  G::Arms,  S::Right,  0.8f,  true},
            {"right_hand",      B::RightLowerArm, B::LeftHand,      G::Arms,  S::Right,  1.0f,  true},
            {"left_upper_leg",  B::Hips,          B::RightUpperLeg, G::Legs,  S::Left,   0.3f,  true},
            {"left_lower_leg",  B::LeftUpperLeg,  B::RightLowerLeg, G::Legs,  S::Left,   0.6f,  true},
            {"left_foot",       B::LeftLowerLeg,  B::RightFoot,     G::Legs,  S::Left,   0.8f,  true},
            {"left_toes",       B::LeftFoot,      B::RightToes,     G::Legs,  S::Left,   1.0f,  false},
            {"right_upper_leg", B::Hips,          B::LeftUpperLeg,  G::Legs,  S::Right,  0.3f,  true},
            {"right_lower_leg", B::RightUpperLeg, B::LeftLowerLeg,  G::Legs,  S::Right,  0.6f,  true},
            {"right_foot",      B::RightLowerLeg, B::LeftFoot,      G::Legs,  S::Right,  0.8f,  true},
            {"right_toes",      B::RightFoot,     B::LeftToes,      G::Legs,  S::Right,  1.0f,  false},
        };

        const char* GROUP_NAMES[static_cast<int>(BodyGroup::Count)] = {"spine", "head", "arms", "legs"};

    }

    const HumanBoneInfo& humanBoneInfo(HumanBone bone){
        return BONES[static_cast<int>(bone)];
    }

    const char* humanBoneName(HumanBone bone){
        return BONES[static_cast<int>(bone)].name;
    }

    bool humanBoneFromName(const std::string& name, HumanBone& bone){
        for (int i = 0; i < HUMAN_BONE_COUNT; i++){
            if (name == BONES[i].name){
                bone = static_cast<HumanBone>(i);
                return true;
            }
        }
        return false;
    }

    const char* bodyGroupName(BodyGroup group){
        return GROUP_NAMES[static_cast<int>(group)];
    }

}
