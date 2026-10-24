// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "Project.h"
#include "Catalog.h"
#include "command/PrefabEditUndo.h"
#include "yaml-cpp/yaml.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace jasperengine::editor {

    class DeleteEntityCmd;

    struct RemoveComponentData {
        Entity entity = NULL_ENTITY;
        YAML::Node oldComponent;
        bool removed = false;
    };

    class RemoveComponentCmd : public Command {
    private:
        Project* project;
        size_t sceneId;
        ComponentType componentType;

        std::vector<RemoveComponentData> entities;
        std::vector<std::unique_ptr<DeleteEntityCmd>> ownedEntityDeletes;
        bool ownedEntitiesCaptured = false;

        bool wasModified;
        // Unity: a component removed from a prefab instance's GameObject is a removed
        // component override of that instance (one the instance added is simply gone)
        PrefabEditUndo prefabUndo;

    public:
        RemoveComponentCmd(Project* project, size_t sceneId, Entity entity, ComponentType componentType);
        ~RemoveComponentCmd() override;

        bool execute() override;
        void undo() override;

        bool mergeWith(Command* otherCommand) override;

        // "Remove Component Light (Sun)"
        std::string describe() const override;
        const char* typeName() const override { return "RemoveComponentCmd"; }
        size_t memoryBytes() const override;
        void collectKeepCommands(KeepContext& context) const override;

        uint32_t getSceneId() const { return (uint32_t)sceneId; }
        ComponentType getComponentType() const { return componentType; }
        std::vector<Entity> getEntities() const;
    };
}
