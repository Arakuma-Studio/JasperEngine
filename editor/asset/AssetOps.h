// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Rename, move, copy, delete and restore of project files (S04 R13), the one path every
// editor file operation takes (the Project window, its commands and MCP). Each operation:
//   1. moves or copies the files and their metas (a destination that exists is refused);
//   2. updates the AssetDatabase (GUIDs follow moves, copies get new ones);
//   3. refreshes the path hints of the loaded scenes, prefab stages and prefab assets, and
//      what follows a file (open animation clip documents, the asset the Inspector shows).
// A rename or move writes no file but the moved assets and their metas (the project file
// only when the scene list's hints change). Deleting leaves references intact: they become
// Missing, and restoring the file resolves them again (R15). References in source text
// (Lua require, HTML/CSS, shader #include) are not rewritten: the ones an operation breaks
// are reported (R14).

#pragma once

#include "util/AssetDatabase.h"
#include "asset/DepEdge.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class AssetOps {
    public:
        struct TextReference {
            std::string referrer;   // project-relative
            uint32_t line = 0;
            std::string target;     // project-relative path the reference names
            DepEdgeKind kind = DepEdgeKind::Other;
        };

        // The identity a copied .scene file got (its own scene id and name)
        struct SceneCopy {
            uint32_t id = 0;
            std::string name;
        };

        struct Result {
            bool ok = false;
            std::string error;
            std::vector<AssetDatabase::Change> changes;
            std::vector<TextReference> textReferencesBroken;
            std::vector<std::string> filesWritten;   // project-relative
            // copy: each .scene copied, by project-relative path of the copy
            std::vector<std::pair<std::string, SceneCopy>> copiedScenes;
        };

        // The text references into a file or directory that moving or deleting it breaks
        static std::vector<TextReference> textReferencesInto(Project& project, const std::filesystem::path& path);

        static Result move(Project& project, const std::filesystem::path& from, const std::filesystem::path& to);
        // copyGuids and copyScenes (by relative path) keep a redo's copies at the GUIDs, scene ids
        // and scene names of the first copy. Every copied .scene gets its own scene id and a unique
        // name (a copy from outside the project keeps an id and a name no project scene has).
        static Result copy(Project& project, const std::filesystem::path& from, const std::filesystem::path& to,
                           std::map<std::string, std::string>* copyGuids = nullptr,
                           std::map<std::string, SceneCopy>* copyScenes = nullptr);
        // Undoes a copy: the copied files and their metas are deleted
        static Result removeCopy(Project& project, const std::filesystem::path& path);
        // To the project trash (.trash), metas included; trashPath gets where it went
        static Result remove(Project& project, const std::filesystem::path& path, std::filesystem::path& trashPath);
        // Back from the trash: the asset keeps its GUID, so its references resolve again
        static Result restore(Project& project, const std::filesystem::path& trashPath, const std::filesystem::path& original);

        // A free name in the trash for a file
        static std::filesystem::path uniqueTrashPath(const std::filesystem::path& trashDir, const std::filesystem::path& file);
    };

}
