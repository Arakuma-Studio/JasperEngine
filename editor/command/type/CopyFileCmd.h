// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"
#include "animation/AnimationClipDocument.h"
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
    };

    class CopyFileCmd: public Command{

    private:

        Project* project;
        std::vector<FileCopyData> files;
        bool copy;
        // the GUIDs copied prefabs and clips got (by project-relative path), given again by a redo
        std::map<std::string, std::string> copyGuids;
        // moves: .anim files whose preview/source paths followed (animation clips spec 9.R26)
        std::vector<ClipHintEdit> clipHintEdits;

        void moveFile(const fs::path& source, const fs::path& destination, bool isDirectory);
        // the other file of each Vertex Animation pair (".vat" + ".vat.glb") joins the list
        void addVatPairs();

    public:
        CopyFileCmd(Project* project, std::vector<std::string> sourceFiles, std::string currentDirectory, std::string targetDirectory, bool copy);
        CopyFileCmd(Project* project, std::vector<std::string> sourcePaths, std::string targetDirectory, bool copy);

        bool execute() override;
        void undo() override;

    };

}
