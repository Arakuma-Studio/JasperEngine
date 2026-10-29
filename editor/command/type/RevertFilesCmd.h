// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// A version control revert as an undoable command, after OpenEngine's VcsRevertCommand
// (Apps/Editor/Include/UndoRedo/VcsRevertCommand.h, MIT, Copyright (c) 2026 Gamecrafters Guild
// Limited and contributors; NOTICE.md). Theirs backs up to memory and ignores failures; this one
// backs up to disk, takes the .meta along, reloads open scenes and reports failure.

#pragma once

#include "command/Command.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // "Revert N files" (docs/specs/S29-version-control.md R11): the files as they were before
    // the version control revert, and as it left them, are copies in
    // .jasperengine/vcs-backup/<id>/before and /after. The revert itself ran before the command
    // was made (VcsEditorOps::revertFiles: a process, never on the main thread), so the first
    // execute only reloads what it changed; undo writes the "before" copies back through atomic
    // writes, redo the "after" ones. The backup goes when the command leaves the history (the
    // undo cap, a project close).
    class RevertFilesCmd : public Command {
    public:
        struct File {
            std::filesystem::path path;      // absolute
            bool before = false;             // the file existed before the revert
            bool after = false;              // ... and after it
        };

        // backupDir holds before/<n> and after/<n> for files[n]; files: the reverted files and
        // their .meta sidecars; reverted: how many files the user reverted (the name)
        RevertFilesCmd(Project* project, std::filesystem::path backupDir, std::vector<File> files, size_t reverted);
        ~RevertFilesCmd() override;

        bool execute() override;
        void undo() override;

        std::string describe() const override;
        const char* typeName() const override { return "RevertFilesCmd"; }
        size_t memoryBytes() const override;

        const std::vector<File>& getFiles() const { return files; }
        const std::filesystem::path& getBackupDir() const { return backupDir; }

        // A fresh backup folder below the project's vcs-backup folder
        static std::filesystem::path newBackupDir(const Project& project);
        // The copy of files[index] for a side
        static std::filesystem::path copyPath(const std::filesystem::path& backupDir, bool after, size_t index);
        // After the files changed on disk: the open scenes and prefabs among them read again,
        // the other assets reloaded (the hot reload)
        static void reloadChanged(Project* project, const std::vector<std::filesystem::path>& paths);

    private:
        bool restore(bool after);

        Project* project;
        std::filesystem::path backupDir;
        std::vector<File> files;
        size_t reverted;
        bool firstRun = true;
    };

}
