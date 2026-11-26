// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // One edit of the project's Build Profiles (docs/specs/build-profiles.md R19, R22): the files it
    // writes, creates or removes (a .buildprofile; publishing.json when a rename updates its targets),
    // each with its contents before and after, in the project history. Executing or undoing it puts
    // every file as the step left it, atomically per file. Edits with the same merge key merge
    // (typing in a field, dragging the Scene List), undoing to the contents before the first.
    class EditBuildProfileCmd: public Command{
    public:
        struct FileChange {
            std::filesystem::path path;                // absolute
            std::optional<std::string> before;         // absent: the file did not exist
            std::optional<std::string> after;          // absent: the step removes it
        };

        EditBuildProfileCmd(std::string stepName, std::vector<FileChange> changes);

        bool execute() override;
        void undo() override;
        bool mergeWith(Command* previousCommand) override;
        bool affectsStructure() const override { return false; }
        bool isNoOp() const override;
        size_t memoryBytes() const override;
        std::vector<Touched> touchedPaths(bool undo) const override;
        std::string failure() const override { return lastFailure; }

        void setMergeKey(const std::string& key) { mergeKey = key; }
        // The user's active profile moves with the step (a rename of the active profile): set to
        // `after` by execute and redo, back to `before` by undo. Only that key of build.yaml changes
        void setActiveNameChange(std::filesystem::path projectPath, std::string before, std::string after);
        // where a failed execute writes why (the history frees a command that failed)
        void reportFailureTo(std::shared_ptr<std::string> sink) { failureSink = std::move(sink); }

        std::string describe() const override { return stepName; }
        const char* typeName() const override { return "EditBuildProfileCmd"; }

    private:
        std::string stepName;
        std::vector<FileChange> changes;
        std::string mergeKey;
        std::string lastFailure;
        std::shared_ptr<std::string> failureSink;
        struct ActiveNameChange {
            std::filesystem::path projectPath;
            std::string before;
            std::string after;
        };
        std::optional<ActiveNameChange> activeName;

        bool apply(bool forward);
    };

}
