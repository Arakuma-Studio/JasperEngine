// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.5)
// - Purpose: the register functions of the built-in definition families, one per file of
//   effects/ and generators/, called in Add menu order by registerBuiltinTitleEffects and
//   registerBuiltinTitleGenerators. S83 adds its families here.
// - Thread-safety: called once while TitleEffectRegistry::get() builds the table.

#pragma once

#include "titleart/TitleEffectRegistry.h"

namespace jasperengine::titleart {

    // effects (S80): Glow, Shadow & Depth, Stroke & Edge, Color, Texture & Material, Blur
    void registerGlowEffects(TitleEffectRegistry& registry);
    void registerShadowEffects(TitleEffectRegistry& registry);
    void registerStrokeEffects(TitleEffectRegistry& registry);
    void registerColorEffects(TitleEffectRegistry& registry);
    void registerTextureEffects(TitleEffectRegistry& registry);
    void registerBlurEffects(TitleEffectRegistry& registry);

    // generators (S80): Fill & Pattern
    void registerFillGenerators(TitleEffectRegistry& registry);

}
