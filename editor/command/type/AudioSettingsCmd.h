// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"

namespace jasperengine::editor{

    // Project Settings > Audio before and after one edit (the dialog's OK), in the project
    // history; executing or undoing it applies them (the mixer reloads) and saves project.yaml
    class AudioSettingsCmd: public Command{

    private:
        Project* project;
        AudioSettings before;
        AudioSettings after;

        bool apply(const AudioSettings& settings);

    public:
        AudioSettingsCmd(Project* project, AudioSettings before, AudioSettings after);

        bool execute() override;
        void undo() override;

        std::vector<Touched> touchedPaths(bool undo) const override;
        bool affectsStructure() const override { return false; }
    };

}
