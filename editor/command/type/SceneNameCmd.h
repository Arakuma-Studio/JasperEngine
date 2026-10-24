// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "Project.h"
#include <cstdint>
#include <string>

namespace jasperengine::editor{

    class SceneNameCmd: public Command{

    private:
        std::string oldName;
        std::string newName;

        Project* project;
        uint32_t sceneId;

        bool wasModified;

    public:
        SceneNameCmd(Project* project, uint32_t sceneId, std::string name);

        bool execute() override;
        void undo() override;

        bool mergeWith(Command* otherCommand) override;

        // "Rename Scene Level1 → Level2"
        std::string describe() const override { return "Rename Scene " + oldName + CommandNames::ARROW + newName; }
        const char* typeName() const override { return "SceneNameCmd"; }
        bool isNoOp() const override { return oldName == newName; }
    };

}