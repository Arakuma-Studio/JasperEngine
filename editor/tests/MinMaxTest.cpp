// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// particles/MinMax (particles spec 6.1, 9): the four MinMaxCurve modes and the five
// MinMaxGradient modes against hand values, the baked curve of the simulation against the
// exact one, and a particle's randoms staying the same for its life.

#include <gtest/gtest.h>

#include "particles/MinMax.h"
#include "particles/ParticleRandom.h"
#include "particles/ParticleSimulation.h"

#include <cmath>

using namespace jasperengine;

TEST(MinMaxCurve, Constant) {
    const MinMaxCurve c(3.5f);
    EXPECT_EQ(c.mode, ParticleSystemCurveMode::Constant);
    EXPECT_FLOAT_EQ(c.evaluate(0.0f, 0.0f), 3.5f);
    EXPECT_FLOAT_EQ(c.evaluate(0.7f, 0.9f), 3.5f);
    EXPECT_TRUE(c.isTimeIndependent());
}

TEST(MinMaxCurve, TwoConstantsLerpKeepsTheOrder) {
    const MinMaxCurve c = MinMaxCurve::between(2.0f, 6.0f);
    EXPECT_FLOAT_EQ(c.evaluate(0.3f, 0.0f), 2.0f);
    EXPECT_FLOAT_EQ(c.evaluate(0.3f, 1.0f), 6.0f);
    EXPECT_FLOAT_EQ(c.evaluate(0.9f, 0.25f), 3.0f);
    // min above max: Unity keeps the lerp order
    const MinMaxCurve reversed = MinMaxCurve::between(6.0f, 2.0f);
    EXPECT_FLOAT_EQ(reversed.evaluate(0.0f, 0.25f), 5.0f);
    EXPECT_FLOAT_EQ(reversed.minValue(), 2.0f);
    EXPECT_FLOAT_EQ(reversed.maxValue(), 6.0f);
}

TEST(MinMaxCurve, CurveTimesMultiplier) {
    const MinMaxCurve c = MinMaxCurve::fromCurve(AnimationCurve::linear(0.0f, 0.0f, 1.0f, 2.0f), 3.0f);
    EXPECT_FLOAT_EQ(c.evaluate(0.0f, 0.5f), 0.0f);
    EXPECT_NEAR(c.evaluate(0.5f, 0.5f), 3.0f, 1e-5f);
    EXPECT_NEAR(c.evaluate(1.0f, 0.0f), 6.0f, 1e-5f);
    EXPECT_FALSE(c.isTimeIndependent());
    EXPECT_NEAR(c.maxValue(), 6.0f, 1e-5f);
}

TEST(MinMaxCurve, TwoCurvesBlendByTheRandom) {
    const MinMaxCurve c = MinMaxCurve::between(AnimationCurve::constant(1.0f), AnimationCurve::linear(0.0f, 2.0f, 1.0f, 4.0f), 2.0f);
    // t 0.5: min curve 1, max curve 3; times 2
    EXPECT_NEAR(c.evaluate(0.5f, 0.0f), 2.0f, 1e-5f);
    EXPECT_NEAR(c.evaluate(0.5f, 1.0f), 6.0f, 1e-5f);
    EXPECT_NEAR(c.evaluate(0.5f, 0.5f), 4.0f, 1e-5f);
}

TEST(MinMaxCurve, BakedMatchesTheExactCurve) {
    AnimationCurve curve({CurveKey(0.0f, 0.0f, 0.0f, 2.0f), CurveKey(0.6f, 1.0f, 0.0f, 0.0f), CurveKey(1.0f, 0.2f, -2.0f, -2.0f)});
    const MinMaxCurve exact = MinMaxCurve::between(AnimationCurve::linear(0.0f, 0.5f, 1.0f, 0.0f), curve, 1.5f);
    BakedMinMaxCurve baked;
    baked.bake(exact);
    for (int i = 0; i <= 100; i++) {
        const float t = (float)i / 100.0f;
        for (float r : {0.0f, 0.4f, 1.0f}) {
            EXPECT_NEAR(baked.evaluate(t, r), exact.evaluate(t, r), 0.01f) << "t " << t << " r " << r;
        }
    }
    // constants stay exact
    BakedMinMaxCurve constant;
    constant.bake(MinMaxCurve::between(1.0f, 3.0f));
    EXPECT_FLOAT_EQ(constant.evaluate(0.3f, 0.5f), 2.0f);
}

TEST(MinMaxGradient, TheFiveModes) {
    const Vector4 red(1, 0, 0, 1), blue(0, 0, 1, 0.5f);
    const MinMaxGradient color = MinMaxGradient::fromColor(red);
    EXPECT_EQ(color.evaluate(0.3f, 0.8f), red);

    const MinMaxGradient two = MinMaxGradient::between(red, blue);
    const Vector4 half = two.evaluate(0.9f, 0.5f);
    EXPECT_NEAR(half.x, 0.5f, 1e-6f);
    EXPECT_NEAR(half.z, 0.5f, 1e-6f);
    EXPECT_NEAR(half.w, 0.75f, 1e-6f);

    const Gradient g = Gradient::twoColors(red, blue);
    const MinMaxGradient gradient = MinMaxGradient::fromGradient(g);
    EXPECT_EQ(gradient.evaluate(0.0f, 0.9f), g.evaluate(0.0f));
    EXPECT_EQ(gradient.evaluate(1.0f, 0.1f), g.evaluate(1.0f));

    const Gradient white = Gradient::constant(Vector4(1, 1, 1, 1));
    const MinMaxGradient twoGradients = MinMaxGradient::between(white, g);
    const Vector4 mid = twoGradients.evaluate(1.0f, 0.5f);
    EXPECT_NEAR(mid.x, 0.5f, 1e-5f);
    EXPECT_NEAR(mid.z, 1.0f, 1e-5f);

    MinMaxGradient random = MinMaxGradient::fromGradient(g);
    random.mode = ParticleSystemGradientMode::RandomColor;
    // the random picks the point of the gradient, the time does not matter
    EXPECT_EQ(random.evaluate(0.0f, 1.0f), g.evaluate(1.0f));
    EXPECT_EQ(random.evaluate(0.7f, 0.0f), g.evaluate(0.0f));
    EXPECT_TRUE(random.isTimeIndependent());
}

TEST(MinMaxRandom, ParticleRandomsAreStable) {
    // a particle's value for an id never changes, ids are independent of each other
    for (uint32_t seed : {1u, 77u, 123456789u}) {
        const float a = particles::particleRandom(seed, particles::RANDOM_LIFETIME);
        EXPECT_EQ(a, particles::particleRandom(seed, particles::RANDOM_LIFETIME));
        EXPECT_GE(a, 0.0f);
        EXPECT_LT(a, 1.0f);
        EXPECT_NE(a, particles::particleRandom(seed, particles::RANDOM_SPEED));
    }
    // roughly uniform
    double sum = 0.0;
    for (uint32_t seed = 0; seed < 10000; seed++) sum += particles::particleRandom(seed * 2654435761u, particles::RANDOM_SIZE);
    EXPECT_NEAR(sum / 10000.0, 0.5, 0.02);
}
