// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "titleart/TitleEffectRegistry.h"
#include "titleart/effects/TitleFxFamilies.h"

using namespace jasperengine::titleart;

// The built-in effect definitions, one file per family in this folder (S80's ten core effects,
// S83's catalogue): each family's register function is called from here, in menu order.
void jasperengine::titleart::registerBuiltinTitleEffects(TitleEffectRegistry& registry) {
    registerGlowEffects(registry);
    registerShadowEffects(registry);
    registerStrokeEffects(registry);
    registerColorEffects(registry);
    registerTextureEffects(registry);
    registerBlurEffects(registry);
}
