// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Generator.h"

namespace jasperengine::editor{

    class Project;

    // Project Settings > Graphics > Shader Creation (spec S32): the budget and warm draws apply to
    // the editor at once and undo restores them; project.yaml is written either way.
    class ShaderCreationSettingsCmd: public Command{
    private:
        Project* project;
        ShaderCreationProjectSettings oldSettings;
        ShaderCreationProjectSettings newSettings;

    public:
        ShaderCreationSettingsCmd(Project* project, const ShaderCreationProjectSettings& newSettings);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }

        std::string describe() const override { return "Change Shader Creation Settings"; }
        const char* typeName() const override { return "ShaderCreationSettingsCmd"; }
    };

}
