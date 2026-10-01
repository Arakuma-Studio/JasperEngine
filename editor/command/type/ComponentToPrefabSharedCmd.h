// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"
#include "Catalog.h"

namespace jasperengine::editor {

    struct ComponentToPrefabSharedData {
        Entity entity;
        YAML::Node recovery;
        std::set<std::string> properties;  // what was overridden
        bool reverted = false;
    };

    class ComponentToPrefabSharedCmd: public Command {
    private:
        Project* project;
        uint32_t sceneId;
        ComponentType componentType;

        std::vector<ComponentToPrefabSharedData> entities;

        bool wasModified;

    public:
        ComponentToPrefabSharedCmd(Project* project, uint32_t sceneId, Entity entity, ComponentType componentType);

        bool execute() override;
        void undo() override;

        bool mergeWith(Command* otherCommand) override;
    };

}
