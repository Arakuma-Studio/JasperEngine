// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
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

    public:
        DeleteFileCmd(Project* project, std::vector<fs::path> filePaths, fs::path rootPath);

        bool execute() override;
        void undo() override;

        // "Delete File a.png", "Delete 3 Files"
        std::string describe() const override {
            std::vector<fs::path> names;
            for (const DeleteFilesData& file : files) {
                names.push_back(file.originalFile);
            }
            return std::string("Delete ") + (files.size() == 1 ? "File " : "") + CommandNames::files(names);
        }
        const char* typeName() const override { return "DeleteFileCmd"; }
        size_t memoryBytes() const override {
            size_t bytes = sizeof(*this) + trash.native().size();
            for (const DeleteFilesData& file : files) {
                bytes += sizeof(file) + file.originalFile.native().size() + file.trashFile.native().size();
            }
            return bytes;
        }

    };

}