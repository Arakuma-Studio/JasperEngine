// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Prefab instantiation: importing/unimporting instances into scenes, instantiating
// removed branches back, prefab order placement, and the Play-mode prefab factories.

#include "Project.h"
#include "Factory.h"
#include "Out.h"
#include "Stream.h"
#include "PrefabManager.h"
#include "command/type/DeleteEntityCmd.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectUtils.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;

void editor::Project::updateScenePrefabs(SceneProject* sceneProject) {
    if (!sceneProject) {
        return;
    }

    sceneProject->prefabs.clear();

    for (const auto& [prefabPath, prefab] : prefabAssets) {
        if (prefab.instances.find(sceneProject->id) != prefab.instances.end()) {
            PrefabSceneInfo info;
            info.prefabPath = prefabPath;
            info.functionName = Factory::prefabToFunctionName(prefabPath);
            sceneProject->prefabs.push_back(std::move(info));
        }
    }
}

std::vector<editor::PrefabSceneInfo> editor::Project::collectAllPrefabs() const {
    std::unordered_set<std::string> uniquePaths;
    std::vector<PrefabSceneInfo> result;

    for (const auto& sceneProject : scenes) {
        for (const auto& prefab : sceneProject.prefabs) {
            std::string pathKey = prefab.prefabPath.generic_string();
            if (!uniquePaths.insert(pathKey).second) {
                continue;
            }

            fs::path fullPath = prefab.prefabPath;
            if (fullPath.is_relative()) {
                fullPath = getProjectPath() / fullPath;
            }
            if (!fs::exists(fullPath)) {
                continue;
            }

            result.push_back(prefab);
        }
    }

    // Standalone prefabs have no scene recording them, and only the loaded ones get a source
    for (const fs::path& prefabPath : standalonePrefabs) {
        const Prefab* prefab = getPrefab(prefabPath);
        if (!prefab || !prefab->registry || !uniquePaths.insert(prefabPath.generic_string()).second) {
            continue;
        }

        PrefabSceneInfo info;
        info.prefabPath = prefabPath;
        info.functionName = Factory::prefabToFunctionName(prefabPath);
        result.push_back(std::move(info));
    }

    return result;
}

std::vector<Entity> editor::Project::importPrefab(SceneProject* sceneProject, std::vector<Entity>* entities, const std::filesystem::path& filepath, Entity rootEntity, bool needSaveScene, const YAML::Node& prefabOverrides, const YAML::Node& prefabLocalEntities, std::unordered_map<Entity, Entity>* entityRemap, const YAML::Node& prefabRemovals) {
    if (!filepath.is_relative()) {
        Out::error("Prefab filepath must be relative: %s", filepath.string().c_str());
        return {};
    }

    // Cycle detection for nested prefabs
    static thread_local std::unordered_set<std::string> loadingPaths;
    std::string pathStr = filepath.string();
    if (loadingPaths.count(pathStr)) {
        Out::error("Circular prefab reference detected: %s", pathStr.c_str());
        return {};
    }
    loadingPaths.insert(pathStr);
    struct LoadingPathGuard {
        std::unordered_set<std::string>& paths;
        const std::string& path;
        ~LoadingPathGuard() { paths.erase(path); }
    } loadingGuard{loadingPaths, pathStr};

    auto it = prefabAssets.find(filepath);

    if (it == prefabAssets.end()) {
        Prefab newGroup;
        newGroup.registry = std::make_unique<EntityRegistry>();
        newGroup.isModified = false;

        auto [newIt, inserted] = prefabAssets.emplace(filepath, std::move(newGroup));
        it = newIt;
    }

    auto& prefab = it->second;

    Prefab::Instance newInstance;
    newInstance.instanceId = prefab.nextInstanceId++;

    YAML::Node node;
    if (prefab.isModified) {
        YAML::Node prefabNode;
        prefabNode["type"] = "Prefab";
        YAML::Node membersNode(YAML::NodeType::Sequence);
        if (!prefab.registryEntities.empty()) {
            std::vector<Entity> topLevelEntities = getTopLevelEntities(prefab.registry.get(), prefab.registryEntities);
            for (Entity entity : topLevelEntities) {
                membersNode.push_back(Stream::encodeEntity(entity, prefab.registry.get(), this));
            }
        }
        prefabNode["members"] = membersNode;
        node = prefabNode;
    } else {
        try {
            std::filesystem::path fullPrefabPath = getProjectPath() / filepath;
            node = loadPrefabFile(fullPrefabPath);
            prefab.registry->clear();
            prefab.registryEntities.clear();
            Stream::decodeEntitySelection(node, prefab.registry.get(), &prefab.registryEntities);
        } catch (const std::exception& e) {
            Out::error("Failed to load prefab file: %s", e.what());
            return {};
        }
    }

    Scene* scene = sceneProject->scene;

    // Decode prefab entities into the scene
    std::vector<Entity> newEntities = Stream::decodeEntitySelection(node, scene, entities);

    // Reparent top-level transformed prefab entities under root
    bool hasTopLevelTransform = scene->getSignature(rootEntity).test(scene->getComponentId<Transform>());
    if (hasTopLevelTransform) {
        std::vector<Entity> sceneTopLevelEntities = getTopLevelEntities(scene, newEntities);
        for (Entity topLevelEntity : sceneTopLevelEntities) {
            if (scene->findComponent<Transform>(topLevelEntity)) {
                scene->addEntityChild(rootEntity, topLevelEntity, false);
            }
        }
    }

    newInstance.rootEntity = rootEntity;

    // Removed GameObject overrides: this instance's own, and the nested instances' by the
    // registry entity of their root in this prefab
    std::map<std::string, YAML::Node> nestedRemovalsMap;
    if (prefabRemovals && prefabRemovals.IsSequence()) {
        for (const auto& entry : prefabRemovals) {
            if (!entry.IsMap()) continue;
            if (entry["prefabPath"]) {
                if (!entry["prefabRootRegistryEntity"]) continue;
                std::string key = entry["prefabPath"].as<std::string>() + "_" +
                    std::to_string(entry["prefabRootRegistryEntity"].as<Entity>());
                if (entry["nested"]) {
                    nestedRemovalsMap[key].push_back(entry["nested"]);
                } else if (entry["registryEntity"]) {
                    YAML::Node inner;
                    inner["registryEntity"] = entry["registryEntity"];
                    nestedRemovalsMap[key].push_back(inner);
                }
            } else if (entry["registryEntity"]) {
                Entity registryEntity = entry["registryEntity"].as<Entity>();
                // An entry whose GameObject left the Prefab since has nothing to remove
                if (prefab.registry->isEntityCreated(registryEntity)) {
                    newInstance.removed.emplace(registryEntity, std::vector<Prefab::EntityMember>());
                }
            }
        }
    }

    std::vector<Entity> regEntities = prefab.registryEntities;
    if (newEntities.size() == regEntities.size()) {
        // Decoded parents come before their children, so one pass finds what sits below a
        // removed GameObject: the removed ones are destroyed before any reference is mapped,
        // leaving references to them None
        std::unordered_set<Entity> absentLocals;
        if (!newInstance.removed.empty()) {
            for (size_t i = 0; i < newEntities.size(); i++) {
                const Transform* transform = scene->findComponent<Transform>(newEntities[i]);
                if (newInstance.removed.count(regEntities[i]) || (transform && absentLocals.count(transform->parent))) {
                    absentLocals.insert(newEntities[i]);
                }
            }
        }
        if (!absentLocals.empty()) {
            std::vector<Entity> presentEntities;
            std::vector<Entity> presentRegistry;
            presentEntities.reserve(newEntities.size() - absentLocals.size());
            presentRegistry.reserve(newEntities.size() - absentLocals.size());
            for (size_t i = 0; i < newEntities.size(); i++) {
                if (absentLocals.count(newEntities[i])) {
                    // Ownership callbacks must not reach a present GameObject
                    if (ActionComponent* action = scene->findComponent<ActionComponent>(newEntities[i])) {
                        action->ownedTarget = false;
                    }
                    if (AnimationComponent* animation = scene->findComponent<AnimationComponent>(newEntities[i])) {
                        animation->ownedActions = false;
                    }
                } else {
                    presentEntities.push_back(newEntities[i]);
                    presentRegistry.push_back(regEntities[i]);
                }
            }
            for (size_t i = newEntities.size(); i-- > 0;) {
                if (absentLocals.count(newEntities[i]) && scene->isEntityCreated(newEntities[i])) {
                    scene->destroyEntity(newEntities[i]);
                }
            }
            if (entities) {
                entities->erase(std::remove_if(entities->begin(), entities->end(),
                    [&absentLocals](Entity e) { return absentLocals.count(e) != 0; }), entities->end());
            }
            newEntities = std::move(presentEntities);
            regEntities = std::move(presentRegistry);
        }

        std::unordered_map<Entity, Entity> registryToLocal;
        for (size_t i = 0; i < newEntities.size(); i++) {
            newInstance.members.push_back({newEntities[i], regEntities[i]});
            registryToLocal[regEntities[i]] = newEntities[i];
        }
        remapEntityProperties(scene, newEntities, registryToLocal);
    } else {
        Out::error("importPrefab(%s): entity count mismatch: newEntities=%zu, regEntities=%zu",
            filepath.string().c_str(), newEntities.size(), regEntities.size());
        // Without a member mapping every ref would keep a registry ID; store None
        // rather than letting it resolve to an arbitrary entity in this scene
        remapEntityProperties(scene, newEntities, {});
    }

    // Remove decoded children of nested prefab roots
    // (they will be recreated by the recursive import below)
    std::unordered_set<Entity> nestedPrefabRoots;
    for (Entity ent : newEntities) {
        if (scene->findComponent<PrefabComponent>(ent)) {
            nestedPrefabRoots.insert(ent);
        }
    }
    if (!nestedPrefabRoots.empty()) {
        std::unordered_set<Entity> entitiesToRemove;
        for (Entity ent : newEntities) {
            if (nestedPrefabRoots.count(ent)) continue;
            Transform* tf = scene->findComponent<Transform>(ent);
            if (!tf) continue;
            Entity ancestor = tf->parent;
            while (ancestor != NULL_ENTITY) {
                if (nestedPrefabRoots.count(ancestor)) {
                    entitiesToRemove.insert(ent);
                    break;
                }
                Transform* parentTf = scene->findComponent<Transform>(ancestor);
                ancestor = parentTf ? parentTf->parent : NULL_ENTITY;
            }
        }
        for (Entity ent : entitiesToRemove) {
            scene->destroyEntity(ent);
            if (entities) {
                entities->erase(std::remove(entities->begin(), entities->end(), ent), entities->end());
            }
        }
        newEntities.erase(
            std::remove_if(newEntities.begin(), newEntities.end(),
                [&entitiesToRemove](Entity e) { return entitiesToRemove.count(e); }),
            newEntities.end());
        newInstance.members.erase(
            std::remove_if(newInstance.members.begin(), newInstance.members.end(),
                [&entitiesToRemove](const Prefab::EntityMember& m) { return entitiesToRemove.count(m.localEntity); }),
            newInstance.members.end());
    }

    // Collect nested overrides/localEntities (entries with prefabPath)
    std::map<std::string, YAML::Node> nestedOverridesMap;
    std::map<std::string, YAML::Node> nestedLocalEntitiesMap;

    if (prefabOverrides && prefabOverrides.IsSequence()) {
        for (const auto& entry : prefabOverrides) {
            if (entry["prefabPath"]) {
                std::string bp = entry["prefabPath"].as<std::string>();
                if (entry["prefabRootRegistryEntity"]) {
                    bp += "_" + std::to_string(entry["prefabRootRegistryEntity"].as<Entity>());
                }
                YAML::Node cleanEntry;
                cleanEntry["registryEntity"] = entry["registryEntity"];
                if (entry["components"]) cleanEntry["components"] = entry["components"];
                nestedOverridesMap[bp].push_back(cleanEntry);
            }
        }
    }

    if (prefabLocalEntities && prefabLocalEntities.IsSequence()) {
        for (const auto& entry : prefabLocalEntities) {
            if (entry["prefabPath"]) {
                std::string bp = entry["prefabPath"].as<std::string>();
                if (entry["prefabRootRegistryEntity"]) {
                    bp += "_" + std::to_string(entry["prefabRootRegistryEntity"].as<Entity>());
                }
                YAML::Node cleanEntry = YAML::Clone(entry);
                cleanEntry.remove("prefabPath");
                cleanEntry.remove("prefabRootRegistryEntity");
                nestedLocalEntitiesMap[bp].push_back(cleanEntry);
            }
        }
    }

    // Apply component overrides keyed by registryEntity (skip nested entries)
    if (prefabOverrides && prefabOverrides.IsSequence()) {
        for (const auto& entry : prefabOverrides) {
            if (entry["prefabPath"]) continue;
            if (!entry["registryEntity"]) continue;
            Entity regEntity = entry["registryEntity"].as<Entity>();

            // Find the local entity for this registryEntity
            Entity localEntity = NULL_ENTITY;
            for (const auto& member : newInstance.members) {
                if (member.registryEntity == regEntity) {
                    localEntity = member.localEntity;
                    break;
                }
            }
            if (localEntity == NULL_ENTITY) continue;

            if (entry["components"]) {
                // Save parent before decoding so Transform overrides don't reparent
                Transform* existingTf = scene->findComponent<Transform>(localEntity);
                Entity savedParent = existingTf ? existingTf->parent : NULL_ENTITY;

                Stream::decodeComponents(localEntity, savedParent, scene, entry["components"]);

                // Track which components are overridden
                uint64_t overrideMask = 0;
                for (auto compIt = entry["components"].begin(); compIt != entry["components"].end(); ++compIt) {
                    ComponentType compType = Catalog::getComponentType(compIt->first.as<std::string>());
                    overrideMask |= 1ULL << static_cast<int>(compType);
                }
                newInstance.overrides[localEntity] = overrideMask;
            }
        }
    }

    std::vector<Entity> allResult = newEntities;

    // Recursively import nested prefabs
    for (Entity ent : newEntities) {
        PrefabComponent* prefabComp = scene->findComponent<PrefabComponent>(ent);
        if (prefabComp && !prefabComp->path.empty()) {
            // Find the registry entity for this nested prefab root in the parent prefab
            Entity nestedRegEntity = NULL_ENTITY;
            for (const auto& member : newInstance.members) {
                if (member.localEntity == ent) {
                    nestedRegEntity = member.registryEntity;
                    break;
                }
            }

            std::string nestedKey = prefabComp->path;
            if (nestedRegEntity != NULL_ENTITY) {
                nestedKey += "_" + std::to_string(nestedRegEntity);
            }

            YAML::Node nestedOvr, nestedLoc, nestedRem;
            auto ovrIt = nestedOverridesMap.find(nestedKey);
            if (ovrIt != nestedOverridesMap.end()) nestedOvr = ovrIt->second;
            auto locIt = nestedLocalEntitiesMap.find(nestedKey);
            if (locIt != nestedLocalEntitiesMap.end()) nestedLoc = locIt->second;
            auto remIt = nestedRemovalsMap.find(nestedKey);
            if (remIt != nestedRemovalsMap.end()) nestedRem = remIt->second;
            std::vector<Entity> nestedEntities = importPrefab(sceneProject, entities, prefabComp->path, ent, false, nestedOvr, nestedLoc, entityRemap, nestedRem);
            allResult.insert(allResult.end(), nestedEntities.begin(), nestedEntities.end());
        }
    }

    // Create scene-specific local entities (skip nested entries)
    if (prefabLocalEntities && prefabLocalEntities.IsSequence()) {
        for (const auto& localEntNode : prefabLocalEntities) {
            if (localEntNode["prefabPath"]) continue;
            Entity parentRegEntity = NULL_ENTITY;
            size_t childIndex = 0;
            if (localEntNode["parentRegistryEntity"]) {
                parentRegEntity = localEntNode["parentRegistryEntity"].as<Entity>();
            }
            if (localEntNode["childIndex"]) {
                childIndex = localEntNode["childIndex"].as<size_t>();
            }

            // Find the parent local entity (or use prefab root)
            Entity parentEntity = rootEntity;
            if (parentRegEntity != NULL_ENTITY) {
                for (const auto& member : newInstance.members) {
                    if (member.registryEntity == parentRegEntity) {
                        parentEntity = member.localEntity;
                        break;
                    }
                }
            }

            std::vector<Entity> decoded = Stream::decodeEntity(localEntNode, scene, entities, this, sceneProject,
                parentEntity, true, false, entityRemap);
            if (!decoded.empty()) {
                Entity localEntity = decoded[0];
                scene->addEntityChild(parentEntity, localEntity, true);

                // Position at childIndex among the parent's children. Each child is a whole
                // branch in the Transform array, so the slot is where the childIndex-th other
                // child starts, or the end of the parent's branch.
                if (scene->findComponent<Transform>(localEntity) && scene->findComponent<Transform>(parentEntity)) {
                    auto transforms = scene->getComponentArray<Transform>();
                    size_t targetIndex = scene->findBranchLastIndex(parentEntity) + 1;
                    size_t siblings = 0;
                    for (size_t i = transforms->getIndex(parentEntity) + 1; i < transforms->size(); i++) {
                        if (transforms->getEntity(i) == localEntity || transforms->getComponentFromIndex(i).parent != parentEntity) {
                            continue;
                        }
                        if (siblings == childIndex) {
                            targetIndex = i;
                            break;
                        }
                        siblings++;
                    }
                    scene->moveChildToIndex(localEntity, targetIndex);
                }

                allResult.insert(allResult.end(), decoded.begin(), decoded.end());
            }
        }
    }

    prefab.instances[sceneProject->id].push_back(std::move(newInstance));

    updateScenePrefabs(sceneProject);

    sceneProject->isModified = needSaveScene;

    return allResult;
}

void editor::Project::removePrefabInstanceTracking(uint32_t sceneId, Entity rootEntity) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return;
    }

    Scene* scene = sceneProject->scene;
    std::unordered_set<Entity> visitedEntities;
    auto removePrefabInstance = [&](auto&& self, Entity entity) -> void {
        if (entity == NULL_ENTITY || !scene->isEntityCreated(entity) ||
            !visitedEntities.insert(entity).second) {
            return;
        }

        const PrefabComponent* component = scene->findComponent<PrefabComponent>(entity);
        Prefab* nestedPrefab = component ? getPrefab(component->path) : nullptr;
        if (!nestedPrefab) {
            return;
        }

        std::vector<Entity> nestedMembers;
        auto sceneIt = nestedPrefab->instances.find(sceneId);
        if (sceneIt != nestedPrefab->instances.end()) {
            auto instance = std::find_if(sceneIt->second.begin(), sceneIt->second.end(),
                [entity](const Prefab::Instance& candidate) {
                    return candidate.rootEntity == entity;
                });
            if (instance != sceneIt->second.end()) {
                nestedMembers.reserve(instance->members.size());
                for (const Prefab::EntityMember& member : instance->members) {
                    nestedMembers.push_back(member.localEntity);
                }
            }
        }

        for (Entity member : nestedMembers) {
            self(self, member);
        }

        sceneIt = nestedPrefab->instances.find(sceneId);
        if (sceneIt != nestedPrefab->instances.end()) {
            auto& instances = sceneIt->second;
            instances.erase(std::remove_if(instances.begin(), instances.end(),
                [entity](const Prefab::Instance& candidate) {
                    return candidate.rootEntity == entity;
                }), instances.end());
            if (instances.empty()) {
                nestedPrefab->instances.erase(sceneIt);
            }
        }
    };

    removePrefabInstance(removePrefabInstance, rootEntity);

    updateScenePrefabs(sceneProject);
}

bool editor::Project::unimportPrefab(uint32_t sceneId, const std::filesystem::path& filepath, Entity rootEntity, const std::vector<Entity>& memberEntities,
                                     bool reconcileTracked) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        return false;
    }

    Scene* scene = sceneProject->scene;

    // Collect all entities to destroy (members + local entities that are children of root/members)
    std::unordered_set<Entity> memberSet(memberEntities.begin(), memberEntities.end());
    memberSet.insert(rootEntity);
    std::vector<Entity> allEntitiesToDestroy;

    // Gather non-member children (local entities) from Transform hierarchy
    auto transforms = scene->getComponentArray<Transform>();
    for (size_t i = 0; i < transforms->size(); i++) {
        Entity ent = transforms->getEntity(i);
        if (memberSet.count(ent)) continue;
        // Walk up parent chain to see if this entity is a descendant of the root or a member
        Transform& tf = transforms->getComponentFromIndex(i);
        Entity ancestor = tf.parent;
        while (ancestor != NULL_ENTITY) {
            if (memberSet.count(ancestor)) {
                allEntitiesToDestroy.push_back(ent);
                break;
            }
            Transform* parentTf = scene->findComponent<Transform>(ancestor);
            ancestor = parentTf ? parentTf->parent : NULL_ENTITY;
        }
    }

    // Drop prefab metadata recursively before any corresponding scene entity disappears.
    // The additional flat pass covers nested prefab roots stored as scene-local children
    // rather than members of their containing prefab.
    removePrefabInstanceTracking(sceneId, rootEntity);
    if (Prefab* rootPrefab = getPrefab(filepath)) {
        auto sceneIt = rootPrefab->instances.find(sceneId);
        if (sceneIt != rootPrefab->instances.end()) {
            auto& instances = sceneIt->second;
            instances.erase(std::remove_if(instances.begin(), instances.end(),
                [rootEntity](const Prefab::Instance& instance) {
                    return instance.rootEntity == rootEntity;
                }), instances.end());
            if (instances.empty()) {
                rootPrefab->instances.erase(sceneIt);
            }
        }
    }
    for (Entity entity : memberEntities) {
        removePrefabInstanceTracking(sceneId, entity);
    }
    for (Entity entity : allEntitiesToDestroy) {
        removePrefabInstanceTracking(sceneId, entity);
    }

    // Local entities first (they may reference members as parents), then the imported
    // members, then the root; the tracked list is reconciled once after all of them
    std::vector<Entity> destroyOrder = std::move(allEntitiesToDestroy);
    destroyOrder.insert(destroyOrder.end(), memberEntities.begin(), memberEntities.end());
    if (rootEntity != NULL_ENTITY) {
        destroyOrder.push_back(rootEntity);
    }
    DeleteEntityCmd::destroyEntities(scene, destroyOrder, sceneProject->entities, this, sceneId, reconcileTracked);

    updateScenePrefabs(sceneProject);

    sceneProject->isModified = true;

    return true;
}

namespace {

Entity propertyEntityTarget(const editor::PropertyData& property) {
    if (!property.ref) {
        return NULL_ENTITY;
    }
    if (property.type == editor::PropertyType::EntityReference) {
        const EntityReference* ref = static_cast<const EntityReference*>(property.ref);
        return ref->sceneId == 0 ? ref->entity : NULL_ENTITY;
    }
    if (property.type == editor::PropertyType::Entity) {
        return *static_cast<const Entity*>(property.ref);
    }
    return NULL_ENTITY;
}

}

void editor::Project::placeInPrefabOrder(SceneProject* sceneProject, const Prefab& prefab, const Prefab::Instance& instance,
        Entity registryEntity, Entity localEntity, Entity localParent, bool keepWorldTransform) {
    Scene* scene = sceneProject->scene;
    const EntityRegistry* registry = prefab.registry.get();
    if (!registry || !scene->isEntityCreated(localEntity)) {
        return;
    }

    std::unordered_map<Entity, Entity> registryToLocal;
    for (const Prefab::EntityMember& member : instance.members) {
        registryToLocal[member.registryEntity] = member.localEntity;
    }
    auto presentLocal = [&](Entity registrySibling) {
        auto it = registryToLocal.find(registrySibling);
        return (it != registryToLocal.end() && it->second != localEntity && scene->isEntityCreated(it->second)) ?
            it->second : NULL_ENTITY;
    };

    if (scene->findComponent<Transform>(localEntity)) {
        if (localParent == NULL_ENTITY || !scene->findComponent<Transform>(localParent)) {
            return;
        }
        // Its Prefab siblings, in order
        const Entity registryParent = prefab.getRegistryParent(registryEntity);
        std::vector<Entity> siblings;
        auto registryTransforms = registry->getComponentArray<Transform>();
        for (size_t i = 0; i < registryTransforms->size(); i++) {
            if (registryTransforms->getComponentFromIndex(i).parent == registryParent) {
                siblings.push_back(registryTransforms->getEntity(i));
            }
        }
        Entity previous = NULL_ENTITY;
        Entity next = NULL_ENTITY;
        auto position = std::find(siblings.begin(), siblings.end(), registryEntity);
        if (position != siblings.end()) {
            for (auto it = position; it != siblings.begin();) {
                --it;
                if ((previous = presentLocal(*it)) != NULL_ENTITY) break;
            }
            for (auto it = std::next(position); it != siblings.end(); ++it) {
                if ((next = presentLocal(*it)) != NULL_ENTITY) break;
            }
        }
        auto parentOf = [scene](Entity entity) {
            const Transform* transform = scene->findComponent<Transform>(entity);
            return transform ? transform->parent : NULL_ENTITY;
        };

        scene->addEntityChild(localParent, localEntity, keepWorldTransform);
        auto transforms = scene->getComponentArray<Transform>();
        size_t target;
        if (previous != NULL_ENTITY && parentOf(previous) == localParent) {
            target = scene->findBranchLastIndex(previous) + 1;
        } else if (next != NULL_ENTITY && parentOf(next) == localParent) {
            target = transforms->getIndex(next);
        } else {
            target = transforms->getIndex(localParent) + 1;
        }
        scene->moveChildToIndex(localEntity, target);
        ProjectUtils::sortEntitiesByTransformOrder(scene, sceneProject->entities);
    } else {
        // A GameObject without a Transform keeps the Prefab's list order
        Entity anchor = instance.rootEntity;
        auto position = std::find(prefab.registryEntities.begin(), prefab.registryEntities.end(), registryEntity);
        for (auto it = position; it != prefab.registryEntities.begin();) {
            --it;
            if (Entity local = presentLocal(*it); local != NULL_ENTITY) {
                anchor = local;
                break;
            }
        }
        std::vector<Entity>& tracked = sceneProject->entities;
        tracked.erase(std::remove(tracked.begin(), tracked.end(), localEntity), tracked.end());
        auto anchorIt = std::find(tracked.begin(), tracked.end(), anchor);
        tracked.insert(anchorIt != tracked.end() ? std::next(anchorIt) : tracked.end(), localEntity);
    }
}

std::vector<editor::Prefab::EntityMember> editor::Project::instantiatePrefabBranch(SceneProject* sceneProject, const std::filesystem::path& filepath,
        uint32_t instanceId, Entity registryEntity, const std::vector<Prefab::EntityMember>& preferredIds) {
    Prefab* prefab = getPrefab(filepath);
    Prefab::Instance* instance = prefab ? prefab->getInstanceById(sceneProject->id, instanceId) : nullptr;
    if (!instance || !prefab->registry || !prefab->registry->isEntityCreated(registryEntity) || !sceneProject->scene) {
        return {};
    }
    Scene* scene = sceneProject->scene;
    EntityRegistry* registry = prefab->registry.get();

    const Entity registryParent = prefab->getRegistryParent(registryEntity);
    Entity localParent = instance->rootEntity;
    if (registryParent != NULL_ENTITY) {
        localParent = prefab->getLocalEntity(sceneProject->id, instanceId, registryParent);
        if (localParent == NULL_ENTITY) {
            return {}; // below a GameObject the instance removed
        }
    }

    // The registry branch less what the instance removed in it; a nested prefab root's
    // GameObjects come from its own import below
    YAML::Node node = Stream::encodeEntity(registryEntity, registry);
    if (!node.IsMap()) {
        return {};
    }
    const std::string prefabKey = Catalog::getComponentName(ComponentType::PrefabComponent, true);
    std::unordered_map<Entity, Entity> preferred;
    for (const Prefab::EntityMember& member : preferredIds) {
        preferred[member.registryEntity] = member.localEntity;
    }
    std::vector<Entity> registryOrder;
    auto prepare = [&](auto&& self, YAML::Node entityNode) -> void {
        const YAML::Node readNode = entityNode;
        const Entity nodeEntity = readNode["entity"].as<Entity>();
        registryOrder.push_back(nodeEntity);
        auto wanted = preferred.find(nodeEntity);
        if (wanted != preferred.end() && wanted->second != NULL_ENTITY && !scene->isEntityCreated(wanted->second)) {
            entityNode["entity"] = wanted->second;
        } else {
            entityNode.remove("entity");
        }
        const YAML::Node components = readNode["components"];
        if (components && components.IsMap() && components[prefabKey]) {
            entityNode.remove("children");
            return;
        }
        const YAML::Node children = readNode["children"];
        if (children && children.IsSequence()) {
            YAML::Node kept(YAML::NodeType::Sequence);
            for (YAML::Node child : children) {
                const YAML::Node readChild = child;
                if (!readChild.IsMap() || !readChild["entity"] || instance->removed.count(readChild["entity"].as<Entity>())) {
                    continue;
                }
                self(self, child);
                kept.push_back(child);
            }
            if (kept.size() > 0) {
                entityNode["children"] = kept;
            } else {
                entityNode.remove("children");
            }
        }
    };
    prepare(prepare, node);

    std::vector<Entity> decoded = Stream::decodeEntity(node, scene, &sceneProject->entities);
    if (decoded.empty() || decoded.size() != registryOrder.size()) {
        Out::error("Could not create the GameObjects of prefab %s again", filepath.string().c_str());
        DeleteEntityCmd::destroyEntities(scene, decoded, sceneProject->entities, this, sceneProject->id);
        return {};
    }

    std::vector<Prefab::EntityMember> created;
    created.reserve(decoded.size());
    for (size_t i = 0; i < decoded.size(); i++) {
        created.push_back({decoded[i], registryOrder[i]});
    }

    placeInPrefabOrder(sceneProject, *prefab, *instance, registryEntity, decoded[0], localParent, false);
    instance->members.insert(instance->members.end(), created.begin(), created.end());

    std::unordered_map<Entity, Entity> registryToLocal;
    for (const Prefab::EntityMember& member : instance->members) {
        registryToLocal[member.registryEntity] = member.localEntity;
    }
    remapEntityProperties(scene, decoded, registryToLocal);

    // The instance's GameObjects that the Prefab has pointing at these get them back
    const std::unordered_set<Entity> createdRegistry(registryOrder.begin(), registryOrder.end());
    const std::unordered_set<Entity> createdLocal(decoded.begin(), decoded.end());
    for (const Prefab::EntityMember& member : instance->members) {
        if (createdLocal.count(member.localEntity) || !scene->isEntityCreated(member.localEntity) ||
            !registry->isEntityCreated(member.registryEntity)) {
            continue;
        }
        auto overrideIt = instance->overrides.find(member.localEntity);
        const uint64_t overridden = overrideIt != instance->overrides.end() ? overrideIt->second : 0;
        for (ComponentType componentType : Catalog::findComponents(registry, member.registryEntity)) {
            if ((overridden & (1ULL << static_cast<int>(componentType))) ||
                !Catalog::mayHoldReferences(registry, member.registryEntity, componentType, true, false)) {
                continue;
            }
            std::map<std::string, PropertyData> localProperties;
            bool localRead = false;
            uint64_t updateFlags = 0;
            for (auto& [propertyName, registryProperty] : Catalog::findEntityProperties(registry, member.registryEntity, componentType)) {
                const Entity target = propertyEntityTarget(registryProperty);
                if (target == NULL_ENTITY || !createdRegistry.count(target)) {
                    continue;
                }
                if (!localRead) {
                    localProperties = Catalog::findEntityProperties(scene, member.localEntity, componentType);
                    localRead = true;
                }
                auto local = localProperties.find(propertyName);
                if (local == localProperties.end() || !local->second.ref || local->second.type != registryProperty.type) {
                    continue;
                }
                if (local->second.type == PropertyType::EntityReference) {
                    EntityReference* ref = static_cast<EntityReference*>(local->second.ref);
                    ref->entity = registryToLocal[target];
                    ref->sceneId = 0;
                } else {
                    *static_cast<Entity*>(local->second.ref) = registryToLocal[target];
                }
                updateFlags |= local->second.updateFlags;
            }
            if (updateFlags != 0) {
                Catalog::updateEntity(scene, member.localEntity, updateFlags);
            }
        }
    }

    // Nested prefab roots bring their own GameObjects
    for (Entity entity : decoded) {
        const PrefabComponent* component = scene->findComponent<PrefabComponent>(entity);
        if (component && !component->path.empty()) {
            const fs::path nestedPath = component->path;
            importPrefab(sceneProject, &sceneProject->entities, nestedPath, entity, false);
        }
    }

    CameraTextureLink::resolve(scene, decoded);
    updateScenePrefabs(sceneProject);
    sceneProject->isModified = true;
    sceneProject->needUpdateRender = true;
    return created;
}

void editor::Project::registerPrefabManager() {
    PrefabManager::clearAll();
    uint32_t prefabId = 0;
    for (const auto& [prefabPath, prefab] : prefabAssets) {
        // Same set the standalone build compiles, see collectAllPrefabs
        if (prefab.instances.empty() && !isStandalonePrefab(prefabPath)) {
            continue;
        }

        prefabId++;
        fs::path capturablePrefabPath = prefabPath;
        fs::path noExt = prefabPath;
        noExt.replace_extension();
        std::string prefabName = noExt.generic_string();

        // Determine if root needs Transform based on prefab's top-level entities
        bool hasTopLevelTransform = false;
        if (prefab.registry) {
            std::vector<Entity> topLevelEntities = getTopLevelEntities(prefab.registry.get(), prefab.registryEntities);
            for (Entity topLevelEntity : topLevelEntities) {
                if (prefab.registry->getSignature(topLevelEntity).test(prefab.registry->getComponentId<Transform>())) {
                    hasTopLevelTransform = true;
                    break;
                }
            }
        }

        PrefabManager::registerPrefab(prefabId, prefabName,
            // Factory: create prefab instance via importPrefab
            [this, capturablePrefabPath, hasTopLevelTransform](Scene* scene, Entity root) -> bool {
                SceneProject* sceneProject = findSceneProjectByScene(scene);
                if (!sceneProject) return false;

                Prefab* existingPrefab = getPrefab(capturablePrefabPath);
                if (existingPrefab) {
                    const Prefab::Instance* existing = existingPrefab->getInstance(sceneProject->id, root);
                    if (existing && existing->rootEntity == root) {
                        Out::error("PrefabManager: root entity %u already hosts prefab '%s'",
                            root, capturablePrefabPath.string().c_str());
                        return false;
                    }
                }

                const std::string oldName = scene->getEntityName(root);
                const bool addedPrefab = !scene->findComponent<PrefabComponent>(root);
                const bool addedTransform = hasTopLevelTransform && !scene->findComponent<Transform>(root);
                const std::vector<Entity> entitiesSnapshot = sceneProject->entities;

                // PrefabManager destroys the entities created by the import, this only has to
                // undo what the factory itself changed on the root plus the instance metadata
                auto restoreRoot = [&]() {
                    removePrefabInstanceTracking(sceneProject->id, root);
                    if (scene->isEntityCreated(root)) {
                        scene->setEntityName(root, oldName);
                        if (addedPrefab)
                            scene->removeComponent<PrefabComponent>(root);
                        if (addedTransform)
                            scene->removeComponent<Transform>(root);
                    }
                    sceneProject->entities = entitiesSnapshot;
                };

                if (oldName.empty()) {
                    // the plain stem, not a unique one: a "Prefab 2" is a name the same script
                    // would not find in the export, which names the root by the stem
                    std::string rootName = capturablePrefabPath.stem().string();
                    if (rootName.empty())
                        rootName = "Prefab";
                    scene->setEntityName(root, rootName);
                }

                if (addedPrefab) {
                    PrefabComponent prefabComp;
                    prefabComp.name = capturablePrefabPath.stem().string();
                    prefabComp.path = capturablePrefabPath.string();
                    scene->addComponent<PrefabComponent>(root, prefabComp);
                }

                if (addedTransform)
                    scene->addComponent<Transform>(root, {});

                if (std::find(sceneProject->entities.begin(), sceneProject->entities.end(), root) == sceneProject->entities.end())
                    sceneProject->entities.push_back(root);

                try {
                    importPrefab(sceneProject, &sceneProject->entities, capturablePrefabPath, root, false);
                } catch (const std::exception& e) {
                    Out::error("PrefabManager: import of '%s' failed: %s",
                        capturablePrefabPath.string().c_str(), e.what());
                    restoreRoot();
                    return false;
                } catch (...) {
                    Out::error("PrefabManager: import of '%s' failed",
                        capturablePrefabPath.string().c_str());
                    restoreRoot();
                    return false;
                }

                Prefab* prefab = getPrefab(capturablePrefabPath);
                const Prefab::Instance* instance = prefab ? prefab->getInstance(sceneProject->id, root) : nullptr;
                if (instance && instance->rootEntity == root)
                    return true;

                restoreRoot();
                return false;
            },
            // Destroyer: remove prefab instance via unimportPrefab
            [this, capturablePrefabPath](Scene* scene, Entity root) -> bool {
                SceneProject* sceneProject = findSceneProjectByScene(scene);
                if (!sceneProject) return false;

                Prefab* prefab = getPrefab(capturablePrefabPath);
                if (!prefab) return false;

                const Prefab::Instance* instance = prefab->getInstance(sceneProject->id, root);
                if (!instance || instance->rootEntity != root) return false;

                std::vector<Entity> members;
                for (const auto& m : instance->members)
                    members.push_back(m.localEntity);
                bool wasModified = sceneProject->isModified;
                bool result = unimportPrefab(sceneProject->id, capturablePrefabPath, root, members);
                sceneProject->isModified = wasModified;
                return result;
            }
        );
    }
}
