// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md "Upstream absorption"; upstream anim layers spec 9, 10)
// - Purpose: who uses an Avatar Mask or an Animator Controller, for the mask and controller
//   Inspectors' Users lists and MCP get_animation {mask} / get_animator {controller}:
//   * a mask: the Animation Players' layers (maskGuid first, then the path) of the open scenes,
//     prefab stages and loaded prefab assets, and the layers of the project's .controller files
//     (the mask ref's GUID first, then its path);
//   * a controller: the Animators (controller GUID first, then the path) of the same places.
// - Thread-safety: main thread (reads the project's scenes; controller files are read from disk).

#pragma once

#include "ecs/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    struct AnimatorAssetUser {
        uint32_t sceneId = 0;               // 0 for a loaded prefab asset that is not open in a stage
        Entity entity = NULL_ENTITY;
        int layer = -1;                     // the player's or the controller's layer
        bool prefabAsset = false;
        std::string prefabPath;             // prefabAsset: its project-relative path
        std::string controllerFile;         // a .controller (project-relative) whose layer `layer` uses the mask
    };

    // `asset`: a path in any accepted form (absolute, project- or asset-relative), or a GUID
    std::vector<AnimatorAssetUser> findMaskUsers(Project* project, const std::string& asset);
    std::vector<AnimatorAssetUser> findControllerUsers(Project* project, const std::string& asset);

}
