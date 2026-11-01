// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"
#include "DebugSettings.h"

namespace jasperengine::editor{

    // Project Settings > Debug before and after one edit (the dialog's OK, MCP
    // set_project_settings debug.*), in the project history; executing or undoing it applies
    // the settings (the next Play and the next build read them) and saves project.yaml
    class DebugSettingsCmd: public Command{

    private:
        Project* project;
        DebugSettings before;
        DebugSettings after;

        bool apply(const DebugSettings& settings);

    public:
        DebugSettingsCmd(Project* project, const DebugSettings& before, const DebugSettings& after);

        bool execute() override;
        void undo() override;

        std::vector<Touched> touchedPaths(bool undo) const override;
        bool affectsStructure() const override { return false; }

        std::string describe() const override { return "Change Debug Settings"; }
        const char* typeName() const override { return "DebugSettingsCmd"; }
    };

}
