// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "Project.h"
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

    public:
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
