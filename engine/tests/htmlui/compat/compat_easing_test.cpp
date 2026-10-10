// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Easing (phase-2 §3.9.7 "Transitions"): RmlUi's tween formulas, the keyword table against
// nearestTween, nearestTween against brute force, cubic-bezier and linear() curves.

#include "htmlui/compat/css/Easing.h"

#include <RmlUi/Core/Tween.h>
#include <RmlUi/Core/StringUtilities.h>

#include <gtest/gtest.h>

#include <cmath>
#include <string>

using namespace jasperengine::htmlui::compat;

namespace {

    // RmlUi's own Tween for a keyword
    Rml::Tween rmluiTween(const std::string& name){
        static const std::pair<const char*, Rml::Tween::Type> types[] = {
            {"back", Rml::Tween::Back}, {"bounce", Rml::Tween::Bounce}, {"circular", Rml::Tween::Circular}, {"cubic", Rml::Tween::Cubic},
            {"elastic", Rml::Tween::Elastic}, {"exponential", Rml::Tween::Exponential}, {"linear", Rml::Tween::Linear},
            {"quadratic", Rml::Tween::Quadratic}, {"quartic", Rml::Tween::Quartic}, {"quintic", Rml::Tween::Quintic}, {"sine", Rml::Tween::Sine}};
        for (const auto& t : types){
            const std::string prefix = std::string(t.first) + "-";
            if (name.compare(0, prefix.size(), prefix) != 0)
                continue;
            const std::string mode = name.substr(prefix.size());
            if (mode == "in")
                return Rml::Tween(t.second, Rml::Tween::In);
            if (mode == "out")
                return Rml::Tween(t.second, Rml::Tween::Out);
            return Rml::Tween(t.second, Rml::Tween::InOut);
        }
        return Rml::Tween();
    }

}

TEST(CompatEasing, TweensMatchRmlUi){
    EXPECT_EQ(Easing::tweenNames().size(), 33u);
    for (const char* name : Easing::tweenNames()){
        Rml::Tween tween = rmluiTween(name);
        for (int i = 0; i <= 100; i++){
            const double t = i / 100.0;
            EXPECT_NEAR(Easing::tween(name, t), tween((float)t), 1e-4) << name << " at " << t;
        }
    }
}

TEST(CompatEasing, KeywordTableIsTheNearestTween){
    const std::pair<const char*, double> measured[] = {{"ease", 0.098}, {"ease-in", 0.024}, {"ease-out", 0.024}, {"ease-in-out", 0.012}, {"linear", 0.0}};
    for (const auto& m : measured){
        const std::array<double, 4> p = *Easing::keywordBezier(m.first);
        auto curve = [&](double t){ return Easing::cubicBezier(p[0], p[1], p[2], p[3], t); };
        const TweenChoice choice = Easing::nearestTween(curve);
        EXPECT_EQ(choice.name, *Easing::keywordTween(m.first)) << m.first;
        EXPECT_NEAR(choice.error, m.second, 0.001) << m.first;
    }
    EXPECT_FALSE(Easing::keywordTween("bounce").has_value());
}

TEST(CompatEasing, NearestTweenIsTheBruteForceMinimum){
    const double curves[][4] = {{0.68, -0.55, 0.265, 1.55}, {0.17, 0.67, 0.83, 0.67}, {0.4, 0, 0.2, 1}, {0, 0, 1, 1}, {0.9, 0.1, 0.1, 0.9}};
    for (const auto& c : curves){
        auto curve = [&](double t){ return Easing::cubicBezier(c[0], c[1], c[2], c[3], t); };
        const TweenChoice choice = Easing::nearestTween(curve);
        double best = 1e9;
        for (const char* name : Easing::tweenNames()){
            double worst = 0;
            for (int i = 0; i <= 100; i++){
                const double t = i / 100.0;
                worst = std::max(worst, std::fabs(curve(t) - Easing::tween(name, t)));
            }
            best = std::min(best, worst);
        }
        EXPECT_NEAR(choice.error, best, 1e-12);
        EXPECT_NEAR(Easing::maxError(curve, choice.name), best, 1e-12);
    }
}

TEST(CompatEasing, Curves){
    // cubic-bezier: end points, symmetry of ease-in-out, a known value of ease
    EXPECT_DOUBLE_EQ(Easing::cubicBezier(0.25, 0.1, 0.25, 1, 0), 0);
    EXPECT_DOUBLE_EQ(Easing::cubicBezier(0.25, 0.1, 0.25, 1, 1), 1);
    EXPECT_NEAR(Easing::cubicBezier(0.42, 0, 0.58, 1, 0.5), 0.5, 1e-6);
    EXPECT_NEAR(Easing::cubicBezier(0.25, 0.1, 0.25, 1, 0.5), 0.8024, 1e-3);
    EXPECT_NEAR(Easing::cubicBezier(0, 0, 1, 1, 0.3), 0.3, 1e-6);
    // linear(): piecewise
    const std::vector<std::pair<double, double>> points{{0, 0}, {0.75, 0.25}, {1, 1}};
    EXPECT_NEAR(Easing::linearFunction(points, 0.375), 0.125, 1e-9);
    EXPECT_NEAR(Easing::linearFunction(points, 0.875), 0.625, 1e-9);
    EXPECT_DOUBLE_EQ(Easing::linearFunction(points, 2), 1);
}
