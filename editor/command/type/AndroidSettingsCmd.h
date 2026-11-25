// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"

namespace jasperengine::editor{

    // Project Settings > Android as one undoable step: the package, icons, signing and Debug
    // Package Suffix the next Android build reads. project.yaml is written either way.
    class AndroidSettingsCmd: public Command{
    private:
        Project* project;
        AndroidProjectSettings oldSettings;
        AndroidProjectSettings newSettings;

    public:
        AndroidSettingsCmd(Project* project, const AndroidProjectSettings& newSettings);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }

        std::string describe() const override { return "Change Android Settings"; }
        const char* typeName() const override { return "AndroidSettingsCmd"; }
    };

}
