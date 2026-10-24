// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "Project.h"
#include "Catalog.h"
#include "command/PrefabEditUndo.h"

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
        // the override records as they were: undo gives them back exactly (S24 R31)
        PrefabEditUndo prefabUndo;

    public:
        ComponentToPrefabSharedCmd(Project* project, uint32_t sceneId, Entity entity, ComponentType componentType);

        bool execute() override;
        void undo() override;

        bool mergeWith(Command* otherCommand) override;

        // "Revert Component Overrides Light (Player)"
        std::string describe() const override;
        const char* typeName() const override { return "ComponentToPrefabSharedCmd"; }
        size_t memoryBytes() const override;
    };

}
