// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "SaveGameSettings.h"

namespace jasperengine::editor{

    class Project;

    // Project Settings > Player > Saved Data as one step of the project history: Play's store
    // takes the settings at its next write, exports bake them in; project.yaml is written either way.
    class SaveGameSettingsCmd: public Command{
    private:
        Project* project;
        SaveGameSettings oldSettings;
        SaveGameSettings newSettings;

    public:
        SaveGameSettingsCmd(Project* project, const SaveGameSettings& newSettings);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }

        std::string describe() const override { return "Change Saved Data Settings"; }
        const char* typeName() const override { return "SaveGameSettingsCmd"; }
    };

}
