// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Acceptance test 2: humanoid auto-detect and validation (spec 4.6)

#include "TestUtil.h"

#include <gtest/gtest.h>

using namespace posetest;
using H = HumanBone;

namespace {

    using BoneList = std::vector<std::pair<std::string, std::string>>;

    void expectMap(const Skeleton& skeleton, const HumanoidMap& expected, const std::string& label){
        std::vector<std::string> notes;
        const HumanoidMap map = autoDetectHumanoid(skeleton, &notes);
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            EXPECT_EQ(map.bones[s], expected.bones[s]) << label << ": slot " << humanBoneName(boneAt(s));
        }
        std::string error;
        EXPECT_TRUE(validateHumanoid(skeleton, map, &error)) << label << ": " << error;
    }

    // every slot name: left/right pairs from a pattern, e.g. side("upperarm_%s", "l", "r")
    std::string sub(const std::string& pattern, const std::string& side){
        std::string result = pattern;
        const size_t at = result.find("%s");
        result.replace(at, 2, side);
        return result;
    }

    HumanoidMap sidedMap(std::initializer_list<std::pair<H, std::string>> centre,
                         std::initializer_list<std::pair<H, std::string>> leftPatterns,
                         const std::string& left, const std::string& right){
        HumanoidMap map;
        for (const auto& c : centre) map.bones[boneIndex(c.first)] = c.second;
        for (const auto& p : leftPatterns){
            map.bones[boneIndex(p.first)] = sub(p.second, left);
            map.bones[boneIndex(humanBoneInfo(p.first).mirror)] = sub(p.second, right);
        }
        return map;
    }

}

TEST(PoseHumanoidMap, NameTokens){
    using V = std::vector<std::string>;
    EXPECT_EQ(humanoidNameTokens("mixamorig:LeftUpLeg"), (V{"left", "up", "leg"}));
    EXPECT_EQ(humanoidNameTokens("mixamorig:LeftHandIndex1"), (V{"left", "hand", "index", "1"}));
    EXPECT_EQ(humanoidNameTokens("upper_arm.L"), (V{"upper", "arm", "l"}));
    EXPECT_EQ(humanoidNameTokens("UpperArm_L"), (V{"upper", "arm", "l"}));
    EXPECT_EQ(humanoidNameTokens("L_UpperArm"), (V{"l", "upper", "arm"}));
    EXPECT_EQ(humanoidNameTokens("LUpperArm"), (V{"l", "upper", "arm"}));
    EXPECT_EQ(humanoidNameTokens("J_Bip_L_UpperArm"), (V{"l", "upper", "arm"}));
    EXPECT_EQ(humanoidNameTokens("DEF-upper_arm.L.001"), (V{"upper", "arm", "l", "001"}));
    EXPECT_EQ(humanoidNameTokens("Bip01 L Thigh"), (V{"l", "thigh"}));
    EXPECT_EQ(humanoidNameTokens("Armature|root|spine_01"), (V{"spine", "01"}));
    EXPECT_EQ(humanoidNameTokens("b_Hips"), (V{"hips"}));
}

TEST(PoseHumanoidMap, FixtureRigsMapFully){
    for (const TestRig* rig : allRigs()){
        expectMap(rig->skeleton, rig->expected, rig->label);
    }
}

TEST(PoseHumanoidMap, GeneratedFixtureModels){
    // tools/fixtures/pose-animation: hero_tpose.gltf (Mixamo names, fingers and end bones)
    const std::string m = "mixamorig:";
    const HumanoidMap hero = sidedMap(
        {{H::Hips, m + "Hips"}, {H::Spine, m + "Spine"}, {H::Chest, m + "Spine1"}, {H::UpperChest, m + "Spine2"},
         {H::Neck, m + "Neck"}, {H::Head, m + "Head"}},
        {{H::LeftShoulder, m + "%sShoulder"}, {H::LeftUpperArm, m + "%sArm"}, {H::LeftLowerArm, m + "%sForeArm"},
         {H::LeftHand, m + "%sHand"}, {H::LeftUpperLeg, m + "%sUpLeg"}, {H::LeftLowerLeg, m + "%sLeg"},
         {H::LeftFoot, m + "%sFoot"}, {H::LeftToes, m + "%sToeBase"}},
        "Left", "Right");
    const BoneList heroBones = heroFixtureBones();
    ASSERT_EQ(heroBones.size(), 43u);
    expectMap(skeletonFromList(heroBones), hero, "hero_tpose");

    // robot_apose.gltf (Blender names, a twist bone between upper_arm and forearm, no upper chest)
    const HumanoidMap robot = sidedMap(
        {{H::Hips, "pelvis"}, {H::Spine, "spine"}, {H::Chest, "spine.001"}, {H::Neck, "neck"}, {H::Head, "head"}},
        {{H::LeftShoulder, "shoulder.%s"}, {H::LeftUpperArm, "upper_arm.%s"}, {H::LeftLowerArm, "forearm.%s"},
         {H::LeftHand, "hand.%s"}, {H::LeftUpperLeg, "thigh.%s"}, {H::LeftLowerLeg, "shin.%s"},
         {H::LeftFoot, "foot.%s"}, {H::LeftToes, "toe.%s"}},
        "L", "R");
    const BoneList robotBones = robotFixtureBones();
    ASSERT_EQ(robotBones.size(), 23u);
    expectMap(skeletonFromList(robotBones), robot, "robot_apose");

    // effective parents on the robot: neck and shoulders hang from the chest
    const HumanoidMap detected = autoDetectHumanoid(skeletonFromList(robotBones));
    EXPECT_EQ(effectiveParentSlot(detected, H::Neck), H::Chest);
    EXPECT_EQ(effectiveParentSlot(detected, H::LeftShoulder), H::Chest);
    EXPECT_EQ(effectiveParentSlot(detected, H::RightShoulder), H::Chest);
}

TEST(PoseHumanoidMap, RigifyLikeNames){
    BoneList bones = {{"root", ""}, {"DEF-hips", "root"}, {"DEF-spine", "DEF-hips"}, {"DEF-spine.001", "DEF-spine"},
                      {"DEF-spine.002", "DEF-spine.001"}, {"DEF-neck", "DEF-spine.002"}, {"DEF-head", "DEF-neck"}};
    for (const char* s : {"L", "R"}){
        const std::string x = std::string(".") + s;
        bones.push_back({"DEF-shoulder" + x, "DEF-spine.002"});
        bones.push_back({"DEF-upper_arm" + x, "DEF-shoulder" + x});
        bones.push_back({"DEF-upper_arm" + x + ".001", "DEF-upper_arm" + x});
        bones.push_back({"DEF-forearm" + x, "DEF-upper_arm" + x + ".001"});
        bones.push_back({"DEF-forearm" + x + ".001", "DEF-forearm" + x});
        bones.push_back({"DEF-hand" + x, "DEF-forearm" + x + ".001"});
        bones.push_back({"DEF-thigh" + x, "DEF-hips"});
        bones.push_back({"DEF-thigh" + x + ".001", "DEF-thigh" + x});
        bones.push_back({"DEF-shin" + x, "DEF-thigh" + x + ".001"});
        bones.push_back({"DEF-shin" + x + ".001", "DEF-shin" + x});
        bones.push_back({"DEF-foot" + x, "DEF-shin" + x + ".001"});
        bones.push_back({"DEF-toe" + x, "DEF-foot" + x});
    }
    const HumanoidMap expected = sidedMap(
        {{H::Hips, "DEF-hips"}, {H::Spine, "DEF-spine"}, {H::Chest, "DEF-spine.001"}, {H::UpperChest, "DEF-spine.002"},
         {H::Neck, "DEF-neck"}, {H::Head, "DEF-head"}},
        {{H::LeftShoulder, "DEF-shoulder.%s"}, {H::LeftUpperArm, "DEF-upper_arm.%s"}, {H::LeftLowerArm, "DEF-forearm.%s"},
         {H::LeftHand, "DEF-hand.%s"}, {H::LeftUpperLeg, "DEF-thigh.%s"}, {H::LeftLowerLeg, "DEF-shin.%s"},
         {H::LeftFoot, "DEF-foot.%s"}, {H::LeftToes, "DEF-toe.%s"}},
        "L", "R");
    expectMap(skeletonFromList(bones), expected, "rigify");
}

TEST(PoseHumanoidMap, UnrealMannequinNames){
    BoneList bones = {{"root", ""}, {"pelvis", "root"}, {"spine_01", "pelvis"}, {"spine_02", "spine_01"}, {"spine_03", "spine_02"},
                      {"spine_04", "spine_03"}, {"spine_05", "spine_04"}, {"neck_01", "spine_05"}, {"neck_02", "neck_01"},
                      {"head", "neck_02"}, {"ik_foot_root", "root"}, {"ik_hand_root", "root"}, {"ik_hand_gun", "ik_hand_root"}};
    for (const char* s : {"l", "r"}){
        const std::string x = std::string("_") + s;
        bones.push_back({"clavicle" + x, "spine_05"});
        bones.push_back({"upperarm" + x, "clavicle" + x});
        bones.push_back({"upperarm_twist_01" + x, "upperarm" + x});
        bones.push_back({"lowerarm" + x, "upperarm" + x});
        bones.push_back({"lowerarm_twist_01" + x, "lowerarm" + x});
        bones.push_back({"hand" + x, "lowerarm" + x});
        bones.push_back({"index_metacarpal" + x, "hand" + x});
        bones.push_back({"index_01" + x, "index_metacarpal" + x});
        bones.push_back({"thumb_01" + x, "hand" + x});
        bones.push_back({"thigh" + x, "pelvis"});
        bones.push_back({"thigh_twist_01" + x, "thigh" + x});
        bones.push_back({"calf" + x, "thigh" + x});
        bones.push_back({"calf_twist_01" + x, "calf" + x});
        bones.push_back({"foot" + x, "calf" + x});
        bones.push_back({"ball" + x, "foot" + x});
        bones.push_back({"ik_foot" + x, "ik_foot_root"});
        bones.push_back({"ik_hand" + x, "ik_hand_gun"});
    }
    const HumanoidMap expected = sidedMap(
        {{H::Hips, "pelvis"}, {H::Spine, "spine_01"}, {H::Chest, "spine_03"}, {H::UpperChest, "spine_05"},
         {H::Neck, "neck_01"}, {H::Head, "head"}},
        {{H::LeftShoulder, "clavicle_%s"}, {H::LeftUpperArm, "upperarm_%s"}, {H::LeftLowerArm, "lowerarm_%s"},
         {H::LeftHand, "hand_%s"}, {H::LeftUpperLeg, "thigh_%s"}, {H::LeftLowerLeg, "calf_%s"},
         {H::LeftFoot, "foot_%s"}, {H::LeftToes, "ball_%s"}},
        "l", "r");
    const Skeleton skeleton = skeletonFromList(bones);
    expectMap(skeleton, expected, "unreal");

    std::vector<std::string> notes;
    autoDetectHumanoid(skeleton, &notes);
    bool spineNote = false, neckNote = false;
    for (const std::string& n : notes){
        spineNote = spineNote || n.find("spine: 5 spine bones") != std::string::npos;
        neckNote = neckNote || n.find("neck: 2 candidates") != std::string::npos;
    }
    EXPECT_TRUE(spineNote);
    EXPECT_TRUE(neckNote);
}

TEST(PoseHumanoidMap, VrmAndUnityNames){
    BoneList vrm = {{"Root", ""}, {"J_Bip_C_Hips", "Root"}, {"J_Bip_C_Spine", "J_Bip_C_Hips"}, {"J_Bip_C_Chest", "J_Bip_C_Spine"},
                    {"J_Bip_C_UpperChest", "J_Bip_C_Chest"}, {"J_Bip_C_Neck", "J_Bip_C_UpperChest"}, {"J_Bip_C_Head", "J_Bip_C_Neck"},
                    {"J_Adj_C_FaceEye", "J_Bip_C_Head"}};
    BoneList unity = {{"Hips", ""}, {"Spine", "Hips"}, {"Chest", "Spine"}, {"UpperChest", "Chest"}, {"Neck", "UpperChest"}, {"Head", "Neck"},
                      {"Jaw", "Head"}};
    for (const char* s : {"L", "R"}){
        const std::string x = std::string("J_Bip_") + s + "_";
        vrm.push_back({x + "Shoulder", "J_Bip_C_UpperChest"});
        vrm.push_back({x + "UpperArm", x + "Shoulder"});
        vrm.push_back({x + "LowerArm", x + "UpperArm"});
        vrm.push_back({x + "Hand", x + "LowerArm"});
        vrm.push_back({x + "Thumb1", x + "Hand"});
        vrm.push_back({x + "Index1", x + "Hand"});
        vrm.push_back({x + "Little1", x + "Hand"});
        vrm.push_back({x + "UpperLeg", "J_Bip_C_Hips"});
        vrm.push_back({x + "LowerLeg", x + "UpperLeg"});
        vrm.push_back({x + "Foot", x + "LowerLeg"});
        vrm.push_back({x + "ToeBase", x + "Foot"});
    }
    for (const char* s : {"Left", "Right"}){
        const std::string x = s;
        unity.push_back({x + "Shoulder", "UpperChest"});
        unity.push_back({x + "UpperArm", x + "Shoulder"});
        unity.push_back({x + "LowerArm", x + "UpperArm"});
        unity.push_back({x + "Hand", x + "LowerArm"});
        unity.push_back({x + "IndexProximal", x + "Hand"});
        unity.push_back({x + "UpperLeg", "Hips"});
        unity.push_back({x + "LowerLeg", x + "UpperLeg"});
        unity.push_back({x + "Foot", x + "LowerLeg"});
        unity.push_back({x + "Toes", x + "Foot"});
    }
    expectMap(skeletonFromList(vrm), sidedMap(
        {{H::Hips, "J_Bip_C_Hips"}, {H::Spine, "J_Bip_C_Spine"}, {H::Chest, "J_Bip_C_Chest"}, {H::UpperChest, "J_Bip_C_UpperChest"},
         {H::Neck, "J_Bip_C_Neck"}, {H::Head, "J_Bip_C_Head"}},
        {{H::LeftShoulder, "J_Bip_%s_Shoulder"}, {H::LeftUpperArm, "J_Bip_%s_UpperArm"}, {H::LeftLowerArm, "J_Bip_%s_LowerArm"},
         {H::LeftHand, "J_Bip_%s_Hand"}, {H::LeftUpperLeg, "J_Bip_%s_UpperLeg"}, {H::LeftLowerLeg, "J_Bip_%s_LowerLeg"},
         {H::LeftFoot, "J_Bip_%s_Foot"}, {H::LeftToes, "J_Bip_%s_ToeBase"}},
        "L", "R"), "vrm");
    expectMap(skeletonFromList(unity), sidedMap(
        {{H::Hips, "Hips"}, {H::Spine, "Spine"}, {H::Chest, "Chest"}, {H::UpperChest, "UpperChest"}, {H::Neck, "Neck"}, {H::Head, "Head"}},
        {{H::LeftShoulder, "%sShoulder"}, {H::LeftUpperArm, "%sUpperArm"}, {H::LeftLowerArm, "%sLowerArm"},
         {H::LeftHand, "%sHand"}, {H::LeftUpperLeg, "%sUpperLeg"}, {H::LeftLowerLeg, "%sLowerLeg"},
         {H::LeftFoot, "%sFoot"}, {H::LeftToes, "%sToes"}},
        "Left", "Right"), "unity");
}

TEST(PoseHumanoidMap, BipedNames){
    BoneList bones = {{"Bip01", ""}, {"Bip01 Pelvis", "Bip01"}, {"Bip01 Spine", "Bip01 Pelvis"}, {"Bip01 Spine1", "Bip01 Spine"},
                      {"Bip01 Neck", "Bip01 Spine1"}, {"Bip01 Head", "Bip01 Neck"}, {"Bip01 HeadNub", "Bip01 Head"}};
    for (const char* s : {"L", "R"}){
        const std::string x = std::string("Bip01 ") + s + " ";
        bones.push_back({x + "Clavicle", "Bip01 Spine1"});
        bones.push_back({x + "UpperArm", x + "Clavicle"});
        bones.push_back({x + "Forearm", x + "UpperArm"});
        bones.push_back({x + "Hand", x + "Forearm"});
        bones.push_back({x + "Finger0", x + "Hand"});
        bones.push_back({x + "Thigh", "Bip01 Pelvis"});
        bones.push_back({x + "Calf", x + "Thigh"});
        bones.push_back({x + "Foot", x + "Calf"});
        bones.push_back({x + "Toe0", x + "Foot"});
        bones.push_back({x + "Toe0Nub", x + "Toe0"});
    }
    // (a real 3ds Max biped hangs the clavicles from the neck: validateHumanoid rejects that,
    // the neck slot would lie between the shoulders and their parent slot)
    expectMap(skeletonFromList(bones), sidedMap(
        {{H::Hips, "Bip01 Pelvis"}, {H::Spine, "Bip01 Spine"}, {H::Chest, "Bip01 Spine1"}, {H::Neck, "Bip01 Neck"}, {H::Head, "Bip01 Head"}},
        {{H::LeftShoulder, "Bip01 %s Clavicle"}, {H::LeftUpperArm, "Bip01 %s UpperArm"}, {H::LeftLowerArm, "Bip01 %s Forearm"},
         {H::LeftHand, "Bip01 %s Hand"}, {H::LeftUpperLeg, "Bip01 %s Thigh"}, {H::LeftLowerLeg, "Bip01 %s Calf"},
         {H::LeftFoot, "Bip01 %s Foot"}, {H::LeftToes, "Bip01 %s Toe0"}},
        "L", "R"), "biped");
}

TEST(PoseHumanoidMap, HelperBonesNeverMapped){
    for (const TestRig* rig : allRigs()){
        const HumanoidMap map = autoDetectHumanoid(rig->skeleton);
        for (const std::string& name : map.bones){
            std::string lower;
            for (char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            EXPECT_EQ(lower.find("twist"), std::string::npos) << name;
            EXPECT_EQ(lower.find("end"), std::string::npos) << name;
            EXPECT_EQ(lower.find("index"), std::string::npos) << name;
            EXPECT_EQ(lower.find("thumb"), std::string::npos) << name;
            EXPECT_EQ(lower.find("middle"), std::string::npos) << name;
        }
    }
    // a lone twist bone with a limb name is not taken as the limb
    const Skeleton skeleton = skeletonFromList({{"Hips", ""}, {"LeftUpperArmTwist", "Hips"}, {"upperarm_roll_l", "Hips"}, {"hand_ik_l", "Hips"}});
    const HumanoidMap map = autoDetectHumanoid(skeleton);
    EXPECT_EQ(map.bones[boneIndex(H::LeftUpperArm)], "");
    EXPECT_EQ(map.bones[boneIndex(H::LeftHand)], "");
}

TEST(PoseHumanoidMap, StructuralHipsAndAmbiguityNotes){
    BoneList bones = {{"root", ""}, {"cog", "root"}, {"Spine", "cog"}, {"Neck", "Spine"}, {"Head", "Neck"}};
    for (const char* s : {"Left", "Right"}){
        const std::string x = s;
        bones.push_back({x + "UpperArm", "Spine"});
        bones.push_back({x + "LowerArm", x + "UpperArm"});
        bones.push_back({x + "Hand", x + "LowerArm"});
        bones.push_back({x + "Hand_prop", x + "Hand"});   // also "hand": deeper, so not chosen
        bones.push_back({x + "UpLeg", "cog"});
        bones.push_back({x + "Leg", x + "UpLeg"});
        bones.push_back({x + "Foot", x + "Leg"});
    }
    bones.push_back({"left_hand", "LeftHand_prop"});
    std::vector<std::string> notes;
    const HumanoidMap map = autoDetectHumanoid(skeletonFromList(bones), &notes);
    EXPECT_EQ(map.bones[boneIndex(H::Hips)], "cog");
    EXPECT_EQ(map.bones[boneIndex(H::LeftHand)], "LeftHand");
    bool hipsNote = false, handNote = false;
    for (const std::string& n : notes){
        hipsNote = hipsNote || (n.find("hips:") == 0 && n.find("'cog'") != std::string::npos);
        handNote = handNote || n.find("left_hand: 2 candidates") == 0;
    }
    EXPECT_TRUE(hipsNote);
    EXPECT_TRUE(handNote);
    std::string error;
    EXPECT_TRUE(validateHumanoid(skeletonFromList(bones), map, &error)) << error;
}

TEST(PoseHumanoidMap, ValidationFailures){
    // a rig missing a required slot fails naming it
    Skeleton skeleton = tposeRig().skeleton;
    for (SkeletonBone& bone : skeleton.bones){
        if (bone.name == "mixamorig:LeftHand") bone.name = "mixamorig:LeftPaw";
    }
    const HumanoidMap map = autoDetectHumanoid(skeleton);
    EXPECT_EQ(map.bones[boneIndex(H::LeftHand)], "");
    std::string error;
    EXPECT_FALSE(validateHumanoid(skeleton, map, &error));
    EXPECT_NE(error.find("missing required slots: left_hand"), std::string::npos) << error;
    PoseRig rig;
    EXPECT_FALSE(rig.build(skeleton, map, &error));
    EXPECT_FALSE(rig.valid());

    // unknown bone, a bone used twice, a slot outside its parent's hierarchy
    const Skeleton good = tposeRig().skeleton;
    HumanoidMap bad = tposeRig().expected;
    bad.bones[boneIndex(H::LeftToes)] = "nope";
    EXPECT_FALSE(validateHumanoid(good, bad, &error));
    EXPECT_NE(error.find("left_toes: no bone named 'nope'"), std::string::npos) << error;
    bad = tposeRig().expected;
    bad.bones[boneIndex(H::LeftToes)] = "mixamorig:LeftFoot";
    EXPECT_FALSE(validateHumanoid(good, bad, &error));
    EXPECT_NE(error.find("mapped to both"), std::string::npos) << error;
    bad = tposeRig().expected;
    std::swap(bad.bones[boneIndex(H::LeftHand)], bad.bones[boneIndex(H::RightHand)]);
    EXPECT_FALSE(validateHumanoid(good, bad, &error));
    EXPECT_NE(error.find("is not a descendant of"), std::string::npos) << error;
}
