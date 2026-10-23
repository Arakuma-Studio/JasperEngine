// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShadowTiers.h"

#include <cstring>

using namespace jasperengine;

namespace {
    constexpr uint64_t MB = 1024ull * 1024ull;

    // the budget table of docs/rendering/shadow-budgets.md (spec S10 §5)
    const ShadowTierSettings tierSettings[] = {
        // cascades, slot, spot, point face, distance, soft filter, transition, receiver plane, freeze, memory
        {2, 1024, 512, 256, 40.0f, ShadowSoftFilter::GRID3, CascadeTransition::DITHER, false, true, 16 * MB},
        {3, 2048, 1024, 512, 60.0f, ShadowSoftFilter::GRID5, CascadeTransition::BLEND, true, true, 48 * MB},
        {4, 2048, 1024, 512, 100.0f, ShadowSoftFilter::VOGEL, CascadeTransition::BLEND, true, false, 96 * MB},
        {4, 4096, 1024, 512, 150.0f, ShadowSoftFilter::PCSS, CascadeTransition::BLEND, true, false, 288 * MB},
    };

    const char* const tierNames[] = {"low", "medium", "high", "ultra"};
}

const ShadowTierSettings& ShadowTiers::getSettings(ShadowTier tier){
    int index = static_cast<int>(tier);
    if (index < 0 || index > 3)
        index = 2;
    return tierSettings[index];
}

float ShadowTiers::pointKernelReach(ShadowTier tier){
    // a bilinear comparison tap reads one texel past its centre; the 3x3 grid's taps sit 1.5
    // texels out, the 5x5's 2.5, a Vogel disk's at most 8 (points never take PCSS)
    switch (getSettings(tier).softFilter){
        case ShadowSoftFilter::GRID3: return 2.5f;
        case ShadowSoftFilter::GRID5: return 3.5f;
        case ShadowSoftFilter::VOGEL:
        case ShadowSoftFilter::PCSS:  return 9.0f;
    }
    return 9.0f;
}

ShadowTier ShadowTiers::defaultTier(int platform){
    // Engine::Platform: MacOS, iOS, Web, Android, Linux, Windows
    switch (platform){
        case 1: // iOS
        case 3: // Android
            return ShadowTier::LOW;
        case 2: // Web
            return ShadowTier::MEDIUM;
        default:
            return ShadowTier::HIGH;
    }
}

const char* ShadowTiers::getName(ShadowTier tier){
    int index = static_cast<int>(tier);
    if (index < 0 || index > 3)
        return "high";
    return tierNames[index];
}

bool ShadowTiers::fromName(const char* name, ShadowTier& tier){
    if (!name)
        return false;
    for (int i = 0; i < 4; i++){
        if (std::strcmp(name, tierNames[i]) == 0){
            tier = static_cast<ShadowTier>(i);
            return true;
        }
    }
    return false;
}
