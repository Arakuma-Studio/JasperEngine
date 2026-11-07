// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TEXT_EFFECT_COMPONENT_H
#define TEXT_EFFECT_COMPONENT_H

#include "Engine.h"
#include "util/FunctionSubscribe.h"
#include "util/TextEffects.h"
#include "math/Vector4.h"

#include <array>
#include <string>
#include <vector>

namespace jasperengine{

    constexpr int MAX_TEXT_EFFECT_LAYERS = 4;

    // Per-character animation of the Text on the same entity (TextMesh Pro Effect, Febucci):
    // up to 4 layers (wave, bounce, typewriter, gradient...) applied in order, positions
    // accumulating and colours composing (util/TextEffects.h has the formulas). UISystem runs it
    // after the layout and uploads only the vertex buffer. Unscaled UI time by default: a pause
    // menu animates at Engine.timeScale 0.
    struct JASPERENGINE_API TextEffectComponent{
        bool enabled = true;

        std::array<TextEffectLayer, MAX_TEXT_EFFECT_LAYERS> layers;

        // starts when the text shows (TMP AutoPlay); loop: one-shot layers start again once
        // they all finished (TMP Repeat)
        bool autoPlay = true;
        bool loop = true;
        bool useUnscaledTime = true;

        // the characters it animates (grapheme clusters: a letter and its marks); length -1 to
        // the end. forWords (space separated whole words, TMP ForWords) overrides the range
        int startIndex = 0;
        int length = -1;
        std::string forWords;

        // events: every one-shot layer reached its end (not with no one-shot layer, nor on an
        // empty text); a typewriter showed character `index` (0-based within the range)
        FunctionSubscribe<void()> onComplete;
        FunctionSubscribe<void(int)> onCharacterRevealed;

        // every event, for the clears at Stop and on tag removal (tools/check_event_clears.py)
        template<typename F> void forEachEvent(F&& f) { f(onComplete); f(onCharacterRevealed); }

        // runtime (not saved)
        bool playing = false;
        bool finished = false;
        bool started = false;           // autoPlay ran (or play() was called)
        double time = 0.0;              // seconds playing, looping layers
        double cycleTime = 0.0;         // seconds into the current one-shot cycle
        int revealed = 0;               // characters a typewriter has shown in this cycle
        bool previewInEditor = false;   // the Inspector's Play: ticks in edit mode
        bool needRestart = false;       // restart() or a text change: from 0 next update
        bool needApply = true;          // the base vertices changed (layout, text, colour)
        bool applied = false;           // the vertices show the effect (not the plain text)
        bool wasActive = false;         // shown last update (autoPlay starts on showing)
        unsigned long appliedBuild = 0; // TextComponent::buildCount of the base below
        // the vertex positions and colour the text was built with, before the effect
        std::vector<Vector3> basePositions;
        Vector4 baseColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        std::string appliedText;
        unsigned int appliedFontSize = 0;
        // the layer types the last edit saw: an edit that changes a type moves an amplitude left
        // at the old type's default to the new one's (TextEffects::setType; editor and MCP)
        std::array<TextEffectType, MAX_TEXT_EFFECT_LAYERS> editedTypes{};
    };

}

#endif //TEXT_EFFECT_COMPONENT_H
