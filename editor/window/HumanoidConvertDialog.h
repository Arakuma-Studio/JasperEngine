// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Convert to Humanoid Clip... (anim-retarget spec 9): the dialog the Project window's clip menu,
// the clip asset Inspector and an Animation Player entry's clip menu open. Source model (default:
// the clip's model, source or preview model; required), Output (default "<name> (Humanoid).anim"
// next to the source) and, from a player entry, Use in this player (the entry switches to the file
// in the same scene step; otherwise the file alone, a project step): ClipSceneActions::convertToHumanoid.
// Main thread; App draws it every frame.

#pragma once

#include "ecs/Entity.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace jasperengine::editor{

    class Project;

    class HumanoidConvertDialog{
    public:
        // `source`: a .anim (project- or asset-relative) or "<model>#<clip>"; `player`/`entry`: the
        // Animation Player entry it was opened from (NULL_ENTITY: none)
        static void open(Project* project, const std::string& source, uint32_t sceneId = 0, Entity player = NULL_ENTITY,
                         size_t entry = SIZE_MAX);
        static void draw(Project* project);
    };

}
