// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Animation clips spec 9.R3: a Model on or under an entity with an Animation Player loads in the
// node-hierarchy layout (one entity per glTF node: the paths model sub-clips address). A model
// already loaded in another layout is reloaded when a player starts to apply to it, in the same
// undo step as what made it apply: adding the component (AddComponentCmd), moving the model under
// a player (MoveEntityOrderCmd), and T2d's composites (Use Animation Player, Convert, drops).
// Main thread (scene commands).

#pragma once

#include "command/Command.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace jasperengine::editor{

    class Project;

    class PlayerModelLayout{
    public:
        typedef std::vector<std::unique_ptr<Command>> Reloads;

        // The ModelComponents on or under `entity` that need the reload
        // (MeshSystem::needsNodeHierarchyReload), parents first
        static std::vector<Entity> modelsNeedingReload(Project* project, uint32_t sceneId, Entity entity);

        // Reloads each of them with a ModelLoadCmd of its own file (keeping its mergeStaticMeshes),
        // executed now and appended to `reloads`, which the calling command owns: undo them with
        // undoReloads before undoing its own change, and redo them with redoReloads after redoing it.
        // A reload that fails is logged and left out. Returns how many ran.
        static size_t ensureNodeHierarchyForPlayer(Project* project, uint32_t sceneId, Entity entity, Reloads& reloads);

        // newest first
        static void undoReloads(Reloads& reloads);
        // in their first order
        static void redoReloads(Reloads& reloads);
    };

}
