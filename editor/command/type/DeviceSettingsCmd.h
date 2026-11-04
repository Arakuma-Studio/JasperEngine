// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "device/DeviceSettings.h"

namespace jasperengine::editor{

    class Project;

    // Project Settings > Quality > Device Tiers (and the LOD biases, Thermal Response and the
    // upload budget, spec S56): the editor's QualityResolver takes the settings at once, undo
    // restores them; project.yaml is written either way
    class DeviceSettingsCmd: public Command{
    private:
        Project* project;
        DeviceSettings oldSettings;
        DeviceSettings newSettings;

    public:
        DeviceSettingsCmd(Project* project, const DeviceSettings& newSettings);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }

        std::string describe() const override { return "Change Device Tier Settings"; }
        const char* typeName() const override { return "DeviceSettingsCmd"; }
    };

}
