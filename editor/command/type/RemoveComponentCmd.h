// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"
#include "Catalog.h"
#include "yaml-cpp/yaml.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace jasperengine::editor {

    class DeleteEntityCmd;

    struct RemoveComponentData {
        Entity entity = NULL_ENTITY;
        YAML::Node oldComponent; // For no shared entities and no override components
        bool hasOverride = false;
        std::set<std::string> overriddenProperties; // what the instance overrode of it
        bool recordedRemoval = false;               // a removed component override was recorded
        bool wasAdded = false;                      // it was a component the instance added
        ComponentRecovery recovery; // For shared entities
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

    public:
        RemoveComponentCmd(Project* project, size_t sceneId, Entity entity, ComponentType componentType);
        ~RemoveComponentCmd() override;

        bool execute() override;
        void undo() override;

        bool mergeWith(Command* otherCommand) override;
    };
}
