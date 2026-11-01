// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_MINMAX_H
#define PARTICLES_MINMAX_H

// Unity's ParticleSystem.MinMaxCurve and MinMaxGradient (particles spec 6.1): a value that is
// a constant, a curve, random between two constants or two curves (and for colours a colour,
// a gradient, two colours, two gradients or a random colour of a gradient). `t` is the
// normalized time the module evaluates at (a particle's age / lifetime, or the system's time /
// duration), `random01` the particle's (or the system's) random number for this value, drawn
// once so a particle keeps one blend for its life. Pure, gtested (editor/tests/MinMaxTest.cpp).

#include "Export.h"
#include "math/AnimationCurve.h"
#include "math/Gradient.h"
#include "math/Vector4.h"

#include <cstdint>

namespace jasperengine {

    // Unity ParticleSystemCurveMode (minMaxState 0, 1, 2, 3)
    enum class ParticleSystemCurveMode : uint8_t { Constant = 0, Curve = 1, TwoCurves = 2, TwoConstants = 3 };

    struct JASPERENGINE_API MinMaxCurve {
        ParticleSystemCurveMode mode = ParticleSystemCurveMode::Constant;
        // Constant: the value; TwoConstants: the maximum; Curve / TwoCurves: the multiplier
        // (Unity's "scalar" / curveMultiplier)
        float constant = 0;
        // TwoConstants: the minimum (it may exceed `constant`: the lerp order is kept, as Unity)
        float constantMin = 0;
        // Curve: curve; TwoCurves: curveMin .. curve (Unity's minCurve .. maxCurve)
        AnimationCurve curve;
        AnimationCurve curveMin;

        MinMaxCurve() = default;
        // a constant
        MinMaxCurve(float value);
        // random between two constants (Unity's MinMaxCurve(min, max))
        MinMaxCurve(float min, float max);
        // a curve times a multiplier
        MinMaxCurve(float multiplier, const AnimationCurve& curve);
        // random between two curves, times a multiplier
        MinMaxCurve(float multiplier, const AnimationCurve& min, const AnimationCurve& max);

        // Constant -> constant; Curve -> curve(t) x constant; TwoCurves -> lerp(curveMin(t),
        // curve(t), random01) x constant; TwoConstants -> lerp(constantMin, constant, random01)
        float evaluate(float t, float random01 = 1.0f) const;

        // the largest value over t in [0, 1] (any random): the editor's range and the simulation's
        // bounds
        float maxValue() const;
        float minValue() const;
        // a value that does not depend on t
        bool isTimeIndependent() const;
        // Unity's curveMultiplier / constantMax / constant: the field the multiplier names write
        float getMultiplier() const { return constant; }

        static MinMaxCurve value(float v);
        static MinMaxCurve between(float a, float b);
        static MinMaxCurve fromCurve(const AnimationCurve& c, float multiplier = 1.0f);
        static MinMaxCurve between(const AnimationCurve& a, const AnimationCurve& b, float multiplier = 1.0f);

        bool operator==(const MinMaxCurve& other) const;
        bool operator!=(const MinMaxCurve& other) const { return !(*this == other); }
    };

    // Unity ParticleSystemGradientMode (minMaxState 0..4)
    enum class ParticleSystemGradientMode : uint8_t { Color = 0, Gradient = 1, TwoColors = 2, TwoGradients = 3, RandomColor = 4 };

    struct JASPERENGINE_API MinMaxGradient {
        ParticleSystemGradientMode mode = ParticleSystemGradientMode::Color;
        // linear RGBA (TwoColors: colorMin .. color)
        Vector4 color = Vector4(1, 1, 1, 1);
        Vector4 colorMin = Vector4(1, 1, 1, 1);
        // Gradient / RandomColor: gradient; TwoGradients: gradientMin .. gradient
        jasperengine::Gradient gradient;
        jasperengine::Gradient gradientMin;

        MinMaxGradient() = default;
        MinMaxGradient(const Vector4& color);

        // Color -> color; Gradient -> gradient(t); TwoColors -> lerp per channel; TwoGradients ->
        // lerp(gradientMin(t), gradient(t), random01); RandomColor -> gradient(random01)
        Vector4 evaluate(float t, float random01 = 1.0f) const;
        bool isTimeIndependent() const;

        static MinMaxGradient fromColor(const Vector4& c);
        static MinMaxGradient between(const Vector4& a, const Vector4& b);
        static MinMaxGradient fromGradient(const jasperengine::Gradient& g);
        static MinMaxGradient between(const jasperengine::Gradient& a, const jasperengine::Gradient& b);

        bool operator==(const MinMaxGradient& other) const;
        bool operator!=(const MinMaxGradient& other) const { return !(*this == other); }
    };

    // A MinMaxCurve baked for the simulation's inner loop (spec 8: no Hermite per particle per
    // frame): the curves sampled at LUT_SIZE + 1 even steps of t in [0, 1], read with a linear
    // blend. Constant modes keep their exact values.
    struct JASPERENGINE_API BakedMinMaxCurve {
        static constexpr int LUT_SIZE = 64;

        ParticleSystemCurveMode mode = ParticleSystemCurveMode::Constant;
        float constant = 0;
        float constantMin = 0;
        float lutMax[LUT_SIZE + 1] = {};
        float lutMin[LUT_SIZE + 1] = {};

        void bake(const MinMaxCurve& curve);
        float evaluate(float t, float random01) const;
    };

}

#endif //PARTICLES_MINMAX_H
