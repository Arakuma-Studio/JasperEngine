// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "TextEffects.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

using namespace jasperengine;

namespace {

    constexpr float PI = 3.14159265358979323846f;

    float clamp01(float value){
        return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
    }

    float fract(float value){
        return value - std::floor(value);
    }

    float lerpFloat(float a, float b, float t){
        return a + (b - a) * t;
    }

    // deterministic [0, 1) from a glyph and a step (Shake)
    float hash01(uint32_t a, uint32_t b){
        uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        h *= 0x297A2D39u;
        h ^= h >> 15;
        return (float)(h >> 8) / 16777216.0f;
    }

    Vector2 centerOf(const TextEffectGlyphState& glyph){
        return (glyph.corners[0] + glyph.corners[1] + glyph.corners[2] + glyph.corners[3]) * 0.25f;
    }

    void translate(TextEffectGlyphState& glyph, const Vector2& offset){
        for (Vector2& corner : glyph.corners)
            corner += offset;
    }

    // the period or length of a layer, never 0
    float durationOf(const TextEffectLayer& layer){
        return (std::isfinite(layer.duration) && layer.duration > 0.0001f) ? layer.duration : 0.0001f;
    }

    const char* const typeNames[] = {"none", "wave", "bounce", "shake", "pulse", "typewriter", "fade_in", "grow", "unfold", "rainbow", "gradient"};
    const char* const mixNames[] = {"multiply", "add", "replace"};

}

bool TextEffectLayer::operator==(const TextEffectLayer& other) const{
    return type == other.type && enabled == other.enabled && amplitude == other.amplitude && duration == other.duration &&
           spread == other.spread && phaseStep == other.phaseStep && frequency == other.frequency &&
           charactersPerSecond == other.charactersPerSecond && fadeTime == other.fadeTime && gradient == other.gradient &&
           mix == other.mix && saturation == other.saturation && lightness == other.lightness;
}

bool TextEffects::isOneShot(TextEffectType type){
    return type == TextEffectType::Typewriter || type == TextEffectType::FadeIn || type == TextEffectType::Grow ||
           type == TextEffectType::Unfold;
}

bool TextEffects::isMotion(TextEffectType type){
    return type == TextEffectType::Wave || type == TextEffectType::Bounce || type == TextEffectType::Shake ||
           type == TextEffectType::Pulse || type == TextEffectType::Grow || type == TextEffectType::Unfold;
}

bool TextEffects::isColor(TextEffectType type){
    return type == TextEffectType::Rainbow || type == TextEffectType::Gradient;
}

float TextEffects::defaultAmplitude(TextEffectType type){
    return (type == TextEffectType::Grow || type == TextEffectType::Unfold) ? 1.0f : 0.15f;
}

void TextEffects::setType(TextEffectLayer& layer, TextEffectType type){
    if (layer.amplitude == defaultAmplitude(layer.type))
        layer.amplitude = defaultAmplitude(type);
    layer.type = type;
}

float TextEffects::oneShotProgress(int index, int count, float progress, float spread, int characters, int textCharacters){
    const float selection = (float)std::max(characters > 0 ? characters : count, 1);
    const float total = (float)std::max(textCharacters > 0 ? textCharacters : (characters > 0 ? characters : count), 1);
    const float s = clamp01(spread);
    const float start = lerpFloat((float)index / selection, 0.0f, s);
    const float length = lerpFloat(1.0f / total, 1.0f, s);
    return clamp01((std::min(progress, 1.0f) - start) / length);
}

float TextEffects::effectLength(const TextEffectLayer* layers, size_t layerCount, int count, bool reducedMotion, bool& oneShot){
    oneShot = false;
    float length = 0.0f;
    for (size_t i = 0; i < layerCount; i++){
        const TextEffectLayer& layer = layers[i];
        if (!layer.enabled || !isOneShot(layer.type))
            continue;
        oneShot = true;
        // held still by reduced motion: done at once
        if (reducedMotion && isMotion(layer.type))
            continue;
        length = std::max(length, oneShotLength(layer, count));
    }
    return length;
}

float TextEffects::effectLength(const std::vector<TextEffectLayer>& layers, int count, bool reducedMotion, bool& oneShot){
    return effectLength(layers.data(), layers.size(), count, reducedMotion, oneShot);
}

float TextEffects::revealTime(const TextEffectLayer& layer, int index, int count){
    if (layer.charactersPerSecond > 0.0f && std::isfinite(layer.charactersPerSecond))
        return (float)(index + 1) / layer.charactersPerSecond;
    return (float)(index + 1) * durationOf(layer) / (float)std::max(count, 1);
}

float TextEffects::oneShotLength(const TextEffectLayer& layer, int count){
    if (!isOneShot(layer.type))
        return 0.0f;
    if (layer.type == TextEffectType::Typewriter)
        return (count > 0 ? revealTime(layer, count - 1, count) : 0.0f) + std::max(layer.fadeTime, 0.0f);
    return durationOf(layer);
}

Vector4 TextEffects::mixColor(const Vector4& base, const Vector4& color, TextEffectMix mix){
    switch (mix){
        case TextEffectMix::Multiply:
            return Vector4(base.x * color.x, base.y * color.y, base.z * color.z, base.w * color.w);
        case TextEffectMix::Add:
            return Vector4(std::min(base.x + color.x, 1.0f), std::min(base.y + color.y, 1.0f), std::min(base.z + color.z, 1.0f),
                           base.w * color.w);
        case TextEffectMix::Replace:
            return color;
    }
    return base;
}

Vector4 TextEffects::evaluateGradient(const std::vector<TextEffectGradientStop>& gradient, float t){
    if (gradient.empty())
        return Vector4(1.0f, 1.0f, 1.0f, 1.0f);

    t = clamp01(t);
    // Ordered gradients are sampled for every glyph; only unordered input needs a copy.
    std::vector<TextEffectGradientStop> sorted;
    bool ordered = true;
    for (size_t i = 1; i < gradient.size(); i++){
        if (!(gradient[i - 1].position <= gradient[i].position)){
            ordered = false;
            break;
        }
    }
    if (!ordered){
        sorted = gradient;
        std::stable_sort(sorted.begin(), sorted.end(), [](const TextEffectGradientStop& a, const TextEffectGradientStop& b){
            return a.position < b.position;
        });
    }
    const std::vector<TextEffectGradientStop>& stops = ordered ? gradient : sorted;

    if (t <= stops.front().position)
        return stops.front().color;
    for (size_t i = 1; i < stops.size(); i++){
        if (t <= stops[i].position){
            const float span = stops[i].position - stops[i - 1].position;
            const float k = span > 0.0f ? (t - stops[i - 1].position) / span : 1.0f;
            const Vector4& a = stops[i - 1].color;
            const Vector4& b = stops[i].color;
            return Vector4(lerpFloat(a.x, b.x, k), lerpFloat(a.y, b.y, k), lerpFloat(a.z, b.z, k), lerpFloat(a.w, b.w, k));
        }
    }
    return stops.back().color;
}

Vector4 TextEffects::hsl(float hue, float saturation, float lightness){
    const float h = fract(hue) * 6.0f;
    const float s = clamp01(saturation);
    const float l = clamp01(lightness);
    const float c = (1.0f - std::fabs(2.0f * l - 1.0f)) * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (h < 1.0f){ r = c; g = x; }
    else if (h < 2.0f){ r = x; g = c; }
    else if (h < 3.0f){ g = c; b = x; }
    else if (h < 4.0f){ g = x; b = c; }
    else if (h < 5.0f){ r = x; b = c; }
    else { r = c; b = x; }
    const float m = l - c * 0.5f;
    return Vector4(r + m, g + m, b + m, 1.0f);
}

void TextEffects::applyLayer(const TextEffectLayer& layer, int index, int count, float time, float fontSize,
                             bool reducedMotion, TextEffectGlyphState& glyph, int characters, int textCharacters){
    if (!layer.enabled || layer.type == TextEffectType::None || count <= 0)
        return;
    if (reducedMotion && isMotion(layer.type))
        return;
    if (!std::isfinite(time))
        time = 0.0f;

    const float duration = durationOf(layer);
    const float progress = time / duration;
    const float n = (float)std::max(count, 1);
    // looping stagger: a glyph lags the previous one by spread / N of a period
    const float spreadDelay = clamp01(layer.spread) / n;
    const float em = fontSize;

    switch (layer.type){
        case TextEffectType::None:
            break;

        case TextEffectType::Wave: {
            // TMP Effect Waves: y_up -= cos(-2 pi t/T + 0.3 i) * 0.3 * pointSize * Amplitude, so glyphs
            // start low (y down: +) and rise
            const float dy = layer.amplitude * em * std::cos(2.0f * PI * progress - layer.phaseStep * (float)index);
            translate(glyph, Vector2(0.0f, dy));
            break;
        }

        case TextEffectType::Bounce: {
            const float dy = -layer.amplitude * em * std::fabs(std::sin(PI * (progress - spreadDelay * (float)index)));
            translate(glyph, Vector2(0.0f, dy));
            break;
        }

        case TextEffectType::Shake: {
            const float frequency = (std::isfinite(layer.frequency) && layer.frequency > 0.0f) ? layer.frequency : 12.0f;
            const uint32_t step = (uint32_t)(int64_t)std::floor(time * frequency);
            const float dx = layer.amplitude * em * (hash01((uint32_t)index * 2u, step) * 2.0f - 1.0f);
            const float dy = layer.amplitude * em * (hash01((uint32_t)index * 2u + 1u, step) * 2.0f - 1.0f);
            translate(glyph, Vector2(dx, dy));
            break;
        }

        case TextEffectType::Pulse: {
            const float scale = 1.0f + layer.amplitude * std::sin(2.0f * PI * (progress - spreadDelay * (float)index));
            const Vector2 center = centerOf(glyph);
            for (Vector2& corner : glyph.corners)
                corner = center + (corner - center) * scale;
            break;
        }

        case TextEffectType::Typewriter: {
            // shown from its reveal time, fading in over fadeTime when it has one
            const float reveal = revealTime(layer, index, count);
            const float alpha = (layer.fadeTime > 0.0f) ? clamp01((time - reveal) / layer.fadeTime) : ((time >= reveal) ? 1.0f : 0.0f);
            glyph.color.w *= alpha;
            break;
        }

        case TextEffectType::FadeIn: {
            glyph.color.w *= oneShotProgress(index, count, progress, layer.spread, characters, textCharacters);
            break;
        }

        case TextEffectType::Grow: {
            // TMP Effect Grow: the top edge rises from the bottom edge (vertices 1, 2 from 0, 3),
            // then every corner LerpUnclamped(base, grown, Amplitude)
            const float q = oneShotProgress(index, count, progress, layer.spread, characters, textCharacters);
            const float a = std::isfinite(layer.amplitude) ? layer.amplitude : 1.0f;
            const Vector2 grownTopLeft = glyph.corners[3] + (glyph.corners[0] - glyph.corners[3]) * q;
            const Vector2 grownTopRight = glyph.corners[2] + (glyph.corners[1] - glyph.corners[2]) * q;
            glyph.corners[0] = glyph.corners[0] + (grownTopLeft - glyph.corners[0]) * a;
            glyph.corners[1] = glyph.corners[1] + (grownTopRight - glyph.corners[1]) * a;
            break;
        }

        case TextEffectType::Unfold: {
            // TMP Effect Unfold: from the horizontal centre line, then LerpUnclamped by Amplitude
            const float q = oneShotProgress(index, count, progress, layer.spread, characters, textCharacters);
            const float a = std::isfinite(layer.amplitude) ? layer.amplitude : 1.0f;
            const Vector2 left = (glyph.corners[0] + glyph.corners[3]) * 0.5f;
            const Vector2 right = (glyph.corners[1] + glyph.corners[2]) * 0.5f;
            const Vector2 middles[4] = {left, right, right, left};
            for (int c = 0; c < 4; c++){
                const Vector2 unfolded = middles[c] + (glyph.corners[c] - middles[c]) * q;
                glyph.corners[c] = glyph.corners[c] + (unfolded - glyph.corners[c]) * a;
            }
            break;
        }

        case TextEffectType::Rainbow: {
            const float hue = fract(progress + layer.phaseStep * (float)index / (2.0f * PI));
            glyph.color = mixColor(glyph.color, hsl(hue, layer.saturation, layer.lightness), layer.mix);
            break;
        }

        case TextEffectType::Gradient: {
            // TMP Effect: the gradient at each glyph's progress, the period wrapping
            const float q = oneShotProgress(index, count, fract(progress), layer.spread, characters, textCharacters);
            glyph.color = mixColor(glyph.color, evaluateGradient(layer.gradient, q), layer.mix);
            break;
        }
    }
}

const char* TextEffects::typeName(TextEffectType type){
    const size_t index = (size_t)type;
    return index < sizeof(typeNames) / sizeof(typeNames[0]) ? typeNames[index] : typeNames[0];
}

bool TextEffects::parseType(const char* name, TextEffectType& type){
    if (!name)
        return false;
    for (size_t i = 0; i < sizeof(typeNames) / sizeof(typeNames[0]); i++){
        if (std::strcmp(typeNames[i], name) == 0){
            type = (TextEffectType)i;
            return true;
        }
    }
    return false;
}

const char* TextEffects::mixName(TextEffectMix mix){
    const size_t index = (size_t)mix;
    return index < 3 ? mixNames[index] : mixNames[0];
}

bool TextEffects::parseMix(const char* name, TextEffectMix& mix){
    if (!name)
        return false;
    for (size_t i = 0; i < 3; i++){
        if (std::strcmp(mixNames[i], name) == 0){
            mix = (TextEffectMix)i;
            return true;
        }
    }
    return false;
}

std::vector<TextEffectLayer> TextEffects::fromTMPEffect(const char* effectType, float durationInSeconds, float amplitude,
                                                        float characterDurationRatio,
                                                        const std::vector<TextEffectGradientStop>& gradient, bool mixAdd){
    std::vector<TextEffectLayer> layers;
    const std::string type = effectType ? effectType : "";

    TextEffectLayer base;
    base.duration = durationInSeconds;
    base.spread = clamp01(characterDurationRatio);

    auto wave = [&](){
        TextEffectLayer layer = base;
        layer.type = TextEffectType::Wave;
        layer.amplitude = 0.3f * amplitude;     // TMP: 0.3 x pointSize x Amplitude
        layer.phaseStep = 0.3f;                 // TMP: 0.3 rad per character
        layers.push_back(layer);
    };
    auto oneShot = [&](TextEffectType oneShotType){
        TextEffectLayer layer = base;
        layer.type = oneShotType;
        layer.amplitude = amplitude;            // TMP: LerpUnclamped(base, shaped, Amplitude)
        layers.push_back(layer);
    };

    if (type == "Waves"){
        wave();
    }else if (type == "Grow"){
        oneShot(TextEffectType::Grow);
    }else if (type == "Unfold"){
        oneShot(TextEffectType::Unfold);
    }else if (type == "UnfoldAndWaves"){
        oneShot(TextEffectType::Unfold);
        wave();
    }else if (type == "Bounce"){
        TextEffectLayer layer = base;
        layer.type = TextEffectType::Bounce;
        layer.amplitude = 0.3f * amplitude;
        layers.push_back(layer);
    }else if (type == "Sketch"){
        TextEffectLayer layer = base;
        layer.type = TextEffectType::Shake;
        layer.amplitude = 0.1f * amplitude;     // TMP: 0.1 x pointSize x Amplitude
        layer.frequency = durationInSeconds > 0.0f ? 1.0f / durationInSeconds : 12.0f;
        layers.push_back(layer);
    }

    // a TMP gradient of more than one white key colours every effect
    bool plain = true;
    for (const TextEffectGradientStop& stop : gradient){
        if (stop.color != Vector4(1.0f, 1.0f, 1.0f, 1.0f))
            plain = false;
    }
    if (!gradient.empty() && !plain){
        TextEffectLayer layer = base;
        layer.type = TextEffectType::Gradient;
        layer.gradient = gradient;
        layer.mix = mixAdd ? TextEffectMix::Add : TextEffectMix::Multiply;
        layers.push_back(layer);
    }

    return layers;
}
