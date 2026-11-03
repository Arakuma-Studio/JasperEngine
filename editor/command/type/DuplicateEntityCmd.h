// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "Project.h"
#include <cstdint>
#include <vector>
#include <string>

#include "yaml-cpp/yaml.h"

namespace jasperengine::editor{

    class DuplicateEntityCmd: public Command{

    private:
        Project* project;
        uint32_t sceneId;

        std::vector<Entity> sourceEntities;
        std::vector<Entity> createdEntities;
        std::vector<Entity> lastSelected;
        bool wasModified;

    public:
        DuplicateEntityCmd(Project* project, uint32_t sceneId, const std::vector<Entity>& entities);

        bool execute() override;
        void undo() override;


        std::vector<Entity> getCreatedEntities() const;

        // "Duplicate Player", "Duplicate 3 Objects"
        std::string describe() const override { return "Duplicate " + CommandNames::entities(project, sceneId, sourceEntities); }
        const char* typeName() const override { return "DuplicateEntityCmd"; }
        void collectKeepCommands(KeepContext& context) const override;
        size_t memoryBytes() const override {
            return sizeof(*this) + (sourceEntities.capacity() + createdEntities.capacity() + lastSelected.capacity()) * sizeof(Entity);
        }
        const std::vector<Entity>& getSourceEntities() const { return sourceEntities; }
        uint32_t getSceneId() const { return sceneId; }
    };

}
