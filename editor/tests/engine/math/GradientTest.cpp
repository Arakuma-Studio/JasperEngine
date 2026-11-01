// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// origin/main's Gradient tests (its lines and trails gradient) on S27's Gradient, which the Line
// and Trail Renderers use now (docs/specs/S27-editor-tools.md, Upstream absorption): blend and
// fixed modes, colour and alpha keys at different times, the game's gradients (the B lightning;
// the bullet trail's orange fading out), the 8-key cap, defaults without keys, and the YAML
// forms of curves and gradients (editor::Stream: S27's own form, and origin/main's "#RRGGBB"
// sRGB strings and linear arrays, a bare number as a constant curve, .inf tangents) in a round
// trip. S27's keys are the sRGB colours an artist picks (origin/main kept linear ones); both
// blend them in sRGB, as Unity does.

#include <gtest/gtest.h>

#include "math/Curve.h"
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

    Gradient gradientOf(std::vector<GradientColorKey> colors, std::vector<GradientAlphaKey> alphas, GradientMode mode = GradientMode::Blend){
        Gradient g;
        g.colorKeys = std::move(colors);
        g.alphaKeys = std::move(alphas);
        g.mode = mode;
        return g;
    }

    float linear(float srgb){
        return Color::sRGBToLinear(Vector3(srgb, srgb, srgb)).x;
    }

}

TEST(GradientTest, DefaultsAndNoKeys){
    // no keys: opaque white
    Gradient gradient;
    expectColor(gradient.evaluate(0.0f), Vector4(1, 1, 1, 1));
    expectColor(gradient.evaluate(0.7f), Vector4(1, 1, 1, 1));

    Gradient single = gradientOf({GradientColorKey{0.4f, Vector3(1, 0, 0)}}, {GradientAlphaKey{0.9f, 0.5f}});
    expectColor(single.evaluate(0.0f), Vector4(1, 0, 0, 0.5f));
    expectColor(single.evaluate(1.0f), Vector4(1, 0, 0, 0.5f));
}

TEST(GradientTest, BlendBetweenKeysAtDifferentTimes){
    Gradient gradient = gradientOf({GradientColorKey{0.2f, Vector3(1, 0, 0)}, GradientColorKey{0.6f, Vector3(0, 0, 1)}},
                                   {GradientAlphaKey{0.0f, 1.0f}, GradientAlphaKey{1.0f, 0.0f}});
    // before the first colour key: the first colour; after the last: the last
    expectColor(gradient.evaluate(0.0f), Vector4(1, 0, 0, 1));
    expectColor(gradient.evaluate(0.8f), Vector4(0, 0, 1, 0.2f));
    // colours mix in sRGB (Unity): halfway is 0.5 sRGB a channel, alpha mixes linearly
    const float half = linear(0.5f);
    expectColor(gradient.evaluate(0.4f), Vector4(half, 0, half, 0.6f));
    // outside [0, 1]: the end keys
    expectColor(gradient.evaluate(-2.0f), Vector4(1, 0, 0, 1));
    expectColor(gradient.evaluate(3.0f), Vector4(0, 0, 1, 0));
}

TEST(GradientTest, FixedMode){
    Gradient gradient = gradientOf({GradientColorKey{0.25f, Vector3(1, 0, 0)}, GradientColorKey{0.5f, Vector3(0, 1, 0)}, GradientColorKey{1.0f, Vector3(0, 0, 1)}},
                                   {GradientAlphaKey{0.5f, 0.2f}, GradientAlphaKey{1.0f, 0.8f}}, GradientMode::Fixed);
    // the first key at or after t
    expectColor(gradient.evaluate(0.1f), Vector4(1, 0, 0, 0.2f));
    expectColor(gradient.evaluate(0.25f), Vector4(1, 0, 0, 0.2f));
    expectColor(gradient.evaluate(0.3f), Vector4(0, 1, 0, 0.2f));
    expectColor(gradient.evaluate(0.5f), Vector4(0, 1, 0, 0.2f));
    expectColor(gradient.evaluate(0.51f), Vector4(0, 0, 1, 0.8f));
    expectColor(gradient.evaluate(1.0f), Vector4(0, 0, 1, 0.8f));
}

TEST(GradientTest, GameGradients){
    // Scenes/B.unity lightning: m_NumColorKeys 2 of the eight stored ones: white at both ends, opaque
    Gradient lightning = Gradient::twoColors(Vector3(1, 1, 1), Vector3(1, 1, 1));
    expectColor(lightning.evaluate(0.35f), Vector4(1, 1, 1, 1));

    // Prefabs/I - Invaders/Bullet.prefab trail: (1, 0.5886716, 0) sRGB, alpha 1 -> 0
    const Vector3 orange(1.0f, 0.5886716f, 0.0f);
    Gradient bullet = Gradient::twoColors(orange, orange, 1.0f, 0.0f);
    const Vector3 lin = Color::sRGBToLinear(orange);
    expectColor(bullet.evaluate(0.25f), Vector4(lin.x, lin.y, lin.z, 0.75f));
}

TEST(GradientTest, NormalizeCapsSortsAndClamps){
    std::vector<GradientColorKey> colors;
    for (int i = 9; i >= 0; i--){
        colors.push_back(GradientColorKey{i / 9.0f, Vector3((float)i, 0, 0)});
    }
    colors.push_back(GradientColorKey{1.5f, Vector3(0, 1, 0)});
    colors.push_back(GradientColorKey{std::nanf(""), Vector3(0, 1, 0)});
    Gradient gradient = gradientOf(colors, {GradientAlphaKey{-1.0f, 0.5f}, GradientAlphaKey{0.5f, 1.0f}});
    gradient.normalize();
    ASSERT_EQ(gradient.colorKeys.size(), Gradient::kMaxKeys);
    for (size_t i = 1; i < gradient.colorKeys.size(); i++){
        EXPECT_LE(gradient.colorKeys[i - 1].time, gradient.colorKeys[i].time);
    }
    // the first 7 by time and the last: both ends stay
    EXPECT_FLOAT_EQ(gradient.colorKeys.back().time, 1.0f);
    EXPECT_FLOAT_EQ(gradient.colorKeys[Gradient::kMaxKeys - 2].time, 6.0f / 9.0f);
    EXPECT_FLOAT_EQ(gradient.colorKeys.front().time, 0.0f);
    EXPECT_FLOAT_EQ(gradient.alphaKeys.front().time, 0.0f);
    EXPECT_FLOAT_EQ(gradient.alphaKeys.front().alpha, 0.5f);
}

TEST(GradientTest, ConstantAndTwoColors){
    Gradient constant = Gradient::constant(Vector3(0.2f, 0.4f, 0.6f), 0.5f);
    expectColor(constant.evaluate(0.9f), Vector4(linear(0.2f), linear(0.4f), linear(0.6f), 0.5f));
    Gradient two = Gradient::twoColors(Vector3(1, 0, 0), Vector3(0, 0, 1), 1.0f, 0.0f);
    const float half = linear(0.5f);
    expectColor(two.evaluate(0.5f), Vector4(half, 0, half, 0.5f));
}

TEST(GradientTest, BlendsInSRGBLikeUnity){
    // black to white: the middle is sRGB 0.5 (linear ~0.218), what Unity shows
    Gradient gradient = Gradient::twoColors(Vector3(0, 0, 0), Vector3(1, 1, 1));
    for (int i = 0; i <= 64; i++){
        const float t = (float)i / 64.0f;
        const Vector4 actual = gradient.evaluate(t);
        EXPECT_NEAR(actual.x, linear(t), 1e-5f) << t;
        EXPECT_NEAR(actual.z, linear(t), 1e-5f) << t;
    }
    // keys changed in place are mixed from their current values
    gradient.colorKeys[1].color = Vector3(0, 1, 0);
    const Vector4 changed = gradient.evaluate(0.5f);
    EXPECT_NEAR(changed.x, 0.0f, 1e-6f);
    EXPECT_NEAR(changed.y, linear(0.5f), 1e-5f);
    // a copy evaluates the same
    const Gradient copy = gradient;
    EXPECT_NEAR(copy.evaluate(0.25f).y, gradient.evaluate(0.25f).y, 1e-7f);
    EXPECT_TRUE(copy == gradient);
    // S27's linear blend: the keys converted first
    Gradient lin = Gradient::twoColors(Vector3(0, 0, 0), Vector3(1, 1, 1));
    lin.linear = true;
    EXPECT_NEAR(lin.evaluate(0.5f).x, 0.5f, 1e-5f);
}

TEST(GradientTest, YamlRoundTrip){
    Gradient gradient = gradientOf({GradientColorKey{0.0f, Vector3(1.0f, 150.0f / 255.0f, 0.0f)}, GradientColorKey{0.5f, Vector3(0.25f, 0.5f, 0.75f)}},
                                   {GradientAlphaKey{0.0f, 1.0f}, GradientAlphaKey{1.0f, 0.0f}}, GradientMode::Fixed);
    YAML::Node node = editor::Stream::encodeGradient(gradient);
    YAML::Emitter out;
    out << node;
    const Gradient decoded = editor::Stream::decodeGradient(YAML::Load(out.c_str()));
    EXPECT_EQ(decoded, gradient);

    // origin/main's sRGB strings: "#FF9600" is (1, 150/255, 0) in sRGB; its arrays are linear
    Gradient parsed = editor::Stream::decodeGradient(YAML::Load("{mode: blend, colors: [[0, \"#FF9600\"], [1, [0.5, 0.25, 0]]], alphas: [[0, 1], [1, 0]]}"));
    ASSERT_EQ(parsed.colorKeys.size(), 2u);
    EXPECT_NEAR(parsed.colorKeys[0].color.x, 1.0f, 1e-6f);
    EXPECT_NEAR(parsed.colorKeys[0].color.y, 150.0f / 255.0f, 1e-6f);
    EXPECT_NEAR(parsed.colorKeys[1].color.x, Color::linearTosRGB(Vector3(0.5f, 0.5f, 0.5f)).x, 1e-5f);
    EXPECT_NEAR(parsed.evaluate(1.0f).x, 0.5f, 1e-4f) << "the linear colour evaluates back to itself";
    EXPECT_FLOAT_EQ(parsed.alphaKeys[1].alpha, 0.0f);
    // a single colour is a constant gradient
    Gradient single = editor::Stream::decodeGradient(YAML::Load("\"#FFFFFF80\""));
    EXPECT_NEAR(single.evaluate(0.5f).w, 128.0f / 255.0f, 1e-5f);
}

TEST(GradientTest, CurveYamlRoundTrip){
    const float INF = std::numeric_limits<float>::infinity();
    AnimationCurve curve;
    CurveKey a, b, c;
    a.time = 0.0f; a.value = 1.0f;
    b.time = 0.5f; b.value = 0.25f; b.inTangent = -2.0f; b.outTangent = INF;
    // (a step is an infinite tangent: S27 writes it as null, read back as +inf)
    c.time = 1.0f; c.value = 0.0f; c.inTangent = INF;
    curve.keys = {a, b, c};
    curve.preWrapMode = CurveWrapMode::Loop;
    curve.postWrapMode = CurveWrapMode::PingPong;
    YAML::Node node = editor::Stream::encodeCurve(curve);
    YAML::Emitter out;
    out << node;
    AnimationCurve decoded = editor::Stream::decodeCurve(YAML::Load(out.c_str()));
    EXPECT_EQ(decoded, curve);

    // origin/main's: a bare number is a constant curve; [t, v] keys have flat tangents
    EXPECT_EQ(editor::Stream::decodeCurve(YAML::Load("0.35")), AnimationCurve::constant(0.35f));
    AnimationCurve parsed = editor::Stream::decodeCurve(YAML::Load("{keys: [[1, 0], [0, 1, -2, -2]], pre: clamp, post: loop}"));
    ASSERT_EQ(parsed.keys.size(), 2u);
    EXPECT_FLOAT_EQ(parsed.keys[0].time, 0.0f);
    EXPECT_FLOAT_EQ(parsed.keys[0].outTangent, -2.0f);
    EXPECT_EQ(parsed.postWrapMode, CurveWrapMode::Loop);
    AnimationCurve steps = editor::Stream::decodeCurve(YAML::Load("{keys: [[0, 1, 0, .inf], [1, 2, -.inf, 0]]}"));
    EXPECT_TRUE(std::isinf(steps.keys[0].outTangent));
    EXPECT_FLOAT_EQ(steps.evaluate(0.5f), 1.0f);
}
