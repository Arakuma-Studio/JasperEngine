// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// .humanoid sidecar IO (spec: pose animation phase 2, section 2.2)

#include <gtest/gtest.h>

#include "pose/HumanBones.h"
#include "pose/HumanoidMapIO.h"
#include "PoseTestUtil.h"

using namespace jasperengine;
using namespace jasperengine::pose;
using posetest::anyWarningContains;

namespace {

    using HumanoidMapIOParsed = posetest::ParsedHumanoid;

    HumanoidMapIOParsed humanoidMapIOParse(const std::string& text){
        return posetest::parseHumanoid(text);
    }

    std::optional<std::string>& slot(HumanoidOverrides& overrides, HumanBone bone){
        return overrides.bones[boneIndex(bone)];
    }

}

TEST(HumanoidMapIO, EmptyOverridesWriteGolden){
    const std::string expected =
        "{\n"
        "  \"format\": \"jasperengine.humanoid\",\n"
        "  \"version\": 1,\n"
        "  \"bones\": {}\n"
        "}\n";
    EXPECT_EQ(writeHumanoidOverrides(HumanoidOverrides()), expected);
    EXPECT_TRUE(HumanoidOverrides().empty());
}

TEST(HumanoidMapIO, WriteGoldenSlotsInTableOrder){
    HumanoidOverrides overrides;
    slot(overrides, HumanBone::LeftToes) = "";                       // set first, written after hips
    slot(overrides, HumanBone::Hips) = "mixamorig:Hips";
    slot(overrides, HumanBone::RightHand) = "Hand \"R\"";
    const std::string expected =
        "{\n"
        "  \"format\": \"jasperengine.humanoid\",\n"
        "  \"version\": 1,\n"
        "  \"bones\": {\n"
        "    \"hips\": \"mixamorig:Hips\",\n"
        "    \"right_hand\": \"Hand \\\"R\\\"\",\n"
        "    \"left_toes\": \"\"\n"
        "  }\n"
        "}\n";
    EXPECT_EQ(writeHumanoidOverrides(overrides), expected);
    EXPECT_FALSE(overrides.empty());
}

TEST(HumanoidMapIO, NulloptEmptyAndNameRoundTrip){
    HumanoidOverrides overrides;
    slot(overrides, HumanBone::Hips) = "mixamorig:Hips";
    slot(overrides, HumanBone::LeftToes) = "";
    slot(overrides, HumanBone::Head) = "Kopf \xC3\xBC";
    const std::string text = writeHumanoidOverrides(overrides);

    HumanoidMapIOParsed back = humanoidMapIOParse(text);
    ASSERT_TRUE(back.ok) << back.error;
    EXPECT_TRUE(back.warnings.empty());
    EXPECT_EQ(back.overrides, overrides);
    EXPECT_EQ(back.overrides.bones[boneIndex(HumanBone::Hips)], std::optional<std::string>("mixamorig:Hips"));
    ASSERT_TRUE(back.overrides.bones[boneIndex(HumanBone::LeftToes)].has_value());
    EXPECT_EQ(*back.overrides.bones[boneIndex(HumanBone::LeftToes)], "");   // forced unmapped, not auto
    EXPECT_FALSE(back.overrides.bones[boneIndex(HumanBone::Chest)].has_value());   // auto
    EXPECT_EQ(writeHumanoidOverrides(back.overrides), text) << "write -> parse -> write must be byte-identical";
}

TEST(HumanoidMapIO, EverySlotRoundTrips){
    HumanoidOverrides overrides;
    for (int i = 0; i < HUMAN_BONE_COUNT; i++){
        overrides.bones[i] = (i % 3 == 0) ? std::string() : std::string("bone_") + std::to_string(i);
    }
    const std::string text = writeHumanoidOverrides(overrides);
    HumanoidMapIOParsed back = humanoidMapIOParse(text);
    ASSERT_TRUE(back.ok) << back.error;
    EXPECT_TRUE(back.warnings.empty());
    EXPECT_EQ(back.overrides, overrides);
    EXPECT_EQ(writeHumanoidOverrides(back.overrides), text);
    // slots appear in table order
    size_t last = 0;
    for (int i = 0; i < HUMAN_BONE_COUNT; i++){
        const size_t at = text.find(std::string("\"") + humanBoneName(boneAt(i)) + "\":");
        ASSERT_NE(at, std::string::npos) << i;
        EXPECT_GT(at, last) << i;
        last = at;
    }
}

TEST(HumanoidMapIO, ParsesTheSpecExample){
    HumanoidMapIOParsed parsed = humanoidMapIOParse("{\"format\": \"jasperengine.humanoid\", \"version\": 1,\n"
                          " \"bones\": {\"hips\": \"mixamorig:Hips\", \"left_toes\": \"\"}}");
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_TRUE(parsed.warnings.empty());
    EXPECT_EQ(parsed.overrides.bones[boneIndex(HumanBone::Hips)], std::optional<std::string>("mixamorig:Hips"));
    EXPECT_EQ(parsed.overrides.bones[boneIndex(HumanBone::LeftToes)], std::optional<std::string>(""));
    int set = 0;
    for (const auto& bone : parsed.overrides.bones){
        set += bone.has_value();
    }
    EXPECT_EQ(set, 2);
}

TEST(HumanoidMapIO, MissingBonesIsAllAuto){
    HumanoidMapIOParsed parsed = humanoidMapIOParse("{\"format\": \"jasperengine.humanoid\", \"version\": 1}");
    ASSERT_TRUE(parsed.ok);
    EXPECT_TRUE(parsed.warnings.empty());
    EXPECT_TRUE(parsed.overrides.empty());
}

TEST(HumanoidMapIO, UnknownSlotsKeysAndBadValuesWarn){
    HumanoidMapIOParsed parsed = humanoidMapIOParse("{\"format\": \"jasperengine.humanoid\", \"version\": 1, \"extra\": 1, "
                          "\"bones\": {\"hips\": \"H\", \"tail\": \"T\", \"head\": 5, \"neck\": null, \"left_hand\": []}}");
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "unknown key 'extra'"));
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "unknown slot 'tail'"));
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "bones.head"));
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "bones.left_hand"));
    EXPECT_EQ(parsed.warnings.size(), 4u);   // a null value is auto, not a problem
    EXPECT_EQ(parsed.overrides.bones[boneIndex(HumanBone::Hips)], std::optional<std::string>("H"));
    EXPECT_FALSE(parsed.overrides.bones[boneIndex(HumanBone::Head)].has_value());
    EXPECT_FALSE(parsed.overrides.bones[boneIndex(HumanBone::Neck)].has_value());

    parsed = humanoidMapIOParse("{\"format\": \"jasperengine.humanoid\", \"version\": 1, \"bones\": [1]}");
    ASSERT_TRUE(parsed.ok);
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "bones"));
    EXPECT_TRUE(parsed.overrides.empty());
}

TEST(HumanoidMapIO, BadFormatOrVersionFails){
    EXPECT_FALSE(humanoidMapIOParse("").ok);
    EXPECT_FALSE(humanoidMapIOParse("nonsense").ok);
    EXPECT_FALSE(humanoidMapIOParse("[]").ok);
    EXPECT_FALSE(humanoidMapIOParse("{\"version\": 1}").ok);
    EXPECT_FALSE(humanoidMapIOParse("{\"format\": \"jasperengine.poseanim\", \"version\": 1}").ok);
    // version 2 is the Avatar form (anim-retarget); 3 is newer than this engine
    EXPECT_TRUE(humanoidMapIOParse("{\"format\": \"jasperengine.humanoid\", \"version\": 2, \"bones\": {}}").ok);
    HumanoidMapIOParsed parsed = humanoidMapIOParse("{\"format\": \"jasperengine.humanoid\", \"version\": 3, \"bones\": {}}");
    EXPECT_FALSE(parsed.ok);
    EXPECT_NE(parsed.error.find("version"), std::string::npos) << parsed.error;
    EXPECT_FALSE(humanoidMapIOParse("{\"format\": \"jasperengine.humanoid\", \"version\": 0}").ok);
    EXPECT_FALSE(humanoidMapIOParse("{\"format\": \"jasperengine.humanoid\", \"version\": \"1\"}").ok);

    HumanoidMapIOParsed missing = humanoidMapIOParse("{\"format\": \"jasperengine.humanoid\"}");
    EXPECT_TRUE(missing.ok);
    EXPECT_TRUE(anyWarningContains(missing.warnings, "version"));

    HumanoidOverrides out;
    std::vector<std::string> warnings;
    EXPECT_FALSE(parseHumanoidOverrides("x", out, warnings, nullptr));   // the error is optional
}

// the naming rules of the model's import (HumanoidMap.h): optional, absent is the original rules,
// written in a version 2 sidecar
TEST(HumanoidMapIO, NameRulesRoundTrip){
    HumanoidOverrides overrides;
    EXPECT_EQ(overrides.detectNameRules(), HUMANOID_NAME_RULES_ORIGINAL);
    overrides.nameRules = HUMANOID_NAME_RULES;
    const std::string expected =
        "{\n"
        "  \"format\": \"jasperengine.humanoid\",\n"
        "  \"version\": 2,\n"
        "  \"name_rules\": 2,\n"
        "  \"bones\": {}\n"
        "}\n";
    EXPECT_EQ(writeHumanoidOverrides(overrides), expected);
    EXPECT_TRUE(overrides.empty()) << "no slot override";
    EXPECT_FALSE(overrides.onlyBones());
    HumanoidMapIOParsed parsed = humanoidMapIOParse(expected);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_TRUE(parsed.warnings.empty());
    EXPECT_EQ(parsed.overrides, overrides);
    EXPECT_EQ(parsed.overrides.detectNameRules(), HUMANOID_NAME_RULES);
    EXPECT_NE(parsed.overrides, HumanoidOverrides()) << "part of the value";

    // a newer editor's rules read as the newest here; anything but a whole number from 1 is ignored
    parsed = humanoidMapIOParse("{\"format\": \"jasperengine.humanoid\", \"version\": 2, \"name_rules\": 9}");
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.overrides.nameRules, std::optional<int>(HUMANOID_NAME_RULES));
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "name_rules: 9 is newer"));
    for (const char* value : {"0", "1.5", "\"2\"", "null"}){
        parsed = humanoidMapIOParse(std::string("{\"format\": \"jasperengine.humanoid\", \"version\": 2, \"name_rules\": ") + value + "}");
        ASSERT_TRUE(parsed.ok) << parsed.error;
        EXPECT_FALSE(parsed.overrides.nameRules.has_value()) << value;
        EXPECT_TRUE(anyWarningContains(parsed.warnings, "name_rules: expected")) << value;
    }
}

TEST(HumanoidMapIO, SidecarPath){
    EXPECT_EQ(humanoidSidecarPath("models/hero.gltf"), "models/hero.gltf.humanoid");
}
