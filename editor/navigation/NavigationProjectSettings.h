// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Project Settings > Navigation (project.yaml `navigation:`, docs/navigation.md): the agent
// types and the area table every navmesh, crowd and grid of the project reads, the crowd, web
// bake, grid and carving budgets, and whether exported builds carry Recast/Detour.

#include "navigation/NavTypes.h"

#include "yaml-cpp/yaml.h"

namespace jasperengine::editor {

    struct NavigationProjectSettings {
        nav::NavigationSettings settings;
        // JASPERENGINE_NAVIGATION in exported builds. Off strips Recast/Detour: navmesh bakes,
        // queries and agents on navmeshes fail (logged once), grids keep working.
        bool includeInBuilds = true;

        bool operator==(const NavigationProjectSettings& other) const {
            return settings == other.settings && includeInBuilds == other.includeInBuilds;
        }
        bool operator!=(const NavigationProjectSettings& other) const { return !(*this == other); }
    };

    // Only what differs from the defaults (an empty node when nothing does: an old project.yaml
    // stays as it was)
    YAML::Node encodeNavigationProjectSettings(const NavigationProjectSettings& settings);
    // Missing keys keep their defaults; the result is sanitized (costs >= 1, fixed areas, ids)
    NavigationProjectSettings decodeNavigationProjectSettings(const YAML::Node& node);

    // The C++ a standalone build's main.cpp runs before its first scene (empty for the defaults)
    std::string generateNavigationSettingsCode(const nav::NavigationSettings& settings, const std::string& indent);

}
