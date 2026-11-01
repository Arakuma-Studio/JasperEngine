// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Prefab asset files: GUID resolution through the AssetDatabase, the standalone prefab
// list, prefab file discovery, load and save, path remapping on rename/move and what a
// deletion leaves (Missing Prefab instances, see ProjectPrefabMissing.cpp). The other asset
// type referenced by GUID, the .anim animation clip, resolves here too (animation clips spec
// 9.R8): its references are Animation Player entries {clip: path hint, guid}.

#include "Project.h"
#include "animation/AnimAssetDocument.h"
#include "animation/AnimationClipDocument.h"
#include "animation/ModelSubClips.h"
#include "vat/VatAssetFiles.h"
#include "vat/VatDocument.h"
#include "Factory.h"
#include "Out.h"
#include "Stream.h"
#include "util/FileUtils.h"
#include "util/Util.h"
#include "util/YamlNumbers.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <map>
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
    // the clips the prefab's Animation Players use follow their GUIDs the same way
    resolveRegistryClipRefs(registry);
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

void editor::Project::applyAssetChanges(const std::vector<AssetDatabase::Change>& allChanges) {
    // .anim clips, .vat Vertex Animations, .mask Avatar Masks and .controller Animator
    // Controllers: their references follow in applyClipAssetChanges,
    // the rest is prefabs'
    std::vector<AssetDatabase::Change> changes;
    std::vector<AssetDatabase::Change> clipChanges;
    for (const AssetDatabase::Change& change : allChanges) {
        const std::string path = change.path.string();
        (Util::isGuidJsonAssetFile(path) ? clipChanges : changes).push_back(change);
    }
    if (!clipChanges.empty()) {
        applyClipAssetChanges(clipChanges);
    }
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

// ---- Animation clip references (animation clips spec 4.1, 9.R8/R26) ----

bool editor::Project::resolveClipReference(AnimationPlayerClip& entry, bool mayImport) {
    // sub-clips ("<model>#<clip>") are path-only: they follow the model's rename instead
    if (anim::isSubClipReference(entry.clip)) {
        return false;
    }
    return resolveAssetReference(entry.clip, entry.guid, mayImport);
}

bool editor::Project::resolveAssetReference(std::string& path, std::string& guid, bool mayImport) {
    if (path.empty() && guid.empty()) {
        return false;
    }
    if (!assetDatabase.isOpen() || projectPath.empty()) {
        return false;
    }

    fs::path hint = path.empty() ? fs::path() : normalizeToProjectRelative(resolveAssetPath(path));
    // a path outside the project is never imported (nor given a GUID)
    if (!hint.empty() && (hint.is_absolute() || *hint.begin() == "..")) {
        hint.clear();
    }
    std::error_code ec;
    const bool hintExists = !hint.empty() && fs::is_regular_file(projectPath / hint, ec);

    fs::path resolved;
    if (!guid.empty()) {
        resolved = assetDatabase.getPath(guid);
        if (resolved.empty() && mayImport && hintExists && assetDatabase.getGuid(hint).empty() &&
            AssetDatabase::readGuid(projectPath / hint) == guid) {
            // made outside the editor since the last refresh (what else the database learns
            // waits for the next refresh, as for prefabs)
            std::vector<AssetDatabase::Change> found = assetDatabase.importAssets(hint);
            pendingAssetChanges.insert(pendingAssetChanges.end(), found.begin(), found.end());
            resolved = assetDatabase.getPath(guid);
        }
        if (resolved.empty()) {
            return false;   // no file has it: Missing, the hint stays (an undone delete relinks it)
        }
    } else {
        if (hint.empty()) {
            return false;
        }
        std::string found = assetDatabase.getGuid(hint);
        if (found.empty() && mayImport && hintExists) {
            std::vector<AssetDatabase::Change> imported = assetDatabase.importAssets(hint);
            pendingAssetChanges.insert(pendingAssetChanges.end(), imported.begin(), imported.end());
            found = assetDatabase.getGuid(hint);
        }
        if (found.empty()) {
            // written before the file had a GUID, and moved since: the GUID its path got
            const fs::path moved = assetDatabase.getPath(AssetDatabase::legacyGuid(hint));
            if (moved.empty()) {
                return false;
            }
            found = AssetDatabase::legacyGuid(hint);
            resolved = moved;
        } else {
            resolved = hint;
        }
        // filled in silently: where the reference points did not change
        guid = found;
    }

    const std::string form = normalizeToAssetsRelative(projectPath / resolved).generic_string();
    if (fs::path(path).lexically_normal().generic_string() == form) {
        return false;
    }
    path = form;
    return true;
}

bool editor::Project::resolveRegistryClipRefs(EntityRegistry* registry, bool mayImport) {
    if (!registry) {
        return false;
    }
    bool changed = false;
    auto players = registry->getComponentArray<AnimationPlayerComponent>();
    for (size_t i = 0; i < players->size(); i++) {
        AnimationPlayerComponent& player = players->getComponentFromIndex(i);
        bool playerChanged = false;
        for (AnimationPlayerClip& entry : player.clips) {
            playerChanged |= resolveClipReference(entry, mayImport);
        }
        // layer masks and the controller follow their GUIDs the same way (anim layers spec 6.3)
        for (AnimationPlayerLayer& layer : player.layers) {
            playerChanged |= resolveAssetReference(layer.mask, layer.maskGuid, mayImport);
        }
        playerChanged |= resolveAssetReference(player.controller, player.controllerGuid, mayImport);
        if (playerChanged) {
            player.needsLoad = true;
            changed = true;
        }
    }
    auto animators = registry->getComponentArray<VertexAnimatorComponent>();
    for (size_t i = 0; i < animators->size(); i++) {
        VertexAnimatorComponent& animator = animators->getComponentFromIndex(i);
        const std::string before = animator.animation;
        if (resolveAssetReference(animator.animation, animator.guid, mayImport)) {
            animator.needsLoad = true;
            changed = true;
            // its GameObject's Model played the .vat's mesh under the old name: the pair was
            // renamed or moved together (vertex animation spec 2.1)
            vat::followVatMesh(registry, animators->getEntity(i), before, animator.animation, false);
        }
    }
    return changed;
}

void editor::Project::applyClipAssetChanges(const std::vector<AssetDatabase::Change>& changes) {
    std::vector<fs::path> touched;
    // the clips and masks back at a path: the controllers naming them by GUID follow
    std::map<std::string, fs::path> relinked;
    for (const AssetDatabase::Change& change : changes) {
        touched.push_back((projectPath / change.path).lexically_normal());
        if (change.type == AssetDatabase::Change::Type::Moved && !change.oldPath.empty()) {
            touched.push_back((projectPath / change.oldPath).lexically_normal());
        }
        if (change.type != AssetDatabase::Change::Type::Moved && change.type != AssetDatabase::Change::Type::Added) {
            continue;   // removed: the references stay (Missing); modified: only the content changed
        }
        // moved outside the editor, or back (an undone delete): every entry with the GUID
        // points at the file again
        const std::string form = normalizeToAssetsRelative(projectPath / change.path).generic_string();
        if (!change.guid.empty() && (Util::isAnimationClipFile(change.path.string()) || Util::isAvatarMaskFile(change.path.string()))) {
            relinked[change.guid] = (projectPath / change.path).lexically_normal();
        }
        auto relink = [&](EntityRegistry* registry) {
            bool changed = false;
            if (!registry) {
                return changed;
            }
            auto players = registry->getComponentArray<AnimationPlayerComponent>();
            for (size_t i = 0; i < players->size(); i++) {
                AnimationPlayerComponent& player = players->getComponentFromIndex(i);
                for (AnimationPlayerClip& entry : player.clips) {
                    if (entry.guid != change.guid || anim::isSubClipReference(entry.clip)) {
                        continue;
                    }
                    player.needsLoad = true;
                    if (fs::path(entry.clip).lexically_normal().generic_string() != form) {
                        entry.clip = form;
                        changed = true;
                    }
                }
                // a layer's mask and the controller (anim layers spec 6.3, L6)
                auto relinkPath = [&](std::string& path, const std::string& guid) {
                    if (guid != change.guid) {
                        return;
                    }
                    player.needsLoad = true;
                    if (fs::path(path).lexically_normal().generic_string() != form) {
                        path = form;
                        changed = true;
                    }
                };
                for (AnimationPlayerLayer& layer : player.layers) {
                    relinkPath(layer.mask, layer.maskGuid);
                }
                relinkPath(player.controller, player.controllerGuid);
            }
            auto animators = registry->getComponentArray<VertexAnimatorComponent>();
            for (size_t i = 0; i < animators->size(); i++) {
                VertexAnimatorComponent& animator = animators->getComponentFromIndex(i);
                if (animator.guid != change.guid) {
                    continue;
                }
                animator.needsLoad = true;
                const std::string before = animator.animation;
                if (fs::path(animator.animation).lexically_normal().generic_string() != form) {
                    animator.animation = form;
                    changed = true;
                }
                // the Model follows its .vat (a pair moved outside the editor), and an undone
                // delete of the pair gives the cleared Model its mesh back
                const bool restored = change.type == AssetDatabase::Change::Type::Added;
                changed |= vat::followVatMesh(registry, animators->getEntity(i), before.empty() ? form : before, form, restored);
            }
            return changed;
        };
        for (SceneProject* sceneProject : getScenesAndPrefabStages()) {
            if (sceneProject->scene && relink(sceneProject->scene)) {
                sceneProject->isModified = true;
            }
        }
        // prefab assets in memory only: their files keep the GUID that finds the clip
        for (auto& [prefabPath, prefab] : prefabAssets) {
            relink(prefab.registry.get());
        }
    }
    // (their documents reload there)
    relinkControllerGuids(this, relinked);
    AnimationClipDocument::filesChanged(touched, this);
    // Vertex Animations: their documents reload, the pool drops them, their animators reload
    VatDocument::filesChanged(touched, this);
}

void editor::Project::remapClipFilePath(const std::filesystem::path& oldPath, const std::filesystem::path& newPath) {
    if (projectPath.empty()) {
        return;
    }
    // open .vat, .mask and .controller documents go with their files first (the asset changes
    // below would see their old paths gone)
    VatDocument::filesMoved(oldPath.lexically_normal(), newPath.lexically_normal());
    AvatarMaskDocument::filesMoved(oldPath.lexically_normal(), newPath.lexically_normal());
    AnimatorControllerDocument::filesMoved(oldPath.lexically_normal(), newPath.lexically_normal());
    std::error_code ec;
    // a directory's tracked files moved with remapPrefabFilePath; a model is not tracked
    if (!fs::is_directory(newPath, ec) && (Util::isGuidJsonAssetFile(oldPath.string()) || Util::isGuidJsonAssetFile(newPath.string()))) {
        applyAssetChanges(assetDatabase.moveAssets(oldPath, newPath));
    }
    // open documents (clips, a model's sub-clips) go with their files
    AnimationClipDocument::filesMoved(oldPath.lexically_normal(), newPath.lexically_normal());
    AnimationClipDocument::filesChanged({oldPath.lexically_normal(), newPath.lexically_normal()}, this);
    VatDocument::filesChanged({oldPath.lexically_normal(), newPath.lexically_normal()}, this);
}

void editor::Project::cleanupClipFilePath(const std::filesystem::path& deletedPath) {
    if (projectPath.empty()) {
        return;
    }
    // what cleanupPrefabFilePath did not remove already (an undone copy)
    assetDatabase.removeAssets(deletedPath);
    AnimationClipDocument::filesChanged({deletedPath.lexically_normal()}, this);
    VatDocument::filesChanged({deletedPath.lexically_normal()}, this);
}

void editor::Project::forEachPrefabAsset(const std::function<void(const std::filesystem::path& path, Prefab& prefab)>& visit) {
    for (auto& [prefabPath, prefab] : prefabAssets) {
        visit(prefabPath, prefab);
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

void editor::Project::savePrefabToDisk(const std::filesystem::path& filepath) {
    Prefab* prefab = getPrefab(filepath);
    YAML::Node encodedNode = encodePrefabNode(filepath);
    if (encodedNode && !encodedNode.IsNull()) {
        std::filesystem::path fullPrefabPath = getProjectPath() / filepath;
        if (FileUtils::writeFileReplacing(fullPrefabPath, YAML::Dump(encodedNode), FileUtils::LineEndings::KeepExisting)) {
            prefab->isModified = false;
            prefab->needsUpgrade = false;
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
