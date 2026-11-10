// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S83-title-effects-generators.md §6.7, §6.10)
// - Purpose: the register functions of S83's generator families, one per file of generators/
//   (Particles, Lines, Light, Rings, Energy, Fill & Pattern), called by
//   registerBuiltinTitleGenerators after S80's Fill family.
// - Thread-safety: called once while TitleEffectRegistry::get() builds the table.

#pragma once

#include "titleart/TitleEffectRegistry.h"

namespace jasperengine::titleart {

    void registerParticleGenerators(TitleEffectRegistry& registry);    // Particles, Ink Splatter
    void registerLineGenerators(TitleEffectRegistry& registry);        // Speed Lines, Light Streaks, Sunburst
    void registerLightGenerators(TitleEffectRegistry& registry);       // Glints, Lens Flare, Backlight
    void registerRingGenerators(TitleEffectRegistry& registry);        // Shockwave, Magic Circle, Brush Swirl
    void registerEnergyGenerators(TitleEffectRegistry& registry);      // Fire, Smoke, Aura, Lightning
    void registerNoiseGenerators(TitleEffectRegistry& registry);       // Fractal Noise

}
