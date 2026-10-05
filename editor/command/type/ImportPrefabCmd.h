// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"
#include "math/Vector3.h"
#include "math/Quaternion.h"
#include <cstdint>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace jasperengine::editor{

    class ImportPrefabCmd: public Command{

    private:
        Project* project;
        uint32_t sceneId;
        fs::path filepath;
        std::string guid;   // the prefab's asset GUID, taken on the first execute
        Entity parent;
        bool needSaveScene;

        std::vector<Entity> importedEntities;
        Entity rootEntity;
        std::vector<Entity> lastSelected;
        bool wasModified;
        bool addedToParentPrefab;
        bool addToParentPrefab = false;

        bool quiet = false;
        bool hasPlacement = false;
        Vector3 placementPosition;
        Quaternion placementRotation;
        Vector3 placementScale = Vector3::UNIT_SCALE;
        bool hasDropPosition = false;
        bool dropKeepsZ = false;
        Vector3 dropPosition;

    public:
        ImportPrefabCmd(Project* project, uint32_t sceneId, const fs::path& filepath, Entity parent = NULL_ENTITY, bool needSaveScene = true);

        bool execute() override;
        void undo() override;


        std::vector<Entity> getImportedEntities() const;

        // Terrain object placement: the root is transformed on every execute, so redo lands
        // in the same spot
        void setPlacement(const Vector3& position, const Quaternion& rotation, const Vector3& scale);
        void setQuiet(bool quiet);
        // Nest the instance in the parent's prefab, as Prefab Mode does (MCP instantiate_prefab)
        void setAddToParentPrefab(bool add);
        // Scene view drop: the root goes to this world position, keeping the prefab root's
        // rotation and scale (keepZ: its z too, a 2D drop), on every execute
        void setDropPosition(const Vector3& position, bool keepZ = false);
        Entity getRootEntity() const;
    };

}
