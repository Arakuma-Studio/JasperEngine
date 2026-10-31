// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationCurve.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

CurveKey::CurveKey(float time, float value, float inTangent, float outTangent):
    time(time), value(value), inTangent(inTangent), outTangent(outTangent){
}

bool CurveKey::operator==(const CurveKey& other) const{
    return time == other.time && value == other.value && inTangent == other.inTangent && outTangent == other.outTangent;
}

AnimationCurve::AnimationCurve(std::vector<CurveKey> keys, CurveWrap preWrap, CurveWrap postWrap):
    keys(std::move(keys)), preWrap(preWrap), postWrap(postWrap){
}

namespace {

    // t folded into [start, start + length] by a wrap mode (Clamp is handled by the caller)
    float wrapTime(float t, float start, float length, CurveWrap wrap){
        if (!(length > 0.0f)){
            return start;
        }
        float u = std::fmod(t - start, wrap == CurveWrap::PingPong ? 2.0f * length : length);
        if (u < 0.0f){
            u += (wrap == CurveWrap::PingPong) ? 2.0f * length : length;
        }
        if (wrap == CurveWrap::PingPong && u > length){
            u = 2.0f * length - u;
        }
        return start + u;
    }

    float hermite(const CurveKey& k0, const CurveKey& k1, float t){
        const float dt = k1.time - k0.time;
        if (!(dt > 0.0f)){
            return (t < k1.time) ? k0.value : k1.value;
        }
        if (std::isinf(k0.outTangent) || std::isinf(k1.inTangent)){
            return k0.value;
        }
        const float s = (t - k0.time) / dt;
        const float s2 = s * s;
        const float s3 = s2 * s;
        const float m0 = k0.outTangent * dt;
        const float m1 = k1.inTangent * dt;
        return (2.0f * s3 - 3.0f * s2 + 1.0f) * k0.value + (s3 - 2.0f * s2 + s) * m0 +
               (-2.0f * s3 + 3.0f * s2) * k1.value + (s3 - s2) * m1;
    }

}

float AnimationCurve::evaluate(float t) const{
    if (keys.empty()){
        return 0.0f;
    }
    if (keys.size() == 1){
        return keys[0].value;
    }

    const CurveKey& first = keys.front();
    const CurveKey& last = keys.back();
    const float length = last.time - first.time;

    if (std::isnan(t)){
        return first.value;
    }
    if (t < first.time){
        if (preWrap == CurveWrap::Clamp){
            return first.value;
        }
        t = wrapTime(t, first.time, length, preWrap);
    }else if (t > last.time){
        if (postWrap == CurveWrap::Clamp){
            return last.value;
        }
        t = wrapTime(t, first.time, length, postWrap);
    }
    if (t >= last.time){
        return last.value;
    }

    // the first key after t: the segment is [it - 1, it]
    auto it = std::upper_bound(keys.begin(), keys.end(), t, [](float time, const CurveKey& key){ return time < key.time; });
    if (it == keys.begin()){
        return first.value;
    }
    if (it == keys.end()){
        return last.value;
    }
    return hermite(*(it - 1), *it, t);
}

AnimationCurve AnimationCurve::constant(float v){
    return AnimationCurve({CurveKey(0.0f, v)});
}

AnimationCurve AnimationCurve::linear(float t0, float v0, float t1, float v1){
    if (t0 == t1){
        return AnimationCurve({CurveKey(t0, v1)});
    }
    // both tangents of both keys (Unity's AnimationCurve.Linear): the line whichever key is first
    const float slope = (v1 - v0) / (t1 - t0);
    AnimationCurve curve({CurveKey(t0, v0, slope, slope), CurveKey(t1, v1, slope, slope)});
    curve.normalize();
    return curve;
}

AnimationCurve AnimationCurve::easeInOut(float t0, float v0, float t1, float v1){
    if (t0 == t1){
        return AnimationCurve({CurveKey(t0, v1)});
    }
    AnimationCurve curve({CurveKey(t0, v0), CurveKey(t1, v1)});
    curve.normalize();
    return curve;
}

void AnimationCurve::normalize(){
    keys.erase(std::remove_if(keys.begin(), keys.end(), [](const CurveKey& key){
        return !std::isfinite(key.time) || !std::isfinite(key.value) || std::isnan(key.inTangent) || std::isnan(key.outTangent);
    }), keys.end());
    std::stable_sort(keys.begin(), keys.end(), [](const CurveKey& a, const CurveKey& b){ return a.time < b.time; });
    keys.erase(std::unique(keys.begin(), keys.end(), [](const CurveKey& a, const CurveKey& b){ return a.time == b.time; }), keys.end());
}

int AnimationCurve::addKey(const CurveKey& key){
    if (!std::isfinite(key.time) || !std::isfinite(key.value) || std::isnan(key.inTangent) || std::isnan(key.outTangent)){
        return -1;
    }
    auto it = std::lower_bound(keys.begin(), keys.end(), key.time, [](const CurveKey& k, float time){ return k.time < time; });
    if (it != keys.end() && it->time == key.time){
        return -1;
    }
    it = keys.insert(it, key);
    return (int)(it - keys.begin());
}

int AnimationCurve::moveKey(size_t index, const CurveKey& key){
    if (index >= keys.size() || !std::isfinite(key.time) || !std::isfinite(key.value) ||
        std::isnan(key.inTangent) || std::isnan(key.outTangent)){
        return -1;
    }
    for (size_t i = 0; i < keys.size(); i++){
        if (i != index && keys[i].time == key.time){
            return -1;
        }
    }
    keys.erase(keys.begin() + (long)index);
    return addKey(key);
}

bool AnimationCurve::removeKey(size_t index){
    if (index >= keys.size()){
        return false;
    }
    keys.erase(keys.begin() + (long)index);
    return true;
}

void AnimationCurve::valueRange(float minTime, float maxTime, float& minValue, float& maxValue) const{
    if (keys.empty()){
        minValue = maxValue = 0.0f;
        return;
    }
    minValue = maxValue = evaluate(minTime);
    const int samples = 64;
    for (int i = 1; i <= samples; i++){
        const float v = evaluate(minTime + (maxTime - minTime) * (float)i / (float)samples);
        minValue = std::min(minValue, v);
        maxValue = std::max(maxValue, v);
    }
    for (const CurveKey& key : keys){
        if (key.time >= minTime && key.time <= maxTime){
            minValue = std::min(minValue, key.value);
            maxValue = std::max(maxValue, key.value);
        }
    }
}

float AnimationCurve::firstTime() const{
    return keys.empty() ? 0.0f : keys.front().time;
}

float AnimationCurve::lastTime() const{
    return keys.empty() ? 0.0f : keys.back().time;
}

bool AnimationCurve::isConstant() const{
    for (size_t i = 1; i < keys.size(); i++){
        if (keys[i].value != keys[0].value){
            return false;
        }
    }
    for (size_t i = 0; i + 1 < keys.size(); i++){
        // a flat segment: a step, or flat tangents at both ends
        const bool step = std::isinf(keys[i].outTangent) || std::isinf(keys[i + 1].inTangent);
        if (!step && (keys[i].outTangent != 0.0f || keys[i + 1].inTangent != 0.0f)){
            return false;
        }
    }
    return true;
}

bool AnimationCurve::operator==(const AnimationCurve& other) const{
    return preWrap == other.preWrap && postWrap == other.postWrap && keys == other.keys;
}
