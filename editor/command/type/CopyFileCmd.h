// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "Project.h"
#include "asset/AssetOps.h"
#include "animation/AnimationClipDocument.h"
#include "vat/VatAssetFiles.h"
#include <map>
#include <memory>
#include <vector>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

namespace jasperengine::editor{

    struct FileCopyData{
        fs::path filename;
        fs::path sourceDirectory;
        fs::path targetDirectory;
        fs::path destination;   // where it went (a free name when the target's was taken)
        fs::path backup;        // overwrite: the replaced file's bytes, in the trash, for the undo
        bool went = false;      // the last execute moved or copied it (gone or already there: not)
    };

    class CopyFileCmd: public Command{

    public:
        // What an execute did, for the caller to select and report it: the history deletes a
        // command whose execute failed, this outlives it (reportTo)
        struct Report {
            std::vector<fs::path> landed;   // where the entries went (absolute), in order
            size_t gone = 0;                // sources no longer there: skipped
            std::string error;              // why it failed or was refused ("" when it did not)
        };

    private:

        Project* project;
        std::vector<FileCopyData> files;
        bool copy;
        // a copy over an existing file replaces its content and keeps its identity (meta, GUID)
        bool overwrite = false;
        // the GUIDs the copies got (by project-relative path), given again by a redo
        std::map<std::string, std::string> copyGuids;
        // the scene ids and names the copied scenes got, given again by a redo
        std::map<std::string, AssetOps::SceneCopy> copyScenes;
        // moves: .anim files whose preview/source paths followed (animation clips spec 9.R26)
        std::vector<ClipHintEdit> clipHintEdits;
        // .vat.glb image URIs rewritten for their new directory (vat/VatAssetFiles.h), and the moves
        // they followed: a move's undo writes the old URIs back
        std::vector<vat::VatUriEdit> vatUriEdits;
        std::vector<std::pair<fs::path, fs::path>> vatUriMoves;
        // moves: .vat files that took a free name, and the text they had (the undo writes it back)
        std::vector<std::pair<fs::path, std::string>> vatMeshFieldEdits;
        // Duplicate (Unity's Edit > Duplicate) names its step so
        bool duplicate = false;
        std::shared_ptr<Report> report;
        std::string lastError;

        // the destination an entry takes: its target's name, else the free Unity name ("hero
        // 1.png"); a move into the folder it is in stays where it is (empty)
        fs::path destinationFor(const FileCopyData& fdata) const;
        // The refusal of any entry (a folder into itself...), checked before anything changes
        std::string refusal() const;
        bool fail(std::string error);
        void beginReport();

        // the other file of each Vertex Animation pair (".vat" + ".vat.glb") joins the list
        void addVatPairs();
        bool executeFiles();   // no project: the files alone
        void undoFiles();
    public:
        // Every file operation of this command (execute, undo, redo) stays beneath `root` when it
        // happens (util/SafeWrite.h: resolved inside it first, no link planted on the way since
        // followed; Command::writeRoot, which a command made in an MCP call's write scope has
        // already). Its undo and redo are refused while a path it acts on resolves outside the
        // root (touchedPaths). Without a root: the std::filesystem calls as before (in an active
        // write scope, beneath its root).
        void writeBeneath(fs::path root) { setWriteRoot(std::move(root)); }
        std::vector<Touched> touchedPaths(bool undo) const override;
        CopyFileCmd(Project* project, std::vector<std::string> sourceFiles, std::string currentDirectory, std::string targetDirectory, bool copy);
        CopyFileCmd(Project* project, std::vector<std::string> sourcePaths, std::string targetDirectory, bool copy);
        // Unity's free name for `target`: itself when free, else "hero 1.png", "hero 2.png"... (a
        // folder: "Sprites 1"; a Vertex Animation file: a name free for both files of its pair)
        static fs::path freeName(const fs::path& target);
        // entries with their own target folders (Duplicate: each beside itself)
        CopyFileCmd(Project* project, std::vector<FileCopyData> entries, bool copy);

        void setOverwrite(bool overwrite) { this->overwrite = overwrite; }
        void setDuplicate(bool duplicate) { this->duplicate = duplicate; }
        void reportTo(std::shared_ptr<Report> sink) { report = std::move(sink); }

        bool execute() override;
        void undo() override;
        // nothing went (every source gone, or moved into the folder it is in): no undo step
        bool isNoOp() const override;
        std::string failure() const override { return lastError; }

        // "Copy File a.png", "Move 2 Files", "Duplicate File a.png", "Move File a.png as a 1.png"
        std::string describe() const override;
        const char* typeName() const override { return "CopyFileCmd"; }

    };

}
