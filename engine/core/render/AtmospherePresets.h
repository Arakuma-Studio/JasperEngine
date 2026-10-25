// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ATMOSPHERE_PRESETS_H
#define ATMOSPHERE_PRESETS_H

#include "component/FogComponent.h"
#include "component/VolumetricCloudsComponent.h"

#include <string>
#include <vector>

namespace jasperengine{

    // The looks the Fog and Volumetric Clouds inspectors, MCP (list_atmosphere_presets,
    // apply_atmosphere_preset) and Lua (Fog:applyPreset, VolumetricClouds.preset) offer. A
    // preset sets the fields of its look and leaves the others: a fog preset makes the fog a
    // height fog of that shape and colour, and keeps its colour mode and volumetric settings.
    struct AtmospherePreset{
        std::string id;     // snake_case, as MCP and Lua name it
        std::string name;   // as the inspector shows it
        std::string description;
    };

    class JASPERENGINE_API AtmospherePresets{
    public:
        // Morning Haze, Ground Mist, Mountain Valley, Horizon Haze, Night
        static const std::vector<AtmospherePreset>& getFogPresets();
        // Custom, Sparse, Cloudy, Overcast, Stormy (in CloudsPreset order)
        static const std::vector<AtmospherePreset>& getCloudsPresets();

        // false for an unknown id
        static bool applyFogPreset(const std::string& id, FogComponent& fog);
        // sets the fields of the preset and clouds.preset; CUSTOM sets nothing but the field
        static void applyCloudsPreset(CloudsPreset preset, VolumetricCloudsComponent& clouds);
        static bool applyCloudsPreset(const std::string& id, VolumetricCloudsComponent& clouds);

        static const char* getCloudsPresetId(CloudsPreset preset);
        static bool findCloudsPreset(const std::string& id, CloudsPreset& preset);
    };

}

#endif // ATMOSPHERE_PRESETS_H
