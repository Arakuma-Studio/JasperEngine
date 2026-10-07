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

    class RenameFileCmd: public Command{

    private:

        Project* project;
        fs::path oldFilename;
        fs::path newFilename;
        fs::path directory;

        // Relocates the cached thumbnail so a rename doesn't force it to regenerate.
        void moveThumbnail(const fs::path& oldThumbnail, const fs::path& renamedFile);
        void renameFile(const fs::path& source, const fs::path& destination);

    public:
        RenameFileCmd(Project* project, std::string oldName, std::string newName, std::string directory);

        bool execute() override;
        void undo() override;

    };

}
