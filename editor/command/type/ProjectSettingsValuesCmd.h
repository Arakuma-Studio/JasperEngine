// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "mcp/McpJson.h"

#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor{

    class Project;

    // Several Project Settings rows (SettingsRegistry keys) set at once and undone at once: the
    // Pixel Art preset and check_pixel_art's fixes (docs/specs/S42-pixel-art-mode.md R2, R13).
    // The values go through each row's own set, in order, so they are validated and applied as
    // set_project_settings applies them; project.yaml is written after each direction.
    class ProjectSettingsValuesCmd: public Command{
    private:
        Project* project;
        std::vector<std::pair<std::string, mcp::Json>> newValues;
        std::vector<std::pair<std::string, mcp::Json>> oldValues;
        std::string name;
        std::string error;

        bool apply(const std::vector<std::pair<std::string, mcp::Json>>& values, std::string& why);

    public:
        // values: (key, value) in the order they apply; the old values are read now
        ProjectSettingsValuesCmd(Project* project, std::vector<std::pair<std::string, mcp::Json>> values, std::string name);

        // false with the row's error (nothing is changed then)
        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }
        std::string describe() const override { return name; }
        const char* typeName() const override { return "ProjectSettingsValuesCmd"; }
        std::string failure() const override { return error; }
        const std::vector<std::pair<std::string, mcp::Json>>& values() const { return newValues; }
    };

}
