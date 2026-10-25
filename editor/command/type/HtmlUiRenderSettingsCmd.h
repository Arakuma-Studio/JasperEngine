// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Generator.h"

namespace jasperengine::editor{

    class Project;

    // Project Settings > Graphics > HTML UI (S16): the glyph atlas budget, SDF primitives and
    // transform-aware text apply to the editor's documents at once, and undo restores them.
    // project.yaml is written either way.
    class HtmlUiRenderSettingsCmd: public Command{
    private:
        Project* project;
        HtmlUiRenderSettings oldSettings;
        HtmlUiRenderSettings newSettings;

    public:
        HtmlUiRenderSettingsCmd(Project* project, const HtmlUiRenderSettings& newSettings);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }

        std::string describe() const override { return "Change HTML UI Rendering Settings"; }
        const char* typeName() const override { return "HtmlUiRenderSettingsCmd"; }
    };

}
