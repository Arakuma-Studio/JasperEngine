// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The game.json of a build (engine/core/player/PlayerConfig.h): what the project's settings make
// of a game at start, the same for a prebuilt build, a Compile build and the project's own CMake
// build (docs/specs/S98-editor-tooling-gaps.md D1)

#include "ShippingTypes.h"
#include "cook/SceneCooker.h"
#include "player/PlayerConfig.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::cook {

    struct GameConfigTarget {
        // a build for one platform (its native plugins, its app id); none: a source project that
        // builds for several, whose plugins are named per target
        std::optional<BuildPlatform> platform;
        // the scene the game starts with; 0: the project's start scene
        uint32_t startScene = 0;
        // the name when the project has none (Exporter::getAppName)
        std::string fallbackName;
        // the prefabs SceneCooker::cookProject (or cookPrefabs) wrote, by PrefabManager id
        std::vector<CookedPrefabInfo> prefabs;
        // a Build Profile's settings snapshot (Project::makeSettingsSnapshot) the settings come
        // from, and the scenes it ships (its Scene List and the scenes they involve); null: the
        // project's own settings, every scene in the build
        Project* settings = nullptr;
        const std::set<uint32_t>* scenes = nullptr;
    };

    // Everything but the icon and the Build Profile's name and defines, which the caller sets when
    // its build has them
    player::PlayerConfig makeGameConfig(Project& project, const GameConfigTarget& target);

}
