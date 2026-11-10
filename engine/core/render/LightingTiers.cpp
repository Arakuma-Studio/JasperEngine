// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LightingTiers.h"

#include "Engine.h"
#include "device/Device.h"

#include <algorithm>
#include <cstring>

using namespace jasperengine;

namespace {
    // -1: the platform's default
    int currentTier = -1;
    LightMode currentMode = LightMode::AUTOMATIC;

    const char* const modeNames[] = {"automatic", "per_object", "tiled"};
    const char* const lightingTierNames[] = {"low", "medium", "high", "ultra"};
}

LightingTier LightingTiers::defaultTier(int platform){
    // Engine::Platform: MacOS, iOS, Web, Android, Linux, Windows
    switch (platform){
        case 1: // iOS
        case 3: // Android
            return LightingTier::LOW;
        case 2: // Web
            return LightingTier::MEDIUM;
        default:
            return LightingTier::HIGH;
    }
}

LightingTier LightingTiers::getTier(){
    if (currentTier >= 0)
        return static_cast<LightingTier>(currentTier);
    // the device's tier where one applies (S56: detected in a game, simulated or set in the
    // editor); DeviceTier and LightingTier share their order
    if (Device::isTierApplied())
        return static_cast<LightingTier>(std::clamp(static_cast<int>(Device::getTier()), 0, 3));
    return defaultTier(static_cast<int>(Engine::getPlatform()));
}

void LightingTiers::setTier(LightingTier tier){
    currentTier = std::clamp(static_cast<int>(tier), 0, 3);
}

void LightingTiers::resetTier(){
    currentTier = -1;
}

void LightingTiers::setLightMode(LightMode mode){
    const int value = static_cast<int>(mode);
    currentMode = (value >= 0 && value <= 2) ? mode : LightMode::AUTOMATIC;
}

LightMode LightingTiers::getLightMode(){
    return currentMode;
}

bool LightingTiers::usesTiledLights(LightMode mode, LightingTier tier){
    switch (mode){
        case LightMode::PER_OBJECT: return false;
        case LightMode::TILED: return true;
        case LightMode::AUTOMATIC: return tier != LightingTier::LOW;
    }
    return false;
}

bool LightingTiers::usesTiledLights(){
    return usesTiledLights(currentMode, getTier());
}

void LightingTiers::tileGrid(LightingTier tier, int& columns, int& rows){
    if (tier == LightingTier::HIGH || tier == LightingTier::ULTRA){
        columns = 32;
        rows = 18;
    }else{
        columns = 16;
        rows = 9;
    }
}

int LightingTiers::contactShadowDivisor(LightingTier tier){
    switch (tier){
        case LightingTier::LOW: return 0;
        case LightingTier::MEDIUM: return 2;
        default: return 1;
    }
}

const char* LightingTiers::getModeName(LightMode mode){
    const int index = static_cast<int>(mode);
    return (index >= 0 && index <= 2) ? modeNames[index] : modeNames[0];
}

bool LightingTiers::modeFromName(const char* name, LightMode& mode){
    if (!name)
        return false;
    for (int i = 0; i < 3; i++){
        if (std::strcmp(name, modeNames[i]) == 0){
            mode = static_cast<LightMode>(i);
            return true;
        }
    }
    return false;
}

const char* LightingTiers::getTierName(LightingTier tier){
    const int index = static_cast<int>(tier);
    return (index >= 0 && index <= 3) ? lightingTierNames[index] : lightingTierNames[2];
}

bool LightingTiers::tierFromName(const char* name, LightingTier& tier){
    if (!name)
        return false;
    for (int i = 0; i < 4; i++){
        if (std::strcmp(name, lightingTierNames[i]) == 0){
            tier = static_cast<LightingTier>(i);
            return true;
        }
    }
    return false;
}
