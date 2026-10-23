// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"

#include "yaml-cpp/yaml.h"

#include <string>

namespace jasperengine::editor {

    // Removes a component this editor does not know from a GameObject: the file kept it
    // (docs/data-safety.md), the next save leaves it out. Undo puts it back where it was.
    class RemovePreservedComponentCmd : public Command {
    public:
        RemovePreservedComponentCmd(Project* project, uint32_t sceneId, Entity entity, std::string component);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }

    private:
        Project* project;
        uint32_t sceneId;
        Entity entity;
        std::string component;
        YAML::Node authored;
        size_t index = 0;
        bool wasModified = false;
    };

}
