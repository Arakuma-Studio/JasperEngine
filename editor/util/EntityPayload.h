// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "ecs/Entity.h"

namespace jasperengine {

    // The "entity" drag payload: a GameObject id and the scene it belongs to. An id means
    // something only in its own scene (every scene numbers its GameObjects from the same value),
    // so every drop target checks the scene (sameSceneDropProblem, referenceDropProblem below).
    // A Project window drop encodes the dragged selection itself: nothing follows the header.
    struct EntityPayload {
        Entity entity;
        Entity parent;
        size_t order;
        bool hasTransform;
        uint32_t entitySceneId = 0; // the scene the id belongs to (0 = the selected scene)
        bool referenceOnly = false; // a child scene's GameObject shown under its parent: usable as a reference only
    };

    struct MaterialPayload {
        uint32_t magic;
        uint32_t sceneId;
        Entity entity;
        unsigned int submeshIndex;
    }; // YAML string follows

    struct TileRectPayload {
        uint32_t sceneId;
        Entity entity;
        int rectIndex;
    };

}

namespace jasperengine::editor {

    class Project;

    // The scene a dropped GameObject belongs to (0 resolves to `fallbackSceneId`)
    uint32_t payloadSceneId(const EntityPayload& payload, uint32_t fallbackSceneId);
    // "" when a target acting on a GameObject of `targetSceneId` may take the payload, else the
    // tooltip ("GameObject belongs to child scene \"HUD\""). moves: the target moves or copies the
    // GameObject itself (reparent, make a prefab), which a reference-only drag never does
    std::string sameSceneDropProblem(const Project& project, const EntityPayload& payload, uint32_t targetSceneId, bool moves = false);
    // "" when a GameObject reference of `ownerSceneId` may point at the payload's GameObject (a
    // scene loaded with it: Project::getRelatedScenes), else the tooltip
    std::string referenceDropProblem(const Project& project, const EntityPayload& payload, uint32_t ownerSceneId);

}
