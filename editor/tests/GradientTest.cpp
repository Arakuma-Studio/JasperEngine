// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Gradient (lines and trails spec 6.1): blend and fixed modes, colour and alpha keys at
// different times, the game's gradients (the B lightning: two of its eight stored colour keys
// in use; the bullet trail's orange fading out), the 8-key cap, defaults without keys, and the
// YAML forms of curves and gradients (editor::Stream: "#RRGGBB" sRGB strings and linear
// arrays, a bare number as a constant curve, .inf tangents) in a round trip.

#include <gtest/gtest.h>

#include "math/Gradient.h"
#include "math/AnimationCurve.h"
#include "util/Color.h"
#include "Stream.h"

#include "yaml-cpp/yaml.h"

#include <cmath>
#include <limits>

using namespace jasperengine;

namespace {

    void expectColor(const Vector4& actual, const Vector4& expected, float tolerance = 1e-5f){
        EXPECT_NEAR(actual.x, expected.x, tolerance);
        EXPECT_NEAR(actual.y, expected.y, tolerance);
        EXPECT_NEAR(actual.z, expected.z, tolerance);
        EXPECT_NEAR(actual.w, expected.w, tolerance);
    }

}

TEST(GradientTest, DefaultsAndNoKeys){
    Gradient gradient;
    expectColor(gradient.evaluate(0.0f), Vector4(1, 1, 1, 1));
    expectColor(gradient.evaluate(0.7f), Vector4(1, 1, 1, 1));
    EXPECT_EQ(gradient.colorKeys.size(), 2u);
    EXPECT_EQ(gradient.alphaKeys.size(), 2u);

    Gradient empty({}, {});
    expectColor(empty.evaluate(0.3f), Vector4(1, 1, 1, 1));

    Gradient single({GradientColorKey(0.4f, Vector3(1, 0, 0))}, {GradientAlphaKey(0.9f, 0.5f)});
    expectColor(single.evaluate(0.0f), Vector4(1, 0, 0, 0.5f));
    expectColor(single.evaluate(1.0f), Vector4(1, 0, 0, 0.5f));
}

TEST(GradientTest, BlendBetweenKeysAtDifferentTimes){
    Gradient gradient({GradientColorKey(0.2f, Vector3(1, 0, 0)), GradientColorKey(0.6f, Vector3(0, 0, 1))},
                      {GradientAlphaKey(0.0f, 1.0f), GradientAlphaKey(1.0f, 0.0f)});
    // before the first colour key: the first colour; after the last: the last
    expectColor(gradient.evaluate(0.0f), Vector4(1, 0, 0, 1));
    expectColor(gradient.evaluate(0.8f), Vector4(0, 0, 1, 0.2f));
    expectColor(gradient.evaluate(0.4f), Vector4(0.5f, 0, 0.5f, 0.6f));
    // t is clamped
    expectColor(gradient.evaluate(-2.0f), Vector4(1, 0, 0, 1));
    expectColor(gradient.evaluate(3.0f), Vector4(0, 0, 1, 0));
}

TEST(GradientTest, FixedMode){
    Gradient gradient({GradientColorKey(0.25f, Vector3(1, 0, 0)), GradientColorKey(0.5f, Vector3(0, 1, 0)), GradientColorKey(1.0f, Vector3(0, 0, 1))},
                      {GradientAlphaKey(0.5f, 0.2f), GradientAlphaKey(1.0f, 0.8f)}, GradientMode::Fixed);
    // the first key at or after t
    expectColor(gradient.evaluate(0.1f), Vector4(1, 0, 0, 0.2f));
    expectColor(gradient.evaluate(0.25f), Vector4(1, 0, 0, 0.2f));
    expectColor(gradient.evaluate(0.3f), Vector4(0, 1, 0, 0.2f));
    expectColor(gradient.evaluate(0.5f), Vector4(0, 1, 0, 0.2f));
    expectColor(gradient.evaluate(0.51f), Vector4(0, 0, 1, 0.8f));
    expectColor(gradient.evaluate(1.0f), Vector4(0, 0, 1, 0.8f));
}

TEST(GradientTest, GameGradients){
    // Scenes/B.unity lightning: m_NumColorKeys 2 of the eight stored ones (the importer reads
    // the first two): white at both ends, opaque
    Gradient lightning({GradientColorKey(0.0f, Vector3(1, 1, 1)), GradientColorKey(1.0f, Vector3(1, 1, 1))},
                       {GradientAlphaKey(0.0f, 1.0f), GradientAlphaKey(1.0f, 1.0f)});
    expectColor(lightning.evaluate(0.35f), Vector4(1, 1, 1, 1));
    EXPECT_EQ(lightning, Gradient());

    // Prefabs/I - Invaders/Bullet.prefab trail: (1, 0.5886716, 0) sRGB, alpha 1 -> 0
    const Vector3 orange = Color::sRGBToLinear(Vector3(1.0f, 0.5886716f, 0.0f));
    Gradient bullet({GradientColorKey(0.0f, orange), GradientColorKey(1.0f, orange)},
                    {GradientAlphaKey(0.0f, 1.0f), GradientAlphaKey(1.0f, 0.0f)});
    expectColor(bullet.evaluate(0.25f), Vector4(orange.x, orange.y, orange.z, 0.75f));
}

TEST(GradientTest, NormalizeCapsSortsAndClamps){
    std::vector<GradientColorKey> colors;
    for (int i = 9; i >= 0; i--){
        colors.push_back(GradientColorKey(i / 9.0f, Vector3((float)i, 0, 0)));
    }
    colors.push_back(GradientColorKey(1.5f, Vector3(0, 1, 0)));
    colors.push_back(GradientColorKey(std::nanf(""), Vector3(0, 1, 0)));
    Gradient gradient(colors, {GradientAlphaKey(-1.0f, 0.5f), GradientAlphaKey(0.5f, 1.0f)});
    gradient.normalize();
    ASSERT_EQ(gradient.colorKeys.size(), Gradient::MAX_KEYS);
    for (size_t i = 1; i < gradient.colorKeys.size(); i++){
        EXPECT_LE(gradient.colorKeys[i - 1].time, gradient.colorKeys[i].time);
    }
    EXPECT_FLOAT_EQ(gradient.colorKeys.front().time, 0.0f);
    EXPECT_FLOAT_EQ(gradient.alphaKeys.front().time, 0.0f);
    EXPECT_FLOAT_EQ(gradient.alphaKeys.front().alpha, 0.5f);
}

TEST(GradientTest, ConstantAndTwoColors){
    Gradient constant = Gradient::constant(Vector4(0.2f, 0.4f, 0.6f, 0.5f));
    expectColor(constant.evaluate(0.9f), Vector4(0.2f, 0.4f, 0.6f, 0.5f));
    Gradient two = Gradient::twoColors(Vector4(1, 0, 0, 1), Vector4(0, 0, 1, 0));
    expectColor(two.evaluate(0.5f), Vector4(0.5f, 0, 0.5f, 0.5f));
}

TEST(GradientTest, YamlRoundTrip){
    const Vector3 orange = Color::sRGBToLinear(Vector3(1.0f, 150.0f / 255.0f, 0.0f));
    Gradient gradient({GradientColorKey(0.0f, orange), GradientColorKey(0.5f, Vector3(0.25f, 0.5f, 0.75f))},
                      {GradientAlphaKey(0.0f, 1.0f), GradientAlphaKey(1.0f, 0.0f)}, GradientMode::Fixed);
    YAML::Node node = editor::Stream::encodeGradient(gradient);
    YAML::Emitter out;
    out << node;
    Gradient decoded = editor::Stream::decodeGradient(YAML::Load(out.c_str()));
    ASSERT_EQ(decoded.colorKeys.size(), 2u);
    EXPECT_EQ(decoded.mode, GradientMode::Fixed);
    for (size_t i = 0; i < 2; i++){
        EXPECT_NEAR(decoded.colorKeys[i].color.x, gradient.colorKeys[i].color.x, 1e-5f);
        EXPECT_NEAR(decoded.colorKeys[i].color.y, gradient.colorKeys[i].color.y, 1e-5f);
        EXPECT_NEAR(decoded.colorKeys[i].color.z, gradient.colorKeys[i].color.z, 1e-5f);
    }
    EXPECT_EQ(decoded.alphaKeys, gradient.alphaKeys);

    // sRGB strings: "#FF9600" is (1, 150/255, 0) in sRGB
    Gradient parsed = editor::Stream::decodeGradient(YAML::Load("{mode: blend, colors: [[0, \"#FF9600\"], [1, [0.5, 0.25, 0]]], alphas: [[0, 1], [1, 0]]}"));
    ASSERT_EQ(parsed.colorKeys.size(), 2u);
    EXPECT_NEAR(parsed.colorKeys[0].color.x, orange.x, 1e-5f);
    EXPECT_NEAR(parsed.colorKeys[0].color.y, orange.y, 1e-5f);
    EXPECT_NEAR(parsed.colorKeys[1].color.x, 0.5f, 1e-6f);
    EXPECT_FLOAT_EQ(parsed.alphaKeys[1].alpha, 0.0f);
    // a single colour is a constant gradient
    Gradient single = editor::Stream::decodeGradient(YAML::Load("\"#FFFFFF80\""));
    EXPECT_NEAR(single.evaluate(0.5f).w, 128.0f / 255.0f, 1e-5f);
}

TEST(GradientTest, CurveYamlRoundTrip){
    const float INF = std::numeric_limits<float>::infinity();
    AnimationCurve curve({CurveKey(0.0f, 1.0f), CurveKey(0.5f, 0.25f, -2.0f, INF), CurveKey(1.0f, 0.0f, -INF, 0.0f)},
                         CurveWrap::Loop, CurveWrap::PingPong);
    YAML::Node node = editor::Stream::encodeCurve(curve);
    YAML::Emitter out;
    out << node;
    AnimationCurve decoded = editor::Stream::decodeCurve(YAML::Load(out.c_str()));
    EXPECT_EQ(decoded, curve);

    // a bare number is a constant curve; [t, v] keys have flat tangents
    EXPECT_EQ(editor::Stream::decodeCurve(YAML::Load("0.35")), AnimationCurve::constant(0.35f));
    AnimationCurve parsed = editor::Stream::decodeCurve(YAML::Load("{keys: [[1, 0], [0, 1, -2, -2]], pre: clamp, post: loop}"));
    ASSERT_EQ(parsed.keys.size(), 2u);
    EXPECT_FLOAT_EQ(parsed.keys[0].time, 0.0f);
    EXPECT_FLOAT_EQ(parsed.keys[0].outTangent, -2.0f);
    EXPECT_EQ(parsed.postWrap, CurveWrap::Loop);
    AnimationCurve steps = editor::Stream::decodeCurve(YAML::Load("{keys: [[0, 1, 0, .inf], [1, 2, -.inf, 0]]}"));
    EXPECT_TRUE(std::isinf(steps.keys[0].outTangent));
    EXPECT_FLOAT_EQ(steps.evaluate(0.5f), 1.0f);
}
