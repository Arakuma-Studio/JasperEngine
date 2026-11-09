// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "pixelart/ScreenTransition.h"

namespace jasperengine::editor{

    class Project;

    // Project Settings > Graphics > Screen Transitions (S48 R41): the engine takes them at once
    // (Lua's defaults, the Game view's preview), and undo restores them. project.yaml is written
    // either way.
    class TransitionSettingsCmd: public Command{
    private:
        Project* project;
        jasperengine::pixelart::TransitionDefaults oldSettings;
        jasperengine::pixelart::TransitionDefaults newSettings;

    public:
        TransitionSettingsCmd(Project* project, const jasperengine::pixelart::TransitionDefaults& newSettings);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }

        std::string describe() const override { return "Change Screen Transitions"; }
        const char* typeName() const override { return "TransitionSettingsCmd"; }
    };

}
