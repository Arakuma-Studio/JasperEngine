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
using namespace jasperengine::editor::prefabinternal;

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

    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !entityNode || !entityNode.IsMap()) {
        return false;
    }
    Scene* scene = sceneProject->scene;

    // Unity's model: a prefab has one root GameObject, the selected one, which becomes the
    // instance's root. GameObjects without a Transform (an action's virtual children) can go
    // with it. Several roots make several prefabs (ProjectWindow::saveEntityFile splits them).
    std::vector<YAML::Node> topNodes;
    if (entityNode["members"] && entityNode["members"].IsSequence()) {
        for (YAML::Node member : entityNode["members"]) {
            topNodes.push_back(member);
        }
    } else {
        topNodes.push_back(entityNode);
    }
    const std::string transformKey = Catalog::getComponentName(ComponentType::Transform, true);
    const std::string prefabKey = Catalog::getComponentName(ComponentType::PrefabComponent, true);
    // (by index: a default yaml-cpp node is a defined Null one, and assigning to a node
    // writes into what it refers to)
    size_t rootIndex = topNodes.size();
    size_t transformRoots = 0;
    for (size_t i = 0; i < topNodes.size(); i++) {
        const YAML::Node components = topNodes[i]["components"];
        if (components && components.IsMap() && components[transformKey]) {
            transformRoots++;
            if (rootIndex == topNodes.size()) {
                rootIndex = i;
            }
        }
    }
    if (transformRoots > 1) {
        Out::error("A prefab has one root GameObject: make one prefab per selected root (%s)", filepath.string().c_str());
        return false;
    }
    if (rootIndex == topNodes.size()) {
        rootIndex = 0;
    }
    const YAML::Node rootNode = topNodes[rootIndex];
    if (!rootNode["entity"]) {
        Out::error("createPrefab(%s): the root GameObject has no id", filepath.string().c_str());
        return false;
    }
    const Entity rootLocal = rootNode["entity"].as<Entity>();
    if (!scene->isEntityCreated(rootLocal)) {
        Out::error("createPrefab(%s): GameObject %u is not in the scene", filepath.string().c_str(), rootLocal);
        return false;
    }
    if (isPrefabInstanceRoot(sceneId, rootLocal) || isEntityInPrefab(sceneId, rootLocal)) {
        Out::error("createPrefab(%s): '%s' is part of a prefab instance; unpack it first", filepath.string().c_str(),
            scene->getEntityName(rootLocal).c_str());
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

    // A clone: the node is the command's, and a redo reads its entity ids again. The root's
    // PrefabComponent (none: it is not an instance) and nested instances' blocks are not
    // GameObject data of the prefab.
    YAML::Node registryNode = clearEntitiesNode(YAML::Clone(entityNode));
    std::vector<Entity> regEntities = Stream::decodeEntitySelection(registryNode, newGroup.registry.get(), &newGroup.registryEntities);
    std::unordered_map<Entity, Entity> localToRegistry;
    if (branchEntities.size() == regEntities.size()) {
        for (size_t i = 0; i < branchEntities.size(); ++i) {
            localToRegistry[branchEntities[i]] = regEntities[i];
        }
        remapEntityProperties(newGroup.registry.get(), regEntities, localToRegistry);
    } else {
        Out::error("createPrefab(%s): entity count mismatch: branchEntities=%zu, regEntities=%zu",
            filepath.string().c_str(), branchEntities.size(), regEntities.size());
        remapEntityProperties(newGroup.registry.get(), regEntities, {});
        return false;
    }
    newGroup.root = localToRegistry[rootLocal];
    auto rootPosition = std::find(newGroup.registryEntities.begin(), newGroup.registryEntities.end(), newGroup.root);
    if (rootPosition != newGroup.registryEntities.end() && rootPosition != newGroup.registryEntities.begin()) {
        newGroup.registryEntities.erase(rootPosition);
        newGroup.registryEntities.insert(newGroup.registryEntities.begin(), newGroup.root);
    }

    // A new asset, a new GUID; a redo gives the file the one it had
    std::string assetGuid = guid;
    if (!AssetDatabase::isGuid(assetGuid) || !assetDatabase.getPath(assetGuid).empty()) {
        do {
            assetGuid = AssetDatabase::generateGuid();
        } while (!assetDatabase.getPath(assetGuid).empty());
    }

    // The selected GameObject is the instance root now
    PrefabComponent prefabComp;
    prefabComp.name = filepath.stem().string();
    prefabComp.path = filepath.string();
    prefabComp.guid = assetGuid;
    if (PrefabComponent* existing = scene->findComponent<PrefabComponent>(rootLocal)) {
        *existing = prefabComp;
    } else {
        scene->addComponent<PrefabComponent>(rootLocal, prefabComp);
    }

    newInstance.rootEntity = rootLocal;
    newInstance.rootRegistryEntity = newGroup.root;
    for (size_t i = 0; i < regEntities.size() && i < branchEntities.size(); i++) {
        if (branchEntities[i] != rootLocal) {
            newInstance.members.push_back({branchEntities[i], regEntities[i]});
        }
    }

    // References to entities outside the prefab stayed on the scene GameObjects while the
    // registry stored None; keep and persist them by overriding those components.
    overrideExternalRefComponents(scene, newInstance);

    newGroup.instances[sceneId].push_back(std::move(newInstance));

    auto [newIt, inserted] = prefabAssets.emplace(filepath, std::move(newGroup));
    Prefab& prefab = newIt->second;

    // The instances nested in the selection: what their scene data overrides of them is what
    // the new prefab overrides of them (asset-level data); their added GameObjects stay the
    // scene's
    for (const Prefab::EntityMember& member : prefab.instances[sceneId].front().members) {
        if (prefab.isNestedRoot(member.registryEntity) && isPrefabInstanceRoot(sceneId, member.localEntity)) {
            moveNestedInstanceDataToAsset(prefab, filepath, member.registryEntity, sceneProject, member.localEntity);
        }
    }

    // The file is written with the GUID the database gives its path
    assetDatabase.registerAsset(filepath, assetGuid);
    savePrefabToDisk(filepath);

    updateScenePrefabs(sceneProject);

    sceneProject->isModified = true;
    sceneProject->structureVersion++;

    return true;
}

void editor::Project::moveNestedInstanceDataToAsset(Prefab& prefab, const std::filesystem::path& prefabPath, Entity registryRoot,
        SceneProject* sceneProject, Entity localRoot) {
    Scene* scene = sceneProject->scene;
    const PrefabComponent* component = scene->findComponent<PrefabComponent>(localRoot);
    Prefab* nestedPrefab = component ? getPrefab(component->path) : nullptr;
    Prefab::Instance* nested = nestedPrefab ? nestedPrefab->getInstance(sceneProject->id, localRoot) : nullptr;
    if (!nested || nested->rootEntity != localRoot) {
        return;
    }
    // The nested instance's data as a scene would write it, without what is the scene's
    std::function<void(PrefabInstanceData&)> strip = [&](PrefabInstanceData& data) {
        data.memberIds.clear();
        data.addedGameObjects.clear();
        data.legacy = false;
        for (PrefabInstanceData& entry : data.nested) {
            strip(entry);
        }
    };
    PrefabInstanceData data = Stream::decodePrefabInstanceData(Stream::encodePrefabInstanceData(localRoot, scene, this, sceneProject, nestedPrefab));
    strip(data);
    data.present = true;
    data.legacy = false;
    data.nestedRoot = NULL_ENTITY;
    data.nestedPath.clear();
    if (!data.empty()) {
        prefab.nestedInstances[registryRoot] = data;
    } else {
        PrefabInstanceData empty;
        empty.present = true;
        prefab.nestedInstances[registryRoot] = empty;
    }
    (void)prefabPath;

    // The scene's instance no longer overrides what the asset now does
    std::function<void(Prefab::Instance&, size_t)> clear = [&](Prefab::Instance& instance, size_t depth) {
        instance.overrides.clear();
        instance.removed.clear();
        if (depth > 64) {
            return;
        }
        for (const Prefab::EntityMember& member : instance.members) {
            const PrefabComponent* memberComponent = scene->findComponent<PrefabComponent>(member.localEntity);
            Prefab* memberPrefab = memberComponent ? getPrefab(memberComponent->path) : nullptr;
            Prefab::Instance* inner = memberPrefab ? memberPrefab->getInstance(sceneProject->id, member.localEntity) : nullptr;
            if (inner && inner->rootEntity == member.localEntity) {
                clear(*inner, depth + 1);
            }
        }
    };
    clear(*nested, 0);
}

bool editor::Project::removePrefab(const std::filesystem::path& filepath) {
    auto it = prefabAssets.find(filepath);
    if (it == prefabAssets.end()) {
        return false;
    }

    // Undo scene-side changes introduced by createPrefab: each instance root is a plain
    // GameObject again, its members its plain children
    for (const auto& [sceneId, instances] : it->second.instances) {
        SceneProject* sceneProject = getScene(sceneId);
        if (!sceneProject || !sceneProject->scene) {
            continue;
        }
        Scene* scene = sceneProject->scene;
        for (const auto& instance : instances) {
            if (instance.rootEntity == NULL_ENTITY || !scene->isEntityCreated(instance.rootEntity)) {
                continue;
            }
            // the instances nested in it stay instances: only its own link goes, with the asset
            if (scene->findComponent<PrefabComponent>(instance.rootEntity)) {
                scene->removeComponent<PrefabComponent>(instance.rootEntity);
            }
            sceneProject->structureVersion++;
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
    SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || !findPrefabObject(sceneId, entity, ref)) {
        Out::error("Entity %u in scene %u is not part of any prefab", entity, sceneId);
        return false;
    }

    // A component added to an instance root is that instance's (Unity's added component),
    // or one of the prefab root's it removed coming back
    if (ref.isRoot) {
        Scene* scene = sceneProject->scene;
        if (!hasComponent(scene, entity, componentType)) {
            YAML::Node node;
            auto saved = recovery.find("root");
            if (saved != recovery.end() && saved->second.entity == entity) {
                node = saved->second.node;
            }
            ProjectUtils::addEntityComponent(scene, entity, componentType, sceneProject->entities, node);
        }
        Prefab::ObjectOverrides& overrides = ref.instance->overrides[entity];
        if (hasComponent(ref.prefab->registry.get(), ref.registryEntity, componentType)) {
            overrides.removedComponents.erase(componentType);
        } else {
            overrides.addedComponents.insert(componentType);
        }
        if (overrides.empty()) {
            ref.instance->overrides.erase(entity);
        }
        sceneProject->isModified = true;
        return true;
    }

    fs::path filepath = ref.path;
    Prefab* prefab = ref.prefab;
    uint32_t instanceId = ref.instance->instanceId;

    if (prefab->hasComponentOverride(sceneId, entity, componentType)){
        Out::warning("Component %s of entity %u in scene %u is overridden", Catalog::getComponentName(componentType).c_str(), entity, sceneId);
        return false;
    }

    Entity registryEntity = ref.registryEntity;

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
    recordNestedRootAssetOverride(*prefab, filepath, registryEntity, componentType, {});

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
                Entity otherEntity = instance.getLocalEntity(registryEntity);
                if (otherEntity == NULL_ENTITY) {
                    continue;
                }
                auto overrides = instance.overrides.find(otherEntity);
                if (overrides != instance.overrides.end() && overrides->second.addedComponents.count(componentType)) {
                    // The instance had added one already: its values stay, overriding the prefab's
                    overrides->second.addedComponents.erase(componentType);
                    overrides->second.addWholeComponent(componentType);
                    otherScene->isModified = true;
                    continue;
                }
                if (hasComponent(otherScene->scene, otherEntity, componentType)) {
                    continue;
                }
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
    for (uint32_t staleId : stalePrefabScenes) {
        cleanupPrefabsForScene(staleId);
    }

    prefab->isModified = true;

    return true;
}

editor::ComponentRecovery editor::Project::removeComponentFromPrefab(uint32_t sceneId, Entity entity, ComponentType componentType, bool encodeComponent, bool removeToItself){
    SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || !findPrefabObject(sceneId, entity, ref)) {
        Out::error("Entity %u in scene %u is not part of any prefab", entity, sceneId);
        return {};
    }

    // From an instance root: a removed component of the prefab root, or an added one gone
    if (ref.isRoot) {
        ComponentRecovery recovery;
        Scene* scene = sceneProject->scene;
        if (removeToItself && hasComponent(scene, entity, componentType)) {
            recovery["root"].entity = entity;
            recovery["root"].node = ProjectUtils::removeEntityComponent(scene, entity, componentType, sceneProject->entities, encodeComponent);
            ProjectUtils::reconcileTrackedEntities(scene, sceneProject->entities, this, sceneId);
        }
        Prefab::ObjectOverrides& overrides = ref.instance->overrides[entity];
        overrides.properties.erase(componentType);
        if (!overrides.addedComponents.erase(componentType) &&
            hasComponent(ref.prefab->registry.get(), ref.registryEntity, componentType)) {
            overrides.removedComponents.insert(componentType);
        }
        if (overrides.empty()) {
            ref.instance->overrides.erase(entity);
        }
        if (recovery.empty()) {
            // the caller removed it: the undo only needs to know it came from a root
            recovery["root"].entity = entity;
        }
        sceneProject->isModified = true;
        return recovery;
    }

    fs::path filepath = ref.path;
    Prefab* prefab = ref.prefab;
    uint32_t instanceId = ref.instance->instanceId;

    if (prefab->hasComponentOverride(sceneId, entity, componentType)){
        Out::warning("Component %s of entity %u in scene %u is overridden", Catalog::getComponentName(componentType).c_str(), entity, sceneId);
        return {};
    }

    Entity registryEntity = ref.registryEntity;

    // Off a nested root of this prefab: this prefab removes it from the nested prefab's root
    if (prefab->isNestedRoot(registryEntity)) {
        const PrefabComponent* component = prefab->registry->findComponent<PrefabComponent>(registryEntity);
        Prefab* nested = component ? loadPrefabAsset(component->path) : nullptr;
        if (nested && nested != prefab) {
            PrefabInstanceData* layer = nestedAssetLayer(*prefab, filepath, registryEntity, NULL_ENTITY, nullptr);
            PrefabObjectOverrides& overrides = layer->objects[nested->root].overrides;
            overrides.properties.erase(componentType);
            if (!overrides.addedComponents.erase(componentType) && hasComponent(nested->registry.get(), nested->root, componentType)) {
                overrides.removedComponents.insert(componentType);
            }
        }
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
                Entity otherEntity = instance.getLocalEntity(registryEntity);
                if (otherEntity == NULL_ENTITY) {
                    continue;
                }
                auto overrides = instance.overrides.find(otherEntity);
                if (overrides != instance.overrides.end()) {
                    // removed here already: nothing left to keep out
                    overrides->second.removedComponents.erase(componentType);
                    if (overrides->second.properties.erase(componentType)) {
                        // An instance overriding it keeps its values, as a component of its own
                        overrides->second.addedComponents.insert(componentType);
                        otherScene->isModified = true;
                        continue;
                    }
                    if (overrides->second.empty()) {
                        instance.overrides.erase(overrides);
                    }
                }
                if (!hasComponent(otherScene->scene, otherEntity, componentType)) {
                    continue;
                }
                std::string recoveryKey = std::to_string(otherSceneId) + "_" + std::to_string(instance.instanceId);
                recovery[recoveryKey].entity = otherEntity;
                recovery[recoveryKey].node = ProjectUtils::removeEntityComponent(otherScene->scene, otherEntity, componentType, otherScene->entities, encodeComponent);

                otherScene->isModified = true;
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

    // The parent's registry entity, the prefab root's for the instance root
    Entity registryParent = prefab->getObjectRegistryEntity(sceneId, parent);

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

    // A prefab instance put in: what its scene data overrides of it is this prefab's now
    if (isNestedPrefab && isPrefabInstanceRoot(sceneId, entity)) {
        moveNestedInstanceDataToAsset(*prefab, filepath, regEntities[0], sceneProject, entity);
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
            Entity otherParent = instance.getLocalEntity(registryParent);
            if (otherParent == NULL_ENTITY) {
                otherParent = instance.rootEntity;
            }

            if ((otherSceneId != sceneId) || (instance.instanceId != instanceId) || createItself) {

                YAML::Node nodeData = Stream::encodeEntity(regEntities[0], prefab->registry.get());
                newOtherEntities = Stream::decodeEntity(nodeData, otherScene->scene, &otherScene->entities);
                if (hasTransform && !newOtherEntities.empty()) {
                    ProjectUtils::moveEntityOrderByTransform(otherScene->scene, otherScene->entities, newOtherEntities[0], otherParent, 0, false);
                }

                // For nested prefabs, import the prefab contents for this instance too, with
                // what this prefab overrides of it
                if (isNestedPrefab && !newOtherEntities.empty()) {
                    PrefabComponent* bc = otherScene->scene->findComponent<PrefabComponent>(newOtherEntities[0]);
                    if (bc) {
                        const std::string nestedPath = bc->path;
                        std::vector<PrefabInstanceData*> layers{nestedAssetLayer(*prefab, filepath, regEntities[0], newOtherEntities[0], otherScene)};
                        importPrefab(otherScene, &otherScene->entities, nestedPath, newOtherEntities[0], false, nullptr, nullptr, nullptr, layers);
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

    return restoreRegistryEntityToPrefab(filepath, prefab->getObjectRegistryEntity(sceneId, parent), recoveryData,
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

void jasperengine::editor::prefabinternal::copyPrefabValueToInstance(EntityRegistry* registry, Entity registryEntity, EntityRegistry* scene, Entity local,
        ComponentType type, const std::vector<std::string>& properties, const PrefabObjectOverrides* overrides, bool isRoot,
        const std::unordered_map<Entity, Entity>& registryToLocal) {
    if ((overrides && overrides->isWholeComponent(type)) || !hasComponent(scene, local, type) || !hasComponent(registry, registryEntity, type)) {
        return;
    }
    if (!properties.empty()) {
        for (const std::string& property : properties) {
            if ((overrides && overrides->hasProperty(type, property)) || (isRoot && Project::isPrefabRootDefaultOverride(type, property))) {
                continue;
            }
            Catalog::copyPropertyValue(registry, registryEntity, scene, local, type, property);
            Project::remapEntityPropertiesInComponent(scene, local, type, {property}, registryToLocal);
        }
        return;
    }
    std::set<std::string> keep;
    if (overrides) {
        auto it = overrides->properties.find(type);
        if (it != overrides->properties.end()) {
            keep = it->second;
        }
    }
    if (isRoot) {
        for (auto& [name, data] : Catalog::findEntityProperties(scene, local, type)) {
            if (Project::isPrefabRootDefaultOverride(type, name)) {
                keep.insert(name);
            }
        }
    }
    ScratchEntity saved;
    if (!keep.empty()) {
        saved.takeComponents(scene, local, Catalog::componentTypeToSignature(scene, type));
    }
    Catalog::copyComponent(registry, registryEntity, scene, local, type);
    Project::remapEntityPropertiesInComponent(scene, local, type, {}, registryToLocal);
    if (!keep.empty()) {
        copyProperties(saved, scene, local, type, keep, "");
    }
}

namespace {

    std::unordered_map<Entity, Entity> registryToLocalOf(const editor::Prefab::Instance& instance) {
        std::unordered_map<Entity, Entity> map;
        map[instance.rootRegistryEntity] = instance.rootEntity;
        for (const editor::Prefab::EntityMember& member : instance.members) {
            map[member.registryEntity] = member.localEntity;
        }
        return map;
    }

}

void editor::Project::syncRootPropertyOverrides(uint32_t sceneId, Entity root, ComponentType type, const std::vector<std::string>& properties) {
    SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || !findPrefabObject(sceneId, root, ref) || !ref.isRoot ||
        type == ComponentType::PrefabComponent) {
        return;
    }
    Scene* scene = sceneProject->scene;
    if (!hasComponent(scene, root, type)) {
        return;
    }
    ScratchEntity source;
    if (!buildPrefabSource(sceneId, root, type, source)) {
        // the prefab root has no such component: the instance added it
        recordAddedComponent(sceneId, root, type);
        return;
    }
    Prefab::ObjectOverrides& overrides = ref.instance->overrides[root];
    if (properties.empty()) {
        // the whole component: the same as the prefab's, the default overrides aside?
        ScratchEntity own;
        own.takeComponents(scene, root, Catalog::componentTypeToSignature(scene, type));
        std::set<std::string> defaults;
        for (auto& [name, data] : Catalog::findEntityProperties(own.registry.get(), own.entity, type)) {
            if (isPrefabRootDefaultOverride(type, name)) {
                defaults.insert(name);
            }
        }
        copyProperties(own, source.registry.get(), source.entity, type, defaults, ref.path.string());
        if (YAML::Dump(encodeComponent(scene, root, type)) == YAML::Dump(encodeComponent(source.registry.get(), source.entity, type))) {
            overrides.properties.erase(type);
        } else {
            overrides.addWholeComponent(type);
        }
    } else if (!overrides.isWholeComponent(type)) {
        for (const std::string& property : properties) {
            if (isPrefabRootDefaultOverride(type, property)) {
                continue;
            }
            const PropertyData own = Catalog::findProperty(scene, root, type, property);
            const PropertyData prefabValue = Catalog::findProperty(source.registry.get(), source.entity, type, property);
            if (!own.ref || !prefabValue.ref) {
                continue;
            }
            if (samePropertyValue(own, prefabValue)) {
                auto names = overrides.properties.find(type);
                if (names != overrides.properties.end()) {
                    names->second.erase(property);
                    if (names->second.empty()) {
                        overrides.properties.erase(names);
                    }
                }
            } else {
                overrides.addProperty(type, property);
            }
        }
    }
    if (overrides.empty()) {
        ref.instance->overrides.erase(root);
    }
    sceneProject->isModified = true;
}

bool editor::Project::prefabPropertyChanged(uint32_t sceneId, Entity entity, ComponentType componentType, std::vector<std::string> properties, bool changeItself){
    SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || !findPrefabObject(sceneId, entity, ref)) {
        Out::error("Entity %u in scene %u is not part of any prefab", entity, sceneId);
        return false;
    }

    // An instance root's values are the instance's (default overrides, and overrides of the
    // prefab root): what now differs from the prefab is an override
    if (ref.isRoot) {
        syncRootPropertyOverrides(sceneId, entity, componentType, properties);
        return true;
    }

    Prefab* prefab = ref.prefab;
    const fs::path filepath = ref.path;
    const uint32_t instanceId = ref.instance->instanceId;
    const Entity registryEntity = ref.registryEntity;
    EntityRegistry* registry = prefab->registry.get();
    Scene* scene = sceneProject->scene;

    // A component the instance added is its own
    if (!hasComponent(registry, registryEntity, componentType)) {
        return true;
    }

    // What this instance overrides stays its own
    const Prefab::ObjectOverrides* own = ref.instance->findOverrides(entity);
    if (own && own->isWholeComponent(componentType)) {
        return true;
    }
    std::vector<std::string> shared;
    if (properties.empty()) {
        if (own && own->hasComponent(componentType)) {
            // a change to all of a component it overrides in part: all of it is its own now
            ref.instance->overrides[entity].addWholeComponent(componentType);
            sceneProject->isModified = true;
            return true;
        }
    } else {
        for (const std::string& property : properties) {
            if (!own || !own->hasProperty(componentType, property)) {
                shared.push_back(property);
            }
        }
        if (shared.empty()) {
            return true;
        }
    }

    // Build the source instance's member map (the only shareable reference targets).
    std::unordered_map<Entity, Entity> localToRegistry;
    std::unordered_set<Entity> memberLocals;
    localToRegistry[ref.instance->rootEntity] = ref.instance->rootRegistryEntity;
    memberLocals.insert(ref.instance->rootEntity);
    for (const auto& member : ref.instance->members) {
        localToRegistry[member.localEntity] = member.registryEntity;
        memberLocals.insert(member.localEntity);
    }

    if (componentHasExternalEntityRef(scene, entity, componentType, memberLocals)) {
        // The component now points outside the prefab, so it is per-instance data.
        // Promote it to an override on this instance and leave the shared registry
        // (and the other instances) untouched.
        prefab->setComponentOverride(sceneId, entity, componentType);
        sceneProject->isModified = true;
        return true;
    }

    if (shared.empty()){
        Catalog::copyComponent(scene, entity, registry, registryEntity, componentType);
    }else{
        for (const auto& property : shared) {
            Catalog::copyPropertyValue(scene, entity, registry, registryEntity, componentType, property);
        }
    }
    // Remap entity references from scene-local IDs to registry IDs.
    remapEntityPropertiesInComponent(registry, registryEntity, componentType, shared, localToRegistry);
    // A nested root's values in this prefab are this prefab's override of the nested one
    recordNestedRootAssetOverride(*prefab, filepath, registryEntity, componentType, shared);

    std::vector<uint32_t> stalePrefabScenes;
    for (auto& [otherSceneId, sceneInstances] : prefab->instances) {
        SceneProject* otherScene = getScene(otherSceneId);
        if (!otherScene || !otherScene->scene) {
            Out::error("Scene %u is not loaded", otherSceneId);
            stalePrefabScenes.push_back(otherSceneId);
            continue;
        }
        for (auto& instance : sceneInstances) {
            if ((otherSceneId == sceneId) && (instance.instanceId == instanceId) && !changeItself) {
                continue;
            }
            Entity otherEntity = instance.getLocalEntity(registryEntity);
            if (otherEntity == NULL_ENTITY || !otherScene->scene->isEntityCreated(otherEntity)) {
                continue; // removed from that instance
            }
            // Instances overriding these keep their values
            copyPrefabValueToInstance(registry, registryEntity, otherScene->scene, otherEntity, componentType, shared,
                instance.findOverrides(otherEntity), false, registryToLocalOf(instance));
            if (otherScene->isVisible){
                otherScene->needUpdateRender = true;
            }
        }
    }
    for (uint32_t staleId : stalePrefabScenes) {
        cleanupPrefabsForScene(staleId);
    }

    prefab->isModified = true;

    return true;
}

bool editor::Project::prefabNameChanged(uint32_t sceneId, Entity entity, std::string name, bool changeItself){
    PrefabObjectRef ref;
    if (!findPrefabObject(sceneId, entity, ref)) {
        Out::error("Entity %u in scene %u is not part of any prefab", entity, sceneId);
        return false;
    }

    // An instance root's name is the instance's (a default override), and so is one the
    // instance overrides
    const Prefab::ObjectOverrides* own = ref.instance->findOverrides(entity);
    if (ref.isRoot || (own && (own->state & PrefabObjectOverrides::StateName))) {
        return true;
    }

    Prefab* prefab = ref.prefab;
    const uint32_t instanceId = ref.instance->instanceId;
    const Entity registryEntity = ref.registryEntity;
    prefab->registry->setEntityName(registryEntity, name);

    std::vector<uint32_t> stalePrefabScenes;
    for (auto& [otherSceneId, sceneInstances] : prefab->instances) {
        SceneProject* otherScene = getScene(otherSceneId);
        if (!otherScene || !otherScene->scene) {
            Out::error("Scene %u is not loaded", otherSceneId);
            stalePrefabScenes.push_back(otherSceneId);
            continue;
        }
        for (auto& instance : sceneInstances) {
            if ((otherSceneId == sceneId) && (instance.instanceId == instanceId) && !changeItself) {
                continue;
            }
            Entity otherEntity = instance.getLocalEntity(registryEntity);
            if (otherEntity == NULL_ENTITY) {
                continue; // removed from that instance
            }
            const Prefab::ObjectOverrides* overrides = instance.findOverrides(otherEntity);
            if (overrides && (overrides->state & PrefabObjectOverrides::StateName)) {
                continue;
            }
            otherScene->scene->setEntityName(otherEntity, name);
            otherScene->isModified = true;
            otherScene->structureVersion++;
        }
    }
    for (uint32_t staleId : stalePrefabScenes) {
        cleanupPrefabsForScene(staleId);
    }

    prefab->isModified = true;

    return true;
}

bool editor::Project::prefabStateChanged(uint32_t sceneId, Entity entity, uint8_t stateBits, bool changeItself) {
    SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || !findPrefabObject(sceneId, entity, ref)) {
        return false;
    }
    stateBits &= ~PrefabObjectOverrides::StateName;
    Scene* scene = sceneProject->scene;
    EntityRegistry* registry = ref.prefab->registry.get();

    if (ref.isRoot) {
        // The root's state is an override where it differs from the prefab root's
        Prefab::ObjectOverrides& overrides = ref.instance->overrides[entity];
        auto sync = [&](uint8_t bit, bool same) {
            if (!(stateBits & bit)) return;
            if (same) overrides.state &= ~bit; else overrides.state |= bit;
        };
        sync(PrefabObjectOverrides::StateActive, scene->isEntityActive(entity) == registry->isEntityActive(ref.registryEntity));
        sync(PrefabObjectOverrides::StateTag, scene->getEntityTag(entity) == registry->getEntityTag(ref.registryEntity));
        sync(PrefabObjectOverrides::StateLayer, scene->getEntityLayer(entity) == registry->getEntityLayer(ref.registryEntity));
        if (overrides.empty()) {
            ref.instance->overrides.erase(entity);
        }
        sceneProject->isModified = true;
        return true;
    }

    // Shared like a name, but what this instance overrides stays its own
    const Prefab::ObjectOverrides* own = ref.instance->findOverrides(entity);
    if (own) {
        stateBits &= ~own->state;
    }
    if (stateBits == 0) {
        return true;
    }
    if (stateBits & PrefabObjectOverrides::StateActive) registry->setEntityActive(ref.registryEntity, scene->isEntityActive(entity));
    if (stateBits & PrefabObjectOverrides::StateTag) registry->setEntityTag(ref.registryEntity, scene->getEntityTag(entity));
    if (stateBits & PrefabObjectOverrides::StateLayer) registry->setEntityLayer(ref.registryEntity, scene->getEntityLayer(entity));
    // A nested root's state in this prefab is this prefab's override of the nested one
    if (ref.prefab->isNestedRoot(ref.registryEntity)) {
        const PrefabComponent* component = registry->findComponent<PrefabComponent>(ref.registryEntity);
        Prefab* nested = component ? loadPrefabAsset(component->path) : nullptr;
        if (nested && nested != ref.prefab) {
            nestedAssetLayer(*ref.prefab, ref.path, ref.registryEntity, NULL_ENTITY, nullptr)->objects[nested->root].overrides.state |= stateBits;
        }
    }

    const uint32_t instanceId = ref.instance->instanceId;
    for (auto& [otherSceneId, sceneInstances] : ref.prefab->instances) {
        SceneProject* otherScene = getScene(otherSceneId);
        if (!otherScene || !otherScene->scene) {
            continue;
        }
        for (auto& instance : sceneInstances) {
            if ((otherSceneId == sceneId) && (instance.instanceId == instanceId) && !changeItself) {
                continue;
            }
            Entity otherEntity = instance.getLocalEntity(ref.registryEntity);
            if (otherEntity == NULL_ENTITY || !otherScene->scene->isEntityCreated(otherEntity)) {
                continue;
            }
            const Prefab::ObjectOverrides* overrides = instance.findOverrides(otherEntity);
            const uint8_t bits = stateBits & ~(overrides ? overrides->state : 0);
            if (bits & PrefabObjectOverrides::StateActive) otherScene->scene->setEntityActive(otherEntity, registry->isEntityActive(ref.registryEntity));
            if (bits & PrefabObjectOverrides::StateTag) otherScene->scene->setEntityTag(otherEntity, registry->getEntityTag(ref.registryEntity));
            if (bits & PrefabObjectOverrides::StateLayer) otherScene->scene->setEntityLayer(otherEntity, registry->getEntityLayer(ref.registryEntity));
            if (bits != 0) {
                otherScene->isModified = true;
                otherScene->structureVersion++;
            }
        }
    }
    ref.prefab->isModified = true;
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
    Entity registryTarget = prefab->getObjectRegistryEntity(sceneId, target);
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
    Entity registryTarget = prefab->getObjectRegistryEntity(sceneId, target);
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
