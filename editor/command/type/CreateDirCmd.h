// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

namespace jasperengine::editor{

    class CreateDirCmd: public Command{

    private:
        fs::path beneathRoot;

        fs::path directory;

    public:
        // Every rename (and folder creation) of this command stays beneath `root` when it happens
        // (util/SafeWrite.h: resolved inside it first, no link planted on the way since followed;
        // the MCP tools set the project directory). Empty (the default): fs::rename as before.
        void writeBeneath(fs::path root) { beneathRoot = std::move(root); }
        CreateDirCmd(std::string dirName, std::string dirPath);

        bool execute() override;
        void undo() override;

    };

}
