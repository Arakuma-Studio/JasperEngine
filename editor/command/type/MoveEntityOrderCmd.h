// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"
#include <cstdint>
#include <filesystem>
#include <string>

namespace jasperengine::editor{

    class MoveEntityOrderCmd: public Command{

    friend class Project;
    friend class DeleteEntityCmd;

    private:
        Project* project;
        uint32_t sceneId;

        Entity source;
        Entity target;
        InsertionType type;

        size_t oldIndex;
        bool hasTransform;
        Entity oldParent;

        bool wasModified;

    public:
        MoveEntityOrderCmd(Project* project, uint32_t sceneId, Entity source, Entity target, InsertionType type);

        // Unity: the GameObjects of a prefab instance (its members, nested instance roots
        // included) cannot be reordered, reparented or moved out of it; the prefab asset is
        // restructured in Prefab Mode. Added GameObjects and instance roots move freely (an
        // added GameObject override), and so do a Prefab Mode stage's own GameObjects (asset
        // edits). Returns why `source` cannot move (empty when it can), and the prefab and
        // instance root to open to restructure it.
        static std::string getPrefabRestructureBlock(Project* project, uint32_t sceneId, Entity source,
            std::filesystem::path* prefabPath = nullptr, Entity* instanceRoot = nullptr);
        static constexpr const char* PREFAB_RESTRUCTURE_MESSAGE =
            "Children of a Prefab instance cannot be rearranged. Open the Prefab to restructure it.";

        bool execute() override;
        void undo() override;

    };

}