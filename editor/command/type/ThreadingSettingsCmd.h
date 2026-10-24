// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Generator.h"

namespace jasperengine::editor{

    class Project;

    // Project Settings > Player > Threading: what applies live (the reserved frame workers,
    // the web pump budget) changes the editor's job system at once, and undo restores it.
    // project.yaml is written either way.
    class ThreadingSettingsCmd: public Command{
    private:
        Project* project;
        ThreadingProjectSettings oldSettings;
        ThreadingProjectSettings newSettings;

    public:
        ThreadingSettingsCmd(Project* project, const ThreadingProjectSettings& newSettings);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }
    };

}
