// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Unity's Missing Prefab: prefab instances whose asset cannot be found keep their root and
// everything the scene stores for them, are saved unchanged, and are rebuilt when the asset
// comes back. The data path is described in editor/project/PREFABS.md.

#include "Project.h"
#include "component/PrefabComponent.h"
#include "Out.h"
#include "Stream.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectUtils.h"

#include <algorithm>
#include <functional>
#include <set>
#include <tuple>
#include <unordered_set>

using namespace jasperengine;

const editor::MissingPrefabInstance* editor::Project::getMissingPrefab(uint32_t sceneId, Entity entity) const {
    const SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || sceneProject->missingPrefabs.empty()) {
        return nullptr;
    }
    auto it = sceneProject->missingPrefabs.find(entity);
    if (it == sceneProject->missingPrefabs.end() || !sceneProject->scene->isEntityCreated(entity)) {
        return nullptr;
    }
    // A record outlives its root when the root is destroyed: it only counts for the
    // same reference on the same entity, while it is not an instance
    const PrefabComponent* component = sceneProject->scene->findComponent<PrefabComponent>(entity);
    if (!component) {
        return nullptr;
    }
    const MissingPrefabInstance& missing = it->second;
    if (!missing.guid.empty() ? component->guid != missing.guid : component->path != missing.path) {
        return nullptr;
    }
    if (const Prefab* prefab = getPrefab(component->path)) {
        const Prefab::Instance* instance = prefab->getInstance(sceneId, entity);
        if (instance && instance->rootEntity == entity) {
            return nullptr;
        }
    }
    return &missing;
}

bool editor::Project::isMissingPrefab(uint32_t sceneId, Entity entity) const {
    return getMissingPrefab(sceneId, entity) != nullptr;
}

void editor::Project::setMissingPrefab(SceneProject* sceneProject, Entity root, const YAML::Node& data, const PrefabMemberIds* memberIds) {
    if (!sceneProject || !sceneProject->scene) {
        return;
    }
    const PrefabComponent* component = sceneProject->scene->findComponent<PrefabComponent>(root);

    MissingPrefabInstance missing;
    if (component) {
        missing.guid = component->guid;
        missing.path = component->path;
    }
    missing.data = YAML::Node(YAML::NodeType::Map);
    if (data && data.IsMap()) {
        for (auto it = data.begin(); it != data.end(); ++it) {
            const std::string key = it->first.as<std::string>();
            if (!Stream::isGameObjectKey(key)) {
                missing.data[key] = YAML::Clone(it->second);
            }
        }
    }
    if (memberIds) {
        missing.memberIds = *memberIds;
    }

    auto existing = sceneProject->missingPrefabs.find(root);
    const bool known = existing != sceneProject->missingPrefabs.end() && existing->second.guid == missing.guid &&
        existing->second.path == missing.path;
    if (known && !memberIds) {
        // the ids of an earlier record (the conversion that made it) still apply
        missing.memberIds = std::move(existing->second.memberIds);
        missing.mainCamera = existing->second.mainCamera;
    }
    sceneProject->missingPrefabs[root] = std::move(missing);

    if (!known && !sceneProject->prefabPreview) {
        Out::warning("Missing Prefab \"%s\" (GUID %s) on GameObject '%s' in scene '%s': its instance data is kept and comes back with the prefab",
            component ? component->path.c_str() : "", component && !component->guid.empty() ? component->guid.c_str() : "none",
            sceneProject->scene->getEntityName(root).c_str(), sceneProject->name.c_str());
    }
    sceneProject->structureVersion++;
}

void editor::Project::clearMissingPrefab(uint32_t sceneId, Entity root) {
    if (SceneProject* sceneProject = getScene(sceneId)) {
        sceneProject->missingPrefabs.erase(root);
    }
}

void editor::Project::collectPrefabMemberIds(uint32_t sceneId, const Prefab::Instance& instance, PrefabMemberIds& ids, size_t depth) const {
    const SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || depth > 64) {
        return;
    }
    // Every level at each root: a variant's bases' members under prefabLevelKey(root, n)
    std::function<void(const std::filesystem::path&, const Prefab::Instance&, size_t, size_t)> collect =
            [&](const std::filesystem::path& path, const Prefab::Instance& current, size_t level, size_t nesting) {
        std::unordered_map<Entity, Entity>& members = ids[prefabLevelKey(current.rootEntity, level)];
        for (const Prefab::EntityMember& member : current.members) {
            members[member.registryEntity] = member.localEntity;
        }
        if (nesting > 64) {
            return;
        }
        for (const NestedInstanceLevel& nested : getNestedInstanceLevels(sceneProject, path, current)) {
            if (nested.instance) {
                collect(nested.path, *nested.instance, nested.variantBase ? level + 1 : 0, nesting + 1);
            }
        }
    };
    std::filesystem::path path;
    for (const auto& [candidate, prefab] : getPrefabs(sceneId)) {
        auto sceneIt = prefab->instances.find(sceneId);
        if (sceneIt != prefab->instances.end()) {
            for (const Prefab::Instance& other : sceneIt->second) {
                if (&other == &instance) {
                    path = candidate;
                }
            }
        }
    }
    collect(path, instance, 0, depth);
}

void editor::Project::convertPrefabInstancesToMissing(const std::filesystem::path& filepath) {
    Prefab* prefab = getPrefab(filepath);
    if (!prefab) {
        return;
    }

    std::vector<std::pair<uint32_t, Entity>> roots;
    for (const auto& [sceneId, instances] : prefab->instances) {
        for (const Prefab::Instance& instance : instances) {
            roots.emplace_back(sceneId, instance.rootEntity);
        }
    }

    std::unordered_set<uint32_t> touchedScenes;
    // An instance nested in a live one goes missing with that instance built again once the
    // asset is gone: what the prefabs holding it add to it and override stays theirs
    // (PREFABS.md 17). Their nodes are taken now, while the asset still answers.
    std::vector<std::tuple<uint32_t, Entity, YAML::Node>> outerRoots;
    std::set<std::pair<uint32_t, Entity>> nestedRoots;
    for (const auto& [sceneId, root] : roots) {
        SceneProject* sceneProject = getScene(sceneId);
        if (!sceneProject || !sceneProject->scene) {
            continue;
        }
        Entity outer = getOutermostPrefabRoots(sceneId, {root})[0];
        // the base of a variant instance (its root names the variant): the variant's
        // instance goes Missing as a whole when built again (PREFABS.md 19)
        std::filesystem::path outermostPath;
        const bool variantBase = findInstanceRootedAt(sceneProject, root, &outermostPath) && outermostPath != filepath;
        if (outer == NULL_ENTITY || (outer == root && !variantBase)) {
            continue;
        }
        nestedRoots.insert({sceneId, root});
        const bool listed = std::any_of(outerRoots.begin(), outerRoots.end(), [&](const auto& entry) {
            return std::get<0>(entry) == sceneId && std::get<1>(entry) == outer;
        });
        if (!listed) {
            outerRoots.emplace_back(sceneId, outer, Stream::encodeEntity(outer, sceneProject->scene, this, sceneProject));
        }
    }
    for (const auto& [sceneId, root] : roots) {
        SceneProject* sceneProject = getScene(sceneId);
        prefab = getPrefab(filepath);
        const Prefab::Instance* instance = prefab ? prefab->getInstance(sceneId, root) : nullptr;
        if (!sceneProject || !sceneProject->scene || !instance || instance->rootEntity != root) {
            continue;
        }
        if (nestedRoots.count({sceneId, root})) {
            continue; // nested: below
        }

        // Everything the scene file would hold for the instance, as it would write it
        YAML::Node data = Stream::encodePrefabInstanceData(root, sceneProject->scene, this, sceneProject, prefab);
        PrefabMemberIds memberIds;
        collectPrefabMemberIds(sceneId, *instance, memberIds);
        std::vector<Entity> members;
        members.reserve(instance->members.size());
        for (const Prefab::EntityMember& member : instance->members) {
            members.push_back(member.localEntity);
        }
        Entity mainCamera = NULL_ENTITY;
        for (const auto& [instanceRoot, ids] : memberIds) {
            for (const auto& [registryEntity, localEntity] : ids) {
                if (localEntity == sceneProject->mainCamera && localEntity != NULL_ENTITY) {
                    mainCamera = localEntity;
                }
            }
        }

        unimportPrefab(sceneId, filepath, root, members, true, true);

        setMissingPrefab(sceneProject, root, data, &memberIds);
        sceneProject->missingPrefabs[root].mainCamera = mainCamera;
        touchedScenes.insert(sceneId);
    }

    for (uint32_t sceneId : touchedScenes) {
        if (SceneProject* sceneProject = getScene(sceneId)) {
            sceneProject->isModified = true;
            sceneProject->needUpdateRender = true;
            sceneProject->structureVersion++;
        }
    }

    // The asset is gone
    auto it = prefabAssets.find(filepath);
    if (it != prefabAssets.end()) {
        if (it->second.registry) {
            it->second.registry->clear();
        }
        prefabAssets.erase(it);
    }
    for (const auto& [sceneId, outer, node] : outerRoots) {
        if (SceneProject* sceneProject = getScene(sceneId)) {
            reimportPrefabInstance(sceneProject, outer, node);
            sceneProject->isModified = true;
            sceneProject->needUpdateRender = true;
            sceneProject->structureVersion++;
        }
    }
    for (SceneProject& sceneProject : scenes) {
        if (sceneProject.scene) {
            updateScenePrefabs(&sceneProject);
        }
    }
}

size_t editor::Project::relinkMissingPrefabs() {
    size_t relinked = 0;
    for (SceneProject& sceneProject : scenes) {
        if (!sceneProject.scene || sceneProject.missingPrefabs.empty() || sceneProject.playState != ScenePlayState::STOPPED) {
            continue;
        }
        Scene* scene = sceneProject.scene;

        std::vector<Entity> roots;
        for (const auto& [root, missing] : sceneProject.missingPrefabs) {
            roots.push_back(root);
        }

        bool changed = false;
        // A Missing Prefab nested in a live instance comes back with the instance holding it
        // built again: the GameObjects the prefabs above add to it go below their nested
        // parents, their references into it resolve (stripped objects, PREFABS.md 17)
        std::vector<Entity> outerRoots;
        for (Entity root : roots) {
            if (!getMissingPrefab(sceneProject.id, root)) {
                continue;
            }
            const PrefabComponent& reference = scene->getComponent<PrefabComponent>(root);
            if (resolvePrefabPath(reference.guid, reference.path).empty()) {
                continue;
            }
            const Entity outer = getOutermostPrefabRoots(sceneProject.id, {root})[0];
            if (outer != NULL_ENTITY && outer != root && std::find(outerRoots.begin(), outerRoots.end(), outer) == outerRoots.end()) {
                outerRoots.push_back(outer);
            }
        }
        for (Entity outer : outerRoots) {
            if (reimportPrefabInstance(&sceneProject, outer, YAML::Node())) {
                changed = true;
            }
        }
        for (Entity root : roots) {
            if (!outerRoots.empty() && scene->isEntityCreated(root) && isPrefabInstanceRoot(sceneProject.id, root)) {
                sceneProject.missingPrefabs.erase(root); // relinked with its holder
                relinked++;
                continue;
            }
            if (!getMissingPrefab(sceneProject.id, root)) {
                sceneProject.missingPrefabs.erase(root); // its root is gone or changed
                continue;
            }
            PrefabComponent reference = scene->getComponent<PrefabComponent>(root);
            if (resolvePrefabPath(reference.guid, reference.path).empty()) {
                continue;
            }

            MissingPrefabInstance missing = std::move(sceneProject.missingPrefabs[root]);
            sceneProject.missingPrefabs.erase(root);

            std::unordered_map<Entity, Entity> entityRemap;
            const PrefabInstanceData data = Stream::decodePrefabInstanceData(missing.data);
            // a nested root comes back with what the prefabs holding it override of it
            importPrefab(&sceneProject, &sceneProject.entities, reference.path, root, false,
                &data, &entityRemap, missing.memberIds.empty() ? nullptr : &missing.memberIds,
                getPrefabAssetLayers(sceneProject.id, root));

            const PrefabComponent* component = scene->findComponent<PrefabComponent>(root);
            const Prefab* prefab = component ? getPrefab(component->path) : nullptr;
            const Prefab::Instance* instance = prefab ? prefab->getInstance(sceneProject.id, root) : nullptr;
            if (!instance || instance->rootEntity != root) {
                // Still unreadable: keep what it had, the keys a scene file gave it included
                sceneProject.missingPrefabs[root] = std::move(missing);
                continue;
            }

            if (missing.mainCamera != NULL_ENTITY && sceneProject.mainCamera == NULL_ENTITY &&
                scene->isEntityCreated(missing.mainCamera)) {
                sceneProject.mainCamera = missing.mainCamera;
            }
            Out::info("Prefab \"%s\" found again: instance '%s' in scene '%s' relinked",
                component->path.c_str(), scene->getEntityName(root).c_str(), sceneProject.name.c_str());
            changed = true;
            relinked++;
        }

        if (changed) {
            ProjectUtils::sortEntitiesByTransformOrder(scene, sceneProject.entities);
            CameraTextureLink::resolve(scene);
            updateScenePrefabs(&sceneProject);
            sceneProject.isModified = true;
            sceneProject.needUpdateRender = true;
            sceneProject.structureVersion++;
        }
    }
    return relinked;
}
