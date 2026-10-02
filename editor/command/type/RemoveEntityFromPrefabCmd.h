// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"
#include <cstdint>
#include <string>

namespace jasperengine::editor{

    class RemoveEntityFromPrefabCmd: public Command{

    private:
        Project* project;
        uint32_t sceneId;
        Entity entity;
        Entity parent;

        bool wasModified;

        NodeRecovery recovery;

    public:
        RemoveEntityFromPrefabCmd(Project* project, uint32_t sceneId, Entity entity, Entity parent);

        bool execute() override;
        void undo() override;

    };

}
