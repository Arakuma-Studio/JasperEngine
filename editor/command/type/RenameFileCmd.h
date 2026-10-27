// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
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

        // Relocates the cached thumbnail so a rename doesn't force it to regenerate.
        void moveThumbnail(const fs::path& oldThumbnail, const fs::path& renamedFile);
        // false when there was no source to rename
        bool renameFile(const fs::path& source, const fs::path& destination);

    public:
        RenameFileCmd(Project* project, std::string oldName, std::string newName, std::string directory);

        bool execute() override;
        void undo() override;

    };

}
