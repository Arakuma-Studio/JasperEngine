// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Prefab asset files: GUID resolution through the AssetDatabase, the standalone prefab
// list, prefab file discovery, load and save, path remapping on rename/move and what a
// deletion leaves (Missing Prefab instances, see ProjectPrefabMissing.cpp).

#include "Project.h"
#include "Factory.h"
#include "Out.h"
#include "Stream.h"
#include "util/Util.h"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <unordered_set>

using namespace jasperengine;

// ---- GUIDs ----

std::string editor::Project::getPrefabGuid(const std::filesystem::path& filepath) const {
    if (filepath.empty()) {
        return {};
    }
    return assetDatabase.getGuid(normalizeToProjectRelative(filepath));
}

std::filesystem::path editor::Project::resolvePrefabPath(const std::string& guid, const std::filesystem::path& pathHint) {
    const fs::path hint = normalizeToProjectRelative(pathHint);
    std::error_code ec;
    const bool hintExists = !hint.empty() && !projectPath.empty() && fs::is_regular_file(projectPath / hint, ec);

    if (!assetDatabase.isOpen()) {
        return hintExists ? hint : fs::path();
    }

    if (!guid.empty()) {
        fs::path path = assetDatabase.getPath(guid);
        if (path.empty() && hintExists && assetDatabase.getGuid(hint).empty() &&
            AssetDatabase::readGuid(projectPath / hint) == guid) {
            // Made outside the editor since the last refresh. Resolving happens in the middle
            // of imports, so what else the database learns waits for the next refresh.
            std::vector<AssetDatabase::Change> changes = assetDatabase.importAssets(hint);
            pendingAssetChanges.insert(pendingAssetChanges.end(), changes.begin(), changes.end());
            path = assetDatabase.getPath(guid);
        }
        return path;
    }

    // A reference written before GUIDs: the GUID its path got on the upgrade follows the file
    if (hint.empty()) {
        return {};
    }
    fs::path path = assetDatabase.getPath(AssetDatabase::legacyGuid(hint));
    if (!path.empty()) {
        return path;
    }
    if (hintExists && assetDatabase.getGuid(hint).empty()) {
        std::vector<AssetDatabase::Change> changes = assetDatabase.importAssets(hint);
        pendingAssetChanges.insert(pendingAssetChanges.end(), changes.begin(), changes.end());
    }
    return assetDatabase.getGuid(hint).empty() ? fs::path() : hint;
}

bool editor::Project::resolvePrefabReference(PrefabComponent& component, bool* pathChanged) {
    if (pathChanged) {
        *pathChanged = false;
    }
    const fs::path resolved = resolvePrefabPath(component.guid, component.path);
    if (resolved.empty()) {
        return false;
    }
    const bool changed = fs::path(component.path).lexically_normal() != resolved.lexically_normal();
    if (changed) {
        component.path = resolved.string();
    }
    if (component.guid.empty()) {
        component.guid = assetDatabase.getGuid(resolved);
    }
    if (pathChanged) {
        *pathChanged = changed;
    }
    return true;
}

void editor::Project::resolveRegistryPrefabRefs(EntityRegistry* registry) {
    if (!registry) {
        return;
    }
    auto components = registry->getComponentArray<PrefabComponent>();
    for (size_t i = 0; i < components->size(); i++) {
        PrefabComponent& component = components->getComponentFromIndex(i);
        if (!component.path.empty() || !component.guid.empty()) {
            resolvePrefabReference(component);
        }
    }
}

void editor::Project::resolveScenePrefabInfos(SceneProject& sceneProject) {
    for (PrefabSceneInfo& info : sceneProject.prefabs) {
        const fs::path resolved = resolvePrefabPath(info.guid, info.prefabPath);
        if (resolved.empty()) {
            continue;
        }
        if (info.prefabPath.lexically_normal() != resolved.lexically_normal()) {
            info.prefabPath = resolved;
            info.functionName = Factory::prefabToFunctionName(resolved);
        }
        if (info.guid.empty()) {
            info.guid = assetDatabase.getGuid(resolved);
        }
    }
}

void editor::Project::invalidateClosedSceneSources(const std::function<bool(const PrefabSceneInfo&)>& affected) {
    bool invalidated = false;
    for (const SceneProject& sceneProject : scenes) {
        if (sceneProject.scene || sceneProject.filepath.empty()) {
            continue;
        }
        if (std::any_of(sceneProject.prefabs.begin(), sceneProject.prefabs.end(), affected)) {
            generator.clearSceneSource(sceneProject.name, getProjectInternalPath());
            invalidated = true;
        }
    }
    if (invalidated) {
        generatedSourcesDirty.store(true);
    }
}

void editor::Project::refreshAssetDatabase(bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - lastAssetRefreshTime < std::chrono::seconds(2)) {
        return;
    }
    // Not while a scene plays or saves (a background save writes prefab files)
    if (!assetDatabase.isOpen() || isAnyScenePlaying()) {
        return;
    }
    lastAssetRefreshTime = now;
    std::vector<AssetDatabase::Change> changes = std::move(pendingAssetChanges);
    pendingAssetChanges.clear();
    std::vector<AssetDatabase::Change> found = assetDatabase.refresh();
    changes.insert(changes.end(), found.begin(), found.end());
    applyAssetChanges(changes);
}

void editor::Project::importPrefabFiles(const std::filesystem::path& path, bool copy, std::map<std::string, std::string>* copyGuids) {
    if (projectPath.empty()) {
        return;
    }
    applyAssetChanges(assetDatabase.importAssets(path, copy, copyGuids));
}

void editor::Project::applyAssetChanges(const std::vector<AssetDatabase::Change>& changes) {
    if (changes.empty()) {
        return;
    }
    bool added = false;
    std::unordered_set<std::string> touchedGuids;
    for (const AssetDatabase::Change& change : changes) {
        touchedGuids.insert(change.guid);
    }
    // What went first: a file replaced by another asset at the same path leaves its
    // instances missing before anything is resolved again
    for (const AssetDatabase::Change& change : changes) {
        if (change.type == AssetDatabase::Change::Type::Removed) {
            convertPrefabInstancesToMissing(change.path);
            prefabFileCache.erase(projectPath / change.path);
        }
    }
    for (const AssetDatabase::Change& change : changes) {
        if (change.type == AssetDatabase::Change::Type::Moved) {
            remapPrefabPathsInMemory(change.oldPath, change.path);
            prefabFileCache.erase(projectPath / change.oldPath);
        } else if (change.type == AssetDatabase::Change::Type::Added) {
            added = true;
        }
    }
    if (added) {
        for (const fs::path& prefabPath : std::vector<fs::path>(standalonePrefabs)) {
            if (!getPrefab(prefabPath)) {
                loadStandalonePrefab(prefabPath);
            }
        }
        relinkMissingPrefabs();
    }
    invalidateClosedSceneSources([&](const PrefabSceneInfo& info) {
        return !info.guid.empty() && touchedGuids.count(info.guid);
    });
}

// ---- Rename, move, delete ----

void editor::Project::remapPrefabFilePath(const std::filesystem::path& oldPath, const std::filesystem::path& newPath) {
    if (projectPath.empty()) {
        return;
    }

    fs::path oldRelative = normalizeToProjectRelative(oldPath);
    fs::path newRelative = normalizeToProjectRelative(newPath);

    if (oldRelative.empty() || newRelative.empty()) {
        return;
    }

    std::vector<AssetDatabase::Change> changes = assetDatabase.moveAssets(oldPath, newPath);

    // A prefab renamed to another file type is gone, a file renamed to .prefab is a new one
    std::vector<AssetDatabase::Change> removed;
    std::vector<AssetDatabase::Change> others;
    for (const AssetDatabase::Change& change : changes) {
        (change.type == AssetDatabase::Change::Type::Removed ? removed : others).push_back(change);
    }
    applyAssetChanges(removed);

    // By path too: a prefab the database does not track moves with its directory all the
    // same. A prefab renamed to another type is not one any more: its references keep the
    // path they had (and the GUID that finds it if it is named back).
    if (!AssetDatabase::isTrackedFile(oldRelative) || AssetDatabase::isTrackedFile(newRelative)) {
        remapPrefabPathsInMemory(oldRelative, newRelative);
    }

    others.erase(std::remove_if(others.begin(), others.end(), [](const AssetDatabase::Change& change) {
        return change.type == AssetDatabase::Change::Type::Moved;
    }), others.end());
    applyAssetChanges(others);
}

void editor::Project::remapPrefabPathsInMemory(const fs::path& oldRelative, const fs::path& newRelative) {
    if (oldRelative.empty() || newRelative.empty()) {
        return;
    }

    std::vector<std::pair<fs::path, Prefab>> remappedPrefabs;
    std::unordered_set<uint32_t> affectedSceneIds;

    for (auto it = prefabAssets.begin(); it != prefabAssets.end();) {
        fs::path updatedPath;
        if (!remapRelativePath(oldRelative, newRelative, it->first, updatedPath)) {
            ++it;
            continue;
        }

        Prefab updatedPrefab = std::move(it->second);
        for (const auto& [sceneId, instances] : updatedPrefab.instances) {
            if (!instances.empty()) {
                affectedSceneIds.insert(sceneId);
            }
        }

        remappedPrefabs.emplace_back(updatedPath, std::move(updatedPrefab));
        it = prefabAssets.erase(it);
    }

    for (auto& [filepath, prefab] : remappedPrefabs) {
        prefabAssets.emplace(std::move(filepath), std::move(prefab));
    }

    bool standaloneChanged = false;
    for (fs::path& prefabPath : standalonePrefabs) {
        fs::path updatedPath;
        if (remapRelativePath(oldRelative, newRelative, prefabPath, updatedPath)) {
            prefabPath = updatedPath;
            standaloneChanged = true;
        }
    }

    if (standaloneChanged) {
        saveProject();
    }

    // Nested prefab references held by loaded prefabs: the files keep the GUID, which is
    // what finds the prefab, and get the new path as a hint when they are saved anyway
    for (auto& [prefabPath, prefab] : prefabAssets) {
        if (!prefab.registry) {
            continue;
        }
        auto prefabArray = prefab.registry->getComponentArray<PrefabComponent>();
        for (size_t i = 0; i < prefabArray->size(); ++i) {
            PrefabComponent& prefabComp = prefabArray->getComponentFromIndex(i);
            std::string updatedPath;
            if (remapRelativeString(oldRelative, newRelative, prefabComp.path, updatedPath)) {
                prefabComp.path = updatedPath;
            }
        }
    }

    // Closed scenes too: the build's prefab list comes from them, and their generated
    // sources call the prefabs by names made from the paths
    invalidateClosedSceneSources([&](const PrefabSceneInfo& info) {
        return matchesRelativePath(oldRelative, info.prefabPath);
    });
    for (auto& sceneProject : scenes) {
        for (PrefabSceneInfo& info : sceneProject.prefabs) {
            fs::path updatedPath;
            if (remapRelativePath(oldRelative, newRelative, info.prefabPath, updatedPath)) {
                info.prefabPath = updatedPath;
                info.functionName = Factory::prefabToFunctionName(updatedPath);
            }
        }

        if (!sceneProject.scene) {
            continue;
        }

        bool scenePrefabChanged = false;
        auto prefabArray = sceneProject.scene->getComponentArray<PrefabComponent>();
        for (size_t i = 0; i < prefabArray->size(); ++i) {
            PrefabComponent& prefabComp = prefabArray->getComponentFromIndex(i);
            std::string updatedPath;
            if (remapRelativeString(oldRelative, newRelative, prefabComp.path, updatedPath)) {
                prefabComp.path = updatedPath;
                sceneProject.isModified = true;
                scenePrefabChanged = true;
            }
        }

        if (scenePrefabChanged) {
            updateScenePrefabs(&sceneProject);
        }
    }

    for (uint32_t sceneId : affectedSceneIds) {
        if (SceneProject* sceneProject = getScene(sceneId)) {
            sceneProject->isModified = true;
            if (sceneProject->scene) {
                updateScenePrefabs(sceneProject);
            }
        }
    }
}

void editor::Project::cleanupPrefabFilePath(const std::filesystem::path& deletedPath) {
    if (projectPath.empty()) {
        return;
    }

    fs::path deletedRelative = normalizeToProjectRelative(deletedPath);
    if (deletedRelative.empty()) {
        return;
    }

    assetDatabase.removeAssets(deletedPath);
    invalidateClosedSceneSources([&](const PrefabSceneInfo& info) {
        return matchesRelativePath(deletedRelative, info.prefabPath);
    });

    // Unity's Missing Prefab: the instances in the open scenes keep their roots and what
    // the scenes store for them, and come back when the file does (an undo of the delete).
    // A standalone prefab stays listed for the same reason; a project load drops a listed
    // prefab whose file is gone.
    std::vector<fs::path> prefabsToRemove;
    for (const auto& [filepath, prefab] : prefabAssets) {
        if (matchesRelativePath(deletedRelative, filepath)) {
            prefabsToRemove.push_back(filepath);
        }
    }
    for (const fs::path& prefabPath : prefabsToRemove) {
        convertPrefabInstancesToMissing(prefabPath);
    }

    for (auto it = prefabFileCache.begin(); it != prefabFileCache.end();) {
        if (matchesRelativePath(deletedRelative, normalizeToProjectRelative(it->first))) {
            it = prefabFileCache.erase(it);
        } else {
            ++it;
        }
    }
}

// ---- Standalone prefabs ----

const std::vector<std::filesystem::path>& editor::Project::getStandalonePrefabs() const {
    return standalonePrefabs;
}

bool editor::Project::isStandalonePrefab(const std::filesystem::path& filepath) const {
    const std::string key = filepath.generic_string();
    for (const fs::path& prefabPath : standalonePrefabs) {
        if (prefabPath.generic_string() == key) {
            return true;
        }
    }
    return false;
}

void editor::Project::loadStandalonePrefab(const std::filesystem::path& filepath) {
    const fs::path fullPath = getProjectPath() / filepath;
    std::error_code ec;
    if (prefabAssets.count(filepath) || !fs::exists(fullPath, ec)) {
        return;
    }

    Prefab prefab;
    try {
        prefab.loadRegistry(YAML::LoadFile(fullPath.string()));
        resolveRegistryPrefabRefs(prefab.registry.get());
        prefabAssets.emplace(filepath, std::move(prefab));
    } catch (const std::exception& e) {
        Out::error("Failed to load prefab: %s", e.what());
    }
}

// Loads the listed prefabs, dropping the ones whose file is gone or cannot be read
void editor::Project::setStandalonePrefabs(std::vector<std::filesystem::path> prefabPaths) {
    standalonePrefabs = std::move(prefabPaths);

    for (auto it = prefabAssets.begin(); it != prefabAssets.end();) {
        if (it->second.instances.empty() && !isStandalonePrefab(it->first)) {
            it = prefabAssets.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = standalonePrefabs.begin(); it != standalonePrefabs.end();) {
        loadStandalonePrefab(*it);

        if (!prefabAssets.count(*it)) {
            Out::warning("Prefab \"%s\" is not available, removed from the standalone prefabs", it->string().c_str());
            it = standalonePrefabs.erase(it);
            continue;
        }

        ++it;
    }
}

std::vector<std::filesystem::path> editor::Project::findPrefabFiles() const {
    return findProjectFiles(&Util::isPrefabFile);
}

// ---- Prefab files ----

void editor::Project::savePrefabToDisk(const std::filesystem::path& filepath) {
    Prefab* prefab = getPrefab(filepath);
    YAML::Node encodedNode = encodePrefabNode(filepath);
    if (encodedNode && !encodedNode.IsNull()) {
        std::filesystem::path fullPrefabPath = getProjectPath() / filepath;
        std::ofstream fout(fullPrefabPath.string());
        if (fout.is_open()) {  // Check if file opened successfully
            fout << YAML::Dump(encodedNode);
            fout.close();
            prefab->isModified = false;
            markReferencedSceneIdsPersisted(prefab->registry.get());
            // the database keeps the GUID and learns the file's new time and size
            assetDatabase.registerAsset(filepath, encodedNode["guid"].as<std::string>());
        } else {
            Out::error("Failed to open file for writing: %s", fullPrefabPath.string().c_str());
        }
    }
}

YAML::Node editor::Project::encodePrefabNode(const std::filesystem::path& filepath) const {
    const Prefab* prefab = getPrefab(filepath);
    if (!prefab || !prefab->registry) {
        return YAML::Node();
    }

    std::string guid = getPrefabGuid(filepath);
    if (guid.empty()) {
        guid = AssetDatabase::legacyGuid(normalizeToProjectRelative(filepath));
    }

    // Prefab file v2 (editor/project/PREFABS.md): the header always, the GameObjects
    // under members even when there is only one
    YAML::Node prefabNode;
    prefabNode["type"] = "Prefab";
    prefabNode["version"] = Prefab::FILE_VERSION;
    prefabNode["guid"] = guid;
    prefabNode["nextEntity"] = prefab->getNextEntity();
    YAML::Node membersNode(YAML::NodeType::Sequence);
    for (Entity entity : getTopLevelEntities(prefab->registry.get(), prefab->registryEntities)) {
        membersNode.push_back(Stream::encodeEntity(entity, prefab->registry.get(), this));
    }
    prefabNode["members"] = membersNode;
    return prefabNode;
}

YAML::Node editor::Project::loadPrefabFile(const std::filesystem::path& fullPath) {
    // What YAML::LoadFile gives, parsed again only when the file changed
    std::error_code timeError;
    std::error_code sizeError;
    const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(fullPath, timeError);
    const std::uintmax_t size = std::filesystem::file_size(fullPath, sizeError);
    if (timeError || sizeError) {
        prefabFileCache.erase(fullPath);
        return YAML::LoadFile(fullPath.string()); // throws as before for a missing file
    }
    auto cached = prefabFileCache.find(fullPath);
    if (cached != prefabFileCache.end() && cached->second.writeTime == writeTime && cached->second.size == size) {
        return cached->second.node;
    }
    YAML::Node node = YAML::LoadFile(fullPath.string());
    PrefabFileCache& entry = prefabFileCache[fullPath];
    entry.writeTime = writeTime;
    entry.size = size;
    entry.node.reset(node);
    return node;
}

void editor::Project::cleanupPrefabsForScene(uint32_t sceneId){
    for (auto it = prefabAssets.begin(); it != prefabAssets.end(); ) {
        it->second.instances.erase(sceneId);
        if (it->second.instances.empty() && !isStandalonePrefab(it->first)) {
            it = prefabAssets.erase(it);
        } else {
            ++it;
        }
    }
}
