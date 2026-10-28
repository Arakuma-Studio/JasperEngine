// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"
#include <vector>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

namespace jasperengine::editor{

    class DeleteFileCmd: public Command{

        struct DeleteFilesData{
            fs::path originalFile;
            fs::path trashFile;
        };

    private:
        fs::path beneathRoot;

        Project* project;
        std::vector<DeleteFilesData> files;
        fs::path trash;
        // why execute() refused (Play running: a scene would leave the project), "" otherwise
        std::string refusal;
        bool alertOnRefusal = true;

        fs::path generateUniqueTrashPath(const fs::path& trashDir, const fs::path& originalFile);

    public:
        // Every rename (and folder creation) of this command stays beneath `root` when it happens
        // (util/SafeWrite.h: resolved inside it first, no link planted on the way since followed;
        // the MCP tools set the project directory). Empty (the default): fs::rename as before.
        void writeBeneath(fs::path root) { beneathRoot = std::move(root); }
        DeleteFileCmd(Project* project, std::vector<fs::path> filePaths, fs::path rootPath);

        // An agent's call reports the refusal itself: no editor alert
        void setAlertOnRefusal(bool alert) { alertOnRefusal = alert; }
        const std::string& getRefusal() const { return refusal; }

        bool execute() override;
        void undo() override;

    };

}