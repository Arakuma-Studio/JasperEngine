// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Gradient.h"

#include "util/Color.h"

#include <algorithm>
#include <array>
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
    updateSRGBKeys();
}

Gradient::Gradient(std::vector<GradientColorKey> colorKeys, std::vector<GradientAlphaKey> alphaKeys, GradientMode mode):
    colorKeys(std::move(colorKeys)), alphaKeys(std::move(alphaKeys)), mode(mode){
    updateSRGBKeys();
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

    // linear <-> the engine's sRGB (util/Color)
    float toSRGB(float linear){
        return linear > 0.0f ? Color::linearTosRGB(Vector3(linear, 0.0f, 0.0f)).x : 0.0f;
    }

    float toLinearExact(float srgb){
        return Color::sRGBToLinear(Vector3(srgb, 0.0f, 0.0f)).x;
    }

    // sRGB to linear: a table with linear interpolation inside [0, 1] (a trail evaluates its
    // gradient at every point every frame), exact outside it
    constexpr int SRGB_TABLE_STEPS = 1024;
    struct LinearTable {
        std::array<float, SRGB_TABLE_STEPS + 2> values;
        LinearTable(){
            for (int i = 0; i <= SRGB_TABLE_STEPS + 1; i++){
                values[(size_t)i] = toLinearExact(std::min(1.0f, (float)i / (float)SRGB_TABLE_STEPS));
            }
        }
    };

    float toLinear(float srgb){
        static const LinearTable table;
        if (!(srgb > 0.0f)){
            return 0.0f;
        }
        if (srgb >= 1.0f){
            return toLinearExact(srgb);
        }
        const float x = srgb * (float)SRGB_TABLE_STEPS;
        const int i = (int)x;
        const float f = x - (float)i;
        return table.values[(size_t)i] + (table.values[(size_t)i + 1] - table.values[(size_t)i]) * f;
    }

    Vector3 toSRGB(const Vector3& linear){
        return Vector3(toSRGB(linear.x), toSRGB(linear.y), toSRGB(linear.z));
    }

}

void Gradient::updateSRGBKeys(){
    srgbKeys.clear();
    srgbKeys.reserve(colorKeys.size());
    for (const GradientColorKey& key : colorKeys){
        srgbKeys.emplace_back(key.color, toSRGB(key.color));
    }
}

Vector3 Gradient::keySRGB(size_t index) const{
    const Vector3& linear = colorKeys[index].color;
    if (index < srgbKeys.size()){
        const std::pair<Vector3, Vector3>& cached = srgbKeys[index];
        if (cached.first.x == linear.x && cached.first.y == linear.y && cached.first.z == linear.z){
            return cached.second;
        }
    }
    return toSRGB(linear);
}

Vector3 Gradient::evaluateColor(float t) const{
    t = clamp01(t);
    if (colorKeys.empty()){
        return Vector3(1, 1, 1);
    }
    if (colorKeys.size() == 1 || t <= colorKeys.front().time){
        return colorKeys.front().color;
    }
    if (t >= colorKeys.back().time){
        return colorKeys.back().color;
    }
    size_t next = 1;
    while (next < colorKeys.size() && colorKeys[next].time < t){
        next++;
    }
    const GradientColorKey& k1 = colorKeys[next];
    if (mode == GradientMode::Fixed){
        return k1.color;
    }
    const GradientColorKey& k0 = colorKeys[next - 1];
    const float span = k1.time - k0.time;
    if (!(span > 0.0f)){
        return k1.color;
    }
    const float f = (t - k0.time) / span;
    if (f <= 0.0f || (k0.color.x == k1.color.x && k0.color.y == k1.color.y && k0.color.z == k1.color.z)){
        return k0.color;
    }
    if (f >= 1.0f){
        return k1.color;
    }
    // mixed in sRGB as Unity does, back to linear
    const Vector3 s0 = keySRGB(next - 1);
    const Vector3 s1 = keySRGB(next);
    return Vector3(toLinear(s0.x + (s1.x - s0.x) * f), toLinear(s0.y + (s1.y - s0.y) * f), toLinear(s0.z + (s1.z - s0.z) * f));
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
    // too many: the first MAX_KEYS - 1 and the last (both ends kept)
    if (colorKeys.size() > MAX_KEYS){
        colorKeys[MAX_KEYS - 1] = colorKeys.back();
        colorKeys.resize(MAX_KEYS);
    }
    if (alphaKeys.size() > MAX_KEYS){
        alphaKeys[MAX_KEYS - 1] = alphaKeys.back();
        alphaKeys.resize(MAX_KEYS);
    }
    updateSRGBKeys();
}

bool Gradient::operator==(const Gradient& other) const{
    return mode == other.mode && colorKeys == other.colorKeys && alphaKeys == other.alphaKeys;
}
