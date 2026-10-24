// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "Project.h"

namespace jasperengine::editor{

    class SetMainCameraCmd: public Command{

    private:
        Project* project;
        uint32_t sceneId;
        Entity newMainCamera;
        Entity oldMainCamera;
        bool wasModified;

    public:
        SetMainCameraCmd(Project* project, uint32_t sceneId, Entity newMainCamera);

        bool execute() override;
        void undo() override;

        std::string describe() const override { return "Set Main Camera " + CommandNames::entity(project, sceneId, newMainCamera); }
        const char* typeName() const override { return "SetMainCameraCmd"; }
        bool isNoOp() const override { return newMainCamera == oldMainCamera; }

    };

}