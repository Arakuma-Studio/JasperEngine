// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Curve.h"

#include "action/Ease.h"
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

    // the Hermite value of segment i at `time` (inside it)
    float evaluateSegment(const std::vector<CurveKey>& keys, size_t i, float time){
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

    // the Hermite value inside [first key, last key] (keys has at least two keys)
    float evaluateInside(const std::vector<CurveKey>& keys, float time){
        if (!(time > keys.front().time))
            return keys.front().value;
        if (time >= keys.back().time)
            return keys.back().value;
        return evaluateSegment(keys, findSegment(keys, time), time);
    }

    float derivativeInside(const std::vector<CurveKey>& keys, float time){
        if (!(time > keys.front().time) || time >= keys.back().time)
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

    // Where `time` (outside [start, end], before it when `before`) lands inside under `wrap`:
    // the time inside, the value offset to add (LoopWithOffset) and the slope's sign (PingPong
    // runs backwards on odd cycles). False for Clamp and Linear, which do not map inside.
    bool wrapInside(CurveWrapMode wrap, float time, float start, float end, float startValue, float endValue,
                    float& inside, float& offset, float& sign){
        const float length = end - start;
        offset = 0.0f;
        sign = 1.0f;
        if (!(length > 0.0f))
            return false;
        const float cycles = std::floor((time - start) / length);
        const float phase = (time - start) - cycles * length;
        switch (wrap){
            case CurveWrapMode::Loop:
                inside = start + phase;
                return true;
            case CurveWrapMode::LoopWithOffset:
                inside = start + phase;
                offset = cycles * (endValue - startValue);
                return true;
            case CurveWrapMode::PingPong: {
                const bool backwards = std::fmod(std::fabs(cycles), 2.0f) >= 1.0f;
                inside = backwards ? end - phase : start + phase;
                sign = backwards ? -1.0f : 1.0f;
                return true;
            }
            case CurveWrapMode::Clamp:
            case CurveWrapMode::Linear:
                break;
        }
        return false;
    }

    float slope(const CurveKey& a, const CurveKey& b){
        const float dt = b.time - a.time;
        return dt > 0.0f ? (b.value - a.value) / dt : 0.0f;
    }

    // the Auto / Clamped Auto tangent of keys[i] (keys has at least two keys)
    float smoothTangent(const std::vector<CurveKey>& keys, size_t i, bool clamped){
        if (i == 0)
            return slope(keys[0], keys[1]);
        if (i + 1 == keys.size())
            return slope(keys[i - 1], keys[i]);
        const float m1 = slope(keys[i - 1], keys[i]);
        const float m2 = slope(keys[i], keys[i + 1]);
        const float m = 0.5f * (m1 + m2);
        if (!clamped)
            return m;
        // a peak or a valley is flat; elsewhere the slope is kept within three times the
        // smaller neighbour slope (Fritsch-Carlson), so the segment never overshoots its keys
        if (m1 * m2 <= 0.0f)
            return 0.0f;
        const float limit = 3.0f * std::min(std::fabs(m1), std::fabs(m2));
        return std::clamp(m, -limit, limit);
    }

    // the tangent `mode` gives a side of keys[i]; false for Free (left as it is)
    bool modeTangent(const std::vector<CurveKey>& keys, size_t i, CurveTangentMode mode, bool inSide, float& out){
        switch (mode){
            case CurveTangentMode::Free:
                return false;
            case CurveTangentMode::Constant:
                out = INFINITY;
                return true;
            case CurveTangentMode::Linear:
                if (inSide){
                    if (i == 0) return false;
                    out = slope(keys[i - 1], keys[i]);
                }else{
                    if (i + 1 >= keys.size()) return false;
                    out = slope(keys[i], keys[i + 1]);
                }
                return true;
            case CurveTangentMode::Auto:
            case CurveTangentMode::ClampedAuto:
                if (keys.size() < 2) {
                    out = 0.0f;
                    return true;
                }
                out = smoothTangent(keys, i, mode == CurveTangentMode::ClampedAuto);
                return true;
        }
        return false;
    }

    void applyModes(std::vector<CurveKey>& keys, size_t i){
        CurveKey& key = keys[i];
        float t;
        if (modeTangent(keys, i, key.inMode(), true, t)) key.inTangent = t;
        if (modeTangent(keys, i, key.outMode(), false, t)) key.outTangent = t;
    }

    bool keyBefore(const CurveKey& a, const CurveKey& b){
        return a.time < b.time;
    }

}

float Curve::evaluate(float time) const{
    if (keys.empty())
        return 0.0f;
    if (keys.size() == 1)
        return keys.front().value;
    const CurveKey& first = keys.front();
    const CurveKey& last = keys.back();
    if (time < first.time || time > last.time){
        const bool before = time < first.time;
        const CurveWrapMode wrap = before ? preWrapMode : postWrapMode;
        if (wrap == CurveWrapMode::Linear){
            const float tangent = before ? first.inTangent : last.outTangent;
            if (!std::isfinite(tangent))
                return before ? first.value : last.value;
            return before ? first.value + (time - first.time) * tangent : last.value + (time - last.time) * tangent;
        }
        float inside, offset, sign;
        if (wrapInside(wrap, time, first.time, last.time, first.value, last.value, inside, offset, sign))
            return evaluateInside(keys, inside) + offset;
        return before ? first.value : last.value;
    }
    return evaluateInside(keys, time);
}

float Curve::derivative(float time) const{
    if (keys.size() < 2)
        return 0.0f;
    const CurveKey& first = keys.front();
    const CurveKey& last = keys.back();
    if (time < first.time || time > last.time){
        const bool before = time < first.time;
        const CurveWrapMode wrap = before ? preWrapMode : postWrapMode;
        if (wrap == CurveWrapMode::Linear){
            const float tangent = before ? first.inTangent : last.outTangent;
            return std::isfinite(tangent) ? tangent : 0.0f;
        }
        float inside, offset, sign;
        if (wrapInside(wrap, time, first.time, last.time, first.value, last.value, inside, offset, sign))
            return sign * derivativeInside(keys, inside);
        return 0.0f;
    }
    return derivativeInside(keys, time);
}

void Curve::bake(std::span<float> out, float t0, float t1) const{
    const size_t n = out.size();
    if (n == 0)
        return;
    if (n == 1){
        out[0] = evaluate(t0);
        return;
    }
    // the times ascend: the segment moves forward with them instead of being searched for each
    // entry (the same segment findSegment picks, so every entry equals evaluate at its time)
    const bool ascending = t1 >= t0 && keys.size() >= 2;
    size_t segment = 0;
    for (size_t i = 0; i < n; i++){
        const float t = i + 1 == n ? t1 : t0 + (t1 - t0) * (float)i / (float)(n - 1);
        if (!ascending || !(t > keys.front().time) || !(t < keys.back().time)){
            out[i] = evaluate(t);
            continue;
        }
        while (segment + 2 < keys.size() && keys[segment + 1].time <= t){
            segment++;
        }
        out[i] = evaluateSegment(keys, segment, t);
    }
}

int Curve::addKey(float time, float value){
    CurveKey key;
    key.time = time;
    key.value = value;
    key.setBothModes(CurveTangentMode::ClampedAuto);
    return addKey(key);
}

int Curve::addKey(const CurveKey& key){
    if (!std::isfinite(key.time))
        return -1;
    for (const CurveKey& k : keys){
        if (k.time == key.time)
            return -1;
    }
    const auto at = std::upper_bound(keys.begin(), keys.end(), key, keyBefore);
    const size_t index = (size_t)(at - keys.begin());
    keys.insert(at, key);
    updateTangents(index);
    return (int)index;
}

int Curve::moveKey(size_t index, const CurveKey& key){
    if (index >= keys.size() || !std::isfinite(key.time))
        return -1;
    CurveKey moved = key;
    for (size_t i = 0; i < keys.size(); i++){
        if (i != index && keys[i].time == key.time){
            moved.time = keys[index].time;
            break;
        }
    }
    keys.erase(keys.begin() + (std::ptrdiff_t)index);
    // the old neighbours of the removed key follow (their Auto and Linear sides)
    if (!keys.empty())
        updateTangents(std::min(index, keys.size() - 1));
    const auto at = std::upper_bound(keys.begin(), keys.end(), moved, keyBefore);
    const size_t newIndex = (size_t)(at - keys.begin());
    keys.insert(at, moved);
    updateTangents(newIndex);
    return (int)newIndex;
}

void Curve::removeKey(size_t index){
    if (index >= keys.size())
        return;
    keys.erase(keys.begin() + (std::ptrdiff_t)index);
    if (keys.empty())
        return;
    // the neighbours that are now adjacent follow
    const size_t right = std::min(index, keys.size() - 1);
    updateTangents(right);
    if (right > 0)
        updateTangents(right - 1);
}

void Curve::smoothTangents(size_t index, float weight){
    if (index >= keys.size() || keys.size() < 2)
        return;
    float m;
    if (index == 0){
        m = slope(keys[0], keys[1]);
    }else if (index + 1 == keys.size()){
        m = slope(keys[index - 1], keys[index]);
    }else{
        const float m1 = slope(keys[index - 1], keys[index]);
        const float m2 = slope(keys[index], keys[index + 1]);
        m = (1.0f + weight) * 0.5f * m1 + (1.0f - weight) * 0.5f * m2;
    }
    keys[index].inTangent = m;
    keys[index].outTangent = m;
}

void Curve::updateTangents(size_t index){
    if (index >= keys.size())
        return;
    const size_t from = index > 0 ? index - 1 : 0;
    const size_t to = std::min(index + 1, keys.size() - 1);
    for (size_t i = from; i <= to; i++){
        applyModes(keys, i);
    }
}

void Curve::updateAllTangents(){
    for (size_t i = 0; i < keys.size(); i++){
        applyModes(keys, i);
    }
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

Curve Curve::constant(float timeStart, float timeEnd, float value){
    if (timeEnd == timeStart)
        return constant(value);
    return Curve({{timeStart, value, 0.0f, 0.0f}, {timeEnd, value, 0.0f, 0.0f}});
}

Curve Curve::linear(float from, float to){
    const float slope = to - from;
    return Curve({{0.0f, from, slope, slope}, {1.0f, to, slope, slope}});
}

Curve Curve::linear(float timeStart, float valueStart, float timeEnd, float valueEnd){
    if (timeEnd == timeStart)
        return constant(valueStart);
    const float m = (valueEnd - valueStart) / (timeEnd - timeStart);
    return Curve({{timeStart, valueStart, m, m}, {timeEnd, valueEnd, m, m}});
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
    if (timeEnd == timeStart)
        return constant(valueStart);
    return Curve({{timeStart, valueStart, 0.0f, 0.0f}, {timeEnd, valueEnd, 0.0f, 0.0f}});
}

Curve Curve::fromEase(EaseType type, int keys){
    if (type == EaseType::LINEAR || type == EaseType::CUSTOM || type == EaseType::CURVE)
        return Curve::linear(0.0f, 1.0f);
    Ease ease(type);
    return fromFunction([&ease](float t){ return ease.call(t); }, keys, 0.005f, 64);
}

Curve Curve::fromFunction(const std::function<float(float)>& function, int keyCount, float tolerance, int maxKeys){
    constexpr int kSamples = 2048;
    constexpr float h = 1.0e-4f;
    const int keysWanted = std::max(keyCount, 2);
    const int keysAtMost = std::max(keysWanted, maxKeys);
    auto f = [&](float t){ return function(std::clamp(t, 0.0f, 1.0f)); };
    auto keyAt = [&](float t){
        CurveKey key;
        key.time = t;
        key.value = f(t);
        // the slopes on each side, from the function (a kink keeps both)
        key.inTangent = t > 0.0f ? (key.value - f(t - h)) / h : (f(t + h) - key.value) / h;
        key.outTangent = t < 1.0f ? (f(t + h) - key.value) / h : key.inTangent;
        return key;
    };
    // the ends, then a key where the curve is worst, until there are `keys` (kinks and the steep
    // parts get the keys). Each new key is placed by a search around the worst point: the time
    // giving the smallest error over the two segments it splits (a key on a kink makes the
    // bounces' parabolic arcs exact)
    Curve curve;
    curve.keys = {keyAt(0.0f), keyAt(1.0f)};
    std::vector<float> target(kSamples + 1);
    for (int i = 0; i <= kSamples; i++){
        target[(size_t)i] = f((float)i / (float)kSamples);
    }
    auto segmentError = [&](const Curve& c, float from, float to){
        float worst = 0.0f;
        const int a = std::max(1, (int)std::floor(from * kSamples)), b = std::min(kSamples - 1, (int)std::ceil(to * kSamples));
        for (int i = a; i <= b; i++){
            worst = std::max(worst, std::fabs(c.evaluate((float)i / (float)kSamples) - target[(size_t)i]));
        }
        return worst;
    };
    while ((int)curve.keys.size() < keysAtMost){
        float worst = 0.0f;
        int worstIndex = -1;
        for (int i = 1; i < kSamples; i++){
            const float error = std::fabs(curve.evaluate((float)i / (float)kSamples) - target[(size_t)i]);
            if (error > worst){
                worst = error;
                worstIndex = i;
            }
        }
        if (worstIndex < 0 || worst < 1.0e-7f)
            break;
        // past the keys asked for, only while the curve is not close enough yet
        if ((int)curve.keys.size() >= keysWanted && !(tolerance > 0.0f && worst > tolerance))
            break;
        const float worstT = (float)worstIndex / (float)kSamples;
        // the neighbours the new key falls between
        const auto upper = std::upper_bound(curve.keys.begin(), curve.keys.end(), worstT, [](float t, const CurveKey& k){ return t < k.time; });
        const float left = std::prev(upper)->time, right = upper->time;
        CurveKey best = keyAt(worstT);
        float bestError = INFINITY;
        constexpr int kCandidates = 64;
        const float reach = 3.0f / (float)kSamples;
        for (int c = 0; c <= kCandidates; c++){
            const float t = worstT - reach + 2.0f * reach * (float)c / (float)kCandidates;
            if (!(t > left) || !(t < right)) continue;
            Curve trial = curve;
            const CurveKey key = keyAt(t);
            trial.keys.insert(std::upper_bound(trial.keys.begin(), trial.keys.end(), key, keyBefore), key);
            const float error = segmentError(trial, left, right);
            if (error < bestError){
                bestError = error;
                best = key;
            }
        }
        curve.keys.insert(std::upper_bound(curve.keys.begin(), curve.keys.end(), best, keyBefore), best);
    }
    // a jump inside a segment (STEP): hold the left value
    for (size_t k = 0; k + 1 < curve.keys.size(); k++){
        CurveKey& a = curve.keys[k];
        const CurveKey& b = curve.keys[k + 1];
        const float mid = f(0.5f * (a.time + b.time));
        if (std::fabs(b.value - a.value) > 1.0e-4f && std::fabs(mid - a.value) < 1.0e-6f && std::fabs(a.outTangent) < 1.0e-3f){
            a.outTangent = INFINITY;
            curve.keys[k + 1].inTangent = INFINITY;
        }
    }
    return curve;
}

namespace {

    // the gradient's colour at `time` blended from its keys; `toLinear` converts each key from
    // sRGB before the blend (a linear gradient) instead of after it
    Vector3 blendColor(const Gradient& g, float time, bool toLinear){
        const std::vector<GradientColorKey>& colorKeys = g.colorKeys;
        auto color = [toLinear](const GradientColorKey& key){ return toLinear ? Color::sRGBToLinear(key.color) : key.color; };
        if (colorKeys.empty())
            return Vector3(1.0f, 1.0f, 1.0f);
        if (colorKeys.size() == 1 || !(time > colorKeys.front().time))
            return color(colorKeys.front());
        if (time >= colorKeys.back().time)
            return color(colorKeys.back());
        size_t i = 0;
        while (i + 2 < colorKeys.size() && colorKeys[i + 1].time < time){
            i++;
        }
        const GradientColorKey& a = colorKeys[i];
        const GradientColorKey& b = colorKeys[i + 1];
        if (g.mode == GradientMode::Fixed)
            return color(b);
        const float span = b.time - a.time;
        const float t = span > 0.0f ? (time - a.time) / span : 1.0f;
        const Vector3 ca = color(a);
        return ca + (color(b) - ca) * t;
    }

}

Vector3 Gradient::evaluateEncoded(float time) const{
    return blendColor(*this, time, false);
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
    // linear keys: the sRGB values are converted before they are blended
    if (linear)
        return Vector4(blendColor(*this, time, true), evaluateAlpha(time));
    return Vector4(Color::sRGBToLinear(blendColor(*this, time, false)), evaluateAlpha(time));
}

void Gradient::bake(std::span<Vector4> out) const{
    const size_t n = out.size();
    if (n == 0)
        return;
    if (n == 1){
        out[0] = evaluate(0.0f);
        return;
    }
    for (size_t i = 0; i < n; i++){
        out[i] = evaluate(i + 1 == n ? 1.0f : (float)i / (float)(n - 1));
    }
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
