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

    using Parsed = posetest::ParsedHumanoid;

    Parsed parse(const std::string& text){
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

    Parsed back = parse(text);
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
    Parsed back = parse(text);
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
    Parsed parsed = parse("{\"format\": \"jasperengine.humanoid\", \"version\": 1,\n"
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
    Parsed parsed = parse("{\"format\": \"jasperengine.humanoid\", \"version\": 1}");
    ASSERT_TRUE(parsed.ok);
    EXPECT_TRUE(parsed.warnings.empty());
    EXPECT_TRUE(parsed.overrides.empty());
}

TEST(HumanoidMapIO, UnknownSlotsKeysAndBadValuesWarn){
    Parsed parsed = parse("{\"format\": \"jasperengine.humanoid\", \"version\": 1, \"extra\": 1, "
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

    parsed = parse("{\"format\": \"jasperengine.humanoid\", \"version\": 1, \"bones\": [1]}");
    ASSERT_TRUE(parsed.ok);
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "bones"));
    EXPECT_TRUE(parsed.overrides.empty());
}

TEST(HumanoidMapIO, BadFormatOrVersionFails){
    EXPECT_FALSE(parse("").ok);
    EXPECT_FALSE(parse("nonsense").ok);
    EXPECT_FALSE(parse("[]").ok);
    EXPECT_FALSE(parse("{\"version\": 1}").ok);
    EXPECT_FALSE(parse("{\"format\": \"jasperengine.poseanim\", \"version\": 1}").ok);
    // version 2 is the Avatar form (anim-retarget); 3 is newer than this engine
    EXPECT_TRUE(parse("{\"format\": \"jasperengine.humanoid\", \"version\": 2, \"bones\": {}}").ok);
    Parsed parsed = parse("{\"format\": \"jasperengine.humanoid\", \"version\": 3, \"bones\": {}}");
    EXPECT_FALSE(parsed.ok);
    EXPECT_NE(parsed.error.find("version"), std::string::npos) << parsed.error;
    EXPECT_FALSE(parse("{\"format\": \"jasperengine.humanoid\", \"version\": 0}").ok);
    EXPECT_FALSE(parse("{\"format\": \"jasperengine.humanoid\", \"version\": \"1\"}").ok);

    Parsed missing = parse("{\"format\": \"jasperengine.humanoid\"}");
    EXPECT_TRUE(missing.ok);
    EXPECT_TRUE(anyWarningContains(missing.warnings, "version"));

    HumanoidOverrides out;
    std::vector<std::string> warnings;
    EXPECT_FALSE(parseHumanoidOverrides("x", out, warnings, nullptr));   // the error is optional
}

TEST(HumanoidMapIO, SidecarPath){
    EXPECT_EQ(humanoidSidecarPath("models/hero.gltf"), "models/hero.gltf.humanoid");
}
