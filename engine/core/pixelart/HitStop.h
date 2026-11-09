// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_HITSTOP_H
#define PIXELART_HITSTOP_H

// Hit-Stop (docs/specs/S48-pixel-fx.md R20-R21, D9): the game freezes (or slows) for a moment on
// a hit, on the engine clock. The countdown runs on unscaled time; the frame's game delta
// (Time.deltaTime, the fixed-step accumulator, the game-time scheduler, scene time) is multiplied
// by the hit's factor for the part of the frame it covers. Systems that pick a delta per entity
// ask deltaFor(), which gives Hit Stop Exempt GameObjects (and a hit's `exclude` list) the
// unfrozen delta, and, for a hit with an `only` list, the frozen delta to those GameObjects
// alone while the world runs. Idle, beginFrame is a compare and deltaFor returns its argument.
//
// Main thread only (like the engine clock it feeds).

#include "Export.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine{
    class Scene;
}

namespace jasperengine::pixelart{

    // a GameObject of a hit's exclude / only list (its descendants count too)
    struct JASPERENGINE_API HitStopTarget{
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
    };

    // what the hit-stops asked for since resetStats (the contact sheet's hit_stop_length lint)
    struct JASPERENGINE_API HitStopStats{
        uint64_t requests = 0;
        double totalSeconds = 0.0;
        double longestSeconds = 0.0;
    };

    class JASPERENGINE_API HitStop{
    public:
        struct Options{
            // the game delta's factor while it runs, clamped to [0, 1] (0: frozen)
            float timeScale = 0.0f;
            // these GameObjects (and descendants) keep running for this hit
            std::vector<HitStopTarget> exclude;
            // non-empty: only these GameObjects (and descendants) freeze, the world runs
            std::vector<HitStopTarget> only;
            // a Shake Preset (name or path) played on every global shaker, on unscaled time
            std::string shakePreset;
            // > 0: a pixel kick of this amplitude (art pixels) over shakeSeconds, unscaled
            float shakeAmplitudePx = 0.0f;
            float shakeSeconds = 0.0f;
        };

        // Starts (or extends) a hit-stop of `seconds` of unscaled time. Overlapping calls keep
        // the longer remaining time and the lower scale; an `only` hit replaces a running world
        // hit's lists (and the other way round), lists of the same kind add up.
        static void play(float seconds);
        static void play(float seconds, const Options& options);
        static void stop();
        static bool isActive();
        // unscaled seconds left
        static float getRemaining();
        // the factor of the current hit (1 when idle)
        static float getFactor();
        // the frame's game delta without the hit-stop (frame time times Time.timeScale)
        static double getUnfrozenDeltaTime();

        // the frame's resolved flags: a Hit Stop Exempt GameObject (includeChildren reaching
        // down), or one of the hit's exclude targets and their descendants
        static bool isExempt(Scene* scene, Entity entity);
        // The delta an entity's per-entity system should use, from the delta `dt` it got this
        // frame: unchanged when idle; the unfrozen delta for an exempt entity; for an `only`
        // hit, the frozen delta for those GameObjects and `dt` for the rest.
        static double deltaFor(Scene* scene, Entity entity, double dt);
        // true while this frame is (partly) covered by a hit-stop: deltaFor may change a delta
        static bool isFrameActive();

        // Engine::runFrame, before the frame's game delta: counts down by the frame's unscaled
        // delta and returns the factor of the world's game delta for this frame (1 when idle or
        // for an `only` hit)
        static double beginFrame(double unscaledDelta, double unfrozenDelta);
        // the factor beginFrame gave the world this frame (the game-time scheduler's)
        static double getWorldFactor();

        // The script being called (Lua engine events, messages): Time.deltaTime and
        // Engine.deltatime inside it are deltaFor(that GameObject)
        class JASPERENGINE_API ScriptScope{
        public:
            ScriptScope(Scene* scene, Entity entity);
            ~ScriptScope();
            ScriptScope(const ScriptScope&) = delete;
            ScriptScope& operator=(const ScriptScope&) = delete;
        private:
            Scene* previousScene;
            Entity previousEntity;
        };
        // Engine::getDeltatime() as the current script sees it
        static double getScriptDeltaTime();

        static HitStopStats getStats();
        static void resetStats();

        // engine shutdown and Play stop: no hit, no flags, no stats
        static void reset();
    };

}

#endif // PIXELART_HITSTOP_H
