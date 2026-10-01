// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Helpers shared by the Project prefab translation units (editor/project/ProjectPrefab*.cpp).
// Internal to Project: not part of its interface.

#pragma once

#include "Catalog.h"
#include "registry/EntityRegistry.h"

#include <unordered_set>

namespace jasperengine::editor::prefabinternal {

    // True when a component holds an entity reference pointing outside the prefab instance
    // (defined in ProjectPrefabRefs.cpp)
    bool componentHasExternalEntityRef(EntityRegistry* registry, Entity entity, editor::ComponentType componentType, const std::unordered_set<Entity>& memberLocals);

}
