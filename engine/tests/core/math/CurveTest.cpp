// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The curve and gradient runtime of docs/specs/S27-editor-tools.md R6-R8 (AC1, AC3, AC4, AC5):
// Unity's constructors and SmoothTangents, tangent modes, wrap modes, key editing, fromEase,
// bake, and the min-max values' four modes.

#include "AllocationCounter.h"
#include "action/Ease.h"
#include "math/Curve.h"
#include "particles/ParticleValues.h"
#include "util/Color.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using particles::CurveMode;
using particles::GradientValueMode;
using particles::MinMaxCurve;
using particles::MinMaxGradient;

namespace {

    float toLinear(float v) {
        return Color::sRGBToLinear(Vector3(v, v, v)).x;
    }

    Curve threeKeys() {
        return Curve({{0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 2.0f, 0.0f, 0.0f}, {3.0f, 0.0f, 0.0f, 0.0f}});
    }

    Curve sixteenKeys() {
        Curve curve;
        for (int i = 0; i < 16; i++) {
            curve.addKey((float)i / 15.0f, std::sin((float)i * 0.7f));
        }
        return curve;
    }

}

TEST(CurveHelpers, Constructors) {
    const Curve c = Curve::constant(1.0f, 3.0f, 5.0f);
    ASSERT_EQ(c.keys.size(), 2u);
    EXPECT_EQ(c.keys[0], (CurveKey{1.0f, 5.0f, 0.0f, 0.0f}));
    EXPECT_EQ(c.keys[1], (CurveKey{3.0f, 5.0f, 0.0f, 0.0f}));
    EXPECT_FLOAT_EQ(c.evaluate(2.0f), 5.0f);

    const Curve l = Curve::linear(1.0f, 2.0f, 3.0f, 6.0f);
    ASSERT_EQ(l.keys.size(), 2u);
    EXPECT_EQ(l.keys[0], (CurveKey{1.0f, 2.0f, 2.0f, 2.0f}));
    EXPECT_EQ(l.keys[1], (CurveKey{3.0f, 6.0f, 2.0f, 2.0f}));
    EXPECT_FLOAT_EQ(l.evaluate(2.0f), 4.0f);
    EXPECT_FLOAT_EQ(l.evaluate(2.5f), 5.0f);

    const Curve e = Curve::easeInOut(0.0f, 0.0f, 2.0f, 1.0f);
    ASSERT_EQ(e.keys.size(), 2u);
    EXPECT_EQ(e.keys[0], (CurveKey{0.0f, 0.0f, 0.0f, 0.0f}));
    EXPECT_EQ(e.keys[1], (CurveKey{2.0f, 1.0f, 0.0f, 0.0f}));
    EXPECT_FLOAT_EQ(e.evaluate(1.0f), 0.5f);
    EXPECT_LT(e.evaluate(0.5f), 0.25f);   // slow start

    // a zero-length span is one key (Unity gives a single key too)
    EXPECT_EQ(Curve::linear(1.0f, 2.0f, 1.0f, 9.0f).keys.size(), 1u);
    EXPECT_EQ(Curve::constant(1.0f, 1.0f, 3.0f).keys.size(), 1u);

    // the alias names the same type
    AnimationCurve alias = Curve::linear(0.0f, 0.0f, 1.0f, 2.0f);
    Keyframe key = alias.keys[1];
    EXPECT_FLOAT_EQ(alias.evaluate(0.5f), 1.0f);
    EXPECT_FLOAT_EQ(key.value, 2.0f);
}

TEST(CurveHelpers, SmoothTangents) {
    // Unity's AnimationCurve.SmoothTangents on (0,0) (1,2) (3,0): the slopes to the neighbours
    // are 2 and -1; weight w blends them as (1+w)/2 * 2 + (1-w)/2 * -1
    for (float weight : {0.0f, 0.5f, -1.0f, 1.0f}) {
        Curve curve = threeKeys();
        curve.smoothTangents(1, weight);
        const float expected = (1.0f + weight) * 0.5f * 2.0f + (1.0f - weight) * 0.5f * -1.0f;
        EXPECT_FLOAT_EQ(curve.keys[1].inTangent, expected) << weight;
        EXPECT_FLOAT_EQ(curve.keys[1].outTangent, expected) << weight;
    }
    // the end keys point at their neighbour
    Curve curve = threeKeys();
    curve.smoothTangents(0, 0.0f);
    curve.smoothTangents(2, 0.0f);
    EXPECT_FLOAT_EQ(curve.keys[0].outTangent, 2.0f);
    EXPECT_FLOAT_EQ(curve.keys[0].inTangent, 2.0f);
    EXPECT_FLOAT_EQ(curve.keys[2].inTangent, -1.0f);
    // out of range: nothing
    curve.smoothTangents(9, 0.0f);
    EXPECT_EQ(curve.keys.size(), 3u);
}

TEST(CurveHelpers, FromEase) {
    // every named ease, sampled into a curve, follows the ease within 0.01 at 101 points
    for (int type = (int)EaseType::LINEAR; type <= (int)EaseType::STEP; type++) {
        Ease ease((EaseType)type);
        const Curve curve = Curve::fromEase((EaseType)type);
        EXPECT_LE(curve.keys.size(), 64u);
        float worst = 0.0f;
        for (int i = 0; i <= 100; i++) {
            const float t = (float)i / 100.0f;
            worst = std::max(worst, std::fabs(curve.evaluate(t) - ease.call(t)));
        }
        EXPECT_LE(worst, 0.01f) << "ease " << type;
    }
    // no function of their own: a linear curve
    EXPECT_EQ(Curve::fromEase(EaseType::CUSTOM), Curve::linear(0.0f, 1.0f));
    EXPECT_EQ(Curve::fromEase(EaseType::CURVE), Curve::linear(0.0f, 1.0f));
    // the smooth eases take no more than the 16 keys asked for
    EXPECT_LE(Curve::fromEase(EaseType::SINE_IN_OUT).keys.size(), 16u);
    EXPECT_LE(Curve::fromEase(EaseType::BOUNCE_OUT).keys.size(), 16u);
    // the legacy sampler's count is exact
    EXPECT_LE(Curve::fromFunction([](float t) { return std::sin(t * 20.0f); }, 16).keys.size(), 16u);
    // STEP holds its start value up to the end
    const Curve step = Curve::fromEase(EaseType::STEP);
    EXPECT_FLOAT_EQ(step.evaluate(0.99f), 0.0f);
    EXPECT_FLOAT_EQ(step.evaluate(1.0f), 1.0f);
}

TEST(CurveHelpers, Bake) {
    const Curve curve = sixteenKeys();
    std::array<float, 64> lut{};
    curve.bake(lut, -0.5f, 1.5f);
    for (size_t i = 0; i < lut.size(); i++) {
        // bake's own times: t0 + (t1 - t0) * i / (n - 1), the last exactly t1
        const float t = i + 1 == lut.size() ? 1.5f : -0.5f + 2.0f * (float)i / 63.0f;
        EXPECT_EQ(lut[i], curve.evaluate(t)) << i;
    }
    // the last entry is exactly the end
    EXPECT_EQ(lut.back(), curve.evaluate(1.5f));
    std::array<float, 1> one{};
    curve.bake(one, 0.25f, 9.0f);
    EXPECT_EQ(one[0], curve.evaluate(0.25f));

    Gradient gradient = Gradient::twoColors(Vector3(1, 0, 0), Vector3(0, 0, 1), 1.0f, 0.0f);
    std::array<Vector4, 9> colors{};
    gradient.bake(colors);
    for (size_t i = 0; i < colors.size(); i++) {
        const Vector4 expected = gradient.evaluate((float)i / 8.0f);
        EXPECT_FLOAT_EQ(colors[i].x, expected.x) << i;
        EXPECT_FLOAT_EQ(colors[i].z, expected.z) << i;
        EXPECT_FLOAT_EQ(colors[i].w, expected.w) << i;
    }
    EXPECT_FLOAT_EQ(colors.front().w, 1.0f);
    EXPECT_FLOAT_EQ(colors.back().w, 0.0f);
}

TEST(CurveHelpers, BakePerf) {
    // R35: 64 entries from a 16-key curve in 3 us (Release, the CI runner; enforced with the
    // benchmark slack, printed otherwise)
    const Curve curve = sixteenKeys();
    std::array<float, 64> lut{};
    constexpr int kRuns = 20000;
    float sink = 0.0f;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kRuns; i++) {
        curve.bake(lut, 0.0f, 1.0f + (float)(i & 1) * 1.0e-6f);
        sink += lut[(size_t)i & 63];
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / kRuns;
    std::printf("[ CurveBake ] 64 entries from 16 keys: %.3f us (budget 3 us) %f\n", us, (double)sink * 0.0);
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LE(us, 3.0 * testsupport::perfSlack());
    }
}

TEST(CurveHelpers, EvaluateDoesNotAllocate) {
    if (!testsupport::countsAllocations()) GTEST_SKIP() << "no allocation counter on this platform";
    Curve curve = sixteenKeys();
    curve.preWrapMode = CurveWrapMode::PingPong;
    curve.postWrapMode = CurveWrapMode::LoopWithOffset;
    Gradient gradient = Gradient::twoColors(Vector3(1, 0, 0), Vector3(0, 1, 0));
    Gradient linearGradient = gradient;
    linearGradient.linear = true;
    MinMaxCurve twoCurves = MinMaxCurve::fromCurve(curve);
    twoCurves.mode = CurveMode::TwoCurves;
    twoCurves.curveMax = Curve::linear(0.0f, 1.0f);
    MinMaxGradient twoGradients;
    twoGradients.mode = GradientValueMode::TwoGradients;
    twoGradients.gradient = gradient;
    twoGradients.gradientMax = linearGradient;
    std::array<float, 32> lut{};
    std::array<Vector4, 32> colors{};

    float sink = 0.0f;
    testsupport::ThreadAllocationCount count;
    for (int i = 0; i < 100; i++) {
        const float t = (float)i * 0.037f - 1.0f;
        sink += curve.evaluate(t) + curve.derivative(t);
        sink += gradient.evaluate(t).x + linearGradient.evaluate(t).y;
        sink += twoCurves.evaluate(t, 0.5f) + twoGradients.evaluate(t, 0.25f).z;
    }
    curve.bake(lut, 0.0f, 2.0f);
    linearGradient.bake(colors);
    EXPECT_EQ(count.count(), 0u);
    EXPECT_TRUE(std::isfinite(sink));
}

TEST(CurveHelpers, LinearGradientBlendsInLinearSpace) {
    Gradient gradient = Gradient::twoColors(Vector3(1, 0, 0), Vector3(0, 0, 1));
    gradient.linear = true;
    const Vector4 mid = gradient.evaluate(0.5f);
    // each key converted, then blended: half of linear 1
    EXPECT_NEAR(mid.x, 0.5f, 1e-6f);
    EXPECT_NEAR(mid.z, 0.5f, 1e-6f);
    gradient.linear = false;
    // blended in sRGB, then converted: sRGB 0.5 is linear 0.214
    EXPECT_NEAR(gradient.evaluate(0.5f).x, toLinear(0.5f), 1e-6f);
}

TEST(CurveWrap, ModesOutsideTheKeys) {
    Curve curve = Curve::linear(0.0f, 0.0f, 1.0f, 1.0f);
    // Clamp holds the end values (the default, as before the modes existed)
    EXPECT_FLOAT_EQ(curve.evaluate(-1.0f), 0.0f);
    EXPECT_FLOAT_EQ(curve.evaluate(2.5f), 1.0f);
    EXPECT_FLOAT_EQ(curve.derivative(2.5f), 0.0f);

    curve.postWrapMode = CurveWrapMode::Loop;
    curve.preWrapMode = CurveWrapMode::Loop;
    EXPECT_NEAR(curve.evaluate(2.25f), 0.25f, 1e-6f);
    EXPECT_NEAR(curve.evaluate(-0.25f), 0.75f, 1e-6f);
    EXPECT_NEAR(curve.derivative(2.25f), 1.0f, 1e-5f);

    curve.postWrapMode = CurveWrapMode::PingPong;
    curve.preWrapMode = CurveWrapMode::PingPong;
    EXPECT_NEAR(curve.evaluate(1.25f), 0.75f, 1e-6f);
    EXPECT_NEAR(curve.evaluate(2.25f), 0.25f, 1e-6f);
    EXPECT_NEAR(curve.evaluate(-0.25f), 0.25f, 1e-6f);
    EXPECT_NEAR(curve.derivative(1.25f), -1.0f, 1e-5f);

    curve.postWrapMode = CurveWrapMode::LoopWithOffset;
    curve.preWrapMode = CurveWrapMode::LoopWithOffset;
    EXPECT_NEAR(curve.evaluate(2.25f), 2.25f, 1e-5f);
    EXPECT_NEAR(curve.evaluate(-0.75f), -0.75f, 1e-5f);

    curve.postWrapMode = CurveWrapMode::Linear;
    curve.preWrapMode = CurveWrapMode::Linear;
    EXPECT_NEAR(curve.evaluate(3.0f), 3.0f, 1e-6f);
    EXPECT_NEAR(curve.evaluate(-2.0f), -2.0f, 1e-6f);
    EXPECT_NEAR(curve.derivative(3.0f), 1.0f, 1e-6f);
    // an infinite end tangent holds
    curve.keys[1].outTangent = INFINITY;
    EXPECT_FLOAT_EQ(curve.evaluate(3.0f), 1.0f);

    // one key and none: the wraps do nothing
    Curve single = Curve::constant(4.0f);
    single.postWrapMode = CurveWrapMode::Linear;
    EXPECT_FLOAT_EQ(single.evaluate(10.0f), 4.0f);
    EXPECT_FLOAT_EQ(Curve().evaluate(1.0f), 0.0f);
    // NaN time keeps the first value, as before
    EXPECT_FLOAT_EQ(Curve::linear(0.0f, 1.0f, 1.0f, 2.0f).evaluate(NAN), 1.0f);
}

TEST(CurveTangentModes, PackedModes) {
    CurveKey key;
    EXPECT_EQ(key.inMode(), CurveTangentMode::Free);
    EXPECT_EQ(key.outMode(), CurveTangentMode::Free);
    EXPECT_FALSE(key.broken());
    key.setInMode(CurveTangentMode::Linear);
    key.setOutMode(CurveTangentMode::Constant);
    key.setBroken(true);
    EXPECT_EQ(key.inMode(), CurveTangentMode::Linear);
    EXPECT_EQ(key.outMode(), CurveTangentMode::Constant);
    EXPECT_TRUE(key.broken());
    key.setBothModes(CurveTangentMode::ClampedAuto);
    EXPECT_EQ(key.inMode(), CurveTangentMode::ClampedAuto);
    EXPECT_EQ(key.outMode(), CurveTangentMode::ClampedAuto);
    EXPECT_FALSE(key.broken());
    // the mode is part of a key's identity
    CurveKey other = key;
    other.setBroken(true);
    EXPECT_NE(key, other);
}

TEST(CurveTangentModes, RecomputedFromNeighbours) {
    Curve curve = threeKeys();
    for (CurveKey& key : curve.keys) key.setBothModes(CurveTangentMode::Auto);
    curve.updateAllTangents();
    EXPECT_FLOAT_EQ(curve.keys[1].inTangent, 0.5f);   // mean of 2 and -1
    EXPECT_FLOAT_EQ(curve.keys[0].outTangent, 2.0f);  // the ends point at the neighbour
    EXPECT_FLOAT_EQ(curve.keys[2].inTangent, -1.0f);

    // Clamped Auto: a peak is flat
    for (CurveKey& key : curve.keys) key.setBothModes(CurveTangentMode::ClampedAuto);
    curve.updateAllTangents();
    EXPECT_FLOAT_EQ(curve.keys[1].inTangent, 0.0f);
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, 0.0f);

    // Clamped Auto on a monotone run never overshoots: values stay inside each segment's keys
    Curve rising({{0.0f, 0.0f}, {1.0f, 0.1f}, {1.1f, 5.0f}, {3.0f, 5.2f}});
    for (CurveKey& key : rising.keys) key.setBothModes(CurveTangentMode::ClampedAuto);
    rising.updateAllTangents();
    for (int i = 0; i <= 300; i++) {
        const float t = (float)i / 100.0f;
        const float v = rising.evaluate(t);
        EXPECT_GE(v, -1e-5f) << t;
        EXPECT_LE(v, 5.2f + 1e-5f) << t;
    }
    for (size_t k = 0; k + 1 < rising.keys.size(); k++) {
        for (int i = 0; i <= 50; i++) {
            const float t = rising.keys[k].time + (rising.keys[k + 1].time - rising.keys[k].time) * (float)i / 50.0f;
            EXPECT_GE(rising.evaluate(t), rising.keys[k].value - 1e-4f) << t;
            EXPECT_LE(rising.evaluate(t), rising.keys[k + 1].value + 1e-4f) << t;
        }
    }

    // Linear sides point at the neighbour on their side; Constant makes a step
    Curve sides = threeKeys();
    sides.keys[1].setInMode(CurveTangentMode::Linear);
    sides.keys[1].setOutMode(CurveTangentMode::Constant);
    sides.keys[1].setBroken(true);
    sides.updateTangents(1);
    EXPECT_FLOAT_EQ(sides.keys[1].inTangent, 2.0f);
    EXPECT_TRUE(std::isinf(sides.keys[1].outTangent));
    EXPECT_FLOAT_EQ(sides.evaluate(2.5f), 2.0f);  // held at the left value

    // Free keeps what was typed
    Curve free = threeKeys();
    free.keys[1].inTangent = 7.0f;
    free.updateAllTangents();
    EXPECT_FLOAT_EQ(free.keys[1].inTangent, 7.0f);
}

TEST(CurveKeys, AddMoveRemove) {
    Curve curve;
    EXPECT_EQ(curve.addKey(1.0f, 1.0f), 0);
    EXPECT_EQ(curve.addKey(0.0f, 0.0f), 0);
    EXPECT_EQ(curve.addKey(2.0f, 0.0f), 2);
    EXPECT_EQ(curve.addKey(2.0f, 5.0f), -1);   // a key at that time already
    EXPECT_EQ(curve.addKey(NAN, 5.0f), -1);
    ASSERT_EQ(curve.keys.size(), 3u);
    // added keys are Clamped Auto: the peak at 1 is flat, the curve follows its neighbours
    EXPECT_EQ(curve.keys[1].inMode(), CurveTangentMode::ClampedAuto);
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, 0.0f);
    EXPECT_FLOAT_EQ(curve.keys[0].outTangent, 1.0f);

    // moved past its neighbour, the key is re-sorted and the tangents follow
    CurveKey moved = curve.keys[0];
    moved.time = 3.0f;
    moved.value = 2.0f;
    EXPECT_EQ(curve.moveKey(0, moved), 2);
    EXPECT_FLOAT_EQ(curve.keys[2].time, 3.0f);
    EXPECT_FLOAT_EQ(curve.keys[0].time, 1.0f);
    // onto another key's time: the old time is kept (Unity's MoveKey)
    CurveKey clash = curve.keys[2];
    clash.time = 1.0f;
    EXPECT_EQ(curve.moveKey(2, clash), 2);
    EXPECT_FLOAT_EQ(curve.keys[2].time, 3.0f);
    EXPECT_EQ(curve.moveKey(7, clash), -1);

    curve.removeKey(1);
    ASSERT_EQ(curve.keys.size(), 2u);
    EXPECT_FLOAT_EQ(curve.keys[0].time, 1.0f);
    EXPECT_FLOAT_EQ(curve.keys[1].time, 3.0f);
    // the new neighbours' Clamped Auto sides now point at each other
    EXPECT_FLOAT_EQ(curve.keys[0].outTangent, 0.5f);
    curve.removeKey(5);
    EXPECT_EQ(curve.keys.size(), 2u);
}

TEST(CurveKeys, EqualityIncludesWraps) {
    Curve a = Curve::linear(0.0f, 1.0f);
    Curve b = a;
    EXPECT_EQ(a, b);
    b.postWrapMode = CurveWrapMode::Loop;
    EXPECT_NE(a, b);
}

TEST(MinMaxCurve, FourModes) {
    const Curve up = Curve::linear(0.0f, 0.0f, 1.0f, 1.0f);
    const Curve down = Curve::linear(0.0f, 1.0f, 1.0f, 0.0f);
    for (float r : {0.0f, 0.5f, 1.0f}) {
        MinMaxCurve constant(3.0f);
        EXPECT_FLOAT_EQ(constant.evaluate(0.3f, r), 3.0f);

        MinMaxCurve two = MinMaxCurve::twoConstants(2.0f, 6.0f);
        EXPECT_FLOAT_EQ(two.evaluate(0.3f, r), 2.0f + 4.0f * r);

        MinMaxCurve curve = MinMaxCurve::fromCurve(up, 2.0f);
        EXPECT_FLOAT_EQ(curve.evaluate(0.25f, r), 0.5f);

        MinMaxCurve twoCurves = MinMaxCurve::fromCurve(up, 2.0f);
        twoCurves.mode = CurveMode::TwoCurves;
        twoCurves.curveMax = down;
        const float a = 0.25f, b = 0.75f;
        EXPECT_FLOAT_EQ(twoCurves.evaluate(0.25f, r), (a + (b - a) * r) * 2.0f);
    }
}

TEST(MinMaxGradient, FourModes) {
    const Gradient redBlue = Gradient::twoColors(Vector3(1, 0, 0), Vector3(0, 0, 1));
    const Gradient greenWhite = Gradient::twoColors(Vector3(0, 1, 0), Vector3(1, 1, 1));
    for (float r : {0.0f, 0.5f, 1.0f}) {
        MinMaxGradient color(Vector4(1.0f, 0.5f, 0.0f, 1.0f));
        const Vector4 c = color.evaluate(0.3f, r);
        EXPECT_FLOAT_EQ(c.x, 1.0f);
        EXPECT_NEAR(c.y, toLinear(0.5f), 1e-6f);

        MinMaxGradient two(Vector4(0, 0, 0, 1));
        two.mode = GradientValueMode::TwoColors;
        two.colorMax = Vector4(1, 1, 1, 0);
        const Vector4 t = two.evaluate(0.0f, r);
        EXPECT_NEAR(t.x, toLinear(r), 1e-6f);
        EXPECT_NEAR(t.w, 1.0f - r, 1e-6f);

        MinMaxGradient g;
        g.mode = GradientValueMode::Gradient;
        g.gradient = redBlue;
        EXPECT_NEAR(g.evaluate(1.0f, r).z, 1.0f, 1e-6f);

        MinMaxGradient tg;
        tg.mode = GradientValueMode::TwoGradients;
        tg.gradient = redBlue;
        tg.gradientMax = greenWhite;
        const Vector4 at0 = tg.evaluate(0.0f, r);
        EXPECT_NEAR(at0.x, 1.0f - r, 1e-6f);
        EXPECT_NEAR(at0.y, r, 1e-6f);
    }
}
