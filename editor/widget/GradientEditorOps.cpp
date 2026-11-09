// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GradientEditorOps.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    float clamp01(float t){
        return std::isfinite(t) ? std::min(1.0f, std::max(0.0f, t)) : 0.0f;
    }

    // the index a key at `time` takes: after every key at or before it
    template <class Keys>
    size_t insertionIndex(const Keys& keys, float time){
        size_t i = 0;
        while (i < keys.size() && keys[i].time <= time) i++;
        return i;
    }

    template <class Keys>
    int moveKey(Keys& keys, size_t index, float time){
        if (index >= keys.size()) return -1;
        auto key = keys[index];
        key.time = clamp01(time);
        keys.erase(keys.begin() + (std::ptrdiff_t)index);
        const size_t at = insertionIndex(keys, key.time);
        keys.insert(keys.begin() + (std::ptrdiff_t)at, key);
        return (int)at;
    }

    template <class Keys>
    bool removeKey(Keys& keys, size_t index){
        if (index >= keys.size() || keys.size() <= 1) return false;
        keys.erase(keys.begin() + (std::ptrdiff_t)index);
        return true;
    }

}

float gradientops::timeAtX(float x0, float width, float x){
    return width > 0.0f ? clamp01((x - x0) / width) : 0.0f;
}

float gradientops::xAtTime(float x0, float width, float time){
    return x0 + clamp01(time) * width;
}

int gradientops::hitMarker(const std::vector<float>& times, float x0, float width, float x, float radius){
    int best = -1;
    float bestDistance = radius;
    for (size_t i = 0; i < times.size(); i++){
        const float d = std::fabs(xAtTime(x0, width, times[i]) - x);
        if (d <= bestDistance){
            bestDistance = d;
            best = (int)i;
        }
    }
    return best;
}

std::vector<float> gradientops::colorTimes(const Gradient& gradient){
    std::vector<float> times;
    for (const GradientColorKey& key : gradient.colorKeys) times.push_back(key.time);
    return times;
}

std::vector<float> gradientops::alphaTimes(const Gradient& gradient){
    std::vector<float> times;
    for (const GradientAlphaKey& key : gradient.alphaKeys) times.push_back(key.time);
    return times;
}

int gradientops::addColorKey(Gradient& gradient, float time, size_t maxKeys){
    if (gradient.colorKeys.size() >= maxKeys) return -1;
    const float t = clamp01(time);
    // the colour as it is blended there (the keys' own encoding), so the look stays
    const GradientColorKey key{t, gradient.evaluateEncoded(t)};
    const size_t at = insertionIndex(gradient.colorKeys, t);
    gradient.colorKeys.insert(gradient.colorKeys.begin() + (std::ptrdiff_t)at, key);
    return (int)at;
}

int gradientops::addAlphaKey(Gradient& gradient, float time, size_t maxKeys){
    if (gradient.alphaKeys.size() >= maxKeys) return -1;
    const float t = clamp01(time);
    const GradientAlphaKey key{t, gradient.evaluateAlpha(t)};
    const size_t at = insertionIndex(gradient.alphaKeys, t);
    gradient.alphaKeys.insert(gradient.alphaKeys.begin() + (std::ptrdiff_t)at, key);
    return (int)at;
}

int gradientops::moveColorKey(Gradient& gradient, size_t index, float time){
    return moveKey(gradient.colorKeys, index, time);
}

int gradientops::moveAlphaKey(Gradient& gradient, size_t index, float time){
    return moveKey(gradient.alphaKeys, index, time);
}

bool gradientops::removeColorKey(Gradient& gradient, size_t index){
    return removeKey(gradient.colorKeys, index);
}

bool gradientops::removeAlphaKey(Gradient& gradient, size_t index){
    return removeKey(gradient.alphaKeys, index);
}
