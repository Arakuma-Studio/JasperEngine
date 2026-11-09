// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The pixel particle presets (docs/specs/S49-pixel-particles.md R20, §6.6, D7): .particles files
// with pixelfx's tuned constants (src/core/sim.js, MIT License, Copyright (c) 2026 Patch, adapted
// from Sprite Fusion's pixel art VFX, MIT License, Copyright (c) 2026 Hugo Duprez; NOTICE.md),
// embedded from editor/pixelart/particles. Their constants are base pixels per second (and per
// second squared); a preset made for a project is scaled by its Pixels Per Unit and records it.
// Their colours are ramps of builtin:pixelfx-effects (S43 R9); a sub-emitter names another preset
// ("preset:pixel_smoke"), which the caller stores as an asset of its own.

#include "particles/ParticleEffect.h"
#include "pixelart/Palette.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor::pixelart {

    constexpr const char* kPixelPresetPalette = "builtin:pixelfx-effects";
    constexpr const char* kPresetPrefix = "preset:";

    // the nine presets offered by name (create_particle_effect, the menus, the inspector)
    const std::vector<std::string>& pixelParticlePresetNames();
    // "Spark", "Smoke Loop", ...; empty for a name that is not one
    std::string pixelParticlePresetLabel(const std::string& name);
    // the embedded text of a preset or of a child it names ("" when there is none)
    std::string_view pixelParticlePresetText(std::string_view name);
    // the presets a preset's sub-emitters name, in order (pixel_fire: pixel_smoke)
    std::vector<std::string> pixelParticlePresetChildren(const std::string& name);

    struct PixelPresetBuild {
        int pixelsPerUnit = 1;
        // the ramps' palette: an assets-relative .palette (the project's copy of the built-in);
        // empty: the built-in's colours inline (a document needing no project)
        std::string palette;
        // where a child preset is stored (its assets-relative .particles); unset: "preset:<name>"
        std::function<std::string(const std::string& child)> childPath;
    };

    // a preset (or a child) as a document for `build`; false with `error`
    bool buildPixelParticlePreset(const std::string& name, const PixelPresetBuild& build, particles::ParticleEffectDocument& out,
                                  std::string& error);

    // multiplies the effect's lengths and speeds (positions, speeds, accelerations, shape sizes,
    // collision planes and speeds; sizes too unless they are in pixels) by `factor`
    void scaleParticleMotion(particles::ParticleEffectDocument& document, float factor);

    // Match Project Palette: every ramp step to the nearest OKLab colour of the palette's ramp
    // closest to that ramp; the ramps become inline colours. False when the palette has no ramp.
    bool matchPaletteRamps(particles::ParticleEffectDocument& document, const jasperengine::pixelart::Palette& palette);

}
