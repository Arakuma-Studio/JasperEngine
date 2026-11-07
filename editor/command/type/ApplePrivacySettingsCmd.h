// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"

namespace jasperengine::editor{

    // Project Settings > Player > iOS / macOS > Privacy Manifest before and after one edit (the
    // dialog's OK, MCP set_project_settings apple.privacy_*), in the project history; executing
    // or undoing it applies the additions (the next Apple export reads them) and saves project.yaml
    class ApplePrivacySettingsCmd: public Command{

    private:
        Project* project;
        ApplePrivacy::Manifest before;
        ApplePrivacy::Manifest after;

        bool apply(const ApplePrivacy::Manifest& privacy);

    public:
        ApplePrivacySettingsCmd(Project* project, const ApplePrivacy::Manifest& after);

        bool execute() override;
        void undo() override;
        // the keys of one set_project_settings call are one step (the history merges a call's
        // commands only); the dialog's OK is one command already
        bool mergeWith(Command* otherCommand) override;
        bool isNoOp() const override { return before == after; }

        std::vector<Touched> touchedPaths(bool undo) const override;
        bool affectsStructure() const override { return false; }

        std::string describe() const override { return "Change Privacy Manifest"; }
        const char* typeName() const override { return "ApplePrivacySettingsCmd"; }
    };

}
