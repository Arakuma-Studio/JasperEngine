// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// origin/main's AnimationCurve tests (its lines and trails curve) on S27's Curve, which the Line
// and Trail Renderers use now (docs/specs/S27-editor-tools.md, Upstream absorption): Hermite
// values against hand-computed points, step tangents, the wrap modes, the game's sample curves
// (the Invaders bullet trail's width), the factories, normalize (sorting, equal times,
// non-finite keys), key editing and the value range. Pure.

#include <gtest/gtest.h>

#include "math/Curve.h"

#include <cmath>
#include <limits>

using namespace jasperengine;

namespace {

    const float INF = std::numeric_limits<float>::infinity();

    CurveKey key(float time, float value, float in = 0.0f, float out = 0.0f){
        CurveKey k;
        k.time = time;
        k.value = value;
        k.inTangent = in;
        k.outTangent = out;
        return k;
    }

    AnimationCurve curveOf(std::vector<CurveKey> keys){
        AnimationCurve curve;
        curve.keys = std::move(keys);
        return curve;
    }

}

TEST(AnimationCurveTest, EmptyAndSingleKey){
    AnimationCurve empty;
    EXPECT_FLOAT_EQ(empty.evaluate(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(empty.evaluate(5.0f), 0.0f);

    AnimationCurve one = AnimationCurve::constant(0.35f);
    ASSERT_EQ(one.keys.size(), 1u);
    EXPECT_FLOAT_EQ(one.evaluate(-3.0f), 0.35f);
    EXPECT_FLOAT_EQ(one.evaluate(0.5f), 0.35f);
    EXPECT_FLOAT_EQ(one.evaluate(10.0f), 0.35f);
    EXPECT_TRUE(one.isConstant());
}

TEST(AnimationCurveTest, BulletTrailWidthCurve){
    // Prefabs/I - Invaders/Bullet.prefab, child Trail: keys (0, 0.22307944) and (1, 0), flat
    AnimationCurve curve = curveOf({key(0.0f, 0.22307944f), key(1.0f, 0.0f)});
    EXPECT_FLOAT_EQ(curve.evaluate(0.0f), 0.22307944f);
    EXPECT_FLOAT_EQ(curve.evaluate(1.0f), 0.0f);
    EXPECT_FLOAT_EQ(curve.evaluate(0.5f), 0.11153972f);
    // smoothstep shape: h00(0.25) = 0.84375
    EXPECT_NEAR(curve.evaluate(0.25f), 0.22307944f * 0.84375f, 1e-6f);
    EXPECT_FALSE(curve.isConstant());
}

TEST(AnimationCurveTest, HermiteWithTangents){
    // keys (0, 0, out 2) and (2, 1, in -1): dt 2, m0 = 4, m1 = -2
    AnimationCurve curve = curveOf({key(0.0f, 0.0f, 0.0f, 2.0f), key(2.0f, 1.0f, -1.0f, 0.0f)});
    // s = 0.5: h00 0.5, h10 0.125, h01 0.5, h11 -0.125 -> 0.125 * 4 + 0.5 * 1 + (-0.125) * (-2)
    EXPECT_NEAR(curve.evaluate(1.0f), 0.5f + 0.5f + 0.25f, 1e-6f);
    // s = 0.25: h10 = 0.140625, h01 = 0.15625, h11 = -0.046875
    EXPECT_NEAR(curve.evaluate(0.5f), 0.140625f * 4.0f + 0.15625f * 1.0f + (-0.046875f) * (-2.0f), 1e-6f);
    EXPECT_FLOAT_EQ(curve.evaluate(2.0f), 1.0f);
}

TEST(AnimationCurveTest, LinearAndEaseInOut){
    AnimationCurve line = AnimationCurve::linear(0.0f, 1.0f, 1.0f, 0.0f);
    for (float t = 0.0f; t <= 1.0f; t += 0.125f){
        EXPECT_NEAR(line.evaluate(t), 1.0f - t, 1e-6f) << t;
    }
    EXPECT_FLOAT_EQ(line.keys[0].outTangent, -1.0f);
    EXPECT_FLOAT_EQ(line.keys[1].inTangent, -1.0f);

    AnimationCurve ease = AnimationCurve::easeInOut(0.0f, 0.0f, 2.0f, 4.0f);
    EXPECT_FLOAT_EQ(ease.evaluate(1.0f), 2.0f);
    EXPECT_NEAR(ease.evaluate(0.5f), 4.0f * 0.15625f, 1e-6f);

    // reversed times are sorted
    AnimationCurve reversed = AnimationCurve::linear(1.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_FLOAT_EQ(reversed.keys.front().time, 0.0f);
    EXPECT_NEAR(reversed.evaluate(0.25f), 0.75f, 1e-6f);
}

TEST(AnimationCurveTest, StepTangents){
    AnimationCurve outStep = curveOf({key(0.0f, 1.0f, 0.0f, INF), key(1.0f, 3.0f)});
    EXPECT_FLOAT_EQ(outStep.evaluate(0.0f), 1.0f);
    EXPECT_FLOAT_EQ(outStep.evaluate(0.999f), 1.0f);
    EXPECT_FLOAT_EQ(outStep.evaluate(1.0f), 3.0f);
    EXPECT_TRUE(!outStep.isConstant());

    AnimationCurve inStep = curveOf({key(0.0f, 1.0f), key(1.0f, 3.0f, -INF, 0.0f)});
    EXPECT_FLOAT_EQ(inStep.evaluate(0.5f), 1.0f);
}

TEST(AnimationCurveTest, WrapModes){
    AnimationCurve curve = AnimationCurve::linear(0.0f, 0.0f, 1.0f, 1.0f);
    EXPECT_FLOAT_EQ(curve.evaluate(-1.0f), 0.0f);
    EXPECT_FLOAT_EQ(curve.evaluate(2.5f), 1.0f);

    curve.postWrapMode = CurveWrapMode::Loop;
    curve.preWrapMode = CurveWrapMode::Loop;
    EXPECT_NEAR(curve.evaluate(1.25f), 0.25f, 1e-5f);
    EXPECT_NEAR(curve.evaluate(3.75f), 0.75f, 1e-5f);
    EXPECT_NEAR(curve.evaluate(-0.25f), 0.75f, 1e-5f);

    curve.postWrapMode = CurveWrapMode::PingPong;
    curve.preWrapMode = CurveWrapMode::PingPong;
    EXPECT_NEAR(curve.evaluate(1.25f), 0.75f, 1e-5f);
    EXPECT_NEAR(curve.evaluate(2.25f), 0.25f, 1e-5f);
    EXPECT_NEAR(curve.evaluate(-0.25f), 0.25f, 1e-5f);

    // keys not starting at 0
    AnimationCurve shifted = AnimationCurve::linear(1.0f, 0.0f, 3.0f, 2.0f);
    shifted.postWrapMode = CurveWrapMode::Loop;
    EXPECT_NEAR(shifted.evaluate(4.0f), 1.0f, 1e-5f);
    EXPECT_FLOAT_EQ(shifted.evaluate(0.0f), 0.0f);

    // a NaN time reads the first key (a renderer never draws a NaN width)
    EXPECT_FLOAT_EQ(curve.evaluate(std::nanf("")), 0.0f);
}

TEST(AnimationCurveTest, NormalizeSortsAndDropsEqualTimes){
    AnimationCurve curve = curveOf({key(1.0f, 5.0f), key(0.0f, 1.0f), key(1.0f, 7.0f),
                                    key(std::nanf(""), 2.0f), key(0.5f, INF), key(0.25f, 2.0f)});
    curve.normalize();
    ASSERT_EQ(curve.keys.size(), 3u);
    EXPECT_FLOAT_EQ(curve.keys[0].time, 0.0f);
    EXPECT_FLOAT_EQ(curve.keys[1].time, 0.25f);
    EXPECT_FLOAT_EQ(curve.keys[2].time, 1.0f);
    // the first of two keys at the same time stays (stable sort)
    EXPECT_FLOAT_EQ(curve.keys[2].value, 5.0f);
}

TEST(AnimationCurveTest, KeyEditing){
    AnimationCurve curve;
    EXPECT_EQ(curve.addKey(key(1.0f, 1.0f)), 0);
    EXPECT_EQ(curve.addKey(key(0.0f, 0.0f)), 0);
    EXPECT_EQ(curve.addKey(key(0.5f, 2.0f)), 1);
    EXPECT_EQ(curve.addKey(key(0.5f, 3.0f)), -1);
    EXPECT_EQ(curve.addKey(key(INF, 3.0f)), -1);
    ASSERT_EQ(curve.keys.size(), 3u);

    // moving a key past another keeps the list sorted
    EXPECT_EQ(curve.moveKey(0, key(0.75f, 4.0f)), 1);
    EXPECT_FLOAT_EQ(curve.keys[0].time, 0.5f);
    EXPECT_FLOAT_EQ(curve.keys[1].value, 4.0f);
    // onto another key's time: the key keeps its own time (S27: the value still moves)
    EXPECT_EQ(curve.moveKey(1, key(1.0f, 4.5f)), 1);
    EXPECT_FLOAT_EQ(curve.keys[1].time, 0.75f);
    EXPECT_FLOAT_EQ(curve.keys[1].value, 4.5f);
    EXPECT_EQ(curve.moveKey(9, key(0.1f, 0.0f)), -1);
    // a NaN tangent: refused, and the key stays
    EXPECT_EQ(curve.moveKey(1, key(0.6f, 1.0f, std::nanf(""), 0.0f)), -1);
    ASSERT_EQ(curve.keys.size(), 3u);
    EXPECT_FLOAT_EQ(curve.keys[1].time, 0.75f);

    EXPECT_TRUE(curve.removeKey(2));
    EXPECT_FALSE(curve.removeKey(2));
    EXPECT_EQ(curve.keys.size(), 2u);
}

TEST(AnimationCurveTest, ValueRangeAndEquality){
    AnimationCurve curve = curveOf({key(0.0f, 0.0f, 0.0f, 8.0f), key(1.0f, 0.0f, 8.0f, 0.0f)});
    float minValue = 0.0f, maxValue = 0.0f;
    curve.valueRange(0.0f, 1.0f, minValue, maxValue);
    // overshoots above the keys: the peak of s(1-s)^2 * 8 - ... is positive
    EXPECT_GT(maxValue, 0.5f);
    EXPECT_LT(minValue, 0.0f);

    AnimationCurve copy = curve;
    EXPECT_EQ(copy, curve);
    copy.postWrapMode = CurveWrapMode::Loop;
    EXPECT_NE(copy, curve);
    const AnimationCurve one = AnimationCurve::constant(1.0f);
    ASSERT_EQ(one.keys.size(), 1u);
    EXPECT_FLOAT_EQ(one.keys[0].time, 0.0f);
    EXPECT_FLOAT_EQ(one.keys[0].value, 1.0f);
}
