// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"

namespace jasperengine::editor{

    // Project Settings > Dialogue before and after one edit (the Dialogue window's "declare" fixes,
    // dialogue-graphs.md R11, R23), in the project history; executing or undoing it applies them and
    // saves project.yaml
    class DialogueSettingsCmd: public Command{

    private:
        Project* project;
        DialogueSettings before;
        DialogueSettings after;
        std::string label;

        bool apply(const DialogueSettings& settings);

    public:
        DialogueSettingsCmd(Project* project, DialogueSettings before, DialogueSettings after, std::string label);

        bool execute() override;
        void undo() override;

        std::vector<Touched> touchedPaths(bool undo) const override;
        bool affectsStructure() const override { return false; }
        bool isNoOp() const override { return before == after; }

        std::string describe() const override { return label; }
        const char* typeName() const override { return "DialogueSettingsCmd"; }
    };

}
