// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Prefabs at runtime: which prefabs the build holds (collectAllPrefabs, the same list and
// order for Play and the export), the prefabs scripts reference (Prefab script
// properties), and Play's PrefabManager factories, which copy a template imported once.

#include "Project.h"
#include "component/Transform.h"
#include "component/PrefabComponent.h"
#include "component/ScriptComponent.h"
#include "DocumentFile.h"
#include "Factory.h"
#include "Out.h"
#include "PrefabManager.h"
#include "ObjectManager.h"
#include "registry/EntityCloner.h"

#include <algorithm>
#include <unordered_set>

using namespace jasperengine;

namespace {

// Where a prefab reference written as {guid, path} is now, without touching the asset
// database (resolvePrefabPath does, for files made outside the editor; the build list is
// read from const code)
fs::path resolveReference(const editor::AssetDatabase& database, const fs::path& projectPath, const std::string& guid, const fs::path& hint) {
    if (database.isOpen() && !guid.empty()) {
        return database.getPath(guid);
    }
    if (database.isOpen() && !hint.empty()) {
        fs::path path = database.getPath(editor::AssetDatabase::legacyGuid(hint));
        if (!path.empty()) {
            return path;
        }
    }
    std::error_code ec;
    if (!hint.empty() && fs::is_regular_file(projectPath / hint, ec)) {
        return hint;
    }
    return fs::path();
}

}

std::vector<fs::path> editor::Project::findScriptPrefabReferences(EntityRegistry* registry) const {
    std::vector<fs::path> found;
    if (!registry) {
        return found;
    }
    auto scripts = registry->getComponentArray<ScriptComponent>();
    for (size_t i = 0; i < scripts->size(); i++) {
        for (const ScriptEntry& entry : scripts->getComponentFromIndex(i).scripts) {
            for (const ScriptProperty& property : entry.properties) {
                if (property.type != ScriptPropertyType::Prefab) {
                    continue;
                }
                const PrefabAsset* prefab = std::get_if<PrefabAsset>(&property.value);
                if (!prefab || prefab->isEmpty()) {
                    continue;
                }
                fs::path path = resolveReference(assetDatabase, getProjectPath(), prefab->getGuid(), normalizeToProjectRelative(prefab->getPath()));
                if (!path.empty() && std::find(found.begin(), found.end(), path) == found.end()) {
                    found.push_back(path);
                }
            }
        }
    }
    return found;
}

void editor::Project::addScriptPrefabReferences(SceneProject* sceneProject) {
    if (!sceneProject || !sceneProject->scene) {
        return;
    }
    for (const fs::path& path : findScriptPrefabReferences(sceneProject->scene)) {
        const bool listed = std::any_of(sceneProject->prefabs.begin(), sceneProject->prefabs.end(),
            [&path](const PrefabSceneInfo& info) { return info.prefabPath == path; });
        if (!listed) {
            PrefabSceneInfo info;
            info.prefabPath = path;
            info.functionName = Factory::prefabToFunctionName(path);
            info.guid = getPrefabGuid(path);
            sceneProject->prefabs.push_back(std::move(info));
        }
    }
}

std::vector<editor::PrefabSceneInfo> editor::Project::collectAllPrefabs() const {
    std::vector<fs::path> pending;
    std::unordered_set<std::string> seen;
    auto add = [&](const fs::path& path) {
        if (path.empty() || !seen.insert(path.generic_string()).second) {
            return;
        }
        std::error_code ec;
        if (!fs::is_regular_file(getProjectPath() / path, ec)) {
            return; // a Missing Prefab: nothing to build
        }
        pending.push_back(path);
    };

    // What each scene lists (its instances, its scripts' references; closed scenes as saved)
    for (const auto& sceneProject : scenes) {
        for (const auto& prefab : sceneProject.prefabs) {
            add(prefab.prefabPath);
        }
        // an open scene's scripts as they are now, saved or not
        if (sceneProject.scene) {
            for (const fs::path& path : findScriptPrefabReferences(sceneProject.scene)) {
                add(path);
            }
        }
    }
    for (const fs::path& prefabPath : standalonePrefabs) {
        add(prefabPath);
    }

    // ...and what those prefabs nest or their scripts reference, to any depth
    std::vector<fs::path> all;
    while (!pending.empty()) {
        const fs::path path = pending.back();
        pending.pop_back();
        all.push_back(path);

        const Prefab* prefab = getPrefab(path);
        EntityRegistry* registry = prefab ? prefab->registry.get() : nullptr;
        Prefab loaded;
        if (!registry) {
            try {
                loaded.loadRegistry(DocumentFiles::read(getProjectPath() / path, DocumentKind::Prefab).root, path.stem().string());
                registry = loaded.registry.get();
            } catch (const std::exception& e) {
                Out::error("Failed to read prefab %s: %s", path.string().c_str(), e.what());
                continue;
            }
        }
        if (!registry) {
            continue;
        }
        auto nested = registry->getComponentArray<PrefabComponent>();
        for (size_t i = 0; i < nested->size(); i++) {
            const PrefabComponent& component = nested->getComponentFromIndex(i);
            add(resolveReference(assetDatabase, getProjectPath(), component.guid, normalizeToProjectRelative(component.path)));
        }
        for (const fs::path& referenced : findScriptPrefabReferences(registry)) {
            add(referenced);
        }
    }

    // By path: the same order in Play and in the export, whatever loaded first
    std::sort(all.begin(), all.end(), [](const fs::path& a, const fs::path& b) {
        return a.generic_string() < b.generic_string();
    });

    std::vector<PrefabSceneInfo> result;
    result.reserve(all.size());
    for (const fs::path& path : all) {
        PrefabSceneInfo info;
        info.prefabPath = path;
        info.functionName = Factory::prefabToFunctionName(path);
        info.guid = getPrefabGuid(path);
        result.push_back(std::move(info));
    }
    return result;
}

void editor::Project::appendPrefabCppScripts(std::vector<SceneScriptSource>& scripts, std::unordered_set<std::string>& uniquePaths) const {
    for (const PrefabSceneInfo& info : collectAllPrefabs()) {
        const Prefab* prefab = getPrefab(info.prefabPath);
        EntityRegistry* registry = prefab ? prefab->registry.get() : nullptr;
        Prefab loaded;
        if (!registry) {
            try {
                loaded.loadRegistry(DocumentFiles::read(getProjectPath() / info.prefabPath, DocumentKind::Prefab).root, info.prefabPath.stem().string());
                registry = loaded.registry.get();
            } catch (const std::exception&) {
                continue; // collectAllPrefabs reported it
            }
        }
        if (!registry) {
            continue;
        }
        auto components = registry->getComponentArray<ScriptComponent>();
        for (size_t i = 0; i < components->size(); i++) {
            for (const ScriptEntry& entry : components->getComponentFromIndex(i).scripts) {
                // disabled scripts too: they exist at runtime and can be enabled (Unity)
                if (entry.type == ScriptType::LUA || entry.path.empty()) {
                    continue;
                }
                fs::path sourcePath = entry.path;
                if (sourcePath.is_relative()) {
                    sourcePath = getProjectPath() / sourcePath;
                }
                std::error_code ec;
                if (!fs::exists(sourcePath, ec) || !uniquePaths.insert(fs::path(entry.path).lexically_normal().generic_string()).second) {
                    continue;
                }
                SceneScriptSource script;
                script.path = entry.path;
                script.headerPath = entry.headerPath;
                script.className = entry.className;
                fs::path headerPath = script.headerPath;
                if (headerPath.is_relative()) {
                    headerPath = getProjectPath() / headerPath;
                }
                if (!script.headerPath.empty() && fs::exists(headerPath, ec)) {
                    for (const ScriptProperty& property : ScriptParser::parseScriptProperties(headerPath)) {
                        ScriptPropertyInfo propertyInfo;
                        propertyInfo.name = property.name;
                        propertyInfo.isPtr = property.type == ScriptPropertyType::EntityReference || !property.ptrTypeName.empty();
                        propertyInfo.ptrTypeName = property.ptrTypeName;
                        script.properties.push_back(std::move(propertyInfo));
                    }
                }
                scripts.push_back(std::move(script));
            }
        }
    }
}

void editor::Project::loadBuildPrefabs() {
    for (const PrefabSceneInfo& info : collectAllPrefabs()) {
        if (!loadPrefabAsset(info.prefabPath)) {
            Out::error("Prefab %s could not be read: it is left out of the build", info.prefabPath.string().c_str());
        }
    }
}

// ---- Play ----

const editor::Project::PlayPrefabTemplate* editor::Project::getPlayPrefabTemplate(const fs::path& prefabPath) {
    auto found = playPrefabTemplates.find(prefabPath);
    if (found != playPrefabTemplates.end()) {
        return &found->second;
    }

    if (!playTemplateProject) {
        // a scene no window shows nor the Engine runs: its instances are never edited,
        // its scripts never start
        uint32_t id = 0xFFFFFFF0u;
        while (getScene(id)) {
            id--;
        }
        playTemplateProject = std::make_unique<SceneProject>();
        playTemplateProject->id = id;
        playTemplateProject->name = "Prefab templates";
        playTemplateProject->sceneType = SceneType::SCENE_3D;
        playTemplateProject->opened = false;
        playTemplateProject->scene = new Scene();
    }
    SceneProject* templates = playTemplateProject.get();
    Scene* scene = templates->scene;

    const Entity lastBefore = scene->getLastEntity();
    const Entity root = scene->createEntity();
    PrefabComponent component;
    component.name = prefabPath.stem().string();
    component.path = prefabPath.generic_string();
    component.guid = getPrefabGuid(prefabPath);
    scene->addComponent<PrefabComponent>(root, component);
    templates->entities.push_back(root);

    bool imported = true;
    try {
        importPrefab(templates, &templates->entities, prefabPath, root, false);
    } catch (const std::exception& e) {
        Out::error("Prefab %s could not be instantiated: %s", prefabPath.string().c_str(), e.what());
        imported = false;
    }
    // not an instance anyone edits, applies to or reverts
    for (auto& [path, prefab] : prefabAssets) {
        prefab.instances.erase(templates->id);
    }
    const bool missing = templates->missingPrefabs.erase(root) != 0;
    if (!imported || missing || !scene->isEntityCreated(root)) {
        Out::error("Prefab %s could not be instantiated", prefabPath.string().c_str());
        return nullptr;
    }

    PlayPrefabTemplate made;
    made.entities = EntityCloner::collectHierarchy(scene, root);
    // GameObjects without a Transform next to the root that are not actions on it
    std::unordered_set<Entity> listed(made.entities.begin(), made.entities.end());
    const Entity lastAfter = scene->getLastEntity();
    for (Entity e = lastBefore + 1; e <= lastAfter && e != NULL_ENTITY; e++) {
        if (scene->isEntityCreated(e) && !listed.count(e) && !scene->findComponent<Transform>(e)) {
            made.entities.push_back(e);
        }
    }
    return &playPrefabTemplates.emplace(prefabPath, std::move(made)).first->second;
}

void editor::Project::clearPlayPrefabTemplates() {
    playPrefabTemplates.clear();
    if (playTemplateProject) {
        const uint32_t id = playTemplateProject->id;
        for (auto& [path, prefab] : prefabAssets) {
            prefab.instances.erase(id);
        }
        delete playTemplateProject->scene;
        playTemplateProject.reset();
    }
}

void editor::Project::registerPrefabManager() {
    PrefabManager::clearAll();
    ObjectManager::clearAll();
    clearPlayPrefabTemplates();

    // A scene GameObject a script destroys leaves what the editor shows (Stop brings it back)
    ObjectManager::setDestroyListener([this](Scene* scene, const std::vector<Entity>& destroyed) {
        SceneProject* sceneProject = findSceneProjectByScene(scene);
        if (!sceneProject) {
            return;
        }
        const std::unordered_set<Entity> gone(destroyed.begin(), destroyed.end());
        auto drop = [&gone](std::vector<Entity>& list) {
            const size_t before = list.size();
            list.erase(std::remove_if(list.begin(), list.end(), [&gone](Entity entity) { return gone.count(entity) != 0; }), list.end());
            return list.size() != before;
        };
        // both lists lose the destroyed entities
        const bool entitiesChanged = drop(sceneProject->entities);
        const bool selectionChanged = drop(sceneProject->selectedEntities);
        if (entitiesChanged || selectionChanged) {
            sceneProject->structureVersion++;
        }
        sceneProject->needUpdateRender = true;
    });
    // ...and one DontDestroyOnLoad / MoveGameObjectToScene moved goes along
    ObjectManager::setMoveListener([this](Scene* from, Scene* to, const std::unordered_map<Entity, Entity>& moved) {
        onPlayObjectsMoved(from, to, moved);
    });

    // The export's list and order (Generator::configure gets the same collectAllPrefabs),
    // so a prefab id is the same in Play and in the exported game
    const std::vector<PrefabSceneInfo> prefabs = collectAllPrefabs();
    for (size_t i = 0; i < prefabs.size(); i++) {
        const fs::path prefabPath = prefabs[i].prefabPath;
        fs::path name = prefabPath;
        name.replace_extension();

        // The bare root PrefabManager made becomes a copy of the prefab's root GameObject,
        // its other GameObjects copies below it: what the export's build_prefab does
        PrefabManager::registerPrefab((uint32_t)(i + 1), name.generic_string(), prefabs[i].guid,
            [this, prefabPath](Scene* scene, Entity root) -> bool {
                const PlayPrefabTemplate* made = getPlayPrefabTemplate(prefabPath);
                if (!made || made->entities.empty() || !playTemplateProject) {
                    return false;
                }
                return !EntityCloner::clone(playTemplateProject->scene, made->entities, scene, root).empty();
            });
    }
}
