// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

namespace jasperengine::editor{

    class CreateDirCmd: public Command{

    private:

        fs::path directory;

    public:
        // Every file operation of this command (execute, undo, redo) stays beneath `root` when it
        // happens (util/SafeWrite.h: resolved inside it first, no link planted on the way since
        // followed; Command::writeRoot, which a command made in an MCP call's write scope has
        // already). Its undo and redo are refused while a path it acts on resolves outside the
        // root (touchedPaths). Without a root: the std::filesystem calls as before (in an active
        // write scope, beneath its root).
        void writeBeneath(fs::path root) { setWriteRoot(std::move(root)); }
        std::vector<Touched> touchedPaths(bool undo) const override;
        CreateDirCmd(std::string dirName, std::string dirPath);

        bool execute() override;
        void undo() override;

        std::string describe() const override { return "Create Folder " + CommandNames::file(directory); }
        const char* typeName() const override { return "CreateDirCmd"; }

    };

}
