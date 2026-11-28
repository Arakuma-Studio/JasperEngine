// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FogProfileAssets.h"

#include "Project.h"
#include "Scene.h"
#include "component/FogComponent.h"
#include "component/FogZoneComponent.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <system_error>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

// file-local, in a namespace of its own: a unity build merges the editor's sources
namespace jasperengine::editor::fogprofiles::assets {
namespace {

    bool readProfile(const fs::path& file, FogProfileData& out, std::string* error = nullptr) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            if (error) *error = "cannot open the file";
            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return FogProfileIO::parse(text, out, error);
    }

    std::string relativeTo(const fs::path& file, const fs::path& root) {
        return FileUtils::pathToGenericUtf8(file.lexically_normal().lexically_relative(root.lexically_normal()));
    }

    std::string builtinIds() {
        std::string ids;
        for (const FogProfileBuiltin& builtin : FogProfiles::builtins()) ids += (ids.empty() ? "" : ", ") + std::string(builtin.id);
        return ids;
    }

    bool sameReference(const AssetRef& ref, const Guid& guid, const std::string& assetPath) {
        if (!guid.isNull() && !ref.guid.isNull()) return ref.guid == guid;
        return !ref.path.empty() && FogProfileLibrary::normalizeKey(ref.path) == assetPath;
    }

}
}


std::vector<fogprofiles::ProjectFogProfile> jasperengine::editor::fogprofiles::listProjectProfiles(const Project& project) {
    std::vector<ProjectFogProfile> profiles;
    const fs::path assets = project.getAssetsPath();
    std::error_code error;
    if (!fs::is_directory(assets, error)) return profiles;
    for (fs::recursive_directory_iterator it(assets, fs::directory_options::skip_permission_denied, error), end; it != end; it.increment(error)) {
        if (error) break;
        const fs::path& file = it->path();
        // hidden folders (.git, editor caches) hold no project profiles
        if (it->is_directory(error) && !file.filename().empty() && file.filename().string()[0] == '.') {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(error) || !FogProfileIO::isProfilePath(file.filename().string())) continue;
        ProjectFogProfile entry;
        if (!fogprofiles::assets::readProfile(file, entry.data)) continue;
        entry.path = fogprofiles::assets::relativeTo(file, project.getProjectPath());
        entry.assetPath = fogprofiles::assets::relativeTo(file, assets);
        profiles.push_back(std::move(entry));
    }
    std::sort(profiles.begin(), profiles.end(), [](const ProjectFogProfile& a, const ProjectFogProfile& b) { return a.path < b.path; });
    return profiles;
}

std::string jasperengine::editor::fogprofiles::builtinCopyPath(const Project& project, const std::string& id) {
    return fogprofiles::assets::relativeTo(project.getAssetsPath() / FileUtils::pathFromUtf8(FogProfiles::builtinCopyPath(id)), project.getProjectPath());
}

std::string jasperengine::editor::fogprofiles::findBuiltinCopy(const Project& project, const std::string& id) {
    const std::string path = builtinCopyPath(project, id);
    std::error_code error;
    return fs::is_regular_file(project.getProjectPath() / FileUtils::pathFromUtf8(path), error) ? path : std::string();
}

fogprofiles::ResolvedFogProfile jasperengine::editor::fogprofiles::resolveProfileName(const Project& project, const std::string& name) {
    ResolvedFogProfile result;
    if (name.empty()) {
        result.error = "no fog profile named: give a .fogprofile path or builtin:<id> (" + fogprofiles::assets::builtinIds() + ")";
        return result;
    }

    // a built-in: through the project's copy of it
    const FogProfileBuiltin* builtin = FogProfiles::findBuiltin(name);
    if (FogProfiles::isBuiltinKey(name) && !builtin) {
        result.error = "no built-in fog profile '" + name + "': use one of builtin:" + fogprofiles::assets::builtinIds();
        return result;
    }
    if (builtin) {
        result.builtin = true;
        result.builtinId = builtin->id;
        result.path = findBuiltinCopy(project, builtin->id);
        if (result.path.empty()) {
            result.path = builtinCopyPath(project, builtin->id);
            result.needsCopy = true;
            result.copyText = FogProfileIO::write(builtin->data);
            result.data = builtin->data;
        } else {
            std::string error;
            if (!fogprofiles::assets::readProfile(project.getProjectPath() / FileUtils::pathFromUtf8(result.path), result.data, &error)) {
                result.error = "the project's copy of builtin:" + std::string(builtin->id) + " (" + result.path + ") does not read (" + error +
                               "): fix it, or delete it (a new copy is made on the next use)";
                return result;
            }
        }
        result.assetPath = fogprofiles::assets::relativeTo(project.getProjectPath() / FileUtils::pathFromUtf8(result.path), project.getAssetsPath());
        result.ok = true;
        return result;
    }

    // a path: project-relative first, else relative to the assets directory
    if (!FogProfileIO::isProfilePath(name)) {
        result.error = "'" + name + "' is not a .fogprofile path or a built-in (" + fogprofiles::assets::builtinIds() + ")";
        return result;
    }
    fs::path file = project.getProjectPath() / FileUtils::pathFromUtf8(name);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) file = project.getAssetsPath() / FileUtils::pathFromUtf8(name);
    if (!fs::is_regular_file(file, ec)) {
        result.error = "no fog profile at '" + name + "': give a project .fogprofile path or builtin:<id> (" + fogprofiles::assets::builtinIds() + ")";
        return result;
    }
    std::string error;
    if (!fogprofiles::assets::readProfile(file, result.data, &error)) {
        result.error = "'" + name + "' does not read as a fog profile (" + error + "): fix the file or pick another";
        return result;
    }
    result.path = fogprofiles::assets::relativeTo(file, project.getProjectPath());
    result.assetPath = fogprofiles::assets::relativeTo(file, project.getAssetsPath());
    result.ok = true;
    return result;
}

std::vector<fogprofiles::FogProfileUser> jasperengine::editor::fogprofiles::usersInOpenScenes(Project& project, const std::string& assetPath) {
    std::vector<FogProfileUser> users;
    const std::string normal = FogProfileLibrary::normalizeKey(assetPath);
    const std::string projectRelative = fogprofiles::assets::relativeTo(project.getAssetsPath() / FileUtils::pathFromUtf8(normal), project.getProjectPath());
    const AssetDatabase::Resolution resolution = project.getAssetDatabase().lookupPath(projectRelative);
    const Guid guid = resolution.found() ? resolution.guid : Guid();
    for (SceneProject* sceneProject : project.getScenesAndPrefabStages()) {
        Scene* scene = sceneProject->scene;
        if (!scene) continue;
        auto fogs = scene->getComponentArray<FogComponent>();
        for (size_t i = 0; i < fogs->size(); i++) {
            if (fogprofiles::assets::sameReference(fogs->getComponentFromIndex(i).profile, guid, normal))
                users.push_back({sceneProject->id, sceneProject->name, fogs->getEntity(i), false});
        }
        if (!scene->isComponentRegistered<FogZoneComponent>()) continue;
        auto zones = scene->getComponentArray<FogZoneComponent>();
        for (size_t i = 0; i < zones->size(); i++) {
            if (fogprofiles::assets::sameReference(zones->getComponentFromIndex(i).profile, guid, normal))
                users.push_back({sceneProject->id, sceneProject->name, zones->getEntity(i), true});
        }
    }
    return users;
}

std::vector<std::string> jasperengine::editor::fogprofiles::referencingFiles(const Project& project, const std::string& assetPath) {
    const std::string normal = FogProfileLibrary::normalizeKey(assetPath);
    const std::string projectRelative = fogprofiles::assets::relativeTo(project.getAssetsPath() / FileUtils::pathFromUtf8(normal), project.getProjectPath());
    const AssetDatabase::Resolution resolution = project.getAssetDatabase().lookupPath(projectRelative);
    std::set<std::string> files;
    for (const DepEdge& edge : project.getAssetIndex().referencesTo(resolution.found() ? resolution.guid : Guid(), projectRelative, false, 4096)) {
        const std::string referrer = project.getAssetIndex().referrerPath(edge.referrer);
        if (!referrer.empty()) files.insert(referrer);
    }
    return std::vector<std::string>(files.begin(), files.end());
}

void jasperengine::editor::fogprofiles::profileWritten(Project& project, const std::string& assetPath, const FogProfileData& data) {
    FogProfileLibrary::set(assetPath, data);
    for (SceneProject* sceneProject : project.getScenesAndPrefabStages())
        if (sceneProject->scene) sceneProject->needUpdateRender = true;
}

void jasperengine::editor::fogprofiles::profileChanged(Project& project, const std::string& assetPath) {
    FogProfileLibrary::reload(assetPath);
    for (SceneProject* sceneProject : project.getScenesAndPrefabStages())
        if (sceneProject->scene) sceneProject->needUpdateRender = true;
}

fogprofiles::FogProfileUsers jasperengine::editor::fogprofiles::profileUsers(Project& project, const std::string& assetPath) {
    FogProfileUsers users;
    users.open = usersInOpenScenes(project, assetPath);
    // an open scene's or prefab's file holds what it held when saved: its live GameObjects count instead
    std::set<std::string> openFiles;
    for (SceneProject* sceneProject : project.getScenesAndPrefabStages()) {
        if (!sceneProject->scene) continue;
        if (!sceneProject->filepath.empty()) openFiles.insert(project.normalizeToProjectRelative(sceneProject->filepath).generic_string());
        if (const PrefabStage* stage = project.getPrefabStage(sceneProject->id))
            openFiles.insert(project.normalizeToProjectRelative(stage->assetPath).generic_string());
    }
    users.indexReady = project.getAssetIndex().isOpen() && project.getAssetIndex().isReady();
    for (const std::string& file : referencingFiles(project, assetPath)) {
        if (!openFiles.count(file)) users.closedFiles.push_back(file);
    }
    return users;
}

std::string jasperengine::editor::fogprofiles::projectPathOfKey(const Project& project, const std::string& key) {
    if (key.empty() || FogProfiles::isBuiltinKey(key) || FogProfileLibrary::isRuntimeKey(key) || !FogProfileIO::isProfilePath(key)) return key;
    return fogprofiles::assets::relativeTo(project.getAssetsPath() / FileUtils::pathFromUtf8(FogProfileLibrary::normalizeKey(key)), project.getProjectPath());
}
