// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The driver remap follows OpenEngine's RemapParticleInput (Engine/Modules/Particles/Source/
// ParticleValue.cpp), Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT
// License. See NOTICE.md.

#include "ParticleValues.h"

#include "pixelart/Palette.h"
#include "pixelart/PaletteRampTexture.h"
#include "pixelart/PixelColor.h"
#include "util/Color.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::particles;

float particles::remapDriver(float value, float lo, float hi, CurveWrap wrap){
    const float span = hi - lo;
    float normalized = span != 0.0f ? (value - lo) / span : 0.0f;
    switch (wrap){
        case CurveWrap::Repeat:
            return normalized - std::floor(normalized);
        case CurveWrap::PingPong:
            normalized -= std::floor(normalized * 0.5f) * 2.0f;
            return normalized > 1.0f ? 2.0f - normalized : normalized;
        case CurveWrap::Clamp:
        default:
            return std::clamp(normalized, 0.0f, 1.0f);
    }
}

namespace {

    // a preset's curve over [0, 1], fitted once (Curve::fromEase fits it to a tolerance, far too
    // slow for a value a script or a drag writes every frame)
    const Curve& unitEase(EaseType type){
        static std::mutex lock;
        static std::array<std::unique_ptr<const Curve>, 64> curves;
        const int value = static_cast<int>(type);
        const size_t index = value >= 0 && value < (int)curves.size() ? (size_t)value : 0u;
        std::lock_guard<std::mutex> guard(lock);
        if (!curves[index])
            curves[index] = std::make_unique<const Curve>(Curve::fromEase(index == (size_t)value ? type : static_cast<EaseType>(0)));
        return *curves[index];
    }

    // the unit curve from `start` to `end` (its keys reuse `out`'s storage)
    void scaleEase(const Curve& unit, float start, float end, Curve& out){
        out.keys.assign(unit.keys.begin(), unit.keys.end());
        out.preWrapMode = unit.preWrapMode;
        out.postWrapMode = unit.postWrapMode;
        const float span = end - start;
        for (CurveKey& key : out.keys){
            key.value = start + span * key.value;
            // a step (an infinite tangent) stays one whatever the span's sign
            if (std::isfinite(key.inTangent)) key.inTangent *= span;
            if (std::isfinite(key.outTangent)) key.outTangent *= span;
        }
    }

}

Curve MinMaxCurve::easeCurve(EaseType type, float start, float end){
    Curve curve;
    scaleEase(unitEase(type), start, end, curve);
    return curve;
}

MinMaxCurve MinMaxCurve::fromEase(EaseType type, float start, float end){
    MinMaxCurve value;
    value.mode = CurveMode::Ease;
    value.ease = type;
    value.constant = start;
    value.constantMax = end;
    value.rebuildEase();
    return value;
}

void MinMaxCurve::rebuildEase(){
    scaleEase(unitEase(ease), constant, constantMax, curve);
}

bool MinMaxCurve::isZero() const{
    switch (mode){
        case CurveMode::Constant: return constant == 0.0f;
        case CurveMode::TwoConstants: return constant == 0.0f && constantMax == 0.0f;
        case CurveMode::Ease:
        case CurveMode::Curve: {
            if (multiplier == 0.0f || curve.empty()) return true;
            float lo, hi;
            curve.valueRange(lo, hi);
            return lo == 0.0f && hi == 0.0f;
        }
        case CurveMode::TwoCurves: {
            if (multiplier == 0.0f) return true;
            float lo0, hi0, lo1, hi1;
            curve.valueRange(lo0, hi0);
            curveMax.valueRange(lo1, hi1);
            return lo0 == 0.0f && hi0 == 0.0f && lo1 == 0.0f && hi1 == 0.0f;
        }
    }
    return true;
}

float MinMaxCurve::evaluate(float input, float random) const{
    switch (mode){
        case CurveMode::Constant: return constant;
        case CurveMode::TwoConstants: return constant + (constantMax - constant) * random;
        case CurveMode::Ease:
        case CurveMode::Curve: return curve.evaluate(input) * multiplier;
        case CurveMode::TwoCurves: {
            const float a = curve.evaluate(input);
            const float b = curveMax.evaluate(input);
            return (a + (b - a) * random) * multiplier;
        }
    }
    return constant;
}

void MinMaxCurve::range(float& lo, float& hi) const{
    switch (mode){
        case CurveMode::Constant:
            lo = hi = constant;
            return;
        case CurveMode::TwoConstants:
            lo = std::min(constant, constantMax);
            hi = std::max(constant, constantMax);
            return;
        case CurveMode::Ease:
        case CurveMode::Curve:
            curve.valueRange(lo, hi);
            break;
        case CurveMode::TwoCurves: {
            float lo1, hi1;
            curve.valueRange(lo, hi);
            curveMax.valueRange(lo1, hi1);
            lo = std::min(lo, lo1);
            hi = std::max(hi, hi1);
            break;
        }
    }
    lo *= multiplier;
    hi *= multiplier;
    if (lo > hi) std::swap(lo, hi);
}

float MinMaxCurve::remap(float driverValue) const{
    return remapDriver(driverValue, driverMin, driverMax, wrap);
}

bool MinMaxCurve::operator==(const MinMaxCurve& other) const{
    return mode == other.mode && constant == other.constant && constantMax == other.constantMax && curve == other.curve &&
           curveMax == other.curveMax && multiplier == other.multiplier && driver == other.driver &&
           driverMin == other.driverMin && driverMax == other.driverMax && wrap == other.wrap &&
           (mode != CurveMode::Ease || ease == other.ease);
}

bool MinMaxGradient::isWhite() const{
    const Vector4 white(1.0f, 1.0f, 1.0f, 1.0f);
    switch (mode){
        case GradientValueMode::Ramp: return false;
        case GradientValueMode::Color: return color == white;
        case GradientValueMode::TwoColors: return color == white && colorMax == white;
        default: break;
    }
    auto whiteGradient = [](const Gradient& g){
        for (const GradientColorKey& key : g.colorKeys){
            if (key.color != Vector3(1.0f, 1.0f, 1.0f)) return false;
        }
        for (const GradientAlphaKey& key : g.alphaKeys){
            if (key.alpha != 1.0f) return false;
        }
        return true;
    };
    if (mode == GradientValueMode::Gradient || mode == GradientValueMode::RandomColor) return whiteGradient(gradient);
    return whiteGradient(gradient) && whiteGradient(gradientMax);
}

Vector4 MinMaxGradient::evaluate(float input, float random) const{
    switch (mode){
        case GradientValueMode::Color:
            return Color::sRGBToLinear(color);
        case GradientValueMode::TwoColors:
            return Color::sRGBToLinear(color + (colorMax - color) * random);
        case GradientValueMode::Gradient:
            return gradient.evaluate(input);
        case GradientValueMode::RandomColor:
            // the particle's random pick is where it reads the gradient
            return gradient.evaluate(random);
        case GradientValueMode::TwoGradients: {
            const Vector4 a = gradient.evaluate(input);
            const Vector4 b = gradientMax.evaluate(input);
            return a + (b - a) * random;
        }
        case GradientValueMode::Ramp: {
            const std::vector<pixelart::Rgba8> colors = resolveRamp();
            if (colors.empty())
                return Vector4(1.0f, 1.0f, 1.0f, 1.0f);
            return pixelart::toEngineLinear(colors[(size_t)rampStep(input, (int)colors.size())]);
        }
    }
    return Vector4(1.0f, 1.0f, 1.0f, 1.0f);
}

int MinMaxGradient::rampStep(float input, int count) const{
    if (count <= 0)
        return 0;
    float t = std::isfinite(input) ? std::clamp(input, 0.0f, 1.0f) : 0.0f;
    if (rampBias != 1.0f && t > 0.0f)
        t = std::pow(t, std::clamp(rampBias, 0.1f, 10.0f));
    // pixelfx's at(): floor(t n), the last step for t = 1
    const int step = std::clamp((int)std::floor(t * (float)count), 0, count - 1);
    return rampReverse ? count - 1 - step : step;
}

std::vector<pixelart::Rgba8> MinMaxGradient::resolveRamp() const{
    std::vector<pixelart::Rgba8> colors;
    std::string palette, name;
    if (!ramp.empty() && pixelart::parsePaletteRampPath(ramp, &palette, &name)){
        if (std::shared_ptr<const pixelart::Palette> p = pixelart::PaletteLibrary::get(palette))
            colors = p->rampColors(name);
    }else if (ramp.empty()){
        colors = rampColors;
    }
    if (rampSteps >= 2 && !colors.empty())
        colors = pixelart::fitRamp(colors, std::min(rampSteps, kMaxRampColors));
    if ((int)colors.size() > kMaxRampColors)
        colors.resize((size_t)kMaxRampColors);
    return colors;
}

float MinMaxGradient::remap(float driverValue) const{
    return remapDriver(driverValue, driverMin, driverMax, wrap);
}

bool MinMaxGradient::operator==(const MinMaxGradient& other) const{
    return mode == other.mode && color == other.color && colorMax == other.colorMax && gradient == other.gradient &&
           gradientMax == other.gradientMax && driver == other.driver && driverMin == other.driverMin &&
           driverMax == other.driverMax && wrap == other.wrap && ramp == other.ramp && rampColors == other.rampColors &&
           rampSteps == other.rampSteps && rampReverse == other.rampReverse && rampBias == other.rampBias;
}
