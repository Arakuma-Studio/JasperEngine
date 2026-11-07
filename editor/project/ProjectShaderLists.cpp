// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The project's shader list, with its scenes closed or open (docs/specs/S64-shader-lists.md): a
// closed scene's keys computed from its current file as an export's load and save compute them, the
// cache that keeps an unchanged project from reading any, and the list every consumer shares.

#include "Project.h"

#include "DocumentFile.h"
#include "jobs/JobSystem.h"
#include "LoadReport.h"
#include "Out.h"
#include "Factory.h"
#include "System.h"
#include "players/PlayerRuntimes.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCollector.h"
#include "shader/ShaderWarmup.h"
#include "util/FileUtils.h"
#include "util/Util.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <thread>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    std::int64_t shaderListStamp(fs::file_time_type time) {
        return static_cast<std::int64_t>(time.time_since_epoch().count());
    }

    // The files a closed scene's keys read besides its own (ShaderVariantCollector, the prefab
    // instances its load makes): not the other scenes' (each has its own hash), nor textures, which
    // no key depends on, nor what a save writes next to a scene
    bool readsIntoShaderKeys(const std::string& path) {
        return editor::Util::isPrefabFile(path) || editor::Util::isModelFile(path) || editor::Util::isMaterialFile(path) ||
               editor::Util::isScriptFile(path) || editor::Util::isShaderFile(path) || editor::Util::isHtmlFile(path) ||
               editor::Util::isCssFile(path) || editor::Util::isVertexAnimationFile(path);
    }

    // The file's stamp under the name it has wherever the project is: a copy of the project (a
    // checkout moved, its times kept) keeps its cache
    void addFileStamp(std::string& text, const std::string& shownName, const fs::path& file) {
        std::error_code timeError;
        std::error_code sizeError;
        const fs::file_time_type time = fs::last_write_time(file, timeError);
        const std::uintmax_t size = fs::file_size(file, sizeError);
        text += shownName;
        if (!timeError && !sizeError) {
            text += '|' + std::to_string(shaderListStamp(time)) + '|' + std::to_string(size);
        }
        text += '\n';
    }

    // Every file below root that matches, under its name relative to root, in a stable order
    void addTreeStamps(std::string& text, const fs::path& root, const std::function<bool(const std::string&)>& matches) {
        std::error_code ec;
        std::vector<fs::path> files;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
            if (it->is_regular_file(ec) && matches(it->path().string())) files.push_back(it->path());
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files) {
            addFileStamp(text, editor::FileUtils::pathToGenericUtf8(file.lexically_relative(root)), file);
        }
    }

    // The file's path as the cache names it, whatever the platform's separators
    std::string sceneCacheKey(const editor::SceneProject& sceneProject) {
        return editor::FileUtils::pathToGenericUtf8(sceneProject.filepath.lexically_normal());
    }

    std::vector<std::string> listedPrefabPaths(const editor::SceneProject& sceneProject) {
        std::vector<std::string> paths;
        for (const editor::PrefabSceneInfo& info : sceneProject.prefabs) {
            paths.push_back(editor::FileUtils::pathToGenericUtf8(info.prefabPath));
        }
        std::sort(paths.begin(), paths.end());
        return paths;
    }

}

fs::path editor::Project::shaderListCachePath() const {
    return getProjectInternalPath() / "shader-lists.yaml";
}

void editor::Project::loadShaderListCache() {
    if (shaderListCacheProject == getProjectPath()) {
        return;
    }
    shaderListWrite.wait();
    shaderListWrite.reset();
    shaderListCacheProject = getProjectPath();
    shaderListFailures.clear();
    shaderListCache.load(shaderListCachePath());
}

void editor::Project::writeShaderListCache() {
    if (shaderListWrite.isValid()) {
        if (!shaderListWrite.isReady()) {
            return;
        }
        // the next list tries a failed write again
        if (!shaderListWrite.get()) {
            shaderListCache.markDirty();
        }
        shaderListWrite.reset();
    }
    if (!shaderListCache.isDirty()) {
        return;
    }
    jobs::SubmitOptions options;
    options.name = "shader-list-cache";
    shaderListWrite = jobs::system().ioWriteChannel().submit(
        [file = shaderListCachePath(), entries = shaderListCache.takeEntriesToWrite()]() {
            // JASPER_SHADER_LIST_WRITE_HOLD_FILE: while that file exists, for a test to see the
            // reply come first (bounded: a test that died leaves it behind)
            if (const char* hold = std::getenv("JASPER_SHADER_LIST_WRITE_HOLD_FILE"); hold && *hold) {
                std::error_code ec;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
                while (fs::exists(hold, ec) && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            try {
                return ShaderListCache::write(file, entries);
            } catch (const std::exception&) {
                return false;
            }
        }, options);
}

std::string editor::Project::shaderListContext() const {
    std::string text;
    // another editor can derive other keys from the same content
    text += PlayerRuntimes::editorVersion() + '\n';
    addFileStamp(text, FileUtils::pathToGenericUtf8(FileUtils::getExecutablePath()), FileUtils::getExecutablePath());
    text += std::to_string(static_cast<int>(getMobilePbrTier())) + '\n';

    // the prefabs every scene can spawn start from these (collectAllPrefabs, which reads their files)
    std::vector<std::string> prefabs;
    for (const SceneProject& sceneProject : scenes) {
        const std::vector<std::string> listed = listedPrefabPaths(sceneProject);
        prefabs.insert(prefabs.end(), listed.begin(), listed.end());
        if (sceneProject.scene) {
            for (const fs::path& path : findScriptPrefabReferences(sceneProject.scene)) {
                prefabs.push_back(FileUtils::pathToGenericUtf8(path));
            }
        }
    }
    for (const fs::path& path : standalonePrefabs) {
        prefabs.push_back(FileUtils::pathToGenericUtf8(path));
    }
    std::sort(prefabs.begin(), prefabs.end());
    prefabs.erase(std::unique(prefabs.begin(), prefabs.end()), prefabs.end());
    for (const std::string& path : prefabs) {
        text += path + '\n';
    }

    for (const fs::path& file : findProjectFiles(readsIntoShaderKeys)) {
        addFileStamp(text, FileUtils::pathToGenericUtf8(file), getProjectPath() / file);
    }
    // the asset and Lua folders a project keeps outside itself
    const auto outsideProject = [this](const fs::path& dir) {
        const fs::path relative = dir.lexically_normal().lexically_relative(getProjectPath().lexically_normal());
        return relative.empty() || *relative.begin() == "..";
    };
    for (const fs::path& dir : {getAssetsPath(), getLuaPath()}) {
        if (outsideProject(dir)) addTreeStamps(text, dir, readsIntoShaderKeys);
    }
    // an FBX model draws from what the editor imported it into
    const std::string importPath = System::instance().getImportPath();
    if (!importPath.empty()) {
        addTreeStamps(text, FileUtils::pathFromUtf8(importPath), [](const std::string&) { return true; });
    }
    return DocumentFiles::sha1(text);
}

void editor::Project::forgetPrefabsLoadedSince(const std::set<fs::path>& loadedBefore) {
    for (auto it = prefabAssets.begin(); it != prefabAssets.end();) {
        if (!loadedBefore.count(it->first) && it->second.instances.empty() && !isStandalonePrefab(it->first)) {
            it = prefabAssets.erase(it);
        } else {
            ++it;
        }
    }
}

void editor::Project::setClosedScenePrefabs(SceneProject& sceneProject, const std::vector<std::string>& prefabs) const {
    sceneProject.prefabs.clear();
    for (const std::string& path : prefabs) {
        PrefabSceneInfo info;
        info.prefabPath = FileUtils::pathFromUtf8(path);
        info.functionName = Factory::prefabToFunctionName(info.prefabPath);
        info.guid = getPrefabGuid(info.prefabPath);
        sceneProject.prefabs.push_back(std::move(info));
    }
}

bool editor::Project::readClosedSceneShaderKeys(SceneProject& sceneProject, ShaderListCache::Entry& entry, bool& prefabsChanged, std::string& error) {
    fs::path fullPath = sceneProject.filepath;
    if (fullPath.is_relative()) {
        fullPath = getProjectPath() / fullPath;
    }

    std::set<fs::path> loadedBefore;
    for (const auto& [path, prefab] : prefabAssets) {
        loadedBefore.insert(path);
    }
    // a scene of its own, not sceneProject: nothing the editor holds of it changes, and an id no
    // other has, as a runtime copy of this scene in Play has its own instances of its prefabs
    SceneProject source;
    source.filepath = sceneProject.filepath;
    source.id = allocateSceneId();

    std::unique_ptr<SceneProject> probe;
    bool read = false;
    try {
        // what the decode cannot read lands here, as in any load, and goes: this is no scene to save
        LoadReport report;
        ScopedLoadReport scope(report);
        DocumentFile document = DocumentFiles::read(fullPath, DocumentKind::Scene);
        if (!document.root.IsMap() || !document.root["scene"]) {
            throw std::runtime_error("the file holds no scene");
        }
        entry.sha1 = document.sha1;
        probe.reset(createRuntimeCloneFromNode(&source, document.root));
        resolveScenePrefabInfos(*probe);
        resolveSceneAssetRefs(*probe, true);
        updateScenePrefabs(probe.get());

        // the keys read the prefabs of every scene, this one's as its file has them now
        const std::vector<std::string> before = listedPrefabPaths(sceneProject);
        entry.prefabs = listedPrefabPaths(*probe);
        prefabsChanged = before != entry.prefabs;
        if (prefabsChanged) {
            setClosedScenePrefabs(sceneProject, entry.prefabs);
        }
        collectSavedShaderKeys(probe.get(), entry.keys);
        read = true;
    } catch (const std::exception& e) {
        error = e.what();
    }

    if (probe) {
        deleteSceneProject(probe.get());
    }
    for (auto& [path, prefab] : prefabAssets) {
        prefab.instances.erase(source.id);
    }
    forgetPrefabsLoadedSince(loadedBefore);
    return read;
}

void editor::Project::rememberSavedShaderKeys(const SceneProject& sceneProject, const std::string& sha1) {
    if (sceneProject.filepath.empty() || sha1.empty()) {
        return;
    }
    fs::path fullPath = sceneProject.filepath;
    if (fullPath.is_relative()) {
        fullPath = getProjectPath() / fullPath;
    }
    std::error_code timeError;
    std::error_code sizeError;
    const fs::file_time_type time = fs::last_write_time(fullPath, timeError);
    const std::uintmax_t size = fs::file_size(fullPath, sizeError);
    if (timeError || sizeError) {
        return;
    }
    loadShaderListCache();
    ShaderListCache::Entry entry;
    entry.sha1 = sha1;
    entry.writeTime = shaderListStamp(time);
    entry.size = size;
    entry.context = shaderListContext();
    entry.keys = sceneProject.shaderKeys;
    entry.prefabs = listedPrefabPaths(sceneProject);
    shaderListCache.store(sceneCacheKey(sceneProject), std::move(entry));
}

editor::ShaderListStatus editor::Project::refreshClosedSceneShaderKeys(const ShaderListLimit& limit) {
    ShaderListStatus status;
    const auto start = std::chrono::steady_clock::now();
    loadShaderListCache();

    struct ClosedScene {
        SceneProject* scene = nullptr;
        std::string key;
        std::string sha1;
        std::int64_t writeTime = 0;
        std::uintmax_t size = 0;
    };
    std::vector<ClosedScene> closed;

    // First what the files say, without reading what has not changed: the prefabs a known file lists
    // are part of the context every scene is checked against
    for (SceneProject& sceneProject : scenes) {
        if (sceneProject.filepath.empty() || sceneProject.scene) {
            continue;
        }
        fs::path fullPath = sceneProject.filepath;
        if (fullPath.is_relative()) {
            fullPath = getProjectPath() / fullPath;
        }
        ClosedScene item;
        item.scene = &sceneProject;
        item.key = sceneCacheKey(sceneProject);

        const auto unreadable = [&](const std::string& error) {
            status.unreadableScenes.push_back({sceneProject.name, item.key, error});
        };
        std::error_code timeError;
        std::error_code sizeError;
        const fs::file_time_type time = fs::last_write_time(fullPath, timeError);
        const std::uintmax_t size = fs::file_size(fullPath, sizeError);
        if (timeError || sizeError) {
            unreadable("the file is missing");
            continue;
        }
        item.writeTime = shaderListStamp(time);
        item.size = size;

        const auto failure = shaderListFailures.find(item.key);
        if (failure != shaderListFailures.end() && failure->second.writeTime == item.writeTime && failure->second.size == size) {
            unreadable(failure->second.error);
            continue;
        }
        const ShaderListCache::Entry* entry = shaderListCache.find(item.key);
        const bool sameStamp = entry && entry->writeTime == item.writeTime && entry->size == size;
        item.sha1 = sameStamp ? entry->sha1 : DocumentFiles::fileSha1(fullPath);
        if (item.sha1.empty()) {
            unreadable("the file cannot be read");
            continue;
        }
        if (entry && entry->sha1 == item.sha1) {
            if (!sameStamp) {
                shaderListCache.updateStamp(item.key, item.writeTime, size);
            }
            if (listedPrefabPaths(sceneProject) != entry->prefabs) {
                setClosedScenePrefabs(sceneProject, entry->prefabs);
            }
        }
        closed.push_back(std::move(item));
    }

    const auto mayRead = [&](const ClosedScene& item) {
        if (status.scenesRead >= limit.maxScenes || item.size > limit.maxFileBytes) {
            return false;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        return limit.maxMillis < 0 || elapsed < limit.maxMillis;
    };

    std::string context = closed.empty() ? std::string() : shaderListContext();
    std::set<std::string> failed;
    // A scene read may list other prefabs than its file did when last known: then what was
    // checked against the old list is checked again. A scene's list depends only on its file, so this
    // ends once every changed one has been read.
    bool checkAgain = true;
    while (checkAgain) {
        checkAgain = false;
        status.pendingScenes.clear();
        for (ClosedScene& item : closed) {
            if (failed.count(item.key)) {
                continue;
            }
            const ShaderListCache::Entry* entry = shaderListCache.find(item.key);
            if (entry && entry->sha1 == item.sha1 && entry->context == context) {
                item.scene->shaderKeys = entry->keys;
                continue;
            }
            if (!mayRead(item)) {
                status.pendingScenes.push_back(item.scene->name);
                continue;
            }

            ShaderListCache::Entry fresh;
            bool prefabsChanged = false;
            std::string error;
            status.scenesRead++;
            if (!readClosedSceneShaderKeys(*item.scene, fresh, prefabsChanged, error)) {
                shaderListFailures[item.key] = {item.writeTime, item.size, error};
                failed.insert(item.key);
                status.unreadableScenes.push_back({item.scene->name, item.key, error});
                continue;
            }
            shaderListFailures.erase(item.key);
            if (prefabsChanged) {
                context = shaderListContext();
                checkAgain = true;
            }
            item.sha1 = fresh.sha1;
            fresh.writeTime = item.writeTime;
            fresh.size = item.size;
            fresh.context = context;
            item.scene->shaderKeys = fresh.keys;
            shaderListCache.store(item.key, std::move(fresh));
        }
    }

    // What the project lists and could not even load when it opened
    for (const auto& [position, path] : getUnresolvedScenes()) {
        std::error_code ec;
        const std::string shown = FileUtils::pathToGenericUtf8(path);
        status.unreadableScenes.push_back({FileUtils::pathToUtf8(path.stem()), shown,
                                           fs::exists(getProjectPath() / path, ec) ? "the file did not load when the project opened" : "the file is missing"});
    }

    if (status.complete()) {
        writeShaderListCache();
    }
    return status;
}

void editor::Project::collectBuildShaderKeys(std::set<ShaderKey>& keys, std::set<std::string>* deadForks) const {
    // the generated main.cpp registers every scene stack, so any of them can be loaded
    for (const SceneProject& sceneProject : scenes) {
        std::set<ShaderKey> sceneKeys = sceneProject.shaderKeys;
        // stored keys are only as new as the last save of that scene
        collectSceneShaderKeys(&sceneProject, sceneKeys);
        collectRequiredShaderKeys(&sceneProject, sceneKeys);
        for (ShaderKey key : sceneKeys) {
            keys.insert(ShaderPool::normalizeKey(key));
        }
    }
    dropDeadForkKeys(keys, deadForks);
}

std::set<ShaderKey> editor::Project::finishBuildShaderKeys(const std::set<ShaderKey>& sceneKeys, const std::set<ShaderKey>& additions,
                                                           const std::set<ShaderKey>& exclusions, std::set<std::string>* deadForks) const {
    std::set<ShaderKey> keys = sceneKeys;
    // what every build ships (the FLATTEN Canvas Group composite), whatever the scenes hold
    std::set<ShaderKey> required;
    ShaderVariantCollector::addBuildRequiredKeys(required);
    for (ShaderKey key : required) {
        keys.insert(ShaderPool::normalizeKey(key));
    }
    for (ShaderKey key : additions) {
        keys.insert(ShaderPool::normalizeKey(key));
    }
    // the shader warm-up list's variants (spec S32 R11): what Play used ships, even when no scene
    // records it (a material a script swaps in)
    if (getShaderCreationSettings().includeInBuilds) {
        std::ifstream file(getShaderWarmupPath(), std::ios::binary);
        if (file) {
            const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            std::vector<ShaderKey> warmup;
            // one that does not read is warned about where the export writes it
            if (ShaderWarmup::readShaderKeys(text, warmup)) {
                for (ShaderKey key : warmup) {
                    keys.insert(ShaderPool::normalizeKey(key));
                }
            }
        }
    }
    // a deleted fork's variants the additions or the warm-up list still name
    dropDeadForkKeys(keys, deadForks);
    for (ShaderKey key : exclusions) {
        keys.erase(ShaderPool::normalizeKey(key));
    }
    return keys;
}

std::set<ShaderKey> editor::Project::collectStandaloneShaderKeys() {
    // runPlayStartup read what was stale a scene at a time: this is what changed since
    const ShaderListStatus status = refreshClosedSceneShaderKeys();
    for (const UnreadableScene& scene : status.unreadableScenes) {
        Out::warning("Scene '%s' (%s) cannot be read for the shaders Play builds ahead: %s", scene.name.c_str(), scene.path.c_str(), scene.error.c_str());
    }
    const ShaderOverrides& overrides = getShaderOverrides();
    std::set<ShaderKey> sceneKeys;
    collectBuildShaderKeys(sceneKeys);
    return finishBuildShaderKeys(sceneKeys, overrides.additions, overrides.exclusions);
}
