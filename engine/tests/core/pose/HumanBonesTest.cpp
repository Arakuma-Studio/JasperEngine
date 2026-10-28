// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Acceptance test 1: the bone table (spec 4.1) and the DOF table (4.5)

#include "TestUtil.h"

#include <gtest/gtest.h>

#include <set>

using namespace posetest;

TEST(PoseHumanBones, NamesUniqueAndRoundTrip){
    std::set<std::string> names;
    for (int i = 0; i < HUMAN_BONE_COUNT; i++){
        const HumanBone bone = boneAt(i);
        const std::string name = humanBoneName(bone);
        EXPECT_TRUE(names.insert(name).second) << name;
        HumanBone back = HumanBone::Count;
        ASSERT_TRUE(humanBoneFromName(name, back));
        EXPECT_EQ(back, bone);
    }
    HumanBone unused;
    EXPECT_FALSE(humanBoneFromName("left_elbow", unused));
    EXPECT_FALSE(humanBoneFromName("Hips", unused));
}

TEST(PoseHumanBones, ParentsPrecedeChildrenAndMirrorIsAnInvolution){
    EXPECT_EQ(humanBoneInfo(HumanBone::Hips).parent, HumanBone::Count);
    for (int i = 1; i < HUMAN_BONE_COUNT; i++){
        const HumanBoneInfo& info = humanBoneInfo(boneAt(i));
        ASSERT_NE(info.parent, HumanBone::Count);
        EXPECT_LT(boneIndex(info.parent), i) << info.name;
    }
    for (int i = 0; i < HUMAN_BONE_COUNT; i++){
        const HumanBone bone = boneAt(i);
        const HumanBone mirror = humanBoneInfo(bone).mirror;
        EXPECT_EQ(humanBoneInfo(mirror).mirror, bone);
        const BodySide side = humanBoneInfo(bone).side;
        if (side == BodySide::Center){
            EXPECT_EQ(mirror, bone);
        }else{
            EXPECT_NE(mirror, bone);
            EXPECT_NE(humanBoneInfo(mirror).side, side);
            EXPECT_EQ(humanBoneInfo(mirror).group, humanBoneInfo(bone).group);
            EXPECT_EQ(humanBoneInfo(mirror).depth, humanBoneInfo(bone).depth);
        }
    }
}

TEST(PoseHumanBones, GroupsDepthsRequiredAsTable){
    using G = BodyGroup;
    struct Row { const char* name; G group; float depth; bool required; };
    const Row rows[HUMAN_BONE_COUNT] = {
        {"hips", G::Spine, 0.0f, true}, {"spine", G::Spine, 0.2f, true}, {"chest", G::Spine, 0.3f, false},
        {"upper_chest", G::Spine, 0.4f, false}, {"neck", G::Head, 0.55f, false}, {"head", G::Head, 0.7f, true},
        {"left_shoulder", G::Arms, 0.45f, false}, {"left_upper_arm", G::Arms, 0.6f, true}, {"left_lower_arm", G::Arms, 0.8f, true},
        {"left_hand", G::Arms, 1.0f, true}, {"right_shoulder", G::Arms, 0.45f, false}, {"right_upper_arm", G::Arms, 0.6f, true},
        {"right_lower_arm", G::Arms, 0.8f, true}, {"right_hand", G::Arms, 1.0f, true}, {"left_upper_leg", G::Legs, 0.3f, true},
        {"left_lower_leg", G::Legs, 0.6f, true}, {"left_foot", G::Legs, 0.8f, true}, {"left_toes", G::Legs, 1.0f, false},
        {"right_upper_leg", G::Legs, 0.3f, true}, {"right_lower_leg", G::Legs, 0.6f, true}, {"right_foot", G::Legs, 0.8f, true},
        {"right_toes", G::Legs, 1.0f, false},
    };
    for (int i = 0; i < HUMAN_BONE_COUNT; i++){
        const HumanBoneInfo& info = humanBoneInfo(boneAt(i));
        EXPECT_STREQ(info.name, rows[i].name);
        EXPECT_EQ(info.group, rows[i].group) << info.name;
        EXPECT_FLOAT_EQ(info.depth, rows[i].depth) << info.name;
        EXPECT_EQ(info.required, rows[i].required) << info.name;
    }
    EXPECT_STREQ(bodyGroupName(BodyGroup::Legs), "legs");
}

TEST(PoseDofs, NamesRoundTripAndRightSideDerived){
    std::set<std::string> names;
    for (int d = 0; d < DOF_COUNT; d++){
        const std::string name = dofName(d);
        EXPECT_TRUE(names.insert(name).second) << name;
        int back = -1;
        ASSERT_TRUE(dofFromName(name, back)) << name;
        EXPECT_EQ(back, d);
    }
    EXPECT_EQ(dofName(dofIndex(HumanBone::LeftUpperArm, 0)), "left_upper_arm.raise");
    EXPECT_EQ(dofName(dofIndex(HumanBone::Hips, 2)), "hips.turn");
    for (int i = 0; i < HUMAN_BONE_COUNT; i++){
        const HumanBone bone = boneAt(i);
        if (humanBoneInfo(bone).side != BodySide::Right){
            continue;
        }
        const BoneDofInfo& right = boneDofInfo(bone);
        const BoneDofInfo& left = boneDofInfo(humanBoneInfo(bone).mirror);
        EXPECT_EQ(right.neutralDirection.x, -left.neutralDirection.x);
        for (int k = 0; k < DOFS_PER_BONE; k++){
            EXPECT_EQ(right.dofs[k].axis, left.dofs[k].axis);
            EXPECT_EQ(right.dofs[k].sign, left.dofs[k].axis == CanonicalAxis::L ? left.dofs[k].sign : -left.dofs[k].sign);
            EXPECT_EQ(right.dofs[k].minDegrees, left.dofs[k].minDegrees);
            EXPECT_EQ(right.dofs[k].maxDegrees, left.dofs[k].maxDegrees);
        }
    }
}
