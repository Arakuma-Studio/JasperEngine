// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_PARTICLEVALUES_H
#define PARTICLES_PARTICLEVALUES_H

// The value types of particle effect parameters (docs/specs/S20-particles.md R2), with Unity's
// names: a MinMaxCurve is a constant, a random value between two constants, a curve or a random
// value between two curves; a MinMaxGradient the same over colours. Modes, drivers and wraps
// follow OpenEngine's ParticleValueMode / ParticleDriver / ParticleWrap.

#include "math/Curve.h"
#include "math/Vector4.h"

#include <cstdint>

namespace jasperengine { namespace particles {

    enum class CurveMode : uint8_t {
        Constant,
        TwoConstants,   // Random Between Two Constants
        Curve,
        TwoCurves,      // Random Between Two Curves
        // from a start value to an end value along an ease preset: an ordinary curve (curve)
        // made by Curve::fromEase, evaluated as one, with its start (constant), end
        // (constantMax) and preset (ease) kept so it can be made again
        Ease
    };

    enum class GradientValueMode : uint8_t {
        Color,
        Gradient,
        TwoColors,      // Random Between Two Colors
        TwoGradients,   // Random Between Two Gradients
        RandomColor     // the gradient at a random point per particle (Unity's Random Color)
    };

    // What a curve or gradient is evaluated at
    enum class CurveDriver : uint8_t {
        NormalizedAge,  // age / lifetime: 0 at birth, 1 at death
        Age,            // seconds since birth
        Speed,          // the particle's speed (units per second)
        EmitterTime,    // the emitter's time over its duration (0 to 1)
        Random          // a value in [0, 1) fixed per particle
    };

    // How a driver value outside [driverMin, driverMax] maps into the curve's [0, 1]
    enum class CurveWrap : uint8_t {
        Clamp,
        Repeat,
        PingPong
    };

    struct MinMaxCurve {
        CurveMode mode = CurveMode::Constant;
        float constant = 0.0f;      // Constant, and the first of Two Constants
        float constantMax = 0.0f;   // the second of Two Constants
        Curve curve;                // Curve, and the first of Two Curves (time 0 to 1)
        Curve curveMax;             // the second of Two Curves
        float multiplier = 1.0f;    // scales the curves (Unity's curve multiplier)
        CurveDriver driver = CurveDriver::NormalizedAge;
        float driverMin = 0.0f;     // driver values at the curve's 0 and 1
        float driverMax = 1.0f;
        CurveWrap wrap = CurveWrap::Clamp;
        EaseType ease = static_cast<EaseType>(0);   // Ease: the preset (LINEAR by default)

        MinMaxCurve() = default;
        MinMaxCurve(float value): constant(value), constantMax(value) {}
        static MinMaxCurve twoConstants(float lo, float hi){
            MinMaxCurve value(lo);
            value.mode = CurveMode::TwoConstants;
            value.constantMax = hi;
            return value;
        }
        static MinMaxCurve fromCurve(Curve c, float multiplier = 1.0f){
            MinMaxCurve value;
            value.mode = CurveMode::Curve;
            value.curve = std::move(c);
            value.multiplier = multiplier;
            return value;
        }
        // Ease mode from `start` to `end` along `type` (CUSTOM and CURVE ease linearly)
        static MinMaxCurve fromEase(EaseType type, float start, float end);
        // Ease mode: makes the curve again from the start, end and preset
        void rebuildEase();
        // the preset's keys from `start` to `end` over [0, 1]
        static Curve easeCurve(EaseType type, float start, float end);

        bool usesCurve() const { return mode == CurveMode::Curve || mode == CurveMode::TwoCurves || mode == CurveMode::Ease; }
        bool usesRandom() const { return mode == CurveMode::TwoConstants || mode == CurveMode::TwoCurves; }
        // the same value for every particle at every age
        bool isConstant() const { return mode == CurveMode::Constant || (mode == CurveMode::TwoConstants && constant == constantMax); }
        // a constant zero (or no curve keys)
        bool isZero() const;
        // what the value is at `input` (the curve's [0, 1]) with the particle's random pick
        float evaluate(float input, float random) const;
        // the value range over every input and random pick
        void range(float& lo, float& hi) const;
        // maps a raw driver value through driverMin/Max and the wrap into [0, 1]
        float remap(float driverValue) const;

        bool operator==(const MinMaxCurve& other) const;
        bool operator!=(const MinMaxCurve& other) const { return !(*this == other); }
    };

    struct MinMaxGradient {
        GradientValueMode mode = GradientValueMode::Color;
        Vector4 color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);      // sRGB rgb, alpha
        Vector4 colorMax = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        Gradient gradient;
        Gradient gradientMax;
        CurveDriver driver = CurveDriver::NormalizedAge;
        float driverMin = 0.0f;
        float driverMax = 1.0f;
        CurveWrap wrap = CurveWrap::Clamp;

        MinMaxGradient() = default;
        explicit MinMaxGradient(const Vector4& c): color(c), colorMax(c) {}

        bool usesGradient() const { return mode == GradientValueMode::Gradient || mode == GradientValueMode::TwoGradients || mode == GradientValueMode::RandomColor; }
        bool usesRandom() const { return mode == GradientValueMode::TwoColors || mode == GradientValueMode::TwoGradients; }
        bool isWhite() const;
        // linear RGBA at `input` with the random pick
        Vector4 evaluate(float input, float random) const;
        float remap(float driverValue) const;

        bool operator==(const MinMaxGradient& other) const;
        bool operator!=(const MinMaxGradient& other) const { return !(*this == other); }
    };

    // the remap shared by both value kinds
    float remapDriver(float value, float lo, float hi, CurveWrap wrap);

}}

#endif // PARTICLES_PARTICLEVALUES_H
