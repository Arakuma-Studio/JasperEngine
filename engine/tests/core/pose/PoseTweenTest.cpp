// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Acceptance test 10: the ease (spec 4.8) and the feel presets

#include "TestUtil.h"

#include "pose/PoseTween.h"

#include <gtest/gtest.h>

using namespace posetest;

namespace {

    Transition spring(float overshoot, float anticipation){
        Transition t;
        t.curve = TransitionCurve::Spring;
        t.overshoot = overshoot;
        t.anticipation = anticipation;
        return t;
    }

}

TEST(PoseTween, SpringProperties){
    for (float overshoot : {0.0f, 0.1f, 0.25f, 0.5f, 0.9f}){
        for (float anticipation : {0.0f, 0.06f, 0.25f, 0.5f}){
            const Transition t = spring(overshoot, anticipation);
            const double a = anticipation;
            const double mp = std::min(std::max(static_cast<double>(overshoot), 0.001), 0.9);
            double lowest = 1e9, highest = -1e9;
            for (int i = 0; i <= 200000; i++){
                const double x = i * 1e-5 * 3.0;
                const double v = easePrecise(x, t);
                lowest = std::min(lowest, v);
                highest = std::max(highest, v);
            }
            EXPECT_NEAR(lowest, -a, 1e-6) << overshoot << " " << anticipation;
            EXPECT_NEAR(highest, 1.0 + mp * (1.0 + a), 1e-6) << overshoot << " " << anticipation;
            EXPECT_NEAR(easePrecise(1.0, t), 1.0, 1e-12);
            EXPECT_NEAR(ease(1.0f, t), 1.0f, 1e-6f);
            EXPECT_NEAR(easePrecise(1000.0, t), 1.0, 1e-6);   // tends to 1 (slowly for a large overshoot)
            if (overshoot <= 0.5f){
                EXPECT_NEAR(easePrecise(20.0, t), 1.0, 1e-3);
            }
            EXPECT_EQ(easePrecise(0.0, t), 0.0);
            EXPECT_EQ(easePrecise(-1.0, t), 0.0);
            if (anticipation > 0.0f){
                // continuous with zero slope at the wind-up junction p = 0.3
                const double h = 1e-6;
                EXPECT_NEAR(easePrecise(0.3 - h, t), -a, 1e-9);
                EXPECT_NEAR(easePrecise(0.3 + h, t), -a, 1e-9);
                EXPECT_NEAR((easePrecise(0.3 + h, t) - easePrecise(0.3 - h, t)) / (2 * h), 0.0, 1e-4);
            }
        }
    }
    // overshoot 0 means effectively none
    double highest = 0.0;
    for (int i = 0; i <= 3000; i++) highest = std::max(highest, easePrecise(i * 1e-3, spring(0.0f, 0.0f)));
    EXPECT_LT(highest, 1.0015);
}

TEST(PoseTween, LinearAndSmoothIgnoreSpringValues){
    for (TransitionCurve curve : {TransitionCurve::Linear, TransitionCurve::Smooth}){
        Transition a = spring(0.0f, 0.0f), b = spring(0.8f, 0.4f);
        a.curve = b.curve = curve;
        for (double x = -0.5; x <= 2.0; x += 0.01){
            EXPECT_EQ(easePrecise(x, a), easePrecise(x, b));
            const double v = easePrecise(x, a);
            EXPECT_GE(v, 0.0);
            EXPECT_LE(v, 1.0);
        }
        EXPECT_EQ(easePrecise(1.0, a), 1.0);
        EXPECT_EQ(easePrecise(3.0, a), 1.0);
    }
    Transition linear;
    linear.curve = TransitionCurve::Linear;
    EXPECT_DOUBLE_EQ(easePrecise(0.25, linear), 0.25);
    Transition smooth;
    smooth.curve = TransitionCurve::Smooth;
    EXPECT_DOUBLE_EQ(easePrecise(0.5, smooth), 0.5);
    EXPECT_LT(easePrecise(0.1, smooth), 0.1);
}

TEST(PoseTween, Loosen){
    const Transition t = loosen(spring(0.25f, 0.06f), 0.5f);
    EXPECT_FLOAT_EQ(t.duration, 0.12f * 1.75f);
    EXPECT_FLOAT_EQ(t.overshoot, 0.25f + (0.7f - 0.25f) * 0.5f);
    EXPECT_FLOAT_EQ(t.anticipation, 0.06f);
}

TEST(PoseTween, FeelPresets){
    const std::vector<FeelPreset>& presets = feelPresets();
    ASSERT_EQ(presets.size(), 7u);
    const char* names[] = {"Snappy", "Cartoon", "Rubbery", "Stop-motion", "Heavy", "Linear", "Soft"};
    for (size_t i = 0; i < presets.size(); i++){
        EXPECT_STREQ(presets[i].name, names[i]);
    }
    const FeelPreset* stop = findFeelPreset("stop_motion");
    ASSERT_NE(stop, nullptr);
    EXPECT_STREQ(stop->name, "Stop-motion");
    EXPECT_EQ(stop->stepRate, 12.0f);
    EXPECT_EQ(findFeelPreset("CARTOON")->transition.anticipation, 0.12f);
    EXPECT_EQ(findFeelPreset("Soft")->transition.curve, TransitionCurve::Smooth);
    EXPECT_EQ(findFeelPreset("Linear")->transition.curve, TransitionCurve::Linear);
    EXPECT_EQ(findFeelPreset("pixar"), nullptr);

    // the default settings are Snappy
    PoseAnimationSettings settings;
    ASSERT_NE(matchingFeelPreset(settings), nullptr);
    EXPECT_STREQ(matchingFeelPreset(settings)->name, "Snappy");
    applyFeelPreset(*findFeelPreset("Heavy"), settings);
    EXPECT_STREQ(matchingFeelPreset(settings)->name, "Heavy");
    settings.overlap += 0.00009f;
    EXPECT_TRUE(feelPresetMatches(*findFeelPreset("Heavy"), settings));
    settings.overlap += 0.0002f;
    EXPECT_FALSE(feelPresetMatches(*findFeelPreset("Heavy"), settings));
    EXPECT_EQ(matchingFeelPreset(settings), nullptr);
}
