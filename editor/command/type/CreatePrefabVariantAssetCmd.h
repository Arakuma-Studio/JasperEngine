// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"

#include <filesystem>
#include <string>

namespace jasperengine::editor {

    // Unity's Project window Create > Prefab Variant on a prefab asset: a new variant of it
    // (its root an instance of the prefab, nothing overridden) saved next to it, as one undo
    // step of the project history (editor/project/PREFABS.md, section 19)
    class CreatePrefabVariantAssetCmd: public Command {
    private:
        Project* project;
        std::filesystem::path base;       // project-relative .prefab
        std::filesystem::path filepath;   // project-relative, the new variant
        std::string guid;                 // the GUID the first execute gave the file, kept by a redo
        bool created = false;

    public:
        CreatePrefabVariantAssetCmd(Project* project, const std::filesystem::path& base, const std::filesystem::path& filepath);

        // "<Name> Variant.prefab" next to the base, made unique
        static std::filesystem::path defaultPath(const Project* project, const std::filesystem::path& base);

        const std::filesystem::path& getPath() const { return filepath; }

        bool execute() override;

        std::vector<Touched> touchedPaths(bool undo) const override;
        void undo() override;
    };

}
