// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"

#include <string>

namespace jasperengine::editor{

    // Project Settings > Player > Build (Asset Stripping, Always Include, Exclude From Build) and
    // the Project window's Exclude From Build / Always Include in Build: one undo step each,
    // project.yaml written either way (S98 REQ-8, REQ-44)
    class AssetStrippingSettingsCmd: public Command{
    private:
        Project* project;
        AssetStrippingSettings oldSettings;
        AssetStrippingSettings newSettings;
        std::string description;

    public:
        AssetStrippingSettingsCmd(Project* project, const AssetStrippingSettings& newSettings, const std::string& description = "Change Asset Stripping");

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }

        std::string describe() const override { return description; }
        const char* typeName() const override { return "AssetStrippingSettingsCmd"; }
    };

}
