// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Bookmarks.h"

#include "Project.h"
#include "asset/AssetTypes.h"
#include "util/EntityPath.h"

#include <algorithm>
#include <system_error>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

BookmarkTarget Bookmarks::resolve(const Bookmark& bookmark, const BookmarkLookups& lookups) {
    BookmarkTarget target;
    // the GUID's current path, else the hint
    std::string path;
    if (!bookmark.guid.empty() && lookups.pathOfGuid) path = lookups.pathOfGuid(bookmark.guid);
    const bool folder = bookmark.kind == Bookmark::Kind::Folder;
    if (path.empty() && !bookmark.path.empty() && lookups.exists && lookups.exists(bookmark.path, folder)) path = bookmark.path;
    if (path.empty()) return target;
    target.path = path;
    if (bookmark.kind != Bookmark::Kind::GameObject) {
        target.found = true;
        return target;
    }
    const uint32_t sceneId = lookups.openScene ? lookups.openScene(path) : 0;
    if (sceneId == 0) {
        target.sceneClosed = true;
        return target;
    }
    target.sceneId = sceneId;
    if (bookmark.entity != 0 && lookups.hasEntity && lookups.hasEntity(sceneId, bookmark.entity)) {
        target.entity = bookmark.entity;
        target.found = true;
        return target;
    }
    if (!bookmark.entityPath.empty() && lookups.findByPath) {
        const uint64_t entity = lookups.findByPath(sceneId, bookmark.entityPath);
        if (entity != 0) {
            target.entity = entity;
            target.found = true;
        }
    }
    return target;
}

BookmarkLookups Bookmarks::projectLookups(Project* project) {
    BookmarkLookups lookups;
    lookups.pathOfGuid = [project](const std::string& guid) {
        return Guid::isValidText(guid) ? project->getAssetDatabase().getPath(Guid::parse(guid)).generic_string() : std::string();
    };
    lookups.exists = [project](const std::string& path, bool folder) {
        std::error_code ec;
        const fs::path absolute = project->getProjectPath() / path;
        return folder ? fs::is_directory(absolute, ec) : fs::exists(absolute, ec);
    };
    lookups.openScene = [project](const std::string& scenePath) -> uint32_t {
        for (SceneProject* scene : project->getScenesAndPrefabStages()) {
            if (scene && scene->scene && !scene->filepath.empty() &&
                project->normalizeToProjectRelative(scene->filepath).generic_string() == scenePath) {
                return scene->id;
            }
        }
        return 0;
    };
    lookups.hasEntity = [project](uint32_t sceneId, uint64_t entity) {
        SceneProject* scene = project->getScene(sceneId);
        return scene && scene->scene && scene->scene->isEntityCreated(static_cast<Entity>(entity)) &&
               std::find(scene->entities.begin(), scene->entities.end(), static_cast<Entity>(entity)) != scene->entities.end();
    };
    lookups.findByPath = [project](uint32_t sceneId, const std::string& entityPath) -> uint64_t {
        SceneProject* scene = project->getScene(sceneId);
        if (!scene || !scene->scene) return 0;
        const Entity entity = EntityPath::find(*scene, entityPath);
        return entity == NULL_ENTITY ? 0 : entity;
    };
    return lookups;
}

Bookmark Bookmarks::forPath(Project* project, const std::string& projectRelative) {
    Bookmark bookmark;
    std::error_code ec;
    const fs::path absolute = project->getProjectPath() / projectRelative;
    bookmark.path = fs::path(projectRelative).lexically_normal().generic_string();
    bookmark.name = fs::path(bookmark.path).filename().string();
    if (fs::is_directory(absolute, ec)) {
        bookmark.kind = Bookmark::Kind::Folder;
        return bookmark;
    }
    const Guid guid = project->getAssetDatabase().getAssetGuid(bookmark.path);
    if (!guid.isNull()) bookmark.guid = guid.str();
    bookmark.kind = project->getAssetDatabase().typeOf(bookmark.path) == AssetType::Scene ? Bookmark::Kind::Scene : Bookmark::Kind::Asset;
    return bookmark;
}

Bookmark Bookmarks::forGameObject(Project* project, uint32_t sceneId, uint64_t entity) {
    Bookmark bookmark;
    bookmark.kind = Bookmark::Kind::GameObject;
    SceneProject* scene = project->getScene(sceneId);
    if (!scene || !scene->scene) return bookmark;
    bookmark.entity = entity;
    bookmark.name = scene->scene->getEntityName(static_cast<Entity>(entity));
    bookmark.entityPath = EntityPath::of(*scene, static_cast<Entity>(entity));
    if (!scene->filepath.empty()) {
        bookmark.path = project->normalizeToProjectRelative(scene->filepath).generic_string();
        const Guid guid = project->getAssetDatabase().getAssetGuid(bookmark.path);
        if (!guid.isNull()) bookmark.guid = guid.str();
    }
    return bookmark;
}
