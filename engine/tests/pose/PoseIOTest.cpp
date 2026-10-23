// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// .poseanim file IO and the value ranges (spec: pose animation phase 2 section 2.1, phase 1 section 4.2)

#include <gtest/gtest.h>

#include "pose/PoseAnimationIO.h"
#include "pose/PoseDofs.h"
#include "pose/PoseRanges.h"
#include "PoseTestSystem.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::pose;

namespace {

    const char* HEADER = "\"format\": \"jasperengine.poseanim\", \"version\": 1";

    std::string withHeader(const std::string& body){
        return std::string("{") + HEADER + (body.empty() ? "" : ", " + body) + "}";
    }

    struct Parsed {
        PoseAnimationData data;
        std::vector<std::string> warnings;
        std::string error;
        bool ok = false;
        std::optional<PoseAnimationTemplateInfo> info;
    };

    Parsed parse(const std::string& text){
        Parsed result;
        result.ok = parsePoseAnimation(text, result.data, result.warnings, &result.error, &result.info);
        return result;
    }

    bool anyWarningContains(const std::vector<std::string>& warnings, const std::string& part){
        return std::any_of(warnings.begin(), warnings.end(),
                           [&](const std::string& w){ return w.find(part) != std::string::npos; });
    }

    int dof(const char* name){
        int index = -1;
        EXPECT_TRUE(dofFromName(name, index)) << name;
        return index;
    }

    void expectTransitionEq(const Transition& a, const Transition& b){
        EXPECT_EQ(a.curve, b.curve);
        EXPECT_FLOAT_EQ(a.duration, b.duration);
        EXPECT_FLOAT_EQ(a.overshoot, b.overshoot);
        EXPECT_FLOAT_EQ(a.anticipation, b.anticipation);
    }

    void expectDataEq(const PoseAnimationData& a, const PoseAnimationData& b){
        EXPECT_EQ(a.previewModel, b.previewModel);
        EXPECT_EQ(a.settings.loop, b.settings.loop);
        EXPECT_EQ(a.settings.frameRate, b.settings.frameRate);
        expectTransitionEq(a.settings.transition, b.settings.transition);
        EXPECT_FLOAT_EQ(a.settings.stepRate, b.settings.stepRate);
        EXPECT_FLOAT_EQ(a.settings.overlap, b.settings.overlap);
        EXPECT_FLOAT_EQ(a.settings.looseness, b.settings.looseness);
        for (int g = 0; g < 4; g++){
            EXPECT_FLOAT_EQ(a.settings.weights[g], b.settings.weights[g]) << g;
        }
        const HumanizeSettings& ha = a.settings.humanize;
        const HumanizeSettings& hb = b.settings.humanize;
        EXPECT_EQ(ha.enabled, hb.enabled);
        EXPECT_EQ(ha.jointLimits, hb.jointLimits);
        EXPECT_FLOAT_EQ(ha.movingHolds, hb.movingHolds);
        EXPECT_FLOAT_EQ(ha.inbetweens, hb.inbetweens);
        EXPECT_FLOAT_EQ(ha.life, hb.life);
        EXPECT_EQ(ha.seed, hb.seed);
        EXPECT_FLOAT_EQ(ha.headFollow, hb.headFollow);
        EXPECT_FLOAT_EQ(ha.torsoFollow, hb.torsoFollow);
        EXPECT_FLOAT_EQ(ha.springiness, hb.springiness);
        ASSERT_EQ(a.poses.size(), b.poses.size());
        for (size_t i = 0; i < a.poses.size(); i++){
            const PoseKey& ka = a.poses[i];
            const PoseKey& kb = b.poses[i];
            EXPECT_EQ(ka.name, kb.name) << i;
            EXPECT_FLOAT_EQ(ka.hold, kb.hold) << i;
            EXPECT_EQ(ka.customTransition, kb.customTransition) << i;
            if (ka.customTransition){
                expectTransitionEq(ka.transition, kb.transition);
            }
            EXPECT_EQ(ka.grounded, kb.grounded) << i;
            EXPECT_FLOAT_EQ(ka.lift, kb.lift) << i;
            EXPECT_FLOAT_EQ(ka.body.offset.x, kb.body.offset.x) << i;
            EXPECT_FLOAT_EQ(ka.body.offset.y, kb.body.offset.y) << i;
            EXPECT_FLOAT_EQ(ka.body.offset.z, kb.body.offset.z) << i;
            for (int d = 0; d < DOF_COUNT; d++){
                EXPECT_FLOAT_EQ(ka.body.angles[d], kb.body.angles[d]) << i << " " << dofName(d);
            }
        }
    }

    // Every field away from its default, with values exact in binary and in 4 decimals
    PoseAnimationData fullData(){
        PoseAnimationData data;
        data.previewModel = "models/h\xC3\xA9ro \"quoted\" \\ back.gltf";
        PoseAnimationSettings& s = data.settings;
        s.loop = false;
        s.frameRate = 60;
        s.transition = {TransitionCurve::Smooth, 0.25f, 0.5f, 0.125f};
        s.stepRate = 12.5f;
        s.overlap = 0.125f;
        s.looseness = 0.75f;
        s.weights = {0.5f, 1.5f, 2.0f, 0.25f};
        s.humanize = {true, false, 0.25f, 0.75f, 2.5f, 12345u, 1.25f, 0.5f, 0.875f};

        PoseKey a;
        a.name = "Contact R";
        a.hold = 0.375f;
        a.grounded = true;
        a.body.offset = Vector3(0.0625f, 0, -0.5f);
        a.body.angles[dof("left_upper_arm.raise")] = 8.5f;
        a.body.angles[dof("right_upper_leg.forward")] = -25.25f;
        data.poses.push_back(a);

        PoseKey b;
        b.name = "Air \xE2\x80\x93 lifted";
        b.hold = 2.0f;
        b.customTransition = true;
        b.transition = {TransitionCurve::Linear, 0.5f, 0.0f, 0.5f};
        b.grounded = false;
        b.lift = 1.5f;
        b.body.offset = Vector3(-1.0f, 0, 2.0f);
        b.body.angles[0] = 180.0f;
        b.body.angles[DOF_COUNT - 1] = -179.5f;
        data.poses.push_back(b);

        PoseKey c;   // an empty, default key
        data.poses.push_back(c);
        return data;
    }

    std::filesystem::path scratchDir(){
        static int counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                          ("pose-io-test-" + std::to_string(stamp) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(dir);
        return dir;
    }

}

// ---- writing ----

TEST(PoseIO, DefaultsWriteGolden){
    const std::string expected =
        "{\n"
        "  \"format\": \"jasperengine.poseanim\",\n"
        "  \"version\": 1,\n"
        "  \"preview_model\": \"\",\n"
        "  \"frame_rate\": 30,\n"
        "  \"loop\": true,\n"
        "  \"feel\": {\n"
        "    \"transition\": {\"curve\": \"spring\", \"duration\": 0.12, \"overshoot\": 0.25, \"anticipation\": 0.06},\n"
        "    \"step_rate\": 0, \"overlap\": 0.05, \"looseness\": 0.2,\n"
        "    \"weights\": {\"spine\": 1, \"head\": 1, \"arms\": 1, \"legs\": 0}\n"
        "  },\n"
        "  \"humanize\": {\"enabled\": false, \"joint_limits\": true, \"moving_holds\": 0.08, \"inbetweens\": 0.4,\n"
        "               \"life\": 0.6, \"seed\": 1, \"head_follow\": 0.5, \"torso_follow\": 0.4, \"springiness\": 0.5},\n"
        "  \"poses\": []\n"
        "}\n";
    EXPECT_EQ(writePoseAnimation(PoseAnimationData()), expected);
}

TEST(PoseIO, PoseWriteGolden){
    PoseAnimationData data;
    PoseKey key;
    key.name = "Contact R";
    key.hold = 0.1f;
    key.body.offset = Vector3(0, 0, 0.03f);
    key.body.angles[dof("left_upper_arm.raise")] = 8;
    key.body.angles[dof("right_upper_leg.forward")] = 25;
    data.poses.push_back(key);
    PoseKey custom;
    custom.name = "B";
    custom.customTransition = true;
    custom.transition = {TransitionCurve::Smooth, 0.2f, 0, 0};
    custom.grounded = false;
    custom.lift = 0.5f;
    data.poses.push_back(custom);

    const std::string text = writePoseAnimation(data);
    const std::string expected =
        "  \"poses\": [\n"
        "    {\"name\": \"Contact R\", \"hold\": 0.1, \"grounded\": true, \"lift\": 0,\n"
        "     \"transition\": null,\n"
        "     \"offset\": {\"left\": 0, \"forward\": 0.03},\n"
        "     \"angles\": {\"left_upper_arm.raise\": 8, \"right_upper_leg.forward\": 25}},\n"
        "    {\"name\": \"B\", \"hold\": 0.1, \"grounded\": false, \"lift\": 0.5,\n"
        "     \"transition\": {\"curve\": \"smooth\", \"duration\": 0.2, \"overshoot\": 0, \"anticipation\": 0},\n"
        "     \"offset\": {\"left\": 0, \"forward\": 0},\n"
        "     \"angles\": {}}\n"
        "  ]\n"
        "}\n";
    ASSERT_GE(text.size(), expected.size());
    EXPECT_EQ(text.substr(text.size() - expected.size()), expected);
    EXPECT_EQ(text.back(), '\n');
}

TEST(PoseIO, KeyOrderOfTheTopLevel){
    const std::string text = writePoseAnimation(PoseAnimationData());
    size_t last = 0;
    for (const char* key : {"\"format\"", "\"version\"", "\"preview_model\"", "\"frame_rate\"", "\"loop\"",
                            "\"feel\"", "\"humanize\"", "\"poses\""}){
        const size_t at = text.find(key);
        ASSERT_NE(at, std::string::npos) << key;
        EXPECT_GT(at, last) << key;
        last = at;
    }
}

TEST(PoseIO, NumbersRoundedToFourDecimalsWithoutNegativeZero){
    PoseAnimationData data;
    data.settings.overlap = 0.123456f;
    data.settings.stepRate = 10.0f;
    PoseKey key;
    key.hold = 0.00004f;                       // rounds to 0
    key.lift = 0.0f;
    key.body.offset = Vector3(-0.00001f, 0, 1.00005f);
    data.poses.push_back(key);
    const std::string text = writePoseAnimation(data);
    EXPECT_NE(text.find("\"overlap\": 0.1235"), std::string::npos) << text;
    EXPECT_NE(text.find("\"step_rate\": 10,"), std::string::npos);
    EXPECT_NE(text.find("\"hold\": 0,"), std::string::npos);
    EXPECT_NE(text.find("\"left\": 0,"), std::string::npos) << "no -0";
    EXPECT_EQ(text.find("-0,"), std::string::npos);
}

TEST(PoseIO, TinyAnglesOmittedOnWrite){
    PoseAnimationData data;
    PoseKey key;
    key.body.angles[dof("head.bend")] = 0.0004f;
    key.body.angles[dof("head.side")] = -0.0004f;
    key.body.angles[dof("neck.bend")] = 0.0005f;
    key.body.angles[dof("neck.side")] = -0.0005f;
    key.body.angles[dof("spine.bend")] = 0.0f;
    data.poses.push_back(key);
    const std::string text = writePoseAnimation(data);
    EXPECT_EQ(text.find("head.bend"), std::string::npos);
    EXPECT_EQ(text.find("head.side"), std::string::npos);
    EXPECT_EQ(text.find("spine.bend"), std::string::npos);
    EXPECT_NE(text.find("\"neck.bend\": 0.0005"), std::string::npos);
    EXPECT_NE(text.find("\"neck.side\": -0.0005"), std::string::npos);

    Parsed back = parse(text);
    ASSERT_TRUE(back.ok) << back.error;
    EXPECT_FLOAT_EQ(back.data.poses[0].body.angles[dof("head.bend")], 0.0f);
    EXPECT_FLOAT_EQ(back.data.poses[0].body.angles[dof("neck.bend")], 0.0005f);
}

TEST(PoseIO, AnglesAreInDofOrder){
    PoseAnimationData data;
    PoseKey key;
    key.body.angles[dof("right_toes.bend")] = 1;
    key.body.angles[dof("hips.lean")] = 2;
    key.body.angles[dof("left_upper_arm.raise")] = 3;
    data.poses.push_back(key);
    const std::string text = writePoseAnimation(data);
    const size_t hips = text.find("hips.lean");
    const size_t arm = text.find("left_upper_arm.raise");
    const size_t toes = text.find("right_toes.bend");
    ASSERT_NE(hips, std::string::npos);
    ASSERT_NE(arm, std::string::npos);
    ASSERT_NE(toes, std::string::npos);
    EXPECT_LT(hips, arm);
    EXPECT_LT(arm, toes);
}

TEST(PoseIO, AllDofsRoundTripAndLinesStayShort){
    PoseAnimationData data;
    PoseKey key;
    for (int d = 0; d < DOF_COUNT; d++){
        key.body.angles[d] = static_cast<float>((d % 7) + 1) * (d % 2 ? 1.5f : -2.25f);
    }
    data.poses.push_back(key);
    const std::string text = writePoseAnimation(data);
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)){
        EXPECT_LE(line.size(), 130u) << line;
    }
    Parsed back = parse(text);
    ASSERT_TRUE(back.ok) << back.error;
    EXPECT_TRUE(back.warnings.empty()) << back.warnings[0];
    expectDataEq(data, back.data);
    EXPECT_EQ(writePoseAnimation(back.data), text);
}

// ---- round trip ----

TEST(PoseIO, DefaultsRoundTrip){
    const PoseAnimationData defaults;
    const std::string text = writePoseAnimation(defaults);
    Parsed back = parse(text);
    ASSERT_TRUE(back.ok) << back.error;
    EXPECT_TRUE(back.warnings.empty());
    expectDataEq(defaults, back.data);
    EXPECT_EQ(writePoseAnimation(back.data), text);
}

TEST(PoseIO, EveryFieldRoundTrips){
    const PoseAnimationData data = fullData();
    const std::string text = writePoseAnimation(data);
    Parsed back = parse(text);
    ASSERT_TRUE(back.ok) << back.error;
    EXPECT_TRUE(back.warnings.empty()) << (back.warnings.empty() ? "" : back.warnings[0]);
    expectDataEq(data, back.data);
    EXPECT_EQ(writePoseAnimation(back.data), text) << "write -> parse -> write must be byte-identical";
}

TEST(PoseIO, RoundTripOfUnroundedValuesSettlesAfterOneWrite){
    PoseAnimationData data;
    data.settings.overlap = 0.123456789f;
    data.settings.transition.duration = 0.3333333f;
    PoseKey key;
    key.body.angles[dof("head.bend")] = 12.34567f;
    key.body.offset = Vector3(0.1234567f, 0, 0.7654321f);
    data.poses.push_back(key);
    const std::string first = writePoseAnimation(data);
    Parsed back = parse(first);
    ASSERT_TRUE(back.ok) << back.error;
    EXPECT_EQ(writePoseAnimation(back.data), first);
    EXPECT_NEAR(back.data.poses[0].body.angles[dof("head.bend")], 12.34567f, 0.00006f);
    EXPECT_NEAR(back.data.settings.overlap, 0.123456789f, 0.00006f);
}

TEST(PoseIO, ParsesTheSpecExample){
    const std::string text = R"({
  "format": "jasperengine.poseanim",
  "version": 1,
  "preview_model": "models/hero.gltf",
  "frame_rate": 30,
  "loop": true,
  "feel": {
    "transition": {"curve": "spring", "duration": 0.12, "overshoot": 0.25, "anticipation": 0.06},
    "step_rate": 0, "overlap": 0.05, "looseness": 0.2,
    "weights": {"spine": 1, "head": 1, "arms": 1, "legs": 0}
  },
  "humanize": {"enabled": false, "joint_limits": true, "moving_holds": 0.08, "inbetweens": 0.4,
               "life": 0.6, "seed": 1, "head_follow": 0.5, "torso_follow": 0.4, "springiness": 0.5},
  "poses": [
    {"name": "Contact R", "hold": 0.1, "grounded": true, "lift": 0,
     "transition": null,
     "offset": {"left": 0, "forward": 0.03},
     "angles": {"left_upper_arm.raise": 8, "right_upper_leg.forward": 25}}
  ]
})";
    Parsed parsed = parse(text);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_TRUE(parsed.warnings.empty());
    EXPECT_EQ(parsed.data.previewModel, "models/hero.gltf");
    ASSERT_EQ(parsed.data.poses.size(), 1u);
    const PoseKey& key = parsed.data.poses[0];
    EXPECT_EQ(key.name, "Contact R");
    EXPECT_FALSE(key.customTransition);
    EXPECT_FLOAT_EQ(key.body.offset.z, 0.03f);
    EXPECT_FLOAT_EQ(key.body.angles[dof("left_upper_arm.raise")], 8.0f);
    EXPECT_FLOAT_EQ(key.body.angles[dof("right_upper_leg.forward")], 25.0f);
    expectDataEq(parsed.data, parse(writePoseAnimation(parsed.data)).data);
}

// ---- defaults for missing keys ----

TEST(PoseIO, MissingKeysKeepTheDefaults){
    Parsed parsed = parse(withHeader(""));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_TRUE(parsed.warnings.empty());
    expectDataEq(parsed.data, PoseAnimationData());
    EXPECT_FALSE(parsed.info.has_value());
}

TEST(PoseIO, PartialBlocksKeepTheRestOfTheDefaults){
    Parsed parsed = parse(withHeader(
        "\"feel\": {\"transition\": {\"duration\": 0.3}, \"weights\": {\"legs\": 1}},"
        "\"humanize\": {\"enabled\": true},"
        "\"poses\": [{\"name\": \"A\", \"transition\": {\"curve\": \"linear\"}}, {}]"));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_TRUE(parsed.warnings.empty());
    const PoseAnimationSettings& s = parsed.data.settings;
    EXPECT_FLOAT_EQ(s.transition.duration, 0.3f);
    EXPECT_EQ(s.transition.curve, TransitionCurve::Spring);
    EXPECT_FLOAT_EQ(s.transition.overshoot, 0.25f);
    EXPECT_FLOAT_EQ(s.weights[3], 1.0f);
    EXPECT_FLOAT_EQ(s.weights[0], 1.0f);
    EXPECT_TRUE(s.humanize.enabled);
    EXPECT_TRUE(s.humanize.jointLimits);
    EXPECT_EQ(s.humanize.seed, 1u);
    ASSERT_EQ(parsed.data.poses.size(), 2u);
    const PoseKey& a = parsed.data.poses[0];
    EXPECT_EQ(a.name, "A");
    EXPECT_TRUE(a.customTransition);
    EXPECT_EQ(a.transition.curve, TransitionCurve::Linear);
    EXPECT_FLOAT_EQ(a.transition.duration, 0.12f);
    const PoseKey& b = parsed.data.poses[1];
    EXPECT_EQ(b.name, "");
    EXPECT_FLOAT_EQ(b.hold, 0.1f);
    EXPECT_TRUE(b.grounded);
    EXPECT_FALSE(b.customTransition);
}

TEST(PoseIO, TransitionNullVersusObject){
    Parsed parsed = parse(withHeader(
        "\"poses\": [{\"transition\": null}, {\"transition\": {}}, "
        "{\"transition\": {\"curve\": \"smooth\", \"duration\": 0.5, \"overshoot\": 0.1, \"anticipation\": 0.2}}, {}]"));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    ASSERT_EQ(parsed.data.poses.size(), 4u);
    EXPECT_FALSE(parsed.data.poses[0].customTransition);
    EXPECT_TRUE(parsed.data.poses[1].customTransition);
    EXPECT_TRUE(parsed.data.poses[2].customTransition);
    EXPECT_FALSE(parsed.data.poses[3].customTransition);
    expectTransitionEq(parsed.data.poses[2].transition, {TransitionCurve::Smooth, 0.5f, 0.1f, 0.2f});

    // the write keeps the difference: null stays null, an object stays an object
    const std::string text = writePoseAnimation(parsed.data);
    Parsed back = parse(text);
    ASSERT_TRUE(back.ok);
    EXPECT_FALSE(back.data.poses[0].customTransition);
    EXPECT_TRUE(back.data.poses[1].customTransition);
    EXPECT_EQ(writePoseAnimation(back.data), text);

    // transitionOut follows customTransition
    parsed.data.settings.transition.duration = 0.9f;
    EXPECT_FLOAT_EQ(transitionOut(parsed.data, 0).duration, 0.9f);
    EXPECT_FLOAT_EQ(transitionOut(parsed.data, 2).duration, 0.5f);

    parsed = parse(withHeader("\"poses\": [{\"transition\": 5}]"));
    ASSERT_TRUE(parsed.ok);
    EXPECT_FALSE(parsed.data.poses[0].customTransition);
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "poses[0].transition"));
}

// ---- failures ----

TEST(PoseIO, BadFormatFails){
    EXPECT_FALSE(parse("{\"format\": \"other\", \"version\": 1}").ok);
    EXPECT_FALSE(parse("{\"version\": 1}").ok);
    EXPECT_FALSE(parse("{\"format\": 5, \"version\": 1}").ok);
    Parsed parsed = parse("{\"format\": \"jasperengine.humanoid\", \"version\": 1}");
    EXPECT_FALSE(parsed.ok);
    EXPECT_NE(parsed.error.find("format"), std::string::npos) << parsed.error;
}

TEST(PoseIO, NewerVersionFails){
    Parsed parsed = parse("{\"format\": \"jasperengine.poseanim\", \"version\": 2}");
    EXPECT_FALSE(parsed.ok);
    EXPECT_NE(parsed.error.find("version"), std::string::npos) << parsed.error;
    EXPECT_FALSE(parse("{\"format\": \"jasperengine.poseanim\", \"version\": 99, \"poses\": []}").ok);
    EXPECT_FALSE(parse("{\"format\": \"jasperengine.poseanim\", \"version\": 0}").ok);
    EXPECT_FALSE(parse("{\"format\": \"jasperengine.poseanim\", \"version\": \"1\"}").ok);
    EXPECT_TRUE(parse("{\"format\": \"jasperengine.poseanim\", \"version\": 1}").ok);
}

TEST(PoseIO, NotJsonFails){
    EXPECT_FALSE(parse("").ok);
    EXPECT_FALSE(parse("not json").ok);
    EXPECT_FALSE(parse("[1, 2]").ok);
    EXPECT_FALSE(parse("{\"format\": \"jasperengine.poseanim\", \"version\": 1").ok);   // truncated
    std::string error;
    PoseAnimationData data;
    std::vector<std::string> warnings;
    EXPECT_FALSE(parsePoseAnimation("nope", data, warnings, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(parsePoseAnimation("nope", data, warnings, nullptr));   // the error is optional
}

// ---- warnings, ignored content ----

TEST(PoseIO, UnknownDofAndKeysWarn){
    Parsed parsed = parse(withHeader(
        "\"mystery\": 1, \"feel\": {\"wobble\": 2}, "
        "\"poses\": [{\"name\": \"A\", \"colour\": \"red\", "
        "\"angles\": {\"left_upper_arm.raise\": 10, \"left_upper_arm.wobble\": 5, \"tail.bend\": 1}}]"));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "unknown key 'mystery'"));
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "unknown key 'wobble' in feel"));
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "unknown key 'colour' in poses[0]"));
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "unknown DOF 'left_upper_arm.wobble'"));
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "unknown DOF 'tail.bend'"));
    EXPECT_EQ(parsed.warnings.size(), 5u);
    // the known data is kept
    EXPECT_FLOAT_EQ(parsed.data.poses[0].body.angles[dof("left_upper_arm.raise")], 10.0f);
    int nonZero = 0;
    for (float a : parsed.data.poses[0].body.angles){
        nonZero += a != 0.0f;
    }
    EXPECT_EQ(nonZero, 1);
}

TEST(PoseIO, WronglyTypedValuesWarnAndKeepTheDefault){
    Parsed parsed = parse(withHeader(
        "\"loop\": \"yes\", \"frame_rate\": true, \"preview_model\": 5, \"feel\": 3, "
        "\"humanize\": {\"seed\": \"x\", \"enabled\": 1}, "
        "\"poses\": [{\"hold\": \"long\", \"grounded\": 0, \"name\": 4, \"angles\": {\"head.bend\": \"a\"}, "
        "\"offset\": []}, 7, \"x\"]"));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_GE(parsed.warnings.size(), 10u);
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "loop"));
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "frame_rate"));
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "feel"));
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "poses[0].hold"));
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "poses[0].angles.head.bend"));
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "poses[1]"));
    EXPECT_TRUE(parsed.data.settings.loop);
    EXPECT_EQ(parsed.data.settings.frameRate, 30);
    ASSERT_EQ(parsed.data.poses.size(), 3u);   // a malformed entry keeps its slot as a default pose
    EXPECT_FLOAT_EQ(parsed.data.poses[0].hold, 0.1f);
    EXPECT_TRUE(parsed.data.poses[0].grounded);

    parsed = parse(withHeader("\"feel\": {\"transition\": {\"curve\": \"bouncy\"}}"));
    ASSERT_TRUE(parsed.ok);
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "feel.transition.curve"));
    EXPECT_EQ(parsed.data.settings.transition.curve, TransitionCurve::Spring);
}

TEST(PoseIO, MissingVersionIsAWarning){
    Parsed parsed = parse("{\"format\": \"jasperengine.poseanim\"}");
    ASSERT_TRUE(parsed.ok);
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "version"));
}

// ---- clamping ----

TEST(PoseIO, OutOfRangeValuesAreClampedWithWarnings){
    Parsed parsed = parse(withHeader(
        "\"frame_rate\": 500, "
        "\"feel\": {\"transition\": {\"duration\": 0, \"overshoot\": 3, \"anticipation\": -1}, "
        "\"step_rate\": 99, \"overlap\": 1, \"looseness\": -2, \"weights\": {\"spine\": 5, \"legs\": -1}}, "
        "\"humanize\": {\"moving_holds\": 1, \"inbetweens\": 2, \"life\": 50, \"seed\": 4000000000, "
        "\"head_follow\": 9, \"torso_follow\": -9, \"springiness\": 7}, "
        "\"poses\": [{\"hold\": 9, \"lift\": 7, \"transition\": {\"duration\": 5}, "
        "\"offset\": {\"left\": -10, \"forward\": 10}}]"));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    const PoseAnimationSettings& s = parsed.data.settings;
    EXPECT_EQ(s.frameRate, 120);
    EXPECT_FLOAT_EQ(s.transition.duration, 0.02f);
    EXPECT_FLOAT_EQ(s.transition.overshoot, 0.9f);
    EXPECT_FLOAT_EQ(s.transition.anticipation, 0.0f);
    EXPECT_FLOAT_EQ(s.stepRate, 30.0f);
    EXPECT_FLOAT_EQ(s.overlap, 0.3f);
    EXPECT_FLOAT_EQ(s.looseness, 0.0f);
    EXPECT_FLOAT_EQ(s.weights[0], 2.0f);
    EXPECT_FLOAT_EQ(s.weights[3], 0.0f);
    EXPECT_FLOAT_EQ(s.humanize.movingHolds, 0.5f);
    EXPECT_FLOAT_EQ(s.humanize.inbetweens, 1.0f);
    EXPECT_FLOAT_EQ(s.humanize.life, 5.0f);
    EXPECT_EQ(s.humanize.seed, 2147483647u);
    EXPECT_FLOAT_EQ(s.humanize.headFollow, 1.5f);
    EXPECT_FLOAT_EQ(s.humanize.torsoFollow, 0.0f);
    EXPECT_FLOAT_EQ(s.humanize.springiness, 1.0f);
    const PoseKey& key = parsed.data.poses[0];
    EXPECT_FLOAT_EQ(key.hold, 5.0f);
    EXPECT_FLOAT_EQ(key.lift, 3.0f);
    EXPECT_FLOAT_EQ(key.transition.duration, 1.0f);
    EXPECT_FLOAT_EQ(key.body.offset.x, -5.0f);
    EXPECT_FLOAT_EQ(key.body.offset.z, 5.0f);

    for (const char* field : {"settings.frameRate", "settings.transition.duration", "settings.transition.overshoot",
                              "settings.transition.anticipation", "settings.stepRate", "settings.overlap",
                              "settings.looseness", "settings.weights.spine", "settings.weights.legs",
                              "settings.humanize.movingHolds", "settings.humanize.inbetweens",
                              "settings.humanize.life", "settings.humanize.seed", "settings.humanize.headFollow",
                              "settings.humanize.torsoFollow", "settings.humanize.springiness", "poses[0].hold",
                              "poses[0].lift", "poses[0].transition.duration", "poses[0].offset.x",
                              "poses[0].offset.z"}){
        EXPECT_TRUE(anyWarningContains(parsed.warnings, field)) << field;
    }

    // the clamped data is stable: writing and parsing again warns about nothing
    Parsed again = parse(writePoseAnimation(parsed.data));
    ASSERT_TRUE(again.ok);
    EXPECT_TRUE(again.warnings.empty());
    expectDataEq(parsed.data, again.data);
}

TEST(PoseIO, InRangeBoundariesAreUntouched){
    Parsed parsed = parse(withHeader(
        "\"frame_rate\": 12, \"feel\": {\"transition\": {\"duration\": 1.0, \"overshoot\": 0.9, \"anticipation\": 0.5}, "
        "\"step_rate\": 30, \"overlap\": 0.3, \"looseness\": 1, \"weights\": {\"spine\": 2, \"head\": 0}}, "
        "\"humanize\": {\"seed\": 2147483647, \"life\": 5}, "
        "\"poses\": [{\"hold\": 5, \"lift\": 3, \"offset\": {\"left\": 5, \"forward\": -5}, "
        "\"angles\": {\"hips.turn\": 180, \"head.bend\": -179.99}}]"));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_TRUE(parsed.warnings.empty()) << parsed.warnings[0];
}

TEST(PoseIO, FrameRateIsRoundedToAWholeNumber){
    Parsed parsed = parse(withHeader("\"frame_rate\": 30.4"));
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.data.settings.frameRate, 30);
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "frame_rate"));
    parsed = parse(withHeader("\"frame_rate\": 1e12"));
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.data.settings.frameRate, 120);
    parsed = parse(withHeader("\"frame_rate\": -5"));
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.data.settings.frameRate, 12);
    parsed = parse(withHeader("\"humanize\": {\"seed\": -3}"));
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.data.settings.humanize.seed, 0u);
    EXPECT_FALSE(parsed.warnings.empty());
}

TEST(PoseIO, AnglesWrapToMinus180Plus180){
    Parsed parsed = parse(withHeader(
        "\"poses\": [{\"angles\": {\"head.bend\": 190, \"head.side\": -190, \"head.twist\": 540, "
        "\"neck.bend\": -180, \"neck.side\": 180, \"neck.twist\": 360, \"spine.bend\": -720.5, "
        "\"spine.side\": 179.5}}]"));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    const auto& a = parsed.data.poses[0].body.angles;
    EXPECT_FLOAT_EQ(a[dof("head.bend")], -170.0f);
    EXPECT_FLOAT_EQ(a[dof("head.side")], 170.0f);
    EXPECT_FLOAT_EQ(a[dof("head.twist")], 180.0f);
    EXPECT_FLOAT_EQ(a[dof("neck.bend")], 180.0f);      // (-180, 180]: -180 is 180
    EXPECT_FLOAT_EQ(a[dof("neck.side")], 180.0f);
    EXPECT_FLOAT_EQ(a[dof("neck.twist")], 0.0f);
    EXPECT_FLOAT_EQ(a[dof("spine.bend")], -0.5f);
    EXPECT_FLOAT_EQ(a[dof("spine.side")], 179.5f);
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "poses[0].angles.head.bend: 190 -> -170"));
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "poses[0].angles.neck.bend"));
    EXPECT_FALSE(anyWarningContains(parsed.warnings, "poses[0].angles.neck.side"));
    EXPECT_FALSE(anyWarningContains(parsed.warnings, "poses[0].angles.spine.side"));
}

TEST(PoseIO, ClampDataReportsFieldRequestedAndApplied){
    PoseAnimationData data;
    data.settings.transition.overshoot = 2.0f;
    data.settings.humanize.seed = 3000000000u;
    data.settings.frameRate = 8;
    for (int i = 0; i < 3; i++){
        data.poses.emplace_back();
    }
    data.poses[2].hold = -1.0f;
    data.poses[2].body.angles[dof("left_upper_arm.raise")] = 200.0f;
    data.poses[2].body.offset.y = 0.5f;
    data.poses[1].transition.duration = 50.0f;   // not custom: the key's own transition is unused, not reported

    std::vector<ClampReport> reports;
    clampData(data, reports);
    auto find = [&](const std::string& field) -> const ClampReport* {
        for (const ClampReport& r : reports){
            if (r.field == field){
                return &r;
            }
        }
        return nullptr;
    };
    ASSERT_NE(find("settings.transition.overshoot"), nullptr);
    EXPECT_FLOAT_EQ(find("settings.transition.overshoot")->requested, 2.0f);
    EXPECT_FLOAT_EQ(find("settings.transition.overshoot")->applied, 0.9f);
    ASSERT_NE(find("settings.frameRate"), nullptr);
    EXPECT_FLOAT_EQ(find("settings.frameRate")->applied, 12.0f);
    ASSERT_NE(find("settings.humanize.seed"), nullptr);
    ASSERT_NE(find("poses[2].hold"), nullptr);
    EXPECT_FLOAT_EQ(find("poses[2].hold")->requested, -1.0f);
    EXPECT_FLOAT_EQ(find("poses[2].hold")->applied, 0.0f);
    ASSERT_NE(find("poses[2].angles.left_upper_arm.raise"), nullptr);
    EXPECT_FLOAT_EQ(find("poses[2].angles.left_upper_arm.raise")->requested, 200.0f);
    EXPECT_FLOAT_EQ(find("poses[2].angles.left_upper_arm.raise")->applied, -160.0f);
    ASSERT_NE(find("poses[2].offset.y"), nullptr);
    EXPECT_EQ(find("poses[1].transition.duration"), nullptr);
    EXPECT_EQ(reports.size(), 6u);

    // idempotent: clamped data reports nothing
    std::vector<ClampReport> second;
    clampData(data, second);
    EXPECT_TRUE(second.empty());
}

TEST(PoseIO, ClampFunctionsIndividually){
    Transition transition;
    transition.duration = 0.001f;
    transition.anticipation = 0.7f;
    std::vector<ClampReport> reports;
    clampTransition(transition, "poses[4].transition", reports);
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_EQ(reports[0].field, "poses[4].transition.duration");
    EXPECT_EQ(reports[1].field, "poses[4].transition.anticipation");

    PoseAnimationSettings settings;
    settings.weights[2] = 3.0f;
    settings.looseness = 1.5f;
    reports.clear();
    clampSettings(settings, reports);
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_EQ(reports[0].field, "settings.looseness");
    EXPECT_EQ(reports[1].field, "settings.weights.arms");

    PoseKey key;
    key.lift = 4.0f;
    key.customTransition = true;
    key.transition.overshoot = 1.0f;
    reports.clear();
    clampKey(key, 7, reports);
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_EQ(reports[0].field, "poses[7].transition.overshoot");
    EXPECT_EQ(reports[1].field, "poses[7].lift");

    EXPECT_EQ(describeClamp({"poses[2].hold", 9.0f, 5.0f}), "poses[2].hold: 9 -> 5");
}

TEST(PoseIO, NonFiniteValuesAreReplaced){
    PoseKey key;
    key.hold = std::numeric_limits<float>::quiet_NaN();
    key.lift = std::numeric_limits<float>::infinity();
    key.body.angles[0] = std::numeric_limits<float>::quiet_NaN();
    key.body.angles[1] = std::numeric_limits<float>::infinity();
    std::vector<ClampReport> reports;
    clampKey(key, 0, reports);
    EXPECT_FLOAT_EQ(key.hold, 0.0f);
    EXPECT_FLOAT_EQ(key.lift, 3.0f);
    EXPECT_FLOAT_EQ(key.body.angles[0], 0.0f);
    EXPECT_FLOAT_EQ(key.body.angles[1], 0.0f);
    EXPECT_EQ(reports.size(), 4u);
}

TEST(PoseIO, WrapAngleHelper){
    EXPECT_FLOAT_EQ(wrapAngle(0), 0);
    EXPECT_FLOAT_EQ(wrapAngle(180), 180);
    EXPECT_FLOAT_EQ(wrapAngle(-180), 180);
    EXPECT_FLOAT_EQ(wrapAngle(181), -179);
    EXPECT_FLOAT_EQ(wrapAngle(-181), 179);
    EXPECT_FLOAT_EQ(wrapAngle(360), 0);
    EXPECT_FLOAT_EQ(wrapAngle(-360), 0);
    EXPECT_FLOAT_EQ(wrapAngle(1000), -80);
}

TEST(PoseIO, CurveNames){
    for (TransitionCurve curve : {TransitionCurve::Spring, TransitionCurve::Linear, TransitionCurve::Smooth}){
        TransitionCurve back = TransitionCurve::Smooth;
        if (curve == TransitionCurve::Smooth){
            back = TransitionCurve::Spring;
        }
        EXPECT_TRUE(curveFromName(curveName(curve), back));
        EXPECT_EQ(back, curve);
    }
    EXPECT_STREQ(curveName(TransitionCurve::Spring), "spring");
    EXPECT_STREQ(curveName(TransitionCurve::Linear), "linear");
    EXPECT_STREQ(curveName(TransitionCurve::Smooth), "smooth");
    TransitionCurve untouched = TransitionCurve::Linear;
    EXPECT_FALSE(curveFromName("Spring", untouched));
    EXPECT_FALSE(curveFromName("", untouched));
    EXPECT_EQ(untouched, TransitionCurve::Linear);
}

// ---- templates ----

TEST(PoseIO, TemplateInfoIsPreserved){
    PoseAnimationTemplateInfo info{"Walk", "A relaxed walk cycle, 4 poses.", "Locomotion \"loops\""};
    const PoseAnimationData data = fullData();
    const std::string text = writePoseAnimation(data, &info);

    // right after `version`, before preview_model
    const size_t version = text.find("\"version\"");
    const size_t tmpl = text.find("\"template\"");
    const size_t preview = text.find("\"preview_model\"");
    ASSERT_NE(tmpl, std::string::npos);
    EXPECT_GT(tmpl, version);
    EXPECT_LT(tmpl, preview);

    Parsed back = parse(text);
    ASSERT_TRUE(back.ok) << back.error;
    EXPECT_TRUE(back.warnings.empty());
    ASSERT_TRUE(back.info.has_value());
    EXPECT_EQ(back.info->name, info.name);
    EXPECT_EQ(back.info->description, info.description);
    EXPECT_EQ(back.info->category, info.category);
    expectDataEq(data, back.data);
    EXPECT_EQ(writePoseAnimation(back.data, &*back.info), text);
}

TEST(PoseIO, NoTemplateKeyWithoutTemplateInfo){
    EXPECT_EQ(writePoseAnimation(fullData()).find("template"), std::string::npos);
    Parsed parsed = parse(withHeader("\"template\": {\"name\": \"X\", \"extra\": 1}"));
    ASSERT_TRUE(parsed.ok);
    ASSERT_TRUE(parsed.info.has_value());
    EXPECT_EQ(parsed.info->name, "X");
    EXPECT_EQ(parsed.info->category, "");
    EXPECT_TRUE(anyWarningContains(parsed.warnings, "unknown key 'extra' in template"));

    // the template out-parameter is optional, and a template key does not warn
    PoseAnimationData data;
    std::vector<std::string> warnings;
    ASSERT_TRUE(parsePoseAnimation(withHeader("\"template\": {\"name\": \"X\"}"), data, warnings, nullptr));
    EXPECT_TRUE(warnings.empty());
}

// ---- files ----

TEST(PoseIO, SaveAndLoadFile){
    jasperengine::test::ensurePoseTestSystem();
    const std::filesystem::path dir = scratchDir();
    const std::string path = (dir / "walk.poseanim").string();
    const PoseAnimationData data = fullData();
    PoseAnimationTemplateInfo info{"T", "D", "C"};

    std::string error;
    ASSERT_TRUE(savePoseAnimationFile(path, data, &error, &info)) << error;
    EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));

    std::ifstream file(path, std::ios::binary);
    std::stringstream onDisk;
    onDisk << file.rdbuf();
    EXPECT_EQ(onDisk.str(), writePoseAnimation(data, &info));

    PoseAnimationData loaded;
    std::vector<std::string> warnings;
    std::optional<PoseAnimationTemplateInfo> loadedInfo;
    ASSERT_TRUE(loadPoseAnimationFile(path, loaded, warnings, &error, &loadedInfo)) << error;
    EXPECT_TRUE(warnings.empty());
    expectDataEq(data, loaded);
    ASSERT_TRUE(loadedInfo.has_value());
    EXPECT_EQ(loadedInfo->name, "T");

    // overwriting replaces the content
    PoseAnimationData other;
    other.previewModel = "other.gltf";
    ASSERT_TRUE(savePoseAnimationFile(path, other, &error)) << error;
    ASSERT_TRUE(loadPoseAnimationFile(path, loaded, warnings, &error)) << error;
    EXPECT_EQ(loaded.previewModel, "other.gltf");
    EXPECT_TRUE(loaded.poses.empty());

    std::filesystem::remove_all(dir);
}

TEST(PoseIO, LoadFileErrors){
    jasperengine::test::ensurePoseTestSystem();
    const std::filesystem::path dir = scratchDir();
    PoseAnimationData data;
    std::vector<std::string> warnings;
    std::string error;
    EXPECT_FALSE(loadPoseAnimationFile((dir / "missing.poseanim").string(), data, warnings, &error));
    EXPECT_NE(error.find("missing.poseanim"), std::string::npos) << error;

    const std::string bad = (dir / "bad.poseanim").string();
    {
        std::ofstream file(bad);
        file << "{\"format\": \"jasperengine.poseanim\", \"version\": 7}";
    }
    EXPECT_FALSE(loadPoseAnimationFile(bad, data, warnings, &error));
    EXPECT_NE(error.find("bad.poseanim"), std::string::npos) << error;
    EXPECT_NE(error.find("version"), std::string::npos) << error;

    // an unwritable target reports an error and leaves no temporary file
    error.clear();
    EXPECT_FALSE(savePoseAnimationFile((dir / "no-such-dir" / "x.poseanim").string(), data, &error));
    EXPECT_FALSE(error.empty());
    std::filesystem::remove_all(dir);
}
