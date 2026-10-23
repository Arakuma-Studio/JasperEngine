// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// .humanoid sidecar IO (spec: pose animation phase 2, section 2.2)

#include <gtest/gtest.h>

#include "pose/HumanBones.h"
#include "pose/HumanoidMapIO.h"
#include "PoseTestSystem.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::pose;

namespace {

    struct Parsed {
        HumanoidOverrides overrides;
        std::vector<std::string> warnings;
        std::string error;
        bool ok = false;
    };

    Parsed parse(const std::string& text){
        Parsed result;
        result.ok = parseHumanoidOverrides(text, result.overrides, result.warnings, &result.error);
        return result;
    }

    bool anyWarningContains(const std::vector<std::string>& warnings, const std::string& part){
        return std::any_of(warnings.begin(), warnings.end(),
                           [&](const std::string& w){ return w.find(part) != std::string::npos; });
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
    Parsed parsed = parse("{\"format\": \"jasperengine.humanoid\", \"version\": 2, \"bones\": {}}");
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

TEST(HumanoidMapIO, SaveAndLoadFile){
    jasperengine::test::ensurePoseTestSystem();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("humanoid-io-test-" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    const std::string path = (dir / humanoidSidecarPath("hero.gltf")).string();

    HumanoidOverrides overrides;
    slot(overrides, HumanBone::Hips) = "Root";
    slot(overrides, HumanBone::RightToes) = "";
    std::string error;
    ASSERT_TRUE(saveHumanoidOverridesFile(path, overrides, &error)) << error;

    std::ifstream file(path, std::ios::binary);
    std::stringstream onDisk;
    onDisk << file.rdbuf();
    EXPECT_EQ(onDisk.str(), writeHumanoidOverrides(overrides));

    HumanoidOverrides loaded;
    std::vector<std::string> warnings;
    ASSERT_TRUE(loadHumanoidOverridesFile(path, loaded, warnings, &error)) << error;
    EXPECT_TRUE(warnings.empty());
    EXPECT_EQ(loaded, overrides);

    EXPECT_FALSE(loadHumanoidOverridesFile((dir / "none.humanoid").string(), loaded, warnings, &error));
    EXPECT_NE(error.find("none.humanoid"), std::string::npos) << error;

    {
        std::ofstream bad(dir / "bad.humanoid");
        bad << "{\"format\": \"jasperengine.humanoid\", \"version\": 5}";
    }
    EXPECT_FALSE(loadHumanoidOverridesFile((dir / "bad.humanoid").string(), loaded, warnings, &error));
    EXPECT_NE(error.find("bad.humanoid"), std::string::npos) << error;

    std::filesystem::remove_all(dir);
}
