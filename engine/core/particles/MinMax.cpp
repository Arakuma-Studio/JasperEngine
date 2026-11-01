// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MinMax.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    float lerp(float a, float b, float f){
        return a + (b - a) * f;
    }

    float clamp01(float t){
        if (!(t >= 0.0f)) return 0.0f;   // NaN too
        return t > 1.0f ? 1.0f : t;
    }

}

MinMaxCurve::MinMaxCurve(float value): constant(value), constantMin(value){
}

MinMaxCurve::MinMaxCurve(float min, float max):
    mode(ParticleSystemCurveMode::TwoConstants), constant(max), constantMin(min){
}

MinMaxCurve::MinMaxCurve(float multiplier, const AnimationCurve& curve):
    mode(ParticleSystemCurveMode::Curve), constant(multiplier), curve(curve){
}

MinMaxCurve::MinMaxCurve(float multiplier, const AnimationCurve& min, const AnimationCurve& max):
    mode(ParticleSystemCurveMode::TwoCurves), constant(multiplier), curve(max), curveMin(min){
}

float MinMaxCurve::evaluate(float t, float random01) const{
    switch (mode){
        case ParticleSystemCurveMode::Constant:
            return constant;
        case ParticleSystemCurveMode::TwoConstants:
            return lerp(constantMin, constant, random01);
        case ParticleSystemCurveMode::Curve:
            return curve.evaluate(t) * constant;
        case ParticleSystemCurveMode::TwoCurves:
            return lerp(curveMin.evaluate(t), curve.evaluate(t), random01) * constant;
    }
    return constant;
}

float MinMaxCurve::maxValue() const{
    switch (mode){
        case ParticleSystemCurveMode::Constant:
            return constant;
        case ParticleSystemCurveMode::TwoConstants:
            return std::max(constant, constantMin);
        case ParticleSystemCurveMode::Curve:
        case ParticleSystemCurveMode::TwoCurves: {
            float lo, hi;
            curve.valueRange(0.0f, 1.0f, lo, hi);
            if (mode == ParticleSystemCurveMode::TwoCurves){
                float lo2, hi2;
                curveMin.valueRange(0.0f, 1.0f, lo2, hi2);
                lo = std::min(lo, lo2);
                hi = std::max(hi, hi2);
            }
            return std::max(lo * constant, hi * constant);
        }
    }
    return constant;
}

float MinMaxCurve::minValue() const{
    switch (mode){
        case ParticleSystemCurveMode::Constant:
            return constant;
        case ParticleSystemCurveMode::TwoConstants:
            return std::min(constant, constantMin);
        case ParticleSystemCurveMode::Curve:
        case ParticleSystemCurveMode::TwoCurves: {
            float lo, hi;
            curve.valueRange(0.0f, 1.0f, lo, hi);
            if (mode == ParticleSystemCurveMode::TwoCurves){
                float lo2, hi2;
                curveMin.valueRange(0.0f, 1.0f, lo2, hi2);
                lo = std::min(lo, lo2);
                hi = std::max(hi, hi2);
            }
            return std::min(lo * constant, hi * constant);
        }
    }
    return constant;
}

bool MinMaxCurve::isTimeIndependent() const{
    return mode == ParticleSystemCurveMode::Constant || mode == ParticleSystemCurveMode::TwoConstants;
}

MinMaxCurve MinMaxCurve::value(float v){
    return MinMaxCurve(v);
}

MinMaxCurve MinMaxCurve::between(float a, float b){
    MinMaxCurve c;
    c.mode = ParticleSystemCurveMode::TwoConstants;
    c.constantMin = a;
    c.constant = b;
    return c;
}

MinMaxCurve MinMaxCurve::fromCurve(const AnimationCurve& curve, float multiplier){
    MinMaxCurve c;
    c.mode = ParticleSystemCurveMode::Curve;
    c.constant = multiplier;
    c.curve = curve;
    return c;
}

MinMaxCurve MinMaxCurve::between(const AnimationCurve& a, const AnimationCurve& b, float multiplier){
    MinMaxCurve c;
    c.mode = ParticleSystemCurveMode::TwoCurves;
    c.constant = multiplier;
    c.curveMin = a;
    c.curve = b;
    return c;
}

bool MinMaxCurve::operator==(const MinMaxCurve& other) const{
    return mode == other.mode && constant == other.constant && constantMin == other.constantMin &&
           curve == other.curve && curveMin == other.curveMin;
}

MinMaxGradient::MinMaxGradient(const Vector4& color): color(color), colorMin(color){
}

Vector4 MinMaxGradient::evaluate(float t, float random01) const{
    switch (mode){
        case ParticleSystemGradientMode::Color:
            return color;
        case ParticleSystemGradientMode::Gradient:
            return gradient.evaluate(t);
        case ParticleSystemGradientMode::TwoColors:
            return colorMin + (color - colorMin) * random01;
        case ParticleSystemGradientMode::TwoGradients: {
            const Vector4 a = gradientMin.evaluate(t);
            return a + (gradient.evaluate(t) - a) * random01;
        }
        case ParticleSystemGradientMode::RandomColor:
            return gradient.evaluate(random01);
    }
    return color;
}

bool MinMaxGradient::isTimeIndependent() const{
    return mode == ParticleSystemGradientMode::Color || mode == ParticleSystemGradientMode::TwoColors ||
           mode == ParticleSystemGradientMode::RandomColor;
}

MinMaxGradient MinMaxGradient::fromColor(const Vector4& c){
    return MinMaxGradient(c);
}

MinMaxGradient MinMaxGradient::between(const Vector4& a, const Vector4& b){
    MinMaxGradient g;
    g.mode = ParticleSystemGradientMode::TwoColors;
    g.colorMin = a;
    g.color = b;
    return g;
}

MinMaxGradient MinMaxGradient::fromGradient(const jasperengine::Gradient& gradient){
    MinMaxGradient g;
    g.mode = ParticleSystemGradientMode::Gradient;
    g.gradient = gradient;
    return g;
}

MinMaxGradient MinMaxGradient::between(const jasperengine::Gradient& a, const jasperengine::Gradient& b){
    MinMaxGradient g;
    g.mode = ParticleSystemGradientMode::TwoGradients;
    g.gradientMin = a;
    g.gradient = b;
    return g;
}

bool MinMaxGradient::operator==(const MinMaxGradient& other) const{
    return mode == other.mode && color == other.color && colorMin == other.colorMin &&
           gradient == other.gradient && gradientMin == other.gradientMin;
}

void BakedMinMaxCurve::bake(const MinMaxCurve& source){
    mode = source.mode;
    constant = source.constant;
    constantMin = source.constantMin;
    if (mode == ParticleSystemCurveMode::Curve || mode == ParticleSystemCurveMode::TwoCurves){
        for (int i = 0; i <= LUT_SIZE; i++){
            const float t = (float)i / (float)LUT_SIZE;
            lutMax[i] = source.curve.evaluate(t) * source.constant;
            lutMin[i] = (mode == ParticleSystemCurveMode::TwoCurves) ? source.curveMin.evaluate(t) * source.constant : lutMax[i];
        }
    }
}

float BakedMinMaxCurve::evaluate(float t, float random01) const{
    switch (mode){
        case ParticleSystemCurveMode::Constant:
            return constant;
        case ParticleSystemCurveMode::TwoConstants:
            return lerp(constantMin, constant, random01);
        case ParticleSystemCurveMode::Curve:
        case ParticleSystemCurveMode::TwoCurves: {
            const float x = clamp01(t) * (float)LUT_SIZE;
            int i = (int)x;
            if (i >= LUT_SIZE) i = LUT_SIZE - 1;
            const float f = x - (float)i;
            const float hi = lerp(lutMax[i], lutMax[i + 1], f);
            if (mode == ParticleSystemCurveMode::Curve) return hi;
            return lerp(lerp(lutMin[i], lutMin[i + 1], f), hi, random01);
        }
    }
    return constant;
}
