// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Where a bookmark points now (S25 R28, D8). An asset, folder or scene bookmark holds its S04
// GUID and a path hint: the GUID's current path wins (it follows moves and renames), the hint
// is used when the GUID is unknown (a folder has none). A GameObject bookmark holds its scene's
// GUID, the GameObject's saved entity id and its EntityPath: the id first, then the path, else
// it is Missing. The lookups are passed in, so tests resolve against fakes.

#include "organisation/Organisation.h"

#include <cstdint>
#include <functional>
#include <string>

namespace jasperengine::editor {

    class Project;

    struct BookmarkTarget {
        bool found = false;
        std::string path;         // project-relative (the scene's for a GameObject)
        uint32_t sceneId = 0;     // GameObject: the open scene holding it (0: not open)
        uint64_t entity = 0;      // GameObject: its entity in that scene
        bool sceneClosed = false; // GameObject: the scene exists but is not open (open it to resolve)
    };

    struct BookmarkLookups {
        // GUID -> current project-relative path ("" unknown)
        std::function<std::string(const std::string& guid)> pathOfGuid;
        std::function<bool(const std::string& path, bool folder)> exists;
        // an open scene by its project-relative path (0 when it is not open)
        std::function<uint32_t(const std::string& scenePath)> openScene;
        std::function<bool(uint32_t sceneId, uint64_t entity)> hasEntity;
        // EntityPath -> entity in the open scene (0: none)
        std::function<uint64_t(uint32_t sceneId, const std::string& entityPath)> findByPath;
    };

    namespace Bookmarks {
        BookmarkTarget resolve(const Bookmark& bookmark, const BookmarkLookups& lookups);
        // The project's lookups
        BookmarkLookups projectLookups(Project* project);
        // A bookmark of a project file or folder (project-relative), named after it
        Bookmark forPath(Project* project, const std::string& projectRelative);
        // A bookmark of a GameObject of an open scene
        Bookmark forGameObject(Project* project, uint32_t sceneId, uint64_t entity);
    }

}
