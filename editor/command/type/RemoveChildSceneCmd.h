// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "Project.h"

namespace jasperengine::editor {
    class RemoveChildSceneCmd : public Command {
    private:
        Project* project;
        uint32_t sceneId;
        uint32_t childSceneId;
        ChildSceneRef childSceneRef;
        int index;

        bool wasModified;

    public:
        RemoveChildSceneCmd(Project* project, uint32_t sceneId, uint32_t childSceneId);

        bool execute() override;
        void undo() override;
        bool canUndo() const override;

        std::string describe() const override { return "Remove Child Scene " + CommandNames::scene(project, childSceneId); }
        const char* typeName() const override { return "RemoveChildSceneCmd"; }

    };
}
