// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Generator.h"

namespace jasperengine::editor{

    class Project;

    // Project Settings > Graphics > GPU Instancing (spec S55): the editor's scenes draw with it at
    // once, and undo restores it. project.yaml is written either way.
    class DrawSubmissionSettingsCmd: public Command{
    private:
        Project* project;
        DrawSubmissionSettings oldSettings;
        DrawSubmissionSettings newSettings;

    public:
        DrawSubmissionSettingsCmd(Project* project, const DrawSubmissionSettings& newSettings);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }

        std::string describe() const override { return "Change Draw Submission Settings"; }
        const char* typeName() const override { return "DrawSubmissionSettingsCmd"; }
    };

}
