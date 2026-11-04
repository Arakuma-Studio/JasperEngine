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
#include <vector>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

namespace jasperengine::editor{

    struct FileCopyData{
        fs::path filename;
        fs::path sourceDirectory;
        fs::path targetDirectory;
        fs::path destination;   // where the copy went (a free name when the target's was taken)
        fs::path backup;        // overwrite: the replaced file's bytes, in the trash, for the undo
    };

    class CopyFileCmd: public Command{

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

        void setOverwrite(bool overwrite) { this->overwrite = overwrite; }

        bool execute() override;
        void undo() override;

        // "Copy File a.png", "Move 2 Files"
        std::string describe() const override {
            std::vector<fs::path> names;
            for (const FileCopyData& file : files) {
                names.push_back(file.filename);
            }
            return std::string(copy ? "Copy " : "Move ") + (files.size() == 1 ? "File " : "") + CommandNames::files(names);
        }
        const char* typeName() const override { return "CopyFileCmd"; }

    };

}
