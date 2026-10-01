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

        bool quiet = false;
        // Dropped on a GameObject of a prefab instance: Unity makes it a GameObject added to
        // that instance (an override); Prefab Mode edits the prefab itself and asks for this
        bool addToParentPrefab = false;
        bool hasPlacement = false;
        Vector3 placementPosition;
        Quaternion placementRotation;
        Vector3 placementScale = Vector3::UNIT_SCALE;

    public:
        ImportPrefabCmd(Project* project, uint32_t sceneId, const fs::path& filepath, Entity parent = NULL_ENTITY, bool needSaveScene = true);

        bool execute() override;
        void undo() override;

        bool mergeWith(Command* otherCommand) override;

        std::vector<Entity> getImportedEntities() const;

        // Terrain object placement: the root is transformed on every execute, so redo lands
        // in the same spot
        void setPlacement(const Vector3& position, const Quaternion& rotation, const Vector3& scale);
        void setQuiet(bool quiet);
        // The new instance becomes a GameObject of the prefab of the instance it is dropped
        // on (nested in that prefab), not an added GameObject of that instance
        void setAddToParentPrefab(bool add);
    };

}
