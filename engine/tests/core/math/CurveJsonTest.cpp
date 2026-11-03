// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The one curve and gradient encoding (docs/specs/S27-editor-tools.md R45, D4, D5): S20's form
// read and written unchanged, the mode tokens and wraps added backward-compatibly.

#include "math/CurveJson.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;
using curvejson::Json;

TEST(CurveJson, PlainCurvesKeepTheParticleForm) {
    const Curve curve({{0.0f, 0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, INFINITY}});
    const Json j = curvejson::curveToJson(curve);
    EXPECT_EQ(j.dump(), "[[0.0,0.0,1.0,1.0],[1.0,1.0,1.0,null]]");
    Curve read;
    std::vector<curvejson::Issue> issues;
    ASSERT_TRUE(curvejson::curveFromJson(j, read, issues));
    EXPECT_EQ(read.keys.size(), 2u);
    EXPECT_TRUE(std::isinf(read.keys[1].outTangent));
    EXPECT_EQ(read.keys[0], curve.keys[0]);
    // two-element keys (flat tangents) still read
    ASSERT_TRUE(curvejson::curveFromJson(Json::parse("[[0, 1], [0.5, 2]]"), read, issues));
    EXPECT_EQ(read.keys[1].value, 2.0f);
    EXPECT_EQ(read.keys[1].inTangent, 0.0f);
}

TEST(CurveJson, ModesAndWrapsRoundTrip) {
    Curve curve = Curve::linear(0.0f, 0.0f, 2.0f, 4.0f);
    curve.keys[0].setBothModes(CurveTangentMode::ClampedAuto);
    curve.keys[1].setInMode(CurveTangentMode::Linear);
    curve.keys[1].setOutMode(CurveTangentMode::Constant);
    curve.keys[1].setBroken(true);
    curve.preWrapMode = CurveWrapMode::PingPong;
    curve.postWrapMode = CurveWrapMode::LoopWithOffset;
    const Json j = curvejson::curveToJson(curve);
    ASSERT_TRUE(j.is_object());
    EXPECT_EQ(j["preWrapMode"], "pingPong");
    EXPECT_EQ(j["postWrapMode"], "loopWithOffset");
    EXPECT_EQ(j["keys"][0][4], "clampedAuto");
    EXPECT_EQ(j["keys"][1][4], "in:linear,out:constant,broken");
    Curve read;
    std::vector<curvejson::Issue> issues;
    ASSERT_TRUE(curvejson::curveFromJson(j, read, issues));
    EXPECT_EQ(read, curve);
    // the same text again: byte-stable
    EXPECT_EQ(curvejson::curveToJson(read).dump(), j.dump());
    // only the wrap that is not clamp is written
    Curve onlyPost = Curve::linear(0.0f, 1.0f);
    onlyPost.postWrapMode = CurveWrapMode::Loop;
    const Json post = curvejson::curveToJson(onlyPost);
    EXPECT_FALSE(post.contains("preWrapMode"));
}

TEST(CurveJson, ModeTokens) {
    uint8_t mode = 0;
    EXPECT_EQ(curvejson::modeToken(0), "");
    for (const char* token : {"free", "auto", "linear", "constant", "clampedAuto"}) {
        ASSERT_TRUE(curvejson::modeFromToken(token, mode)) << token;
        if (std::string(token) == "free") {
            EXPECT_EQ(mode, 0u);
        } else {
            EXPECT_EQ(curvejson::modeToken(mode), token);
        }
    }
    ASSERT_TRUE(curvejson::modeFromToken("out:auto,in:linear", mode));
    CurveKey key;
    key.mode = mode;
    EXPECT_EQ(key.inMode(), CurveTangentMode::Linear);
    EXPECT_EQ(key.outMode(), CurveTangentMode::Auto);
    EXPECT_FALSE(key.broken());
    EXPECT_FALSE(curvejson::modeFromToken("weighted", mode));
    EXPECT_FALSE(curvejson::modeFromToken("in:free,in:auto", mode));
    EXPECT_FALSE(curvejson::modeFromToken("", mode));
}

TEST(CurveJson, ErrorsSayWhere) {
    Curve read;
    std::vector<curvejson::Issue> issues;
    EXPECT_FALSE(curvejson::curveFromJson(Json::parse("[[0, 1], [0.5]]"), read, issues));
    ASSERT_FALSE(issues.empty());
    EXPECT_EQ(issues.back().path, "[1]");
    issues.clear();
    EXPECT_FALSE(curvejson::curveFromJson(Json::parse("{\"keys\": [[0, 0, 0, 0, \"bogus\"]]}"), read, issues));
    ASSERT_FALSE(issues.empty());
    EXPECT_EQ(issues.back().path, ".keys[0][4]");
    issues.clear();
    EXPECT_FALSE(curvejson::curveFromJson(Json::parse("{\"keys\": [], \"preWrapMode\": \"spin\"}"), read, issues));
    issues.clear();
    EXPECT_FALSE(curvejson::curveFromJson(Json::parse("{\"preWrapMode\": \"loop\"}"), read, issues));
    issues.clear();
    // an unknown key is a warning, not an error
    EXPECT_TRUE(curvejson::curveFromJson(Json::parse("{\"keys\": [[0, 1]], \"color\": 3}"), read, issues));
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_FALSE(issues[0].error);
}

TEST(CurveJson, GradientsKeepTheParticleForm) {
    Gradient g = Gradient::twoColors(Vector3(1, 0, 0), Vector3(0, 0, 1), 1.0f, 0.5f);
    g.mode = GradientMode::Fixed;
    g.linear = true;
    const Json j = curvejson::gradientToJson(g);
    EXPECT_EQ(j.dump(), "{\"colorKeys\":[[0.0,1.0,0.0,0.0],[1.0,0.0,0.0,1.0]],\"alphaKeys\":[[0.0,1.0],[1.0,0.5]],\"mode\":\"fixed\",\"linear\":true}");
    Gradient read;
    std::vector<curvejson::Issue> issues;
    ASSERT_TRUE(curvejson::gradientFromJson(j, read, issues));
    EXPECT_EQ(read, g);
    EXPECT_FALSE(curvejson::gradientFromJson(Json::parse("{\"mode\": \"smooth\"}"), read, issues));
}

TEST(CurveJson, ShortestFloats) {
    EXPECT_EQ(curvejson::jsonFloat(0.1f), 0.1);
    EXPECT_EQ((float)curvejson::jsonFloat(1.0f / 3.0f), 1.0f / 3.0f);
    EXPECT_EQ(curvejson::jsonFloat(INFINITY), 0.0);
}
