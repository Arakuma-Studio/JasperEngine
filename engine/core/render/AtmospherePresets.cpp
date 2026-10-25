// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The fog presets' looks follow OpenEngine's height fog presets
// (Engine/Modules/HeightFog/Source/HeightFogEffectPresets.cpp, MIT, Copyright (c) 2026
// Gamecrafters Guild Limited and contributors), re-derived in this fog's physical fields.

#include "AtmospherePresets.h"

using namespace jasperengine;

const std::vector<AtmospherePreset>& AtmospherePresets::getFogPresets(){
    static const std::vector<AtmospherePreset> presets = {
        {"morning_haze", "Morning Haze", "A thin warm haze lit by a low sun, the sky's colour at the horizon"},
        {"ground_mist", "Ground Mist", "A dense white layer a few meters deep, clear above"},
        {"mountain_valley", "Mountain Valley", "Fog filling the valleys, peaks above it, long views"},
        {"horizon_haze", "Horizon Haze", "Clear nearby, the distance fading into the sky"},
        {"night", "Night", "A dark blue fog close to the ground, little sun scattering"},
    };
    return presets;
}

const std::vector<AtmospherePreset>& AtmospherePresets::getCloudsPresets(){
    static const std::vector<AtmospherePreset> presets = {
        {"custom", "Custom", "The fields as they are"},
        {"sparse", "Sparse", "A few small fair-weather clouds"},
        {"cloudy", "Cloudy", "Half the sky in heaped cumulus"},
        {"overcast", "Overcast", "A flat grey sheet covering the sky"},
        {"stormy", "Stormy", "Tall, dense, dark clouds"},
    };
    return presets;
}

bool AtmospherePresets::applyFogPreset(const std::string& id, FogComponent& fog){
    FogComponent look = fog;
    look.type = FogType::HEIGHT;
    look.startDistance = 0.0f;
    look.maxOpacity = 1.0f;
    look.skyFill = true;
    if (id == "morning_haze"){
        look.color = Color::sRGBToLinear(Vector3(0.86f, 0.80f, 0.72f));
        look.attenuationDistance = 600.0f;
        look.baseHeight = 0.0f;
        look.maximumHeight = 120.0f;
        look.maxFogDistance = 5000.0f;
        look.maxOpacity = 0.9f;
        look.directionalInscattering = 0.6f;
        look.anisotropy = 0.7f;
        look.skyFillAngle = 6.0f;
    }else if (id == "ground_mist"){
        look.color = Color::sRGBToLinear(Vector3(0.82f, 0.84f, 0.87f));
        look.attenuationDistance = 40.0f;
        look.baseHeight = 0.0f;
        look.maximumHeight = 6.0f;
        look.maxFogDistance = 1000.0f;
        look.directionalInscattering = 0.2f;
        look.anisotropy = 0.5f;
        look.skyFillAngle = 2.0f;
    }else if (id == "mountain_valley"){
        look.color = Color::sRGBToLinear(Vector3(0.74f, 0.79f, 0.86f));
        look.attenuationDistance = 250.0f;
        look.baseHeight = 20.0f;
        look.maximumHeight = 300.0f;
        look.maxFogDistance = 4000.0f;
        look.directionalInscattering = 0.4f;
        look.anisotropy = 0.6f;
        look.skyFillAngle = 3.0f;
    }else if (id == "horizon_haze"){
        look.color = Color::sRGBToLinear(Vector3(0.72f, 0.80f, 0.90f));
        look.attenuationDistance = 3000.0f;
        look.baseHeight = 0.0f;
        look.maximumHeight = 1500.0f;
        look.startDistance = 200.0f;
        look.maxFogDistance = 20000.0f;
        look.directionalInscattering = 0.3f;
        look.anisotropy = 0.75f;
        look.skyFillAngle = 8.0f;
    }else if (id == "night"){
        look.color = Color::sRGBToLinear(Vector3(0.06f, 0.07f, 0.11f));
        look.attenuationDistance = 200.0f;
        look.baseHeight = 0.0f;
        look.maximumHeight = 60.0f;
        look.maxFogDistance = 2000.0f;
        look.maxOpacity = 0.95f;
        look.directionalInscattering = 0.05f;
        look.anisotropy = 0.3f;
        look.skyFillAngle = 4.0f;
    }else{
        return false;
    }
    fog = look;
    return true;
}

void AtmospherePresets::applyCloudsPreset(CloudsPreset preset, VolumetricCloudsComponent& clouds){
    clouds.preset = preset;
    switch (preset){
        case CloudsPreset::SPARSE:
            clouds.coverage = 0.3f;
            clouds.density = 0.4f;
            clouds.shapeScale = 5.0f;
            clouds.shapeFactor = 0.9f;
            clouds.erosionFactor = 0.8f;
            clouds.erosionScale = 110.0f;
            clouds.bottomAltitude = 1500.0f;
            clouds.altitudeRange = 1200.0f;
            clouds.sunLightDimmer = 1.0f;
            clouds.ambientLightDimmer = 1.0f;
            break;
        case CloudsPreset::CLOUDY:
            clouds.coverage = 0.5f;
            clouds.density = 0.5f;
            clouds.shapeScale = 4.0f;
            clouds.shapeFactor = 0.8f;
            clouds.erosionFactor = 0.6f;
            clouds.erosionScale = 100.0f;
            clouds.bottomAltitude = 1200.0f;
            clouds.altitudeRange = 2000.0f;
            clouds.sunLightDimmer = 1.0f;
            clouds.ambientLightDimmer = 1.0f;
            break;
        case CloudsPreset::OVERCAST:
            clouds.coverage = 0.95f;
            clouds.density = 0.35f;
            clouds.shapeScale = 3.0f;
            clouds.shapeFactor = 0.4f;
            clouds.erosionFactor = 0.4f;
            clouds.erosionScale = 80.0f;
            clouds.bottomAltitude = 1000.0f;
            clouds.altitudeRange = 1200.0f;
            clouds.sunLightDimmer = 0.8f;
            clouds.ambientLightDimmer = 1.0f;
            break;
        case CloudsPreset::STORMY:
            clouds.coverage = 0.8f;
            clouds.density = 1.0f;
            clouds.shapeScale = 3.0f;
            clouds.shapeFactor = 0.75f;
            clouds.erosionFactor = 0.5f;
            clouds.erosionScale = 90.0f;
            clouds.bottomAltitude = 800.0f;
            clouds.altitudeRange = 4500.0f;
            clouds.sunLightDimmer = 0.6f;
            clouds.ambientLightDimmer = 0.7f;
            break;
        case CloudsPreset::CUSTOM:
            break;
    }
}

bool AtmospherePresets::applyCloudsPreset(const std::string& id, VolumetricCloudsComponent& clouds){
    CloudsPreset preset;
    if (!findCloudsPreset(id, preset))
        return false;
    applyCloudsPreset(preset, clouds);
    return true;
}

const char* AtmospherePresets::getCloudsPresetId(CloudsPreset preset){
    switch (preset){
        case CloudsPreset::CUSTOM: return "custom";
        case CloudsPreset::SPARSE: return "sparse";
        case CloudsPreset::CLOUDY: return "cloudy";
        case CloudsPreset::OVERCAST: return "overcast";
        case CloudsPreset::STORMY: return "stormy";
    }
    return "custom";
}

bool AtmospherePresets::findCloudsPreset(const std::string& id, CloudsPreset& preset){
    static const CloudsPreset all[] = {CloudsPreset::CUSTOM, CloudsPreset::SPARSE, CloudsPreset::CLOUDY,
                                       CloudsPreset::OVERCAST, CloudsPreset::STORMY};
    for (CloudsPreset candidate : all){
        if (id == getCloudsPresetId(candidate)){
            preset = candidate;
            return true;
        }
    }
    return false;
}
