// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Prefab asset files: standalone prefab list, prefab file discovery, load and save,
// path remapping on rename/move and cleanup on deletion.

#include "Project.h"
#include "Out.h"
#include "Stream.h"
#include "util/Util.h"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <unordered_set>

using namespace jasperengine;

void editor::Project::remapPrefabFilePath(const std::filesystem::path& oldPath, const std::filesystem::path& newPath) {
    if (projectPath.empty()) {
        return;
    }

    fs::path oldRelative = normalizeToProjectRelative(oldPath);
    fs::path newRelative = normalizeToProjectRelative(newPath);

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

    bool changed = !remappedPrefabs.empty();

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

    for (auto& sceneProject : scenes) {
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
                changed = true;
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

    std::vector<fs::path> prefabsToRemove;

    for (const auto& [filepath, prefab] : prefabAssets) {
        if (matchesRelativePath(deletedRelative, filepath)) {
            prefabsToRemove.push_back(filepath);
        }
    }

    bool changed = !prefabsToRemove.empty();
    std::unordered_set<uint32_t> affectedSceneIds;

    const size_t listedBefore = standalonePrefabs.size();
    standalonePrefabs.erase(
        std::remove_if(standalonePrefabs.begin(), standalonePrefabs.end(),
            [&](const fs::path& prefabPath) { return matchesRelativePath(deletedRelative, prefabPath); }),
        standalonePrefabs.end());
    if (standalonePrefabs.size() != listedBefore) {
        saveProject();
    }

    for (const auto& prefabPath : prefabsToRemove) {
        Prefab* prefab = getPrefab(prefabPath);
        if (!prefab) {
            continue;
        }

        std::vector<std::pair<uint32_t, std::pair<Entity, std::vector<Entity>>>> instancesToRemove;
        for (const auto& [sceneId, instances] : prefab->instances) {
            affectedSceneIds.insert(sceneId);
            for (const auto& instance : instances) {
                std::vector<Entity> memberEntities;
                memberEntities.reserve(instance.members.size());

                for (const auto& member : instance.members) {
                    if (member.localEntity != NULL_ENTITY) {
                        memberEntities.push_back(member.localEntity);
                    }
                }

                instancesToRemove.emplace_back(sceneId, std::make_pair(instance.rootEntity, std::move(memberEntities)));
            }
        }

        for (const auto& [sceneId, rootAndMembers] : instancesToRemove) {
            unimportPrefab(sceneId, prefabPath, rootAndMembers.first, rootAndMembers.second);
        }

        removePrefab(prefabPath);
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
        const fs::path fullPath = getProjectPath() / *it;
        std::error_code ec;

        if (!prefabAssets.count(*it) && fs::exists(fullPath, ec)) {
            Prefab prefab;
            prefab.registry = std::make_unique<EntityRegistry>();

            try {
                YAML::Node node = YAML::LoadFile(fullPath.string());
                Stream::decodeEntitySelection(node, prefab.registry.get(), &prefab.registryEntities);
                prefabAssets.emplace(*it, std::move(prefab));
            } catch (const std::exception& e) {
                Out::error("Failed to load prefab: %s", e.what());
            }
        }

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

    std::vector<Entity> topLevelEntities = getTopLevelEntities(prefab->registry.get(), prefab->registryEntities);
    return Stream::encodeEntitySelection(topLevelEntities, prefab->registry.get(), this);
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
