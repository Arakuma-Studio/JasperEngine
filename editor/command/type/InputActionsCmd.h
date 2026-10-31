// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"
#include "input/InputActions.h"

#include <string>
#include <vector>

namespace jasperengine::editor{

    // Project Settings > Input: the whole list of input actions before and after one edit (the
    // dialog's OK, MCP set_project_settings input.actions), in the project history; executing
    // or undoing it applies the list to the engine (a running Play too) and saves project.yaml
    class InputActionsCmd: public Command{

    private:
        Project* project;
        std::vector<InputAction> before;
        std::vector<InputAction> after;

        bool apply(const std::vector<InputAction>& actions);

    public:
        InputActionsCmd(Project* project, std::vector<InputAction> before, std::vector<InputAction> after);

        bool execute() override;
        void undo() override;

        std::vector<Touched> touchedPaths(bool undo) const override;
        bool affectsStructure() const override { return false; }
    };

}
