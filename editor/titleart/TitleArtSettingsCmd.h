// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R52)
// - Purpose: one change of Project Settings > Title Art (the window's Apply, MCP
//   set_project_settings title_art.*) as a step of the project history; executing or undoing it
//   applies the settings and saves project.yaml.

#pragma once

#include "command/Command.h"
#include "titleart/TitleArtSettings.h"

namespace jasperengine::editor {

    class Project;

    class TitleArtSettingsCmd : public Command {
    public:
        TitleArtSettingsCmd(Project* project, const TitleArtProjectSettings& before, const TitleArtProjectSettings& after);

        bool execute() override;
        void undo() override;
        std::vector<Touched> touchedPaths(bool undo) const override;
        bool affectsStructure() const override { return false; }
        std::string describe() const override { return "Change Title Art Settings"; }
        const char* typeName() const override { return "TitleArtSettingsCmd"; }

    private:
        Project* project;
        TitleArtProjectSettings before;
        TitleArtProjectSettings after;

        bool apply(const TitleArtProjectSettings& settings);
    };

}
