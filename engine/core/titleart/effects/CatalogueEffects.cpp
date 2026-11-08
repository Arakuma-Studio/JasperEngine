// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TfxFamilies.h"
#include "TitleFxFamilies.h"

using namespace jasperengine::titleart;

// S83's effect catalogue (docs/specs/S83-title-effects-generators.md §6.6), one family per file of
// this folder, after S80's core effects
void jasperengine::titleart::registerCatalogueEffects(TitleEffectRegistry& registry) {
    registerLightEffects(registry);
    registerCatalogueBlurEffects(registry);
    registerDepthEffects(registry);
    registerDistortEffects(registry);
    registerStylizeEffects(registry);
    registerSurfaceEffects(registry);
    registerEdgeEffects(registry);
    registerShatterEffect(registry);
    registerMaterialEffect(registry);
}
