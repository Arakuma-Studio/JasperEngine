// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GradientFieldLogic.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    float clamp01(float t){
        if (std::isnan(t)) return 0.0f;
        return std::min(1.0f, std::max(0.0f, t));
    }

    // the index a key at `time` goes to: after the keys at or before it
    template<typename Key>
    size_t insertIndex(const std::vector<Key>& keys, float time){
        size_t i = 0;
        while (i < keys.size() && keys[i].time <= time) i++;
        return i;
    }

    template<typename Key>
    int moveKey(std::vector<Key>& keys, size_t index, float time){
        if (index >= keys.size()) return -1;
        Key key = keys[index];
        key.time = clamp01(time);
        keys.erase(keys.begin() + (long)index);
        const size_t to = insertIndex(keys, key.time);
        keys.insert(keys.begin() + (long)to, key);
        return (int)to;
    }

    template<typename Key>
    bool removeKey(std::vector<Key>& keys, size_t index){
        if (index >= keys.size() || keys.size() <= 1) return false;
        keys.erase(keys.begin() + (long)index);
        return true;
    }

}

float gradientfield::timeAtX(float barX0, float barWidth, float x){
    if (!(barWidth > 0.0f)) return 0.0f;
    return clamp01((x - barX0) / barWidth);
}

float gradientfield::xAtTime(float barX0, float barWidth, float time){
    return barX0 + clamp01(time) * barWidth;
}

int gradientfield::hitMarker(const std::vector<float>& times, float barX0, float barWidth, float mouseX, float radiusPx){
    int best = -1;
    float bestDistance = radiusPx;
    for (size_t i = 0; i < times.size(); i++){
        const float d = std::fabs(xAtTime(barX0, barWidth, times[i]) - mouseX);
        if (d <= bestDistance){
            bestDistance = d;
            best = (int)i;
        }
    }
    return best;
}

std::vector<float> gradientfield::colorTimes(const Gradient& gradient){
    std::vector<float> times;
    times.reserve(gradient.colorKeys.size());
    for (const GradientColorKey& key : gradient.colorKeys) times.push_back(key.time);
    return times;
}

std::vector<float> gradientfield::alphaTimes(const Gradient& gradient){
    std::vector<float> times;
    times.reserve(gradient.alphaKeys.size());
    for (const GradientAlphaKey& key : gradient.alphaKeys) times.push_back(key.time);
    return times;
}

int gradientfield::addColorKey(Gradient& gradient, float time){
    if (gradient.colorKeys.size() >= Gradient::MAX_KEYS) return -1;
    time = clamp01(time);
    const GradientColorKey key(time, gradient.evaluateColor(time));
    const size_t to = insertIndex(gradient.colorKeys, time);
    gradient.colorKeys.insert(gradient.colorKeys.begin() + (long)to, key);
    return (int)to;
}

int gradientfield::addAlphaKey(Gradient& gradient, float time){
    if (gradient.alphaKeys.size() >= Gradient::MAX_KEYS) return -1;
    time = clamp01(time);
    const GradientAlphaKey key(time, gradient.evaluateAlpha(time));
    const size_t to = insertIndex(gradient.alphaKeys, time);
    gradient.alphaKeys.insert(gradient.alphaKeys.begin() + (long)to, key);
    return (int)to;
}

int gradientfield::moveColorKey(Gradient& gradient, size_t index, float time){
    return moveKey(gradient.colorKeys, index, time);
}

int gradientfield::moveAlphaKey(Gradient& gradient, size_t index, float time){
    return moveKey(gradient.alphaKeys, index, time);
}

bool gradientfield::removeColorKey(Gradient& gradient, size_t index){
    return removeKey(gradient.colorKeys, index);
}

bool gradientfield::removeAlphaKey(Gradient& gradient, size_t index){
    return removeKey(gradient.alphaKeys, index);
}

const char* gradientfield::presetName(GradientPreset preset){
    switch (preset){
        case GradientPreset::White: return "White";
        case GradientPreset::FadeOut: return "Fade Out";
        case GradientPreset::FadeIn: return "Fade In";
        case GradientPreset::BlackToWhite: return "Black to White";
        case GradientPreset::WhiteToBlack: return "White to Black";
        case GradientPreset::Rainbow: return "Rainbow";
    }
    return "";
}

Gradient gradientfield::presetGradient(GradientPreset preset){
    const Vector3 white(1, 1, 1), black(0, 0, 0);
    switch (preset){
        case GradientPreset::White:
            return Gradient({GradientColorKey(0.0f, white), GradientColorKey(1.0f, white)},
                            {GradientAlphaKey(0.0f, 1.0f), GradientAlphaKey(1.0f, 1.0f)});
        case GradientPreset::FadeOut:
            return Gradient({GradientColorKey(0.0f, white), GradientColorKey(1.0f, white)},
                            {GradientAlphaKey(0.0f, 1.0f), GradientAlphaKey(1.0f, 0.0f)});
        case GradientPreset::FadeIn:
            return Gradient({GradientColorKey(0.0f, white), GradientColorKey(1.0f, white)},
                            {GradientAlphaKey(0.0f, 0.0f), GradientAlphaKey(1.0f, 1.0f)});
        case GradientPreset::BlackToWhite:
            return Gradient({GradientColorKey(0.0f, black), GradientColorKey(1.0f, white)},
                            {GradientAlphaKey(0.0f, 1.0f), GradientAlphaKey(1.0f, 1.0f)});
        case GradientPreset::WhiteToBlack:
            return Gradient({GradientColorKey(0.0f, white), GradientColorKey(1.0f, black)},
                            {GradientAlphaKey(0.0f, 1.0f), GradientAlphaKey(1.0f, 1.0f)});
        case GradientPreset::Rainbow:
            // primaries and secondaries are the same in sRGB and linear
            return Gradient({GradientColorKey(0.0f, Vector3(1, 0, 0)), GradientColorKey(0.2f, Vector3(1, 1, 0)),
                             GradientColorKey(0.4f, Vector3(0, 1, 0)), GradientColorKey(0.6f, Vector3(0, 1, 1)),
                             GradientColorKey(0.8f, Vector3(0, 0, 1)), GradientColorKey(1.0f, Vector3(1, 0, 1))},
                            {GradientAlphaKey(0.0f, 1.0f), GradientAlphaKey(1.0f, 1.0f)});
    }
    return Gradient();
}
