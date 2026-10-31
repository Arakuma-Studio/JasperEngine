// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TEXTEFFECTS_H
#define TEXTEFFECTS_H

#include "Export.h"
#include "math/Vector2.h"
#include "math/Vector4.h"

#include <cstdint>
#include <vector>

namespace jasperengine {

    // Per-character text animation shared by the native Text Effect component and the HTML UI
    // `text-effect` property: one vocabulary, one set of formulas (ui-scaling-text spec 7.3).
    // Coordinates are y down (UI units or CSS px), sizes in the text's font size (em).

    enum class TextEffectType {
        None,
        Wave,       // looping: each glyph rises and falls, a phase step apart
        Bounce,     // looping: each glyph hops up, staggered by spread
        Shake,      // looping: each glyph jitters, `frequency` new offsets per second
        Pulse,      // looping: each glyph scales about its centre
        Typewriter, // one-shot: glyphs appear one after another (onCharacterRevealed)
        FadeIn,     // one-shot: glyphs fade in, staggered by spread
        Grow,       // one-shot: glyphs grow up from their bottom edge (TMP Effect Grow)
        Unfold,     // one-shot: glyphs unfold from their horizontal centre line (TMP Effect Unfold)
        Rainbow,    // looping colour: hue cycles along the text
        Gradient    // looping colour: a gradient sweeps along the text (TMP Effect Gradient)
    };

    // how a colour layer combines with the text colour (TMP Effect Mix, plus Replace)
    enum class TextEffectMix {
        Multiply,
        Add,        // rgb added, alpha multiplied (TMP Effect "Add")
        Replace
    };

    struct JASPERENGINE_API TextEffectGradientStop {
        Vector4 color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);   // sRGB, alpha
        float position = 0.0f;                              // 0..1

        bool operator==(const TextEffectGradientStop& other) const { return color == other.color && position == other.position; }
        bool operator!=(const TextEffectGradientStop& other) const { return !(*this == other); }
    };

    struct JASPERENGINE_API TextEffectLayer {
        TextEffectType type = TextEffectType::None;
        bool enabled = true;
        float amplitude = 0.15f;            // em (Wave, Bounce, Shake), scale delta (Pulse), the share
                                            // of the change (Grow, Unfold: TMP's Amplitude, default 1:
                                            // TextEffects::defaultAmplitude)
        float duration = 1.5f;              // s: the period (looping) or the length (one-shot)
        float spread = 0.5f;                // 0..1 stagger (TMP Effect CharacterDurationRatio)
        float phaseStep = 0.3f;             // rad per glyph (Wave; Rainbow: hue turns x 2 pi)
        float frequency = 12.0f;            // Hz (Shake)
        float charactersPerSecond = 30.0f;  // Typewriter (0: the whole text over duration)
        float fadeTime = 0.0f;              // s, Typewriter per-glyph fade
        std::vector<TextEffectGradientStop> gradient;   // Gradient
        TextEffectMix mix = TextEffectMix::Multiply;    // Rainbow, Gradient
        float saturation = 0.8f;            // Rainbow (HSL)
        float lightness = 0.6f;

        bool operator==(const TextEffectLayer& other) const;
        bool operator!=(const TextEffectLayer& other) const { return !(*this == other); }
    };

    // One glyph while effects apply: its quad corners (top-left, top-right, bottom-right,
    // bottom-left; y down) and its colour (sRGB, alpha; starts as the text colour)
    struct JASPERENGINE_API TextEffectGlyphState {
        Vector2 corners[4];
        Vector4 color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    };

    namespace TextEffects {

        // a one-shot type (Typewriter, FadeIn, Grow, Unfold): ends after its duration
        JASPERENGINE_API bool isOneShot(TextEffectType type);
        // moves or reshapes glyphs (Wave, Bounce, Shake, Pulse, Grow, Unfold): reduced motion skips it
        JASPERENGINE_API bool isMotion(TextEffectType type);
        // a colour type (Rainbow, Gradient)
        JASPERENGINE_API bool isColor(TextEffectType type);

        // A layer's amplitude when nothing set it: 1 (TMP Effect's Amplitude: the whole change) for
        // Grow and Unfold, 0.15 (em, scale) for the others
        JASPERENGINE_API float defaultAmplitude(TextEffectType type);
        // Changes a layer's type; an amplitude left at the old type's default takes the new one's
        JASPERENGINE_API void setType(TextEffectLayer& layer, TextEffectType type);

        // TMP Effect's per-glyph progress of a one-shot effect (TextMeshProEffect.Apply): visible
        // glyph `index` at progress p (time / duration, clamped to 1) with stagger `spread`:
        // start = lerp(i / C, 0, spread), length = lerp(1 / T, 1, spread), q = clamp01((p - start) / length),
        // where C is the selection's characters and T the text's, spaces included (TMP's
        // characterCount); both default to `count` (the visible glyphs)
        JASPERENGINE_API float oneShotProgress(int index, int count, float progress, float spread,
                                               int characters = -1, int textCharacters = -1);

        // Typewriter: the time glyph `index` of `count` appears ((i + 1) / cps, or the
        // whole text over the duration when cps is 0)
        JASPERENGINE_API float revealTime(const TextEffectLayer& layer, int index, int count);
        // seconds a layer runs until every glyph reached its end (one-shot), 0 for looping ones
        JASPERENGINE_API float oneShotLength(const TextEffectLayer& layer, int count);

        // Applies a layer to glyph `index` of `count` at `time` seconds since it started:
        // positions accumulate, colours compose. fontSize is the em. reducedMotion skips motion.
        // `characters` / `textCharacters`: as oneShotProgress (-1: `count`).
        JASPERENGINE_API void applyLayer(const TextEffectLayer& layer, int index, int count, float time, float fontSize,
                                         bool reducedMotion, TextEffectGlyphState& glyph,
                                         int characters = -1, int textCharacters = -1);
        // the seconds until every one-shot layer reached its end, a layer reduced motion holds
        // still counting 0; `oneShot` tells whether there is any (reduced motion or not)
        JASPERENGINE_API float effectLength(const std::vector<TextEffectLayer>& layers, int count, bool reducedMotion, bool& oneShot);
        JASPERENGINE_API float effectLength(const TextEffectLayer* layers, size_t layerCount, int count, bool reducedMotion, bool& oneShot);

        // base (sRGB) combined with a layer colour
        JASPERENGINE_API Vector4 mixColor(const Vector4& base, const Vector4& color, TextEffectMix mix);
        // the gradient's colour at t (0..1); white when it has no stops
        JASPERENGINE_API Vector4 evaluateGradient(const std::vector<TextEffectGradientStop>& gradient, float t);
        // HSL (h in turns, s and l 0..1) to sRGB
        JASPERENGINE_API Vector4 hsl(float hue, float saturation, float lightness);

        // "wave", "bounce", "shake", "pulse", "typewriter", "fade_in", "grow", "unfold",
        // "rainbow", "gradient", "none" (serialization, MCP)
        JASPERENGINE_API const char* typeName(TextEffectType type);
        JASPERENGINE_API bool parseType(const char* name, TextEffectType& type);
        // "multiply", "add", "replace"
        JASPERENGINE_API const char* mixName(TextEffectMix mix);
        JASPERENGINE_API bool parseMix(const char* name, TextEffectMix& mix);

        // The layers a TextMesh Pro Effect component maps to (porting): effectType "Waves",
        // "Grow", "Unfold", "UnfoldAndWaves", "Bounce" (Sketch maps to Shake); amplitude in TMP
        // units (Waves: x 0.3 em); a gradient adds a Gradient layer with TMP's Mix
        JASPERENGINE_API std::vector<TextEffectLayer> fromTMPEffect(const char* effectType, float durationInSeconds, float amplitude,
                                                                     float characterDurationRatio,
                                                                     const std::vector<TextEffectGradientStop>& gradient,
                                                                     bool mixAdd);

    }

}

#endif // TEXTEFFECTS_H
