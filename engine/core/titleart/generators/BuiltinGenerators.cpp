// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "titleart/TitleEffectRegistry.h"
#include "titleart/effects/TitleFxFamilies.h"
#include "titleart/generators/TitleGenFamilies.h"

using namespace jasperengine::titleart;

// The built-in generator definitions, one file per family in this folder (S80's Fill and Pattern
// Fill, S83's catalogue): each family's register function is called from here, in menu order.
void jasperengine::titleart::registerBuiltinTitleGenerators(TitleEffectRegistry& registry) {
    registerFillGenerators(registry);
    registerParticleGenerators(registry);
    registerRingGenerators(registry);
    registerLineGenerators(registry);
    registerLightGenerators(registry);
    registerEnergyGenerators(registry);
    registerNoiseGenerators(registry);
}
