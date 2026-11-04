// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "model/ModelImportSettings.h"

#include <string>

namespace jasperengine::editor {

    class Project;

    // Project Settings > Mesh and Quality's render budgets (spec S53 R19, R25) as one step of the
    // project history: the models without values of their own recook (mostly cache hits), undo
    // restores the settings; project.yaml is written either way.
    class ModelSettingsCmd : public Command {
    private:
        Project* project;
        ModelProjectSettings oldSettings;
        ModelProjectSettings newSettings;
        std::string name;

    public:
        ModelSettingsCmd(Project* project, const ModelProjectSettings& newSettings, std::string name = "Change Mesh Settings");

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }

        std::string describe() const override { return name; }
        const char* typeName() const override { return "ModelSettingsCmd"; }
    };

}
