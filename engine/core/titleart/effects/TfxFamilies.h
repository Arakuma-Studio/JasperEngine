// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S83-title-effects-generators.md R1, §6.10)
// - Purpose: the register functions of S83's effect families, one per file of effects/, called by
//   registerCatalogueEffects (TitleFxFamilies.h) after S80's core effects. The Add Effect menu
//   sorts by category and label itself, so the order here is the families'.
// - Thread-safety: called once while TitleEffectRegistry::get() builds the table.

#pragma once

#include "titleart/TitleEffectRegistry.h"

namespace jasperengine::titleart {

    void registerLightEffects(TitleEffectRegistry& registry);       // Light Glow, Contour Lines, Neon Tube
    void registerCatalogueBlurEffects(TitleEffectRegistry& registry);   // Motion, Lens, Tilt-Shift, Radial Blur
    void registerDepthEffects(TitleEffectRegistry& registry);       // Extrude, Perspective Shadow, Floor Reflection
    void registerDistortEffects(TitleEffectRegistry& registry);     // Wave Warp, Motion Trail, Chromatic Aberration, Glitch
    void registerStylizeEffects(TitleEffectRegistry& registry);     // Halftone, Scanlines, Pixelate, Grain, Holographic Foil
    void registerSurfaceEffects(TitleEffectRegistry& registry);     // Distress, Drips, Frost, Chalk
    void registerEdgeEffects(TitleEffectRegistry& registry);        // Ink Brush Stroke, Stitch
    void registerShatterEffect(TitleEffectRegistry& registry);      // Shatter
    void registerMaterialEffect(TitleEffectRegistry& registry);     // Material

}
