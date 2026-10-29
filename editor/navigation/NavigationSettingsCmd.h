// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "navigation/NavigationProjectSettings.h"

#include <string>

namespace jasperengine::editor {

    class Project;

    // Project Settings > Navigation and Window > AI > Navigation (agent types, areas, budgets):
    // the engine's settings change at once (every scene's crowds and filters refresh at their
    // next fixed tick), undo restores them; project.yaml is written either way.
    class NavigationSettingsCmd : public Command {
    private:
        Project* project;
        NavigationProjectSettings oldSettings;
        NavigationProjectSettings newSettings;
        std::string name;

    public:
        NavigationSettingsCmd(Project* project, const NavigationProjectSettings& newSettings,
                              std::string name = "Change Navigation Settings");

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }

        std::string describe() const override { return name; }
        const char* typeName() const override { return "NavigationSettingsCmd"; }
    };

}
