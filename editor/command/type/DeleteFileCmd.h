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

        Project* project;
        std::vector<DeleteFilesData> files;
        fs::path trash;
        // why execute() refused (Play running: a scene would leave the project), "" otherwise
        std::string refusal;
        bool alertOnRefusal = true;

        fs::path generateUniqueTrashPath(const fs::path& trashDir, const fs::path& originalFile);

    public:
        DeleteFileCmd(Project* project, std::vector<fs::path> filePaths, fs::path rootPath);

        // An agent's call reports the refusal itself: no editor alert
        void setAlertOnRefusal(bool alert) { alertOnRefusal = alert; }
        const std::string& getRefusal() const { return refusal; }

        bool execute() override;
        void undo() override;

    };

}