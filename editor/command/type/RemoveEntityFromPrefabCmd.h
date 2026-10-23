// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "command/CommandMemory.h"
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
        std::string subject;    // the GameObject's name, read before it goes

    public:
        RemoveEntityFromPrefabCmd(Project* project, uint32_t sceneId, Entity entity, Entity parent);

        bool execute() override;
        void undo() override;

        std::string describe() const override { return "Remove GameObject From Prefab " + subject; }
        const char* typeName() const override { return "RemoveEntityFromPrefabCmd"; }
        size_t memoryBytes() const override { return sizeof(*this) + CommandMemory::of(recovery); }

    };

}
