// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Catalog.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // Apply Atmosphere Preset: one undo step that sets the fields a Fog or Volumetric Clouds
    // preset (engine/core/render/AtmospherePresets.h) changes, on every GameObject given, as
    // property edits (each Inspector row and Play Mode Changes see them as such). The
    // Inspector's Presets menus, MCP apply_atmosphere_preset and the scene-dump ops use it.
    class AtmospherePresetCmd {
    public:
        // Null with `error` when the preset is unknown for the component, a GameObject lacks
        // it, or nothing would change (error empty then)
        static Command* build(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, ComponentType component,
                              const std::string& presetId, std::string& error);
    };

}
