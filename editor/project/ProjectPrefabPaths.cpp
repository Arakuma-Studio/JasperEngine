// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Prefab asset files: GUID resolution through the AssetDatabase, the standalone prefab
// list, prefab file discovery, load and save, path remapping on rename/move and what a
// deletion leaves (Missing Prefab instances, see ProjectPrefabMissing.cpp).

#include "Project.h"
#include "DocumentFile.h"
#include "StreamPreserved.h"
#include "Factory.h"
#include "Out.h"
#include "Stream.h"
#include "util/FileUtils.h"
#include "util/Util.h"
#include "util/YamlNumbers.h"

#include <algorithm>
#include <fstream>
#include <functional>
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
            generator.clearSceneSource(sceneProject.id, sceneProject.name, getProjectInternalPath());
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
    // Edited outside the editor: the loaded prefab is read again and its open instances
    // follow (unsaved changes made in the editor win, and are written over it on save)
    for (const AssetDatabase::Change& change : changes) {
        if (change.type != AssetDatabase::Change::Type::Modified) {
            continue;
        }
        prefabFileCache.erase(projectPath / change.path);
        Prefab* prefab = getPrefab(change.path);
        if (!prefab) {
            continue;
        }
        if (prefab->isModified) {
            Out::warning("Prefab \"%s\" changed on disk but has unsaved changes in the editor: the editor's are kept",
                change.path.string().c_str());
            continue;
        }
        Out::info("Prefab \"%s\" changed on disk: its instances are updated", change.path.string().c_str());
        refreshPrefabInstances(change.path, true);
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
    if (readPrefabFile(filepath, prefab)) {
        prefabAssets.emplace(filepath, std::move(prefab));
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

editor::SaveResult editor::Project::savePrefabToDisk(const std::filesystem::path& filepath, DegradedSavePolicy policy) {
    Prefab* prefab = getPrefab(filepath);
    if (!prefab) {
        return SaveResult::Failed;
    }
    // a prefab file that did not load completely is not written over unless the loss was accepted
    if (prefabSaveScopeDepth > 0 && prefabSaveScopePolicy == DegradedSavePolicy::SaveAnyway) {
        policy = DegradedSavePolicy::SaveAnyway;
    }
    if (prefab->loadReport.dropped() > 0 && policy == DegradedSavePolicy::Refuse) {
        if (prefabSaveScopeDepth > 0 &&
                std::find(refusedPrefabSaves.begin(), refusedPrefabSaves.end(), filepath) == refusedPrefabSaves.end()) {
            refusedPrefabSaves.push_back(filepath);
        }
        Out::warning("Prefab \"%s\" not saved: it did not load completely and saving it would remove %zu value(s) "
                     "this editor could not read.", filepath.generic_string().c_str(), prefab->loadReport.dropped());
        return SaveResult::RefusedDegraded;
    }
    YAML::Node encodedNode = encodePrefabNode(filepath);
    if (!encodedNode || encodedNode.IsNull()) {
        return SaveResult::Failed;
    }
    std::filesystem::path fullPrefabPath = getProjectPath() / filepath;
    // a file of a Windows checkout keeps its \r\n
    const std::string text = FileUtils::withLineEndings(fullPrefabPath, YAML::Dump(encodedNode));
    const io::WriteResult written = DocumentFiles::write(fullPrefabPath, text);
    if (!written.ok()) {
        const std::string message = DocumentFiles::saveFailureMessage(filepath, written);
        Out::error(message);
        alertUser("Error", message);
        return SaveResult::Failed;
    }
    prefab->diskSha1 = DocumentFiles::sha1(text);
    if (prefab->loadReport.dropped() > 0) {
        Out::warning("Saved prefab \"%s\" without %zu value(s) this editor could not read.",
                     filepath.generic_string().c_str(), prefab->loadReport.dropped());
        prefab->loadReport.clearDropped();
    }
    prefab->isModified = false;
    prefab->needsUpgrade = false;
    markReferencedSceneIdsPersisted(prefab->registry.get());
    // the database keeps the GUID and learns the file's new time and size
    assetDatabase.registerAsset(filepath, encodedNode["guid"].as<std::string>());
    return SaveResult::Saved;
}

void editor::Project::beginPrefabSaveScope(DegradedSavePolicy policy) {
    if (prefabSaveScopeDepth++ == 0) {
        refusedPrefabSaves.clear();
    }
    prefabSaveScopePolicy = policy;
}

std::vector<std::filesystem::path> editor::Project::endPrefabSaveScope() {
    if (prefabSaveScopeDepth > 0 && --prefabSaveScopeDepth == 0) {
        prefabSaveScopePolicy = DegradedSavePolicy::Refuse;
        return std::move(refusedPrefabSaves);
    }
    return {};
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

    // Prefab file v3 (editor/project/PREFABS.md): the header always, the root GameObject
    // first under members (then any GameObject without a Transform next to it), the
    // instances nested in it with their `prefabInstance` block
    YAML::Node prefabNode;
    prefabNode["type"] = "Prefab";
    prefabNode["version"] = Prefab::FILE_VERSION;
    prefabNode["guid"] = guid;
    prefabNode["nextEntity"] = prefab->getNextEntity();
    prefabNode["root"] = prefab->root;
    std::vector<Entity> topLevel = getTopLevelEntities(prefab->registry.get(), prefab->registryEntities);
    auto rootIt = std::find(topLevel.begin(), topLevel.end(), prefab->root);
    if (rootIt != topLevel.end() && rootIt != topLevel.begin()) {
        topLevel.erase(rootIt);
        topLevel.insert(topLevel.begin(), prefab->root);
    }
    // Without the project: the children of a nested root are written, they are the
    // GameObjects this prefab adds to that nested instance (PREFABS.md 17)
    YAML::Node membersNode(YAML::NodeType::Sequence);
    for (Entity entity : topLevel) {
        membersNode.push_back(Stream::encodeEntity(entity, prefab->registry.get()));
    }

    // What this prefab overrides of the instances nested in it, on their roots, and where
    // the GameObjects it adds to them go
    std::function<void(YAML::Node)> addNestedBlocks = [&](YAML::Node node) {
        if (!node.IsMap()) {
            return;
        }
        const YAML::Node readNode = node;
        if (readNode["entity"]) {
            const Entity entity = YamlNumbers::as<Entity>(readNode["entity"]);
            auto data = prefab->nestedInstances.find(entity);
            if (data != prefab->nestedInstances.end() && prefab->isInstanceRoot(entity)) {
                YAML::Node block = Stream::encodePrefabInstanceBlock(data->second);
                if (block.size() > 0) {
                    node["prefabInstance"] = block;
                }
            }
            auto added = prefab->addedObjects.find(entity);
            if (added != prefab->addedObjects.end()) {
                if (added->second.parent != NULL_ENTITY) {
                    node["addedTo"] = added->second.parent;
                }
                if (added->second.childIndex >= 0) {
                    node["childIndex"] = added->second.childIndex;
                }
            }
        }
        if (readNode["children"] && readNode["children"].IsSequence()) {
            for (YAML::Node child : node["children"]) {
                addNestedBlocks(child);
            }
        }
    };
    if (!prefab->nestedInstances.empty() || !prefab->addedObjects.empty()) {
        for (YAML::Node member : membersNode) {
            addNestedBlocks(member);
        }
    }
    prefabNode["members"] = membersNode;

    if (!prefab->stripped.empty()) {
        YAML::Node strippedNode(YAML::NodeType::Sequence);
        for (const auto& [id, source] : prefab->stripped) {
            if (source.empty() || !prefab->isInstanceRoot(source[0])) {
                continue; // into a nested instance the prefab no longer has
            }
            YAML::Node entry;
            entry["entity"] = id;
            YAML::Node way(YAML::NodeType::Sequence);
            way.SetStyle(YAML::EmitterStyle::Flow);
            for (Entity step : source) {
                way.push_back(step);
            }
            entry["source"] = way;
            strippedNode.push_back(entry);
        }
        if (strippedNode.size() > 0) {
            prefabNode["stripped"] = strippedNode;
        }
    }
    StreamSave::applyRootKeys(prefab->registry.get(), NULL_ENTITY, prefabNode);
    return prefabNode;
}

YAML::Node editor::Project::loadPrefabFile(const std::filesystem::path& fullPath, std::string* sha1) {
    // What reading the file gives, parsed again only when the file changed
    std::error_code timeError;
    std::error_code sizeError;
    const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(fullPath, timeError);
    const std::uintmax_t size = std::filesystem::file_size(fullPath, sizeError);
    if (timeError || sizeError) {
        prefabFileCache.erase(fullPath);
        DocumentFile document = DocumentFiles::read(fullPath, DocumentKind::Prefab); // throws as before for a missing file
        if (sha1) *sha1 = document.sha1;
        return document.root;
    }
    auto cached = prefabFileCache.find(fullPath);
    if (cached != prefabFileCache.end() && cached->second.writeTime == writeTime && cached->second.size == size) {
        DocumentFiles::recordVersionIssues(DocumentKind::Prefab, cached->second.migration);
        if (sha1) *sha1 = cached->second.sha1;
        return cached->second.node;
    }
    DocumentFile document = DocumentFiles::read(fullPath, DocumentKind::Prefab);
    PrefabFileCache& entry = prefabFileCache[fullPath];
    entry.writeTime = writeTime;
    entry.size = size;
    entry.node.reset(document.root);
    entry.sha1 = document.sha1;
    entry.migration = document.migration;
    if (sha1) *sha1 = document.sha1;
    return document.root;
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
