// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Curve.h"

#include "util/Color.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    // the segment [keys[i], keys[i + 1]] that holds time (keys has at least two keys and time
    // lies strictly between the first and the last)
    size_t findSegment(const std::vector<CurveKey>& keys, float time){
        // a handful of keys: a linear scan beats a binary search
        size_t i = 0;
        while (i + 2 < keys.size() && keys[i + 1].time <= time){
            i++;
        }
        return i;
    }

}

float Curve::evaluate(float time) const{
    if (keys.empty())
        return 0.0f;
    if (keys.size() == 1 || !(time > keys.front().time))
        return keys.front().value;
    if (time >= keys.back().time)
        return keys.back().value;

    const size_t i = findSegment(keys, time);
    const CurveKey& a = keys[i];
    const CurveKey& b = keys[i + 1];
    const float span = b.time - a.time;
    if (span <= 0.0f)
        return b.value;
    if (!std::isfinite(a.outTangent) || !std::isfinite(b.inTangent))
        return a.value;

    const float t = (time - a.time) / span;
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const float h10 = t3 - 2.0f * t2 + t;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    const float h11 = t3 - t2;
    return h00 * a.value + h10 * span * a.outTangent + h01 * b.value + h11 * span * b.inTangent;
}

float Curve::derivative(float time) const{
    if (keys.size() < 2 || !(time > keys.front().time) || time >= keys.back().time)
        return 0.0f;
    const size_t i = findSegment(keys, time);
    const CurveKey& a = keys[i];
    const CurveKey& b = keys[i + 1];
    const float span = b.time - a.time;
    if (span <= 0.0f || !std::isfinite(a.outTangent) || !std::isfinite(b.inTangent))
        return 0.0f;
    const float t = (time - a.time) / span;
    const float t2 = t * t;
    const float d00 = 6.0f * t2 - 6.0f * t;
    const float d10 = 3.0f * t2 - 4.0f * t + 1.0f;
    const float d01 = -6.0f * t2 + 6.0f * t;
    const float d11 = 3.0f * t2 - 2.0f * t;
    return (d00 * a.value + d01 * b.value) / span + d10 * a.outTangent + d11 * b.inTangent;
}

void Curve::sortKeys(){
    std::stable_sort(keys.begin(), keys.end(), [](const CurveKey& a, const CurveKey& b){ return a.time < b.time; });
}

void Curve::valueRange(float& lo, float& hi) const{
    if (keys.empty()){
        lo = hi = 0.0f;
        return;
    }
    lo = hi = keys.front().value;
    if (keys.size() == 1)
        return;
    // the keys and the Hermite extrema between them, sampled finely enough for a range
    const float start = keys.front().time;
    const float end = keys.back().time;
    constexpr int kSamples = 256;
    for (int i = 0; i <= kSamples; i++){
        const float v = evaluate(start + (end - start) * ((float)i / (float)kSamples));
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    for (const CurveKey& key : keys){
        lo = std::min(lo, key.value);
        hi = std::max(hi, key.value);
    }
}

Curve Curve::constant(float value){
    return Curve({{0.0f, value, 0.0f, 0.0f}});
}

Curve Curve::linear(float from, float to){
    const float slope = to - from;
    return Curve({{0.0f, from, slope, slope}, {1.0f, to, slope, slope}});
}

Curve Curve::polyline(const std::vector<std::pair<float, float>>& points){
    Curve curve;
    for (size_t i = 0; i < points.size(); i++){
        CurveKey key;
        key.time = points[i].first;
        key.value = points[i].second;
        if (i > 0){
            const float dt = points[i].first - points[i - 1].first;
            key.inTangent = dt > 0.0f ? (points[i].second - points[i - 1].second) / dt : 0.0f;
        }
        if (i + 1 < points.size()){
            const float dt = points[i + 1].first - points[i].first;
            key.outTangent = dt > 0.0f ? (points[i + 1].second - points[i].second) / dt : 0.0f;
        }
        if (i == 0) key.inTangent = key.outTangent;
        if (i + 1 == points.size()) key.outTangent = key.inTangent;
        curve.keys.push_back(key);
    }
    return curve;
}

Curve Curve::easeInOut(float timeStart, float valueStart, float timeEnd, float valueEnd){
    return Curve({{timeStart, valueStart, 0.0f, 0.0f}, {timeEnd, valueEnd, 0.0f, 0.0f}});
}

Vector3 Gradient::evaluateEncoded(float time) const{
    if (colorKeys.empty())
        return Vector3(1.0f, 1.0f, 1.0f);
    if (colorKeys.size() == 1 || !(time > colorKeys.front().time))
        return colorKeys.front().color;
    if (time >= colorKeys.back().time)
        return colorKeys.back().color;
    size_t i = 0;
    while (i + 2 < colorKeys.size() && colorKeys[i + 1].time < time){
        i++;
    }
    const GradientColorKey& a = colorKeys[i];
    const GradientColorKey& b = colorKeys[i + 1];
    if (mode == GradientMode::Fixed)
        return b.color;
    const float span = b.time - a.time;
    const float t = span > 0.0f ? (time - a.time) / span : 1.0f;
    return a.color + (b.color - a.color) * t;
}

float Gradient::evaluateAlpha(float time) const{
    if (alphaKeys.empty())
        return 1.0f;
    if (alphaKeys.size() == 1 || !(time > alphaKeys.front().time))
        return alphaKeys.front().alpha;
    if (time >= alphaKeys.back().time)
        return alphaKeys.back().alpha;
    size_t i = 0;
    while (i + 2 < alphaKeys.size() && alphaKeys[i + 1].time < time){
        i++;
    }
    const GradientAlphaKey& a = alphaKeys[i];
    const GradientAlphaKey& b = alphaKeys[i + 1];
    if (mode == GradientMode::Fixed)
        return b.alpha;
    const float span = b.time - a.time;
    const float t = span > 0.0f ? (time - a.time) / span : 1.0f;
    return a.alpha + (b.alpha - a.alpha) * t;
}

Vector4 Gradient::evaluate(float time) const{
    if (linear){
        // linear keys: the sRGB values are converted before they are blended
        Gradient converted = *this;
        for (GradientColorKey& key : converted.colorKeys){
            key.color = Color::sRGBToLinear(key.color);
        }
        converted.linear = false;
        return Vector4(converted.evaluateEncoded(time), evaluateAlpha(time));
    }
    return Vector4(Color::sRGBToLinear(evaluateEncoded(time)), evaluateAlpha(time));
}

void Gradient::sortKeys(){
    std::stable_sort(colorKeys.begin(), colorKeys.end(), [](const GradientColorKey& a, const GradientColorKey& b){ return a.time < b.time; });
    std::stable_sort(alphaKeys.begin(), alphaKeys.end(), [](const GradientAlphaKey& a, const GradientAlphaKey& b){ return a.time < b.time; });
}

Gradient Gradient::constant(const Vector3& color, float alpha){
    Gradient gradient;
    gradient.colorKeys.push_back({0.0f, color});
    gradient.alphaKeys.push_back({0.0f, alpha});
    return gradient;
}

Gradient Gradient::twoColors(const Vector3& from, const Vector3& to, float alphaFrom, float alphaTo){
    Gradient gradient;
    gradient.colorKeys.push_back({0.0f, from});
    gradient.colorKeys.push_back({1.0f, to});
    gradient.alphaKeys.push_back({0.0f, alphaFrom});
    gradient.alphaKeys.push_back({1.0f, alphaTo});
    return gradient;
}
