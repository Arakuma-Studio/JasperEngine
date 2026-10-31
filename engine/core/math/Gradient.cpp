// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Gradient.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

GradientColorKey::GradientColorKey(float time, const Vector3& color): time(time), color(color){
}

bool GradientColorKey::operator==(const GradientColorKey& other) const{
    return time == other.time && color == other.color;
}

GradientAlphaKey::GradientAlphaKey(float time, float alpha): time(time), alpha(alpha){
}

bool GradientAlphaKey::operator==(const GradientAlphaKey& other) const{
    return time == other.time && alpha == other.alpha;
}

Gradient::Gradient():
    colorKeys({GradientColorKey(0.0f, Vector3(1, 1, 1)), GradientColorKey(1.0f, Vector3(1, 1, 1))}),
    alphaKeys({GradientAlphaKey(0.0f, 1.0f), GradientAlphaKey(1.0f, 1.0f)}){
}

Gradient::Gradient(std::vector<GradientColorKey> colorKeys, std::vector<GradientAlphaKey> alphaKeys, GradientMode mode):
    colorKeys(std::move(colorKeys)), alphaKeys(std::move(alphaKeys)), mode(mode){
}

namespace {

    // the keyed value at t of a list sorted by time (Blend: linear between the keys around
    // t; Fixed: the first key at or after t), `fallback` without keys
    template<typename Key, typename Value, typename Get>
    Value evaluateKeys(const std::vector<Key>& keys, float t, GradientMode mode, const Value& fallback, Get get){
        if (keys.empty()){
            return fallback;
        }
        if (keys.size() == 1 || t <= keys.front().time){
            return get(keys.front());
        }
        if (t >= keys.back().time){
            return get(keys.back());
        }
        size_t next = 1;
        while (next < keys.size() && keys[next].time < t){
            next++;
        }
        const Key& k1 = keys[next];
        if (mode == GradientMode::Fixed){
            return get(k1);
        }
        const Key& k0 = keys[next - 1];
        const float span = k1.time - k0.time;
        if (!(span > 0.0f)){
            return get(k1);
        }
        const float f = (t - k0.time) / span;
        return get(k0) * (1.0f - f) + get(k1) * f;
    }

    float clamp01(float t){
        if (std::isnan(t)){
            return 0.0f;
        }
        return std::min(1.0f, std::max(0.0f, t));
    }

}

Vector3 Gradient::evaluateColor(float t) const{
    return evaluateKeys(colorKeys, clamp01(t), mode, Vector3(1, 1, 1), [](const GradientColorKey& key){ return key.color; });
}

float Gradient::evaluateAlpha(float t) const{
    return evaluateKeys(alphaKeys, clamp01(t), mode, 1.0f, [](const GradientAlphaKey& key){ return key.alpha; });
}

Vector4 Gradient::evaluate(float t) const{
    const Vector3 color = evaluateColor(t);
    return Vector4(color.x, color.y, color.z, evaluateAlpha(t));
}

Gradient Gradient::constant(const Vector4& color){
    return Gradient({GradientColorKey(0.0f, Vector3(color.x, color.y, color.z))}, {GradientAlphaKey(0.0f, color.w)});
}

Gradient Gradient::twoColors(const Vector4& start, const Vector4& end){
    return Gradient({GradientColorKey(0.0f, Vector3(start.x, start.y, start.z)), GradientColorKey(1.0f, Vector3(end.x, end.y, end.z))},
                    {GradientAlphaKey(0.0f, start.w), GradientAlphaKey(1.0f, end.w)});
}

void Gradient::normalize(){
    colorKeys.erase(std::remove_if(colorKeys.begin(), colorKeys.end(), [](const GradientColorKey& key){
        return !std::isfinite(key.time) || !std::isfinite(key.color.x) || !std::isfinite(key.color.y) || !std::isfinite(key.color.z);
    }), colorKeys.end());
    alphaKeys.erase(std::remove_if(alphaKeys.begin(), alphaKeys.end(), [](const GradientAlphaKey& key){
        return !std::isfinite(key.time) || !std::isfinite(key.alpha);
    }), alphaKeys.end());
    for (GradientColorKey& key : colorKeys){
        key.time = clamp01(key.time);
    }
    for (GradientAlphaKey& key : alphaKeys){
        key.time = clamp01(key.time);
    }
    std::stable_sort(colorKeys.begin(), colorKeys.end(), [](const GradientColorKey& a, const GradientColorKey& b){ return a.time < b.time; });
    std::stable_sort(alphaKeys.begin(), alphaKeys.end(), [](const GradientAlphaKey& a, const GradientAlphaKey& b){ return a.time < b.time; });
    if (colorKeys.size() > MAX_KEYS){
        colorKeys.resize(MAX_KEYS);
    }
    if (alphaKeys.size() > MAX_KEYS){
        alphaKeys.resize(MAX_KEYS);
    }
}

bool Gradient::operator==(const Gradient& other) const{
    return mode == other.mode && colorKeys == other.colorKeys && alphaKeys == other.alphaKeys;
}
