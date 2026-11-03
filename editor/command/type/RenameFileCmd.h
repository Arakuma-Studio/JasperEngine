// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "Project.h"
#include "animation/AnimationClipDocument.h"
#include <vector>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

namespace jasperengine::editor{

    class RenameFileCmd: public Command{

    private:

        Project* project;
        fs::path oldFilename;
        fs::path newFilename;
        fs::path directory;

        // .anim files whose preview/source paths followed the rename (animation clips spec
        // 9.R26), put back by the undo
        std::vector<ClipHintEdit> clipHintEdits;

        // A Vertex Animation keeps its pair (vertex animation spec 2.1): renaming "<x>.vat" renames
        // "<x>.vat.glb" too (and the other way round), and the new .vat's "mesh" field names the
        // new mesh. Decided by the first execute; the field text is put back by the undo when the
        // file still holds what this command wrote
        bool pairDecided = false;
        fs::path pairOldName;
        fs::path pairNewName;
        fs::path meshFieldFile;
        std::string meshFieldBefore;
        std::string meshFieldAfter;

        // AssetOps::move: the file and its meta, GUIDs kept, references refreshed in memory;
        // false (an error printed) when it did not move
        bool renameFile(const fs::path& source, const fs::path& destination);

    public:
        // Every file operation of this command (execute, undo, redo) stays beneath `root` when it
        // happens (util/SafeWrite.h: resolved inside it first, no link planted on the way since
        // followed; Command::writeRoot, which a command made in an MCP call's write scope has
        // already). Its undo and redo are refused while a path it acts on resolves outside the
        // root (touchedPaths). Without a root: the std::filesystem calls as before (in an active
        // write scope, beneath its root).
        void writeBeneath(fs::path root) { setWriteRoot(std::move(root)); }
        std::vector<Touched> touchedPaths(bool undo) const override;
        RenameFileCmd(Project* project, std::string oldName, std::string newName, std::string directory);

        bool execute() override;
        void undo() override;

        // "Rename File a.png → b.png"
        std::string describe() const override {
            return "Rename File " + CommandNames::file(oldFilename) + CommandNames::ARROW + CommandNames::file(newFilename);
        }
        const char* typeName() const override { return "RenameFileCmd"; }
        bool isNoOp() const override { return oldFilename == newFilename; }

    };

}
