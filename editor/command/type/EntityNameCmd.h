// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "Project.h"
#include "command/PrefabEditUndo.h"
#include <cstdint>
#include <string>

namespace jasperengine::editor{

    class EntityNameCmd: public Command{

    private:
        std::string oldName;
        std::string newName;

        Project* project;
        uint32_t sceneId;
        Entity entity;

        bool wasModified;
        PrefabEditUndo prefabUndo;

    public:
        EntityNameCmd(Project* project, uint32_t sceneId, Entity entity, std::string name);

        bool execute() override;
        void undo() override;

        bool mergeWith(Command* otherCommand) override;

        // "Rename Player → Hero"
        std::string describe() const override { return "Rename " + oldName + CommandNames::ARROW + newName; }
        const char* typeName() const override { return "EntityNameCmd"; }
        bool isNoOp() const override { return oldName == newName; }
        void collectKeepCommands(KeepContext& context) const override;

        uint32_t getSceneId() const { return sceneId; }
        Entity getEntity() const { return entity; }
        const std::string& getNewName() const { return newName; }
    };

}