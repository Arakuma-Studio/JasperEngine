// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"
#include "Catalog.h"
#include "command/PrefabEditUndo.h"

namespace jasperengine::editor {
    class AddComponentCmd : public Command {
    private:
        Project* project;
        size_t sceneId;
        std::vector<Entity> entities;
        ComponentType componentType;

        bool wasModified;
        // Unity: a component added to a prefab instance's GameObject is an added component
        // override of that instance
        PrefabEditUndo prefabUndo;

    public:
        AddComponentCmd(Project* project, size_t sceneId, Entity entity, ComponentType componentType);

        bool execute() override;
        void undo() override;

        bool mergeWith(Command* otherCommand) override;
    };
}