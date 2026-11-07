// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S81-title-art-runtime.md R6)
// - Purpose: one place that makes a GameObject showing a .titleart, for the Hierarchy, Scene view
//   and Game view drops and S82's create menus: an Image + Title Art Renderer in a UI scene or
//   under a UI element, else a Sprite + Title Art Renderer, at the bake's native size, one undo
//   step "Create Title Art (logo)".
// - Thread-safety: main thread (commands).

#pragma once

#include "ecs/Entity.h"

#include <cstdint>
#include <string>

namespace jasperengine::editor {

    class Project;

    // the title belongs in UI space here (a UI scene, or `parent` is a UI element)
    bool titleArtDropIsUI(Project* project, uint32_t sceneId, Entity parent);
    // creates it (one undo step); `titlePath` absolute or project-relative; NULL_ENTITY with `error`
    Entity createTitleArtObject(Project* project, uint32_t sceneId, const std::string& titlePath, Entity parent, std::string* error = nullptr);
    // a .titleart dropped on an Image's or a Sprite's texture: the component is added (one undo
    // step) instead of the drop failing; false with `error`
    bool addTitleArtToCompanion(Project* project, uint32_t sceneId, Entity entity, const std::string& titlePath, std::string* error = nullptr);

}
