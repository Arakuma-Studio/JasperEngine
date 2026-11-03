// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // Project Settings > Input's Input Manager (the dialog's OK, MCP set_project_settings
    // input.manager_actions): the actions asset's "Input Manager" map replaced by a list of
    // actions, as one step of the project history. A project without an actions asset gets one
    // (InputManager.inputactions in the assets directory, Project Settings > Input's Default
    // Actions Asset); undoing removes it and the setting again. Executing or undoing writes the
    // asset atomically, saves project.yaml, and applies to a running Play
    class InputActionsCmd: public Command{

    private:
        Project* project;
        std::filesystem::path beforeAsset;      // project-relative ("" none)
        std::filesystem::path afterAsset;
        std::string beforeText;                 // the asset file's ("" when the command makes it)
        std::string afterText;
        std::vector<InputManagerAction> after;
        bool created = false;
        std::string label;

        bool apply(bool undoing);

    public:
        // the edit of the project's asset now (made when it has none)
        InputActionsCmd(Project* project, const std::vector<InputManagerAction>& actions);

        bool execute() override;
        void undo() override;

        std::vector<Touched> touchedPaths(bool undo) const override;
        bool affectsStructure() const override { return false; }
        std::string describe() const override { return label; }
        const char* typeName() const override { return "InputActionsCmd"; }
        bool isNoOp() const override { return beforeAsset == afterAsset && beforeText == afterText; }
        size_t memoryBytes() const override { return sizeof(*this) + beforeText.size() + afterText.size(); }

        // the asset the command writes (project-relative)
        const std::filesystem::path& getAsset() const { return afterAsset; }
    };

}
