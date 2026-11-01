// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor's curve encodings (docs/specs/S27-editor-tools.md R45, AC6): scene YAML byte-stable
// and in the particle JSON's structure, the default curve omitted, S20's old `.inf` steps still
// read, a scene saved before the ease curve loads a linear one, and the exported C++ literal
// rebuilds the same curve.

#include "stream/CurveCodec.h"

#include "Catalog.h"
#include "component/TweenComponent.h"
#include "reflect/Reflect.h"

#include "LogCapture.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::test;

namespace {

    Curve sample() {
        Curve curve({{0.0f, 0.0f, 0.0f, 2.5f}, {0.4f, 1.25f, 0.1f, INFINITY}, {1.0f, 1.0f, 0.0f, 0.0f}});
        curve.keys[0].setBothModes(CurveTangentMode::ClampedAuto);
        curve.keys[2].setInMode(CurveTangentMode::Linear);
        curve.postWrapMode = CurveWrapMode::PingPong;
        return curve;
    }

}

TEST(CurveCodec, YamlIsByteStable) {
    const Curve curve = sample();
    const std::string first = YAML::Dump(curvecodec::curveToYaml(curve));
    Curve read;
    std::string reason;
    ASSERT_TRUE(curvecodec::curveFromYaml(YAML::Load(first), read, reason)) << reason;
    EXPECT_EQ(read, curve);
    EXPECT_EQ(YAML::Dump(curvecodec::curveToYaml(read)), first);
    // the structure of the JSON form
    const YAML::Node node = YAML::Load(first);
    ASSERT_TRUE(node.IsMap());
    EXPECT_EQ(node["postWrapMode"].as<std::string>(), "pingPong");
    EXPECT_FALSE(node["preWrapMode"].IsDefined());
    ASSERT_EQ(node["keys"].size(), 3u);
    EXPECT_EQ(node["keys"][0][4].as<std::string>(), "clampedAuto");
    EXPECT_TRUE(node["keys"][1][3].IsNull());   // the step
    // the YAML and the JSON forms hold the same thing
    Curve fromJson;
    ASSERT_TRUE(curvecodec::curveFromJson(curvecodec::curveToJson(curve), fromJson, reason)) << reason;
    EXPECT_EQ(fromJson, curve);
}

TEST(CurveCodec, OldInfinityStepsRead) {
    // S20's particle scenes wrote the step as a YAML float infinity
    const YAML::Node node = YAML::Load("[[0, 0, 0, .inf], [1, 1, -.inf, 0]]");
    Curve read;
    std::string reason;
    ASSERT_TRUE(curvecodec::curveFromYaml(node, read, reason)) << reason;
    ASSERT_EQ(read.keys.size(), 2u);
    EXPECT_TRUE(std::isinf(read.keys[0].outTangent));
    EXPECT_TRUE(std::isinf(read.keys[1].inTangent));
    EXPECT_EQ(read.evaluate(0.5f), 0.0f);   // held until the next key
}

TEST(CurveCodec, ErrorsNameTheKey) {
    Curve read;
    std::string reason;
    EXPECT_FALSE(curvecodec::curveFromYaml(YAML::Load("[[0, 0], [1]]"), read, reason));
    EXPECT_NE(reason.find("[1]"), std::string::npos) << reason;
    EXPECT_FALSE(curvecodec::curveFromYaml(YAML::Load("seven"), read, reason));
    Gradient gradient;
    EXPECT_FALSE(curvecodec::gradientFromYaml(YAML::Load("{colorKeys: [[0, 1, 0]]}"), gradient, reason));
}

TEST(CurveCodec, GradientsRoundTrip) {
    Gradient g = Gradient::twoColors(Vector3(1.0f, 0.5f, 0.0f), Vector3(0.0f, 0.25f, 1.0f), 1.0f, 0.0f);
    g.mode = GradientMode::Fixed;
    const std::string first = YAML::Dump(curvecodec::gradientToYaml(g));
    Gradient read;
    std::string reason;
    ASSERT_TRUE(curvecodec::gradientFromYaml(YAML::Load(first), read, reason)) << reason;
    EXPECT_EQ(read, g);
    EXPECT_EQ(YAML::Dump(curvecodec::gradientToYaml(read)), first);
}

TEST(CurveCodec, TweenCurveDefaultIsOmitted) {
    TweenComponent tween{};
    YAML::Node node = reflect::encodeValue(ComponentType::TweenComponent, &tween);
    EXPECT_FALSE(node["curve"].IsDefined()) << YAML::Dump(node);
    tween.function = EaseType::CURVE;
    tween.curve = Curve::fromEase(EaseType::BACK_OUT);
    node = reflect::encodeValue(ComponentType::TweenComponent, &tween);
    ASSERT_TRUE(node["curve"].IsDefined());
    const TweenComponent decoded = reflect::decodeValue<TweenComponent>(ComponentType::TweenComponent, YAML::Load(YAML::Dump(node)));
    EXPECT_EQ(decoded.function.getType(), EaseType::CURVE);
    EXPECT_EQ(decoded.curve, tween.curve);
    EXPECT_EQ(YAML::Dump(reflect::encodeValue(ComponentType::TweenComponent, &decoded)), YAML::Dump(node));
    EXPECT_NO_ENGINE_ERRORS();
}

TEST(CurveCodec, OldTweenSceneLoadsALinearCurve) {
    // a TweenComponent as scenes wrote it before the ease curve
    TweenComponent before{};
    before.duration = 2.0f;
    before.loop = true;
    YAML::Node node = reflect::encodeValue(ComponentType::TweenComponent, &before);
    node.remove("curve");
    const TweenComponent decoded = reflect::decodeValue<TweenComponent>(ComponentType::TweenComponent, YAML::Load(YAML::Dump(node)));
    EXPECT_EQ(decoded.curve, Curve::linear(0.0f, 0.0f, 1.0f, 1.0f));
    EXPECT_FLOAT_EQ(decoded.curve.evaluate(0.3f), 0.3f);
    EXPECT_FLOAT_EQ(decoded.duration, 2.0f);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST(CurveCodec, CppLiteral) {
    const Curve plain = Curve::linear(0.0f, 0.0f, 1.0f, 1.0f);
    EXPECT_EQ(curvecodec::curveCppLiteral(plain), "Curve({CurveKey{0.0f, 0.0f, 1.0f, 1.0f, 0}, CurveKey{1.0f, 1.0f, 1.0f, 1.0f, 0}})");
    const std::string wrapped = curvecodec::curveCppLiteral(sample());
    EXPECT_NE(wrapped.find("CurveWrapMode::PingPong"), std::string::npos) << wrapped;
    EXPECT_NE(wrapped.find("INFINITY"), std::string::npos) << wrapped;
}
