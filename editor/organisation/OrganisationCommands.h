// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The undoable edits of S25's organisation data, steps of the project history (S24): asset
// labels and their colours ("Set Asset Labels", "Rename Label", "Delete Label", "Set Label
// Color"), saved searches and bookmarks. Each holds the state before and after; execute and
// undo write it (the files, organisation.yaml, project.yaml) and report what they wrote.

#include "command/Command.h"
#include "organisation/AssetLabels.h"
#include "organisation/Organisation.h"

#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class SetAssetLabelsCmd : public Command {
    public:
        using Changes = std::vector<std::pair<Guid, std::vector<std::string>>>;

        // name: the undo step's ("Set Asset Labels"); before: the labels the assets have now
        SetAssetLabelsCmd(Project* project, std::string name, Changes before, Changes after);
        // also project.yaml's label entries (rename, delete, colour)
        void setEntries(std::vector<LabelColor> before, std::vector<LabelColor> after);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }
        bool isNoOp() const override;
        std::string describe() const override { return name; }
        const char* typeName() const override { return "SetAssetLabelsCmd"; }

        // What the last execute or undo wrote (project-relative), and why it failed
        const std::vector<std::string>& filesWritten() const { return written; }
        const std::string& error() const { return lastError; }

    private:
        Project* project;
        std::string name;
        Changes before;
        Changes after;
        bool entries = false;
        std::vector<LabelColor> entriesBefore;
        std::vector<LabelColor> entriesAfter;
        std::vector<std::string> written;
        std::string lastError;

        bool apply(const Changes& changes, const std::vector<LabelColor>* entryList);
    };

    class SetSavedSearchesCmd : public Command {
    public:
        SetSavedSearchesCmd(Project* project, std::string name, std::vector<SavedSearch> userAfter, std::vector<SavedSearch> projectAfter);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }
        bool isNoOp() const override;
        std::string describe() const override { return name; }
        const char* typeName() const override { return "SetSavedSearchesCmd"; }
        const std::vector<std::string>& filesWritten() const { return written; }

    private:
        Project* project;
        std::string name;
        std::vector<SavedSearch> userBefore, userAfter;
        std::vector<SavedSearch> projectBefore, projectAfter;
        std::vector<std::string> written;

        bool apply(const std::vector<SavedSearch>& user, const std::vector<SavedSearch>& shared);
    };

    class SetBookmarksCmd : public Command {
    public:
        SetBookmarksCmd(Project* project, std::string name, std::vector<Bookmark> after);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }
        bool isNoOp() const override { return before == after; }
        std::string describe() const override { return name; }
        const char* typeName() const override { return "SetBookmarksCmd"; }
        const std::vector<std::string>& filesWritten() const { return written; }

    private:
        Project* project;
        std::string name;
        std::vector<Bookmark> before, after;
        std::vector<std::string> written;

        bool apply(const std::vector<Bookmark>& list);
    };

}
