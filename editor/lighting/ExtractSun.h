// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Extract Sun (spec S99 R28, R29): the sun of the scene's sky image (lighting/HdriSun.h), turned
// and tinted as the sky draws it, as the scene's main Directional Light: created when the scene
// has none, else the brightest directional light aligned to it (direction, colour, intensity in
// its unit: lux for a light in physical units). One undo step; with Remove Sun Disc, the sky
// images' import settings too, linked to it. The Skybox Inspector, MCP (set_scene_settings
// extract_sun) and the CLI (call) share it.

#pragma once

#include "ecs/Entity.h"
#include "math/Vector3.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {
    enum class LightUnit;   // component/LightComponent.h
}

namespace jasperengine::editor {

    class Project;

    struct ExtractSunResult {
        Entity light = NULL_ENTITY;
        bool created = false;
        Vector3 direction;                  // the light's world direction (from the sun)
        Vector3 color;                      // linear, largest channel 1
        float intensity = 0.0f;             // in `unit`
        LightUnit unit{};                   // (value 0: ENGINE)
        float engineIntensity = 0.0f;
        float peakRatio = 0.0f;             // the sun's brightest block against the sky's mean luminance
        float discDegrees = 0.0f;           // the sun disc's angular radius
        // the sky images Remove Sun Disc was set on (project-relative)
        std::vector<std::string> sunDiscTextures;
    };

    namespace ExtractSun {
        // false with an error naming the fix: no sky, a sky without an image file, no distinct
        // sun, Remove Sun Disc on an LDR sky, a refused edit
        bool apply(Project* project, uint32_t sceneId, bool removeSunDisc, ExtractSunResult& out, std::string& error);
    }

}
