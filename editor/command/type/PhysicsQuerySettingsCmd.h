// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"

namespace jasperengine::editor{

    // Project Settings > Physics / Physics 2D > Queries Hit Triggers before and after one edit (a
    // checkbox, MCP set_project_settings physics*.queries_hit_triggers), in the project history;
    // executing or undoing it applies the settings to the engine and saves project.yaml
    class PhysicsQuerySettingsCmd: public Command{

    private:
        Project* project;
        PhysicsQuerySettings before;
        PhysicsQuerySettings after;

        bool apply(const PhysicsQuerySettings& settings);

    public:
        PhysicsQuerySettingsCmd(Project* project, const PhysicsQuerySettings& before, const PhysicsQuerySettings& after);

        bool execute() override;
        void undo() override;

        std::vector<Touched> touchedPaths(bool undo) const override;
        bool affectsStructure() const override { return false; }

        std::string describe() const override { return "Change Queries Hit Triggers"; }
        const char* typeName() const override { return "PhysicsQuerySettingsCmd"; }
    };

}
