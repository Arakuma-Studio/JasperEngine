// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The timing of an effect, analytic (docs/specs/S91-effect-authoring.md R19): for every system of
// a .particles file or a GameObject subtree, and for each of their sub-emitter children down the
// tree, when it starts, how long it emits and how long its particles live, read from the
// documents alone (no simulation, so it is the same on every machine and costs microseconds).
// The Particle Effect window draws its timing bars from it; MCP gets the same rows through
// toJson (the preview_particle_effect tool's `timing`).
//
// A child emits on its parent's events: Birth from the parent's first emission to its last,
// Death from the parent's shortest lifetime after its first emission to its last particle's
// death, Collision and Trigger anywhere in the parent's life (`eventDriven`: the times are a
// bound, not a prediction). A looping system emits for ever: `emitEnd` and `end` are infinite.

#include "particles/ParticleEffect.h"

#include "json.hpp"

#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct ParticleTimingRow {
        std::string name;          // the GameObject's name, or the child file's stem
        std::string effect;        // the .particles (assets-relative), empty for an inline effect
        int depth = 0;             // 0: a system; 1 and more: a sub-emitter child
        int parent = -1;           // the parent's row
        int subEmitter = -1;       // the child's entry in the parent's Sub Emitters list
        std::string trigger;       // a child's: birth, death, collision, trigger
        bool looping = false;
        bool prewarm = false;
        bool eventDriven = false;  // Collision or Trigger somewhere up the tree
        bool missing = false;      // the child's file does not read: no times
        float start = 0.0f;        // the first emission (the start delay's lowest)
        float startMax = 0.0f;     // its latest (a random start delay)
        float emitEnd = 0.0f;      // the last emission, infinite when looping
        float lifetimeMin = 0.0f;
        float lifetimeMax = 0.0f;
        float end = 0.0f;          // the last particle dies, infinite when looping
        std::vector<float> bursts; // the bursts' times (a system's, in its first loop)

        static constexpr float kForever = std::numeric_limits<float>::infinity();
    };

    // One system to time: its name, its effect file (empty for inline) and its document
    struct ParticleTimingSystem {
        std::string name;
        std::string effect;
        const particles::ParticleEffectDocument* document = nullptr;
    };

    class ParticleEffectTiming {
    public:
        // the document of a child's file (null: unreadable); the default reads the effect library
        using Resolver = std::function<const particles::ParticleEffectDocument*(const std::string& path)>;

        static std::vector<ParticleTimingRow> compute(const std::vector<ParticleTimingSystem>& systems, const Resolver& resolve = {});
        // the latest finite time of the rows (the window's default span), at least `minimum`
        static float span(const std::vector<ParticleTimingRow>& rows, float minimum = 1.0f);
        // {rows: [{name, effect, depth, parent, sub_emitter, trigger, looping, prewarm, event_driven,
        // start, start_max, emit_end, lifetime: [min, max], end, bursts}], span}; infinite times are null
        static nlohmann::ordered_json toJson(const std::vector<ParticleTimingRow>& rows);
    };

}
