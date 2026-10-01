// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Prefab asset editing propagated to every instance: creating/removing prefabs,
// adding/removing GameObjects and components, property, name and order changes.

#include "Project.h"
#include "ProjectPrefabInternal.h"
#include "Out.h"
#include "Stream.h"
#include "command/type/DeleteEntityCmd.h"
#include "util/ProjectUtils.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;
using editor::prefabinternal::componentHasExternalEntityRef;

bool editor::Project::createPrefab(uint32_t sceneId, fs::path filepath, YAML::Node entityNode, const std::string& guid){
    if (!filepath.is_relative()) {
        Out::error("Prefab filepath must be relative: %s", filepath.string().c_str());
        return false;
    }

    auto it = prefabAssets.find(filepath);
    if (it != prefabAssets.end()) {
        Out::error("Prefab group already exists at %s", filepath.string().c_str());
        return false;
    }

    // Get all entities in the branch (root + children)
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        return false;
    }

    // Create new group
    Prefab newGroup;
    newGroup.registry = std::make_unique<EntityRegistry>();
    newGroup.isModified = true;

    Prefab::Instance newInstance;
    newInstance.instanceId = newGroup.nextInstanceId++;

    // Keep original scene order for member mapping and undo operations.
    std::vector<Entity> branchEntities;
    ProjectUtils::collectEntities(entityNode, branchEntities);

    // A clone: the node is the command's, and a redo reads its entity ids again
    std::vector<Entity> regEntities = Stream::decodeEntitySelection(clearEntitiesNode(YAML::Clone(entityNode)), newGroup.registry.get(), &newGroup.registryEntities);
    if (branchEntities.size() == regEntities.size()) {
        std::unordered_map<Entity, Entity> localToRegistry;
        for (size_t i = 0; i < branchEntities.size(); ++i) {
            localToRegistry[branchEntities[i]] = regEntities[i];
        }
        remapEntityProperties(newGroup.registry.get(), regEntities, localToRegistry);
    } else {
        Out::error("createPrefab(%s): entity count mismatch: branchEntities=%zu, regEntities=%zu",
            filepath.string().c_str(), branchEntities.size(), regEntities.size());
        // Without a member mapping every ref would keep a scene-local ID; store
        // None rather than letting stale IDs into the registry
        remapEntityProperties(newGroup.registry.get(), regEntities, {});
    }

    Scene* scene = sceneProject->scene;
    Entity rootEntity = scene->createUserEntity();

    // A new asset, a new GUID; a redo gives the file the one it had
    std::string assetGuid = guid;
    if (!AssetDatabase::isGuid(assetGuid) || !assetDatabase.getPath(assetGuid).empty()) {
        do {
            assetGuid = AssetDatabase::generateGuid();
        } while (!assetDatabase.getPath(assetGuid).empty());
    }

    PrefabComponent prefabComp;
    prefabComp.name = filepath.stem().string();
    prefabComp.path = filepath.string();
    prefabComp.guid = assetGuid;
    scene->addComponent<PrefabComponent>(rootEntity, prefabComp);

    // Only create a transform root when prefab top-level entities use hierarchy.
    std::vector<Entity> topLevelEntities = getTopLevelEntities(newGroup.registry.get(), regEntities);
    bool hasTopLevelTransform = false;
    for (Entity topLevelEntity : topLevelEntities) {
        if (newGroup.registry->getSignature(topLevelEntity).test(newGroup.registry->getComponentId<Transform>())) {
            hasTopLevelTransform = true;
            break;
        }
    }

    if (hasTopLevelTransform) {
        scene->addComponent<Transform>(rootEntity, {});
    }

    std::string rootName = filepath.stem().string();
    if (rootName.empty()) {
        rootName = "Prefab";
    }

    std::unordered_set<Entity> ignoredEntities(branchEntities.begin(), branchEntities.end());
    scene->setEntityName(rootEntity, ProjectUtils::makeUniqueEntityName(scene, sceneProject->entities, rootName, ignoredEntities));
    sceneProject->entities.push_back(rootEntity);

    Entity firstPrefabEntity = NULL_ENTITY;
    Entity firstPrefabTransformEntity = NULL_ENTITY;
    size_t firstPrefabIndex = std::numeric_limits<size_t>::max();
    size_t firstPrefabTransformIndex = std::numeric_limits<size_t>::max();

    for (Entity branchEntity : branchEntities) {
        auto itSceneEntity = std::find(sceneProject->entities.begin(), sceneProject->entities.end(), branchEntity);
        if (itSceneEntity == sceneProject->entities.end()) {
            continue;
        }

        size_t entityIndex = std::distance(sceneProject->entities.begin(), itSceneEntity);
        if (entityIndex < firstPrefabIndex) {
            firstPrefabIndex = entityIndex;
            firstPrefabEntity = branchEntity;
        }

        if (scene->findComponent<Transform>(branchEntity) && entityIndex < firstPrefabTransformIndex) {
            firstPrefabTransformIndex = entityIndex;
            firstPrefabTransformEntity = branchEntity;
        }
    }

    // Place root before the first bundled entity to preserve visual ordering.
    Entity moveTarget = hasTopLevelTransform ? firstPrefabTransformEntity : firstPrefabEntity;
    if (moveTarget != NULL_ENTITY) {
        Entity oldParent = NULL_ENTITY;
        size_t oldIndex = 0;
        bool hasTransform = false;
        ProjectUtils::moveEntityOrderByTarget(scene, sceneProject->entities, rootEntity, moveTarget, InsertionType::BEFORE, oldParent, oldIndex, hasTransform);
    }

    // Reparent top-level transformed prefab entities under the new root.
    if (hasTopLevelTransform) {
        std::vector<Entity> sceneTopLevelEntities = getTopLevelEntities(scene, branchEntities);
        for (Entity topLevelEntity : sceneTopLevelEntities) {
            if (scene->findComponent<Transform>(topLevelEntity)) {
                scene->addEntityChild(rootEntity, topLevelEntity, true);
            }
        }
        ProjectUtils::sortEntitiesByTransformOrder(scene, sceneProject->entities);
    }

    newInstance.rootEntity = rootEntity;

    for (size_t i = 0; i < regEntities.size() && i < branchEntities.size(); i++) {
        newInstance.members.push_back({branchEntities[i], regEntities[i]});
    }

    // References to entities outside the prefab stayed on the scene members while the
    // registry stored None; keep and persist them by overriding those components.
    overrideExternalRefComponents(scene, newInstance);

    newGroup.instances[sceneId].push_back(std::move(newInstance));

    prefabAssets.emplace(filepath, std::move(newGroup));

    // The file is written with the GUID the database gives its path
    assetDatabase.registerAsset(filepath, assetGuid);
    savePrefabToDisk(filepath);

    updateScenePrefabs(sceneProject);

    sceneProject->isModified = true;

    return true;
}

bool editor::Project::removePrefab(const std::filesystem::path& filepath) {
    auto it = prefabAssets.find(filepath);
    if (it == prefabAssets.end()) {
        return false;
    }

    // Undo scene-side changes introduced by createPrefab for each instance.
    for (const auto& [sceneId, instances] : it->second.instances) {
        SceneProject* sceneProject = getScene(sceneId);
        if (!sceneProject) {
            continue;
        }

        for (const auto& instance : instances) {
            if (instance.rootEntity == NULL_ENTITY) {
                continue;
            }

            Scene* scene = sceneProject->scene;
            if (!scene || !scene->isEntityCreated(instance.rootEntity)) {
                continue;
            }

            // If the prefab root owns transformed members, detach them back before removing root.
            Transform* rootTransform = scene->findComponent<Transform>(instance.rootEntity);
            if (rootTransform) {
                Entity rootParent = rootTransform->parent;

                for (const auto& member : instance.members) {
                    if (member.localEntity == NULL_ENTITY || member.localEntity == instance.rootEntity) {
                        continue;
                    }

                    Transform* memberTransform = scene->findComponent<Transform>(member.localEntity);
                    if (memberTransform && memberTransform->parent == instance.rootEntity) {
                        scene->addEntityChild(rootParent, member.localEntity, true);
                    }
                }

                ProjectUtils::sortEntitiesByTransformOrder(scene, sceneProject->entities);
            }

            DeleteEntityCmd::destroyEntity(sceneProject->scene, instance.rootEntity, sceneProject->entities, this, sceneId);
        }
    }

    if (it->second.registry) {
        it->second.registry->clear();
    }

    prefabAssets.erase(it);

    for (SceneProject& sceneProject : scenes) {
        updateScenePrefabs(&sceneProject);
    }

    editor::Out::info("Removed prefab: %s", filepath.string().c_str());
    return true;
}

bool editor::Project::addComponentToPrefab(uint32_t sceneId, Entity entity, ComponentType componentType, bool addToItself){
    ComponentRecovery recovery;
    return addComponentToPrefab(sceneId, entity, componentType, recovery, addToItself);
}

bool editor::Project::addComponentToPrefab(uint32_t sceneId, Entity entity, ComponentType componentType, const ComponentRecovery& recovery, bool addToItself){
    fs::path filepath = findPrefabPathFor(sceneId, entity);
    if (filepath.empty()) {
        Out::error("Entity %u in scene %u is not part of any prefab", entity, sceneId);
        return false;
    }

    Prefab* prefab = getPrefab(filepath);
    uint32_t instanceId = prefab->getInstanceId(sceneId, entity);

    if (prefab->hasComponentOverride(sceneId, entity, componentType)){
        Out::warning("Component %s of entity %u in scene %u is overridden", Catalog::getComponentName(componentType).c_str(), entity, sceneId);
        return false;
    }

    Entity registryEntity = prefab->getRegistryEntity(sceneId, entity);
    if (registryEntity == NULL_ENTITY) {
        Out::error("Failed to find registry entities for prefab entities %u in scene %u", entity, sceneId);
        return false;
    }

    YAML::Node regNode;
    std::string recoveryDefKey = std::to_string(NULL_PROJECT_SCENE);
    if (recovery.find(recoveryDefKey) != recovery.end()) {
        if (recovery.at(recoveryDefKey).entity == registryEntity){
            regNode = recovery.at(recoveryDefKey).node;
        }else{
            Out::error("Component recovery entity (%u) does not match registry entity (%u)", recovery.at(recoveryDefKey).entity, registryEntity);
            return false;
        }
    }

    ProjectUtils::addEntityComponent(prefab->registry.get(), registryEntity, componentType, prefab->registryEntities, regNode);

    std::vector<uint32_t> stalePrefabScenes;
    for (auto& [otherSceneId, sceneInstances] : prefab->instances) {
        SceneProject* otherScene = getScene(otherSceneId);
        if (!otherScene) {
            Out::error("Failed to find scene %u", otherSceneId);
            stalePrefabScenes.push_back(otherSceneId);
            continue;
        }
        if (!otherScene->scene) {
            Out::error("Scene %u is not loaded", otherSceneId);
            stalePrefabScenes.push_back(otherSceneId);
            continue;
        }
        for (auto& instance : sceneInstances) {
            if ((otherSceneId != sceneId) || (instance.instanceId != instanceId) || addToItself) {
                Entity otherEntity = prefab->getLocalEntity(otherSceneId, instance.instanceId, registryEntity);

                if (otherEntity != NULL_ENTITY) {
                    if (!prefab->hasComponentOverride(otherSceneId, otherEntity, componentType)) {
                        YAML::Node compNode;
                        std::string recoveryKey = std::to_string(otherSceneId) + "_" + std::to_string(instance.instanceId);
                        if (recovery.find(recoveryKey) != recovery.end()) {
                            if (recovery.at(recoveryKey).entity == otherEntity){
                                compNode = recovery.at(recoveryKey).node;
                            }else{
                                Out::warning("Component recovery entity (%u) does not match scene (%u) entity (%u)", recovery.at(recoveryKey).entity, otherSceneId, otherEntity);
                                return false;
                            }
                        }

                        ProjectUtils::addEntityComponent(otherScene->scene, otherEntity, componentType, otherScene->entities, compNode);

                        otherScene->isModified = true;
                    }
                }
            }
        }
    }
    for (uint32_t staleId : stalePrefabScenes) {
        cleanupPrefabsForScene(staleId);
    }

    prefab->isModified = true;

    return true;
}

editor::ComponentRecovery editor::Project::removeComponentFromPrefab(uint32_t sceneId, Entity entity, ComponentType componentType, bool encodeComponent, bool removeToItself){
    fs::path filepath = findPrefabPathFor(sceneId, entity);
    if (filepath.empty()) {
        Out::error("Entity %u in scene %u is not part of any prefab", entity, sceneId);
        return {};
    }

    Prefab* prefab = getPrefab(filepath);
    uint32_t instanceId = prefab->getInstanceId(sceneId, entity);

    if (prefab->hasComponentOverride(sceneId, entity, componentType)){
        Out::warning("Component %s of entity %u in scene %u is overridden", Catalog::getComponentName(componentType).c_str(), entity, sceneId);
        return {};
    }

    Entity registryEntity = prefab->getRegistryEntity(sceneId, entity);
    if (registryEntity == NULL_ENTITY) {
        Out::error("Failed to find registry entities for prefab entities %u in scene %u", entity, sceneId);
        return {};
    }

    ComponentRecovery recovery;
    std::string recoveryDefKey = std::to_string(NULL_PROJECT_SCENE);
    recovery[recoveryDefKey].entity = registryEntity;
    recovery[recoveryDefKey].node = ProjectUtils::removeEntityComponent(prefab->registry.get(), registryEntity, componentType, prefab->registryEntities, encodeComponent);

    std::vector<uint32_t> stalePrefabScenes;
    for (auto& [otherSceneId, sceneInstances] : prefab->instances) {
        SceneProject* otherScene = getScene(otherSceneId);
        if (!otherScene) {
            Out::error("Failed to find scene %u", otherSceneId);
            stalePrefabScenes.push_back(otherSceneId);
            continue;
        }
        if (!otherScene->scene) {
            Out::error("Scene %u is not loaded", otherSceneId);
            stalePrefabScenes.push_back(otherSceneId);
            continue;
        }
        for (auto& instance : sceneInstances) {
            if ((otherSceneId != sceneId) || (instance.instanceId != instanceId) || removeToItself) {
                Entity otherEntity = prefab->getLocalEntity(otherSceneId, instance.instanceId, registryEntity);

                if (otherEntity != NULL_ENTITY) {
                    if (!prefab->hasComponentOverride(otherSceneId, otherEntity, componentType)) {
                        std::string recoveryKey = std::to_string(otherSceneId) + "_" + std::to_string(instance.instanceId);
                        recovery[recoveryKey].entity = otherEntity;
                        recovery[recoveryKey].node = ProjectUtils::removeEntityComponent(otherScene->scene, otherEntity, componentType, otherScene->entities, encodeComponent);

                        otherScene->isModified = true;
                    }
                }
            }
        }
        ProjectUtils::reconcileTrackedEntities(otherScene->scene, otherScene->entities, this, otherSceneId);
    }
    for (uint32_t staleId : stalePrefabScenes) {
        cleanupPrefabsForScene(staleId);
    }

    prefab->isModified = true;

    return recovery;
}

YAML::Node editor::Project::clearEntitiesNode(YAML::Node node) {
    if (!node || !node.IsMap())
        return node;

    node.remove("entity");

    if (node["members"] && node["members"].IsSequence()) {
        for (size_t i = 0; i < node["members"].size(); ++i) {
            node["members"][i] = clearEntitiesNode(node["members"][i]);
        }
        return node;
    }

    if (node["children"] && node["children"].IsSequence()) {
        for (size_t i = 0; i < node["children"].size(); ++i) {
            node["children"][i] = clearEntitiesNode(node["children"][i]);
        }
    }

    return node;
}

YAML::Node editor::Project::changeEntitiesNode(Entity& firstEntity, YAML::Node node) {
    if (!node || !node.IsMap())
        return node;

    if (node["members"] && node["members"].IsSequence()) {
        for (size_t i = 0; i < node["members"].size(); ++i) {
            node["members"][i] = changeEntitiesNode(firstEntity, node["members"][i]);
        }
        return node;
    }

    // Assign the current entity ID
    node["entity"] = firstEntity++;

    // Recursively process children
    if (node["children"] && node["children"].IsSequence()) {
        for (size_t i = 0; i < node["children"].size(); ++i) {
            node["children"][i] = changeEntitiesNode(firstEntity, node["children"][i]);
        }
    }

    return node;
}

bool editor::Project::addEntityToPrefab(uint32_t sceneId, Entity entity, Entity parent, bool createItself){
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject){
        return false;
    }

    Scene* scene = sceneProject->scene;

    // Find which prefab the parent belongs to
    // First check if parent is a prefab root (for explicit "Insert to Prefab" targeting)
    fs::path filepath;
    for (const auto& [prefabPath, prefab] : prefabAssets) {
        auto sceneIt = prefab.instances.find(sceneId);
        if (sceneIt == prefab.instances.end()) continue;
        for (const auto& instance : sceneIt->second) {
            if (instance.rootEntity == parent) {
                filepath = prefabPath;
                break;
            }
        }
        if (!filepath.empty()) break;
    }
    // Fall back to general lookup if parent is not a root
    if (filepath.empty()) {
        filepath = findPrefabPathFor(sceneId, parent);
    }
    if (filepath.empty()) {
        Out::error("Entity parent %u in scene %u is not part of any prefab", parent, sceneId);
        return false;
    }

    Prefab* prefab = getPrefab(filepath);
    if (!prefab) {
        return false;
    }
    uint32_t instanceId = prefab->getInstanceId(sceneId, parent);
    if (instanceId == 0) {
        Out::error("Failed to find instance for parent entity %u in scene %u", parent, sceneId);
        return false;
    }

    Entity registryParent = prefab->getRegistryEntity(sceneId, parent);
    // parent could be the prefab root (no registryEntity)
    // In that case the entity will be a top-level member under root

    // Encode the entity (preserve original for entity ID collection)
    YAML::Node nodeOriginal = Stream::encodeEntity(entity, scene, nullptr, sceneProject);
    YAML::Node nodeRegData = clearEntitiesNode(YAML::Clone(nodeOriginal));

    // Don't add nested prefab children to parent prefab's registry
    bool isNestedPrefab = scene->findComponent<PrefabComponent>(entity) != nullptr;
    if (isNestedPrefab) {
        nodeRegData.remove("children");
    }

    // Decode into registry
    std::vector<Entity> regEntities = Stream::decodeEntity(nodeRegData, prefab->registry.get(), &prefab->registryEntities);
    if (regEntities.empty()) {
        Out::error("Failed to decode entity into prefab registry");
        return false;
    }

    bool hasTransform = scene->getSignature(entity).test(scene->getComponentId<Transform>());
    Entity previousRegistryEntity = NULL_ENTITY;
    auto regEntityIt = std::find(prefab->registryEntities.begin(), prefab->registryEntities.end(), regEntities[0]);
    if (regEntityIt != prefab->registryEntities.begin() && regEntityIt != prefab->registryEntities.end()) {
        previousRegistryEntity = *std::prev(regEntityIt);
    }

    // Position in registry if parent has a registry entity
    if (registryParent != NULL_ENTITY) {
        ProjectUtils::moveEntityOrderByTransform(prefab->registry.get(), prefab->registryEntities, regEntities[0], registryParent, 0, false);
    }

    // Remap the newly decoded registry entities: references to an existing member or
    // to the new subtree translate to registry IDs; anything outside the prefab is
    // cleared to None so a scene-local ID never leaks into the shared registry.
    {
        std::unordered_map<Entity, Entity> localToRegistry;
        if (const Prefab::Instance* srcInstance = prefab->getInstanceById(sceneId, instanceId)) {
            for (const auto& member : srcInstance->members) {
                localToRegistry[member.localEntity] = member.registryEntity;
            }
        }
        std::vector<Entity> srcBranch;
        ProjectUtils::collectEntities(nodeOriginal, srcBranch);
        if (srcBranch.size() == regEntities.size()) {
            for (size_t i = 0; i < regEntities.size(); i++) {
                localToRegistry[srcBranch[i]] = regEntities[i];
            }
        }
        remapEntityProperties(prefab->registry.get(), regEntities, localToRegistry);
    }

    // For each instance, add the entity as a new member
    for (auto& [otherSceneId, sceneInstances] : prefab->instances) {
        SceneProject* otherScene = getScene(otherSceneId);
        if (!otherScene || !otherScene->scene) {
            continue;
        }

        for (auto& instance : sceneInstances) {
            std::vector<Entity> newOtherEntities;

            // Below a GameObject this instance removed, the new one stays out of it too
            if (prefab->isRegistryEntityAbsent(instance, registryParent)) {
                continue;
            }

            // Find the parent in this instance.
            Entity otherParent = instance.rootEntity;
            if (registryParent != NULL_ENTITY) {
                for (const auto& member : instance.members) {
                    if (member.registryEntity == registryParent) {
                        otherParent = member.localEntity;
                        break;
                    }
                }
            }

            if ((otherSceneId != sceneId) || (instance.instanceId != instanceId) || createItself) {

                YAML::Node nodeData = Stream::encodeEntity(regEntities[0], prefab->registry.get());
                newOtherEntities = Stream::decodeEntity(nodeData, otherScene->scene, &otherScene->entities);
                if (hasTransform && !newOtherEntities.empty()) {
                    ProjectUtils::moveEntityOrderByTransform(otherScene->scene, otherScene->entities, newOtherEntities[0], otherParent, 0, false);
                }

                // For nested prefabs, import the prefab contents for this instance too
                if (isNestedPrefab && !newOtherEntities.empty()) {
                    PrefabComponent* bc = otherScene->scene->findComponent<PrefabComponent>(newOtherEntities[0]);
                    if (bc) {
                        importPrefab(otherScene, &otherScene->entities, bc->path, newOtherEntities[0], false);
                    }
                }
            } else {
                // Current instance: collect the existing entity IDs and reparent under the prefab.
                if (isNestedPrefab) {
                    newOtherEntities.push_back(entity);
                } else {
                    ProjectUtils::collectEntities(nodeOriginal, newOtherEntities);
                }

                if (hasTransform && !newOtherEntities.empty()) {
                    ProjectUtils::moveEntityOrderByTransform(otherScene->scene, otherScene->entities, newOtherEntities[0], otherParent, 0, false);
                }
            }

            if (!hasTransform && !newOtherEntities.empty()) {
                Entity orderAnchor = instance.rootEntity;
                if (previousRegistryEntity != NULL_ENTITY) {
                    Entity previousLocalEntity = prefab->getLocalEntity(otherSceneId, instance.instanceId, previousRegistryEntity);
                    if (previousLocalEntity != NULL_ENTITY) {
                        orderAnchor = previousLocalEntity;
                    }
                }

                if (orderAnchor != NULL_ENTITY && orderAnchor != newOtherEntities[0]) {
                    Entity oldParent = NULL_ENTITY;
                    size_t oldIndex = 0;
                    bool hasTransform = false;
                    ProjectUtils::moveEntityOrderByTarget(otherScene->scene, otherScene->entities, newOtherEntities[0], orderAnchor, InsertionType::AFTER, oldParent, oldIndex, hasTransform);
                }
            }

            // Add members mapping
            if (regEntities.size() == newOtherEntities.size()) {
                for (size_t e = 0; e < newOtherEntities.size(); e++) {
                    instance.members.push_back({newOtherEntities[e], regEntities[e]});
                }

                if ((otherSceneId != sceneId) || (instance.instanceId != instanceId) || createItself) {
                    // Freshly decoded from the registry: translate registry IDs to this
                    // instance's local IDs (references outside the prefab stay None).
                    std::unordered_map<Entity, Entity> registryToLocal;
                    for (const auto& member : instance.members) {
                        registryToLocal[member.registryEntity] = member.localEntity;
                    }
                    remapEntityProperties(otherScene->scene, newOtherEntities, registryToLocal);
                } else {
                    // Current instance kept its original scene references; persist any
                    // that point outside the prefab by overriding those components.
                    overrideExternalRefComponents(otherScene->scene, instance);
                }
            }

            otherScene->isModified = true;
        }
    }

    prefab->isModified = true;

    return true;
}

bool editor::Project::addEntityToPrefab(uint32_t sceneId, const NodeRecovery& recoveryData, Entity parent,
        bool createItself, std::unordered_map<Entity, Entity>* entityRemap, std::vector<Entity>* restoredEntities){
    fs::path filepath = findPrefabPathFor(sceneId, parent);
    if (filepath.empty()) {
        Out::error("Entity parent %u in scene %u is not part of any prefab", parent, sceneId);
        return false;
    }

    Prefab* prefab = getPrefab(filepath);
    if (!prefab) {
        return false;
    }

    return restoreRegistryEntityToPrefab(filepath, prefab->getRegistryEntity(sceneId, parent), recoveryData,
        sceneId, prefab->getInstanceId(sceneId, parent), createItself, entityRemap, restoredEntities);
}

bool editor::Project::restoreRegistryEntityToPrefab(const std::filesystem::path& filepath, Entity registryParent, const NodeRecovery& recoveryData,
        uint32_t sceneId, uint32_t sourceInstanceId, bool createItself, std::unordered_map<Entity, Entity>* entityRemap,
        std::vector<Entity>* restoredEntities){
    Prefab* prefab = getPrefab(filepath);
    if (!prefab || !prefab->registry) {
        return false;
    }
    bool wasPrefabModified = prefab->isModified;
    bool deferSourceRemap = entityRemap && restoredEntities;

    // Recover registry data
    std::string recoveryDefKey = std::to_string(NULL_PROJECT_SCENE);
    YAML::Node nodeRegData;
    size_t regTransformIndex = 0;
    if (auto recoveryIt = recoveryData.find(recoveryDefKey); recoveryIt != recoveryData.end()) {
        nodeRegData = recoveryIt->second.node;
        regTransformIndex = recoveryIt->second.transformIndex;
    }

    if (!nodeRegData) {
        Out::error("No registry data in recovery for addEntityToPrefab");
        return false;
    }

    std::unordered_map<Entity, Entity> recoveredRegistryIds;
    std::vector<Entity> regEntities = Stream::decodeEntity(nodeRegData, prefab->registry.get(),
        &prefab->registryEntities, nullptr, nullptr, NULL_ENTITY, true, false, &recoveredRegistryIds);
    if (regEntities.empty()) {
        Out::error("Could not recover registry data for prefab");
        return false;
    }
    remapEntityProperties(prefab->registry.get(), prefab->registryEntities, recoveredRegistryIds, false);
    bool regHasTransform = prefab->registry->getSignature(regEntities[0]).test(prefab->registry->getComponentId<Transform>());
    ProjectUtils::moveEntityOrderByIndex(prefab->registry.get(), prefab->registryEntities, regEntities[0], registryParent, regTransformIndex, regHasTransform);

    struct InstanceRecoveryTarget {
        uint32_t sceneId = NULL_PROJECT_SCENE;
        uint32_t instanceId = 0;
        Entity rootEntity = NULL_ENTITY;
    };

    std::vector<InstanceRecoveryTarget> recoveryTargets;
    for (const auto& [otherSceneId, sceneInstances] : prefab->instances) {
        for (const Prefab::Instance& instance : sceneInstances) {
            recoveryTargets.push_back({otherSceneId, instance.instanceId, instance.rootEntity});
        }
    }

    struct PendingInstanceRecovery {
        uint32_t sceneId = NULL_PROJECT_SCENE;
        uint32_t instanceId = 0;
        const NodeRecoveryEntry* recoveredEntry = nullptr;
        std::vector<Entity> entities;
        std::vector<Prefab::EntityMember> members;
        std::unordered_map<Entity, Entity> localRemap;
        Entity parent = NULL_ENTITY;
        size_t transformIndex = 0;
        bool decodeBranch = false;
    };

    std::vector<PendingInstanceRecovery> pendingRecoveries;
    std::map<uint32_t, bool> sceneModifiedStates;
    std::map<uint32_t, std::vector<Entity>> referenceEntitiesByScene;
    bool validRecovery = true;

    struct PendingRemovalRestore {
        uint32_t sceneId = NULL_PROJECT_SCENE;
        uint32_t instanceId = 0;
        const NodeRecoveryEntry* recoveredEntry = nullptr;
    };
    std::vector<PendingRemovalRestore> removalRestores;
    auto recoveredRegistry = [&recoveredRegistryIds](Entity entity) {
        auto it = recoveredRegistryIds.find(entity);
        return it != recoveredRegistryIds.end() ? it->second : entity;
    };

    // Decode every instance first. Membership and override metadata are committed only after
    // all exact saved pairs have resolved, so a failed nested import can be rolled back cleanly.
    const std::unordered_set<Entity> recoveredRegistryEntities(regEntities.begin(), regEntities.end());
    for (const InstanceRecoveryTarget& target : recoveryTargets) {
        SceneProject* otherScene = getScene(target.sceneId);
        if (!otherScene || !otherScene->scene) {
            continue;
        }
        sceneModifiedStates.emplace(target.sceneId, otherScene->isModified);
        if (!deferSourceRemap || target.sceneId != sceneId) {
            referenceEntitiesByScene.try_emplace(target.sceneId, otherScene->entities);
        }

        Prefab::Instance* instance = prefab->getInstanceById(target.sceneId, target.instanceId);
        if (!instance) {
            validRecovery = false;
            break;
        }

        std::string recoveryKey = std::to_string(target.sceneId) + "_" + std::to_string(target.rootEntity);
        YAML::Node nodeData;
        size_t transformIndex = 0;
        const NodeRecoveryEntry* recoveredEntry = nullptr;

        auto recoveryIt = recoveryData.find(recoveryKey);
        if (recoveryIt != recoveryData.end()) {
            recoveredEntry = &recoveryIt->second;
            nodeData = recoveredEntry->node;
            transformIndex = recoveredEntry->transformIndex;
        }

        // An instance that removed the GameObject, or one above it, gets its removed
        // GameObject entries back (once the recovery holds) and nothing in the scene
        bool branchRemoved = false;
        if (recoveredEntry && !recoveredEntry->removed.empty()) {
            removalRestores.push_back({target.sceneId, target.instanceId, recoveredEntry});
            for (const auto& [removedEntity, removedMembers] : recoveredEntry->removed) {
                if (recoveredRegistry(removedEntity) == regEntities[0]) {
                    branchRemoved = true;
                }
            }
        }
        if (branchRemoved || instance->removed.count(regEntities[0]) ||
            prefab->isRegistryEntityAbsent(*instance, registryParent) ||
            (recoveredEntry && !recoveredEntry->node)) {
            continue;
        }
        if (!recoveredEntry) {
            nodeData = Stream::encodeEntity(regEntities[0], prefab->registry.get());
        }

        // Resolve everything needed from the instance before decoding. A malformed circular
        // prefab can recursively append another instance of this prefab and reallocate its
        // instance vector, so no iterator, reference, or pointer may survive the decode.
        Entity otherParent = instance->rootEntity;
        if (registryParent != NULL_ENTITY) {
            for (const auto& member : instance->members) {
                if (member.registryEntity == registryParent) {
                    otherParent = member.localEntity;
                    break;
                }
            }
        }

        std::vector<Entity> newOtherEntities;
        bool isSourceInstance = target.sceneId == sceneId && target.instanceId == sourceInstanceId;

        bool decodeBranch = !isSourceInstance || createItself;
        std::unordered_map<Entity, Entity> recoveredLocalIds;
        if (decodeBranch) {
            newOtherEntities = Stream::decodeEntity(nodeData, otherScene->scene,
                &otherScene->entities, this, otherScene, NULL_ENTITY, true, false, &recoveredLocalIds);
        } else {
            ProjectUtils::collectEntities(nodeData, newOtherEntities);
        }

        const std::unordered_set<Entity> branchEntities(newOtherEntities.begin(), newOtherEntities.end());
        bool nestedPrefabsMapped = true;
        if (recoveredEntry) {
            for (const NestedPrefabRecovery& savedPrefab : recoveredEntry->nestedPrefabs) {
                Entity nestedRoot = savedPrefab.rootEntity;
                if (auto rootRemap = recoveredLocalIds.find(nestedRoot); rootRemap != recoveredLocalIds.end()) {
                    nestedRoot = rootRemap->second;
                }

                Prefab* nestedPrefab = getPrefab(savedPrefab.path);
                const Prefab::Instance* nestedInstance = nestedPrefab ?
                    nestedPrefab->getInstance(target.sceneId, nestedRoot) : nullptr;
                if (!nestedInstance || nestedInstance->rootEntity != nestedRoot ||
                    !otherScene->scene->isEntityCreated(nestedRoot) ||
                    (decodeBranch && !branchEntities.count(nestedRoot))) {
                    nestedPrefabsMapped = false;
                    break;
                }

                for (const Prefab::EntityMember& savedMember : savedPrefab.members) {
                    Entity registryEntity = savedMember.registryEntity;
                    if (nestedPrefab == prefab) {
                        if (auto registryRemap = recoveredRegistryIds.find(registryEntity);
                            registryRemap != recoveredRegistryIds.end()) {
                            registryEntity = registryRemap->second;
                        }
                    }

                    auto restoredMember = std::find_if(nestedInstance->members.begin(), nestedInstance->members.end(),
                        [&](const Prefab::EntityMember& member) {
                            return member.registryEntity == registryEntity;
                        });
                    if (restoredMember == nestedInstance->members.end() ||
                        !otherScene->scene->isEntityCreated(restoredMember->localEntity) ||
                        (decodeBranch && !branchEntities.count(restoredMember->localEntity))) {
                        nestedPrefabsMapped = false;
                        break;
                    }
                    if (restoredMember->localEntity != savedMember.localEntity) {
                        recoveredLocalIds[savedMember.localEntity] = restoredMember->localEntity;
                    }
                }
                if (!nestedPrefabsMapped) {
                    break;
                }
            }
        }

        std::vector<Prefab::EntityMember> membersToRestore;
        if (recoveredEntry && !recoveredEntry->members.empty()) {
            membersToRestore.reserve(recoveredEntry->members.size());
            for (const Prefab::EntityMember& saved : recoveredEntry->members) {
                Entity localEntity = saved.localEntity;
                auto localRemap = recoveredLocalIds.find(localEntity);
                if (localRemap != recoveredLocalIds.end()) {
                    localEntity = localRemap->second;
                }

                Entity registryEntity = saved.registryEntity;
                auto registryRemap = recoveredRegistryIds.find(registryEntity);
                if (registryRemap != recoveredRegistryIds.end()) {
                    registryEntity = registryRemap->second;
                }

                if (branchEntities.count(localEntity) && recoveredRegistryEntities.count(registryEntity) &&
                    otherScene->scene->isEntityCreated(localEntity) &&
                    prefab->registry->isEntityCreated(registryEntity)) {
                    membersToRestore.push_back({localEntity, registryEntity});
                }
            }
        } else if (regEntities.size() == newOtherEntities.size()) {
            for (size_t i = 0; i < regEntities.size(); ++i) {
                membersToRestore.push_back({newOtherEntities[i], regEntities[i]});
            }
        }

        bool canPairMembers = nestedPrefabsMapped && (recoveredEntry && !recoveredEntry->members.empty() ?
            membersToRestore.size() == recoveredEntry->members.size() :
            membersToRestore.size() == regEntities.size());
        pendingRecoveries.push_back({target.sceneId, target.instanceId, recoveredEntry,
            std::move(newOtherEntities), std::move(membersToRestore),
            std::move(recoveredLocalIds), otherParent, transformIndex,
            decodeBranch});
        if (!canPairMembers) {
            validRecovery = false;
            break;
        }
    }

    if (validRecovery) {
        validRecovery = std::all_of(pendingRecoveries.begin(), pendingRecoveries.end(),
            [&](const PendingInstanceRecovery& pending) {
                return prefab->getInstanceById(pending.sceneId, pending.instanceId) != nullptr;
            });
    }

    if (!validRecovery) {
        for (PendingInstanceRecovery& pending : pendingRecoveries) {
            if (!pending.decodeBranch) {
                continue;
            }
            SceneProject* pendingScene = getScene(pending.sceneId);
            if (!pendingScene || !pendingScene->scene) {
                continue;
            }
            for (Entity entity : pending.entities) {
                removePrefabInstanceTracking(pending.sceneId, entity);
                if (!pendingScene->scene->isEntityCreated(entity)) {
                    continue;
                }
                if (ActionComponent* action = pendingScene->scene->findComponent<ActionComponent>(entity)) {
                    action->ownedTarget = false;
                }
                if (AnimationComponent* animation = pendingScene->scene->findComponent<AnimationComponent>(entity)) {
                    animation->ownedActions = false;
                }
            }
            DeleteEntityCmd::destroyEntities(pendingScene->scene, pending.entities,
                pendingScene->entities, this, pending.sceneId);
        }

        std::unordered_map<Entity, Entity> reverseRegistryRemap;
        for (const auto& [oldEntity, newEntity] : recoveredRegistryIds) {
            reverseRegistryRemap[newEntity] = oldEntity;
        }
        remapEntityProperties(prefab->registry.get(), prefab->registryEntities,
            reverseRegistryRemap, false);
        DeleteEntityCmd::destroyEntities(prefab->registry.get(), regEntities, prefab->registryEntities);
        for (const auto& [otherSceneId, modified] : sceneModifiedStates) {
            if (SceneProject* otherScene = getScene(otherSceneId)) {
                otherScene->isModified = modified;
            }
        }
        prefab->isModified = wasPrefabModified;
        Out::error("Could not pair recovered prefab members");
        return false;
    }

    for (const PendingRemovalRestore& restore : removalRestores) {
        Prefab::Instance* instance = prefab->getInstanceById(restore.sceneId, restore.instanceId);
        if (!instance) {
            continue;
        }
        for (const auto& [removedEntity, removedMembers] : restore.recoveredEntry->removed) {
            instance->removed[recoveredRegistry(removedEntity)] = removedMembers;
        }
        if (SceneProject* removalScene = getScene(restore.sceneId)) {
            removalScene->isModified = true;
        }
    }

    std::map<uint32_t, std::unordered_map<Entity, Entity>> sceneEntityRemaps;
    for (PendingInstanceRecovery& pending : pendingRecoveries) {
        SceneProject* pendingScene = getScene(pending.sceneId);
        Prefab::Instance* instance = prefab->getInstanceById(pending.sceneId, pending.instanceId);
        if (!pendingScene || !pendingScene->scene || !instance) {
            continue;
        }
        Scene* otherScene = pendingScene->scene;

        if (!pending.entities.empty()) {
            bool hasTransform = otherScene->getSignature(pending.entities[0]).test(
                otherScene->getComponentId<Transform>());
            ProjectUtils::moveEntityOrderByIndex(otherScene, pendingScene->entities,
                pending.entities[0], pending.parent, pending.transformIndex, hasTransform);
        }

        instance->members.insert(instance->members.end(),
            pending.members.begin(), pending.members.end());

        if (entityRemap && pending.recoveredEntry && pending.sceneId == sceneId) {
            for (const auto& [oldEntity, newEntity] : pending.localRemap) {
                (*entityRemap)[oldEntity] = newEntity;
            }
        }
        if (pending.recoveredEntry) {
            for (const auto& [oldEntity, overrideMask] : pending.recoveredEntry->overrides) {
                Entity localEntity = oldEntity;
                auto localRemap = pending.localRemap.find(oldEntity);
                if (localRemap != pending.localRemap.end()) {
                    localEntity = localRemap->second;
                }
                if (otherScene->isEntityCreated(localEntity)) {
                    instance->overrides[localEntity] = overrideMask;
                }
            }
        }
        if (restoredEntities && pending.sceneId == sceneId) {
            restoredEntities->insert(restoredEntities->end(),
                pending.entities.begin(), pending.entities.end());
        }
        if (!deferSourceRemap || pending.sceneId != sceneId) {
            auto& referenceEntities = referenceEntitiesByScene[pending.sceneId];
            referenceEntities.insert(referenceEntities.end(), pending.entities.begin(), pending.entities.end());
        }

        if (pending.decodeBranch && !pending.recoveredEntry) {
            std::unordered_map<Entity, Entity> registryToLocal;
            for (const Prefab::EntityMember& member : instance->members) {
                registryToLocal[member.registryEntity] = member.localEntity;
            }
            remapEntityProperties(otherScene, pending.entities, registryToLocal);
        } else {
            if (pending.decodeBranch && !pending.localRemap.empty()) {
                auto& sceneRemap = sceneEntityRemaps[pending.sceneId];
                for (const auto& [oldEntity, newEntity] : pending.localRemap) {
                    sceneRemap[oldEntity] = newEntity;
                }
            }
            if (!pending.recoveredEntry) {
                overrideExternalRefComponents(otherScene, *instance);
            }
        }

        pendingScene->isModified = true;
    }

    for (const auto& [otherSceneId, entityMap] : sceneEntityRemaps) {
        if (deferSourceRemap && otherSceneId == sceneId) {
            continue;
        }
        SceneProject* otherScene = getScene(otherSceneId);
        auto references = referenceEntitiesByScene.find(otherSceneId);
        if (!otherScene || !otherScene->scene || references == referenceEntitiesByScene.end()) {
            continue;
        }
        std::vector<Entity> liveReferences;
        std::unordered_set<Entity> seenReferences;
        for (Entity entity : references->second) {
            if (otherScene->scene->isEntityCreated(entity) && seenReferences.insert(entity).second) {
                liveReferences.push_back(entity);
            }
        }
        remapEntityProperties(otherScene->scene, liveReferences, entityMap, false);
    }

    prefab->isModified = true;

    return true;
}

editor::NodeRecovery editor::Project::removeEntityFromPrefab(uint32_t sceneId, Entity entity, bool destroyItself) {
    fs::path filepath = findPrefabPathFor(sceneId, entity);
    if (filepath.empty()) {
        Out::error("Entity %u in scene %u is not part of any prefab", entity, sceneId);
        return {};
    }

    Prefab* prefab = getPrefab(filepath);
    if (!prefab) {
        return {};
    }

    uint32_t instanceId = prefab->getInstanceId(sceneId, entity);

    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        return {};
    }

    Entity registryEntity = prefab->getRegistryEntity(sceneId, entity);
    if (registryEntity == NULL_ENTITY) {
        Out::error("Failed to find registry entity for prefab entity %u in scene %u", entity, sceneId);
        return {};
    }

    return removeRegistryEntityFromPrefab(filepath, registryEntity, sceneId, instanceId, destroyItself);
}

editor::NodeRecovery editor::Project::removeRegistryEntityFromPrefab(const std::filesystem::path& filepath, Entity registryEntity,
        uint32_t sceneId, uint32_t instanceId, bool destroyItself) {
    Prefab* prefab = getPrefab(filepath);
    if (!prefab || !prefab->registry || !prefab->registry->isEntityCreated(registryEntity)) {
        return {};
    }

    YAML::Node regData = Stream::encodeEntity(registryEntity, prefab->registry.get(), nullptr, nullptr);

    // Removed GameObject entries inside the branch go with it, and come back with an undo
    std::unordered_set<Entity> registryBranch;
    {
        std::vector<Entity> branchEntities;
        ProjectUtils::collectEntities(regData, branchEntities);
        registryBranch.insert(branchEntities.begin(), branchEntities.end());
    }

    size_t transformIndex;
    NodeRecovery recovery;

    if (prefab->registry->findComponent<Transform>(registryEntity)) {
        transformIndex = ProjectUtils::getTransformIndex(prefab->registry.get(), registryEntity);
    } else {
        auto regIt = std::find(prefab->registryEntities.begin(), prefab->registryEntities.end(), registryEntity);
        transformIndex = (regIt != prefab->registryEntities.end()) ? std::distance(prefab->registryEntities.begin(), regIt) : 0;
    }
    std::string recoveryDefKey = std::to_string(NULL_PROJECT_SCENE);
    recovery[recoveryDefKey] = {YAML::Clone(regData), transformIndex};

    // Registry entities behind members the engine cascade orphaned in the scene instances.
    // The shared registry has no systems and never cascades, so these are mirrored into the
    // registry removal below to keep the saved prefab definition consistent with the instances.
    std::unordered_set<Entity> cascadedRegistryEntities;

    // Process each scene that has instances
    for (auto& [otherSceneId, sceneInstances] : prefab->instances) {
        SceneProject* otherScene = getScene(otherSceneId);
        if (!otherScene || !otherScene->scene) {
            continue;
        }

        for (auto it = sceneInstances.rbegin(); it != sceneInstances.rend(); ++it) {
            auto& instance = *it;
            // Find the local entity for this registry entity in this instance
            Entity otherEntity = NULL_ENTITY;
            for (const auto& member : instance.members) {
                if (member.registryEntity == registryEntity) {
                    otherEntity = member.localEntity;
                    break;
                }
            }

            // The instance's removed GameObjects in the branch: the recovery keeps them
            std::unordered_map<Entity, std::vector<Prefab::EntityMember>> removedInBranch;
            for (auto removedIt = instance.removed.begin(); removedIt != instance.removed.end();) {
                if (registryBranch.count(removedIt->first)) {
                    removedInBranch.emplace(removedIt->first, std::move(removedIt->second));
                    removedIt = instance.removed.erase(removedIt);
                } else {
                    ++removedIt;
                }
            }

            if (otherEntity == NULL_ENTITY) {
                // Not in this instance (it removed the GameObject or one above it)
                if (!removedInBranch.empty()) {
                    std::string recoveryKey = std::to_string(otherSceneId) + "_" + std::to_string(instance.rootEntity);
                    recovery[recoveryKey].removed = std::move(removedInBranch);
                    otherScene->isModified = true;
                }
                continue;
            }

            const PrefabComponent* nestedComponent = otherScene->scene->findComponent<PrefabComponent>(otherEntity);
            const Prefab* nestedPrefab = nestedComponent ? getPrefab(nestedComponent->path) : nullptr;
            const Prefab::Instance* nestedInstance = nestedPrefab ?
                nestedPrefab->getInstance(otherSceneId, otherEntity) : nullptr;
            bool isNestedPrefabRoot = nestedInstance && nestedInstance->rootEntity == otherEntity;
            YAML::Node nodeExtend = Stream::encodeEntity(otherEntity, otherScene->scene,
                this, otherScene, !isNestedPrefabRoot);

            std::vector<Entity> allEntities;
            ProjectUtils::collectEntities(nodeExtend, allEntities);

            // Project-aware snapshots keep nested prefab branches opaque. Expand their live
            // instance members and hierarchy here so the scene-side removal still destroys
            // the complete branch while recovery remains prefab-aware.
            std::unordered_set<Entity> collectedEntities(allEntities.begin(), allEntities.end());
            // Expansion records each imported root before appending its members, preserving
            // the parent-before-child order used to resolve deeper roots during recovery.
            std::vector<NestedPrefabRecovery> nestedPrefabs;
            auto appendEntity = [&](Entity candidate) {
                if (candidate != NULL_ENTITY && otherScene->scene->isEntityCreated(candidate) &&
                    collectedEntities.insert(candidate).second) {
                    allEntities.push_back(candidate);
                }
            };
            auto transforms = otherScene->scene->getComponentArray<Transform>();
            for (size_t entityIndex = 0; entityIndex < allEntities.size(); ++entityIndex) {
                Entity branchEntity = allEntities[entityIndex];
                const PrefabComponent* component = otherScene->scene->findComponent<PrefabComponent>(branchEntity);
                const Prefab* childPrefab = component ? getPrefab(component->path) : nullptr;
                const Prefab::Instance* childInstance = childPrefab ?
                    childPrefab->getInstance(otherSceneId, branchEntity) : nullptr;
                if (childInstance && childInstance->rootEntity == branchEntity) {
                    nestedPrefabs.push_back({component->path, branchEntity, childInstance->members});
                    for (const Prefab::EntityMember& member : childInstance->members) {
                        appendEntity(member.localEntity);
                    }
                }

                for (size_t transformIndex = 0; transformIndex < transforms->size(); ++transformIndex) {
                    if (transforms->getComponentFromIndex(transformIndex).parent == branchEntity) {
                        appendEntity(transforms->getEntity(transformIndex));
                    }
                }
            }

            if (otherScene->scene->findComponent<Transform>(otherEntity)) {
                transformIndex = ProjectUtils::getTransformIndex(otherScene->scene, otherEntity);
            } else {
                auto entityIt = std::find(otherScene->entities.begin(), otherScene->entities.end(), otherEntity);
                transformIndex = (entityIt != otherScene->entities.end()) ? std::distance(otherScene->entities.begin(), entityIt) : 0;
            }

            std::string recoveryKey = std::to_string(otherSceneId) + "_" + std::to_string(instance.rootEntity);
            NodeRecoveryEntry& recoveryEntry = recovery[recoveryKey];
            recoveryEntry = {nodeExtend, transformIndex};
            recoveryEntry.nestedPrefabs = std::move(nestedPrefabs);
            recoveryEntry.removed = std::move(removedInBranch);

            // Remove members from this instance
            for (const Entity& e : allEntities) {
                auto itRem = std::find_if(instance.members.begin(), instance.members.end(),
                    [e](const Prefab::EntityMember& member) {
                        return member.localEntity == e;
                    });
                if (itRem != instance.members.end()) {
                    recoveryEntry.members.push_back(*itRem);
                    instance.members.erase(itRem);
                }

                auto override = instance.overrides.find(e);
                if (override != instance.overrides.end()) {
                    recoveryEntry.overrides[e] = override->second;
                }
                instance.overrides.erase(e);
            }

            // Destroy entities in other instances (or in this one if destroyItself)
            if ((otherSceneId != sceneId) || (instance.instanceId != instanceId) || destroyItself) {
                // Remove nested-instance metadata and suppress authored ownership callbacks
                // before any component removal can cascade into a later entity in the branch.
                for (const Entity& entityToDestroy : allEntities) {
                    removePrefabInstanceTracking(otherSceneId, entityToDestroy);
                    if (ActionComponent* action = otherScene->scene->findComponent<ActionComponent>(entityToDestroy)) {
                        action->ownedTarget = false;
                    }
                    if (AnimationComponent* animation = otherScene->scene->findComponent<AnimationComponent>(entityToDestroy)) {
                        animation->ownedActions = false;
                    }
                }
                // DeleteEntityCmd captures ownership dependencies explicitly in the source
                // scene. Other instance overrides must not broaden that shared deletion.
                DeleteEntityCmd::destroyEntities(otherScene->scene, allEntities,
                    otherScene->entities, this, otherSceneId);
            }

            // A destroy above can cascade inside the engine and take other members of this
            // instance with it. Drop any such missing member (and its override entry) to keep
            // the prefab instance metadata in sync with the scene.
            for (auto memberIt = instance.members.begin(); memberIt != instance.members.end(); ) {
                if (!otherScene->scene->isEntityCreated(memberIt->localEntity)) {
                    if (destroyItself && memberIt->registryEntity != NULL_ENTITY) {
                        cascadedRegistryEntities.insert(memberIt->registryEntity);
                    }
                    instance.overrides.erase(memberIt->localEntity);
                    memberIt = instance.members.erase(memberIt);
                } else {
                    ++memberIt;
                }
            }

            otherScene->isModified = true;
        }

        sceneInstances.erase(
            std::remove_if(sceneInstances.begin(), sceneInstances.end(),
                [](const Prefab::Instance& inst) {
                    return inst.members.empty() && inst.rootEntity == NULL_ENTITY;
                }),
            sceneInstances.end()
        );
    }

    for (auto it = prefab->instances.begin(); it != prefab->instances.end(); ) {
        if (it->second.empty()) {
            it = prefab->instances.erase(it);
        } else {
            ++it;
        }
    }

    // Destroy from registry
    std::vector<Entity> registryEntitiesToRemove;
    ProjectUtils::collectEntities(regData, registryEntitiesToRemove);

    // Mirror scene-side system cascades into the shared definition. The registry has no
    // systems and therefore does not perform the equivalent cleanup on its own.
    for (Entity regEntity : cascadedRegistryEntities) {
        registryEntitiesToRemove.push_back(regEntity);
    }

    DeleteEntityCmd::destroyEntities(prefab->registry.get(), registryEntitiesToRemove,
        prefab->registryEntities);

    prefab->isModified = true;

    return recovery;
}

bool editor::Project::prefabPropertyChanged(uint32_t sceneId, Entity entity, ComponentType componentType, std::vector<std::string> properties, bool changeItself){
    fs::path filepath = findPrefabPathFor(sceneId, entity);

    if (filepath.empty()) {
        Out::error("Entity %u in scene %u is not part of any prefab", entity, sceneId);
        return false;
    }

    Prefab* prefab = getPrefab(filepath);
    uint32_t instanceId = prefab->getInstanceId(sceneId, entity);

    // Prefab roots don't have registry entities; their properties are per-instance
    if (prefab->getRootEntity(sceneId, entity) == entity) {
        return true;
    }

    if (!prefab->hasComponentOverride(sceneId, entity, componentType)){
        Entity registryEntity = prefab->getRegistryEntity(sceneId, entity);
        if (registryEntity == NULL_ENTITY) {
            Out::error("Failed to find registry entity for prefab entity %u in scene %u", entity, sceneId);
            return false;
        }
        EntityRegistry* registry = prefab->registry.get();

        // Build the source instance's member map (the only shareable reference targets).
        std::unordered_map<Entity, Entity> localToRegistry;
        std::unordered_set<Entity> memberLocals;
        if (const Prefab::Instance* sourceInstance = prefab->getInstanceById(sceneId, instanceId)) {
            for (const auto& member : sourceInstance->members) {
                localToRegistry[member.localEntity] = member.registryEntity;
                memberLocals.insert(member.localEntity);
            }
        }

        if (componentHasExternalEntityRef(getScene(sceneId)->scene, entity, componentType, memberLocals)) {
            // The component now points outside the prefab, so it is per-instance data.
            // Promote it to an override on this instance and leave the shared registry
            // (and the other instances) untouched.
            prefab->setComponentOverride(sceneId, entity, componentType);
        } else {
            if (properties.size() == 0){
                Catalog::copyComponent(getScene(sceneId)->scene, entity, registry, registryEntity, componentType);
            }else{
                for (const auto& property : properties) {
                    Catalog::copyPropertyValue(getScene(sceneId)->scene, entity, registry, registryEntity, componentType, property);
                }
            }
            // Remap entity references from scene-local IDs to registry IDs.
            remapEntityPropertiesInComponent(registry, registryEntity, componentType, properties, localToRegistry);

            std::vector<uint32_t> stalePrefabScenes;
            for (auto& [otherSceneId, sceneInstances] : prefab->instances) {
                SceneProject* otherScene = getScene(otherSceneId);
                if (!otherScene) {
                    Out::error("Failed to find scene %u", otherSceneId);
                    stalePrefabScenes.push_back(otherSceneId);
                    continue;
                }
                if (!otherScene->scene) {
                    Out::error("Scene %u is not loaded", otherSceneId);
                    stalePrefabScenes.push_back(otherSceneId);
                    continue;
                }
                for (auto& instance : sceneInstances) {
                    if ((otherSceneId != sceneId) || (instance.instanceId != instanceId) || changeItself) {
                        Entity otherEntity = prefab->getLocalEntity(otherSceneId, instance.instanceId, registryEntity);
                        if (otherEntity == NULL_ENTITY) {
                            continue; // removed from that instance
                        }

                        // Instances that hold their own external reference on this component
                        // are overridden, so they are skipped here and keep their wiring.
                        if (!prefab->hasComponentOverride(otherSceneId, otherEntity, componentType)) {
                            if (otherScene->isVisible){
                                otherScene->needUpdateRender = true;
                            }
                            // Copy from registry (with correct registry IDs) to target scene
                            if (properties.size() == 0){
                                Catalog::copyComponent(registry, registryEntity, otherScene->scene, otherEntity, componentType);
                            }else{
                                for (const auto& property : properties) {
                                    Catalog::copyPropertyValue(registry, registryEntity, otherScene->scene, otherEntity, componentType, property);
                                }
                            }
                            // Remap entity references from registry IDs to target-local IDs
                            std::unordered_map<Entity, Entity> registryToOtherLocal;
                            for (const auto& member : instance.members) {
                                registryToOtherLocal[member.registryEntity] = member.localEntity;
                            }
                            remapEntityPropertiesInComponent(otherScene->scene, otherEntity, componentType, properties, registryToOtherLocal);
                        }
                    }
                }
            }
            for (uint32_t staleId : stalePrefabScenes) {
                cleanupPrefabsForScene(staleId);
            }
        }
    }

    prefab->isModified = true;

    return true;
}

bool editor::Project::prefabNameChanged(uint32_t sceneId, Entity entity, std::string name, bool changeItself){
    fs::path filepath = findPrefabPathFor(sceneId, entity);

    if (filepath.empty()) {
        Out::error("Entity %u in scene %u is not part of any prefab", entity, sceneId);
        return false;
    }

    Prefab* prefab = getPrefab(filepath);
    uint32_t instanceId = prefab->getInstanceId(sceneId, entity);

    // Prefab roots don't have registry entities; their name is per-instance
    if (prefab->getRootEntity(sceneId, entity) == entity) {
        return true;
    }

    Entity registryEntity = prefab->getRegistryEntity(sceneId, entity);
    if (registryEntity == NULL_ENTITY) {
        Out::error("Failed to find registry entity for prefab entity %u in scene %u", entity, sceneId);
        return false;
    }
    EntityRegistry* registry = prefab->registry.get();

    registry->setEntityName(registryEntity, name);

    std::vector<uint32_t> stalePrefabScenes;
    for (auto& [otherSceneId, sceneInstances] : prefab->instances) {
        SceneProject* otherScene = getScene(otherSceneId);
        if (!otherScene) {
            Out::error("Failed to find scene %u", otherSceneId);
            stalePrefabScenes.push_back(otherSceneId);
            continue;
        }
        if (!otherScene->scene) {
            Out::error("Scene %u is not loaded", otherSceneId);
            stalePrefabScenes.push_back(otherSceneId);
            continue;
        }
        for (auto& instance : sceneInstances) {
            if ((otherSceneId != sceneId) || (instance.instanceId != instanceId) || changeItself) {
                Entity otherEntity = prefab->getLocalEntity(otherSceneId, instance.instanceId, registryEntity);
                if (otherEntity == NULL_ENTITY) {
                    continue; // removed from that instance
                }

                otherScene->scene->setEntityName(otherEntity, name);
                otherScene->isModified = true;
            }
        }
    }
    for (uint32_t staleId : stalePrefabScenes) {
        cleanupPrefabsForScene(staleId);
    }

    prefab->isModified = true;

    return true;
}

editor::SharedMoveRecovery editor::Project::moveEntityFromPrefab(uint32_t sceneId, Entity entity, Entity target, InsertionType type, bool moveItself){
    fs::path filepath = findPrefabPathFor(sceneId, entity);
    if (filepath.empty()) {
        Out::error("Entity %u in scene %u is not part of any prefab", entity, sceneId);
        return {};
    }

    Prefab* prefab = getPrefab(filepath);
    uint32_t instanceId = prefab->getInstanceId(sceneId, entity);

    if (!isEntityInPrefab(sceneId, target)){
        if (type == InsertionType::INTO){
            Out::error("Cannot move prefab entity %u into non-prefab target %u in scene %u", entity, target, sceneId);
            return {};
        }

        auto& entities = getScene(sceneId)->entities;
        auto entityIt = std::find(entities.begin(), entities.end(), entity);
        auto targetIt = std::find(entities.begin(), entities.end(), target);

        if (entityIt != entities.end() && targetIt != entities.end()) {
            Entity nextPrefab = NULL_ENTITY;

            if (entityIt < targetIt) {
                for (auto it = targetIt - 1; it > entityIt; --it) {
                    if (isEntityInPrefab(sceneId, *it)) {
                        nextPrefab = *it;
                        break;
                    }
                }
            } else {
                for (auto it = targetIt + 1; it < entityIt; ++it) {
                    if (isEntityInPrefab(sceneId, *it)) {
                        nextPrefab = *it;
                        break;
                    }
                }
            }

            if (nextPrefab != NULL_ENTITY) {
                target = nextPrefab;
            }else{
                // Not need to move entity in other scenes and registry if target is not in prefab
                return {};
            }
        }
    }

    Entity registryEntity = prefab->getRegistryEntity(sceneId, entity);
    Entity registryTarget = prefab->getRegistryEntity(sceneId, target);
    if (registryEntity == NULL_ENTITY || registryTarget == NULL_ENTITY) {
        Out::error("Failed to find registry entities for prefab entities %u or %u in scene %u", entity, target, sceneId);
        return {};
    }

    SharedMoveRecovery recovery;

    Entity oldParent;
    size_t oldIndex;
    bool hasTransform;
    ProjectUtils::moveEntityOrderByTarget(prefab->registry.get(), prefab->registryEntities, registryEntity, registryTarget, type, oldParent, oldIndex, hasTransform);
    std::string recoveryDefKey = std::to_string(NULL_PROJECT_SCENE);
    recovery[recoveryDefKey] = {oldParent, oldIndex, hasTransform};

    std::vector<uint32_t> stalePrefabScenes;
    for (auto& [otherSceneId, sceneInstances] : prefab->instances) {
        SceneProject* otherScene = getScene(otherSceneId);
        if (!otherScene) {
            Out::error("Failed to find scene %u", otherSceneId);
            stalePrefabScenes.push_back(otherSceneId);
            continue;
        }
        if (!otherScene->scene) {
            Out::error("Scene %u is not loaded", otherSceneId);
            stalePrefabScenes.push_back(otherSceneId);
            continue;
        }
        // By id: taking a branch out or bringing one in changes the instance lists
        std::vector<uint32_t> instanceIds;
        for (const auto& instance : sceneInstances) {
            instanceIds.push_back(instance.instanceId);
        }
        for (uint32_t otherInstanceId : instanceIds) {
            Prefab::Instance* instance = prefab->getInstanceById(otherSceneId, otherInstanceId);
            if (!instance || ((otherSceneId == sceneId) && (otherInstanceId == instanceId) && !moveItself)) {
                continue;
            }
            Entity otherEntity = prefab->getLocalEntity(otherSceneId, otherInstanceId, registryEntity);
            Entity otherTarget = prefab->getLocalEntity(otherSceneId, otherInstanceId, registryTarget);
            // Where the Prefab puts it now, below a GameObject this instance removed or not
            const bool present = !prefab->isRegistryEntityAbsent(*instance, registryEntity);
            std::string recoveryKey = std::to_string(otherSceneId) + "_" + std::to_string(otherInstanceId);

            if (otherEntity != NULL_ENTITY && otherTarget != NULL_ENTITY && present) {
                Entity otherOldParent;
                size_t otherOldIndex;
                bool otherHasTransform;
                ProjectUtils::moveEntityOrderByTarget(otherScene->scene, otherScene->entities, otherEntity, otherTarget, type, otherOldParent, otherOldIndex, otherHasTransform);
                recovery[recoveryKey] = {otherOldParent, otherOldIndex, otherHasTransform};
                otherScene->isModified = true;
            } else if (otherEntity != NULL_ENTITY && !present) {
                // Moved below a GameObject the instance removed: it leaves the instance too
                SharedMoveRecoveryEntry entry{NULL_ENTITY, 0, false};
                entry.left = removePrefabBranch(otherSceneId, otherEntity, false);
                if (entry.left.valid()) {
                    recovery[recoveryKey] = std::move(entry);
                }
            } else if (otherEntity != NULL_ENTITY && present) {
                // The target is one the instance removed: the Prefab order places it
                Transform* transform = otherScene->scene->findComponent<Transform>(otherEntity);
                Entity newRegistryParent = prefab->getRegistryParent(registryEntity);
                Entity localParent = newRegistryParent == NULL_ENTITY ? instance->rootEntity :
                    prefab->getLocalEntity(otherSceneId, otherInstanceId, newRegistryParent);
                if (localParent == NULL_ENTITY) {
                    continue;
                }
                SharedMoveRecoveryEntry entry{NULL_ENTITY, 0, transform != nullptr};
                if (transform) {
                    entry.oldParent = transform->parent;
                    entry.oldIndex = ProjectUtils::getTransformIndex(otherScene->scene, otherEntity);
                } else {
                    auto tracked = std::find(otherScene->entities.begin(), otherScene->entities.end(), otherEntity);
                    entry.oldIndex = tracked != otherScene->entities.end() ? std::distance(otherScene->entities.begin(), tracked) : 0;
                }
                placeInPrefabOrder(otherScene, *prefab, *instance, registryEntity, otherEntity, localParent, true);
                recovery[recoveryKey] = std::move(entry);
                otherScene->isModified = true;
            } else if (otherEntity == NULL_ENTITY && present) {
                // Out from below a GameObject the instance removed: it comes into the instance
                SharedMoveRecoveryEntry entry{NULL_ENTITY, 0, false};
                std::vector<Prefab::EntityMember> entered = instantiatePrefabBranch(otherScene, filepath, otherInstanceId, registryEntity, {});
                if (!entered.empty()) {
                    entry.entered = entered[0].localEntity;
                    recovery[recoveryKey] = std::move(entry);
                }
            }
        }
    }
    for (uint32_t staleId : stalePrefabScenes) {
        cleanupPrefabsForScene(staleId);
    }

    prefab->isModified = true;

    return recovery;
}

bool editor::Project::undoMoveEntityInPrefab(uint32_t sceneId, Entity entity, Entity target, const SharedMoveRecovery& recovery, bool moveItself){
    fs::path filepath = findPrefabPathFor(sceneId, entity);
    if (filepath.empty()) {
        Out::error("Entity %u in scene %u is not part of any prefab", entity, sceneId);
        return false;
    }

    Prefab* prefab = getPrefab(filepath);
    uint32_t instanceId = prefab->getInstanceId(sceneId, entity);

    Entity registryEntity = prefab->getRegistryEntity(sceneId, entity);
    Entity registryTarget = prefab->getRegistryEntity(sceneId, target);
    if (registryEntity == NULL_ENTITY || registryTarget == NULL_ENTITY) {
        Out::error("Failed to find registry entities for prefab entities %u or %u in scene %u", entity, target, sceneId);
        return false;
    }

    std::string recoveryDefKey = std::to_string(NULL_PROJECT_SCENE);
    if (recovery.find(recoveryDefKey) == recovery.end()) {
        Out::error("No recovery data provided for undoing move of entity %u in scene %u", entity, sceneId);
        return false;
    }
    ProjectUtils::moveEntityOrderByIndex(prefab->registry.get(), prefab->registryEntities, registryEntity, recovery.at(recoveryDefKey).oldParent, recovery.at(recoveryDefKey).oldIndex, recovery.at(recoveryDefKey).hasTransform);

    std::vector<uint32_t> stalePrefabScenes;
    for (auto& [otherSceneId, sceneInstances] : prefab->instances) {
        SceneProject* otherScene = getScene(otherSceneId);
        if (!otherScene) {
            Out::error("Failed to find scene %u", otherSceneId);
            stalePrefabScenes.push_back(otherSceneId);
            continue;
        }
        if (!otherScene->scene) {
            Out::error("Scene %u is not loaded", otherSceneId);
            stalePrefabScenes.push_back(otherSceneId);
            continue;
        }
        // Inverting to get correct entity index; by id, as a branch going or coming back
        // changes the instance lists
        std::vector<uint32_t> instanceIds;
        for (auto it = sceneInstances.rbegin(); it != sceneInstances.rend(); ++it) {
            instanceIds.push_back(it->instanceId);
        }
        for (uint32_t otherInstanceId : instanceIds) {
            if ((otherSceneId == sceneId) && (otherInstanceId == instanceId) && !moveItself) {
                continue;
            }
            std::string recoveryKey = std::to_string(otherSceneId) + "_" + std::to_string(otherInstanceId);
            auto entryIt = recovery.find(recoveryKey);
            if (entryIt == recovery.end()) {
                continue;
            }
            const SharedMoveRecoveryEntry& entry = entryIt->second;
            if (entry.left.valid()) {
                // It left the instance with the move: back as it was
                restorePrefabBranch(otherSceneId, entry.left);
            } else if (entry.entered != NULL_ENTITY) {
                // It came into the instance with the move: out again
                if (otherScene->scene->isEntityCreated(entry.entered)) {
                    removePrefabBranch(otherSceneId, entry.entered, false);
                }
            } else {
                Entity otherEntity = prefab->getLocalEntity(otherSceneId, otherInstanceId, registryEntity);
                if (otherEntity != NULL_ENTITY) {
                    ProjectUtils::moveEntityOrderByIndex(otherScene->scene, otherScene->entities, otherEntity, entry.oldParent, entry.oldIndex, entry.hasTransform);
                }
            }
            otherScene->isModified = true;
        }
    }
    for (uint32_t staleId : stalePrefabScenes) {
        cleanupPrefabsForScene(staleId);
    }

    prefab->isModified = true;

    return true;
}
