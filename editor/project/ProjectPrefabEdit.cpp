// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Prefab asset editing propagated to every instance: creating/removing prefabs, adding and
// removing GameObjects, order changes. Value, name, state and component edits on instances
// are instance overrides (ProjectPrefabInstanceEdit.cpp).

#include "Project.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/Transform.h"
#include "component/PrefabComponent.h"
#include "ProjectPrefabInternal.h"
#include "Out.h"
#include "Stream.h"
#include "command/type/DeleteEntityCmd.h"
#include "util/ProjectUtils.h"
#include "util/CameraTextureLink.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <set>
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

    // A clone: the node is the command's, and a redo reads its entity ids again. Encoded
    // with the project, the instances nested in the selection are their roots only (their
    // GameObjects are their prefabs'); their blocks are not GameObject data of the prefab.
    // References stay the scene's until every GameObject of the new prefab is known.
    YAML::Node registryNode = clearEntitiesNode(YAML::Clone(entityNode));
    std::vector<Entity> regEntities = Stream::decodeEntitySelection(registryNode, newGroup.registry.get(), &newGroup.registryEntities);
    if (branchEntities.size() != regEntities.size()) {
        Out::error("createPrefab(%s): entity count mismatch: branchEntities=%zu, regEntities=%zu",
            filepath.string().c_str(), branchEntities.size(), regEntities.size());
        return false;
    }
    std::unordered_map<Entity, Entity> localToRegistry;
    for (size_t i = 0; i < branchEntities.size(); ++i) {
        localToRegistry[branchEntities[i]] = regEntities[i];
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
    std::vector<Prefab::EntityMember> nestedRoots;
    for (size_t i = 0; i < regEntities.size() && i < branchEntities.size(); i++) {
        if (branchEntities[i] != rootLocal) {
            newInstance.members.push_back({branchEntities[i], regEntities[i]});
            if (newGroup.isNestedRoot(regEntities[i])) {
                nestedRoots.push_back({branchEntities[i], regEntities[i]});
            }
        }
    }

    newGroup.instances[sceneId].push_back(std::move(newInstance));

    auto [newIt, inserted] = prefabAssets.emplace(filepath, std::move(newGroup));
    Prefab& prefab = newIt->second;

    // The instances nested in the selection: what their scene data overrides of them, and the
    // GameObjects added to them, are the new prefab's now (Unity: nested prefabs keep their
    // overrides, as the prefab's)
    moveNestedInstancesToAsset(prefab, filepath, sceneProject, rootLocal, nestedRoots);

    // References: the scene's ids become the prefab's (its GameObjects, and stripped objects
    // for the ones of the instances nested in it); anything outside is None in the prefab
    // and stays on the instance as an override
    for (Entity registryEntity : std::vector<Entity>(prefab.registryEntities)) {
        for (ComponentType type : Catalog::findComponents(prefab.registry.get(), registryEntity)) {
            if (type != ComponentType::PrefabComponent && Catalog::mayHoldReferences(prefab.registry.get(), registryEntity, type, true, true)) {
                remapReferencesToPrefab(sceneId, rootLocal, prefab.registry.get(), registryEntity, type);
            }
        }
    }
    overrideOutsideReferences(sceneId, rootLocal);

    // The file is written with the GUID the database gives its path
    assetDatabase.registerAsset(filepath, assetGuid);
    savePrefabToDisk(filepath);

    updateScenePrefabs(sceneProject);

    sceneProject->isModified = true;
    sceneProject->structureVersion++;

    return true;
}

void editor::Project::overrideOutsideReferences(uint32_t sceneId, Entity instanceRoot, const std::filesystem::path& level,
        const std::unordered_set<Entity>* only) {
    SceneProject* sceneProject = getScene(sceneId);
    Prefab::Instance* instance = findInstanceAt(sceneProject, instanceRoot, level);
    if (!instance) {
        return;
    }
    // References to the GameObjects of the instances nested in it are the prefab's too
    const std::unordered_set<Entity> inside = collectInstanceTreeObjects(sceneId, instanceRoot, level);
    // (the root's overrides are the outermost level's: a variant's base level leaves it)
    std::vector<Entity> objects;
    if (findInstanceRootedAt(sceneProject, instanceRoot) == instance) {
        objects.push_back(instance->rootEntity);
    }
    for (const Prefab::EntityMember& member : instance->members) {
        objects.push_back(member.localEntity);
    }
    for (Entity object : objects) {
        if (!sceneProject->scene->isEntityCreated(object) || (only && !only->count(object))) {
            continue;
        }
        for (ComponentType componentType : Catalog::findComponents(sceneProject->scene, object)) {
            if (componentType != ComponentType::PrefabComponent &&
                componentHasExternalEntityRef(sceneProject->scene, object, componentType, inside)) {
                instance->overrides[object].addWholeComponent(componentType);
            }
        }
    }
}

namespace {

    // The scene GameObjects added (not GameObjects of any instance) right below a GameObject
    // of `inside`, in Transform order
    std::vector<Entity> addedBelow(Scene* scene, const std::unordered_set<Entity>& inside, const std::unordered_set<Entity>& instanceObjects) {
        std::vector<Entity> tops;
        auto transforms = scene->getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++) {
            const Entity entity = transforms->getEntity(i);
            const Entity parent = transforms->getComponentFromIndex(i).parent;
            if (parent != NULL_ENTITY && inside.count(parent) && !inside.count(entity) && !instanceObjects.count(entity)) {
                tops.push_back(entity);
            }
        }
        return tops;
    }

    int childIndexOf(Scene* scene, Entity parent, Entity child) {
        auto transforms = scene->getComponentArray<Transform>();
        int index = 0;
        for (size_t i = transforms->getIndex(parent) + 1; i < transforms->size(); i++) {
            const Entity entity = transforms->getEntity(i);
            if (entity == child) {
                return index;
            }
            if (transforms->getComponentFromIndex(i).parent == parent) {
                index++;
            }
        }
        return -1;
    }

}

void editor::Project::moveNestedInstancesToAsset(Prefab& prefab, const std::filesystem::path& prefabPath, SceneProject* sceneProject,
        Entity holderRoot, const std::vector<Prefab::EntityMember>& nestedRoots) {

    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;
    Prefab::Instance* holder = findInstanceAt(sceneProject, holderRoot, prefabPath);
    if (!holder) {
        return;
    }
    const uint32_t holderId = holder->instanceId;

    // 1. The GameObjects added in the scene to these instances (at any depth) are the
    //    prefab's GameObjects now: registry children of the nested root, below the nested
    //    GameObject a stripped object names, at their place among its children. Prefab
    //    instances among them are nested in the prefab too, and go the same way.
    // (each nested root's prefab is the one this prefab's registry names: for a variant's
    // root, its base, at the same GameObject as the variant's instance)
    auto nestedPathOf = [&](const Prefab::EntityMember& nested) {
        return instancePathAt(prefab, nested.registryEntity);
    };
    std::vector<Prefab::EntityMember> all = nestedRoots;
    for (size_t next = 0; next < all.size(); next++) {
        const Prefab::EntityMember nested = all[next];
        const std::filesystem::path nestedPath = nestedPathOf(nested);
        if (!findInstanceAt(sceneProject, nested.localEntity, nestedPath)) {
            continue;
        }
        const std::unordered_set<Entity> inside = collectInstanceTreeObjects(sceneId, nested.localEntity, nestedPath);
        std::unordered_set<Entity> instanceObjects;
        for (const auto& [path, other] : prefabAssets) {
            auto sceneIt = other.instances.find(sceneId);
            if (sceneIt == other.instances.end()) continue;
            for (const Prefab::Instance& instance : sceneIt->second) {
                for (const Prefab::EntityMember& member : instance.members) {
                    instanceObjects.insert(member.localEntity);
                }
            }
        }
        for (Entity top : addedBelow(scene, inside, instanceObjects)) {
            const Entity parentLocal = transformParent(scene, top);
            const int childIndex = childIndexOf(scene, parentLocal, top);
            YAML::Node node = Stream::encodeEntity(top, scene, this, sceneProject);
            std::vector<Entity> localBranch;
            ProjectUtils::collectEntities(node, localBranch);
            YAML::Node regNode = clearEntitiesNode(YAML::Clone(node));
            std::vector<Entity> regBranch = Stream::decodeEntity(regNode, prefab.registry.get(), &prefab.registryEntities);
            if (regBranch.empty() || regBranch.size() != localBranch.size()) {
                Out::error("Prefab %s: GameObject '%s' added to a nested instance could not be put in it",
                    prefabPath.string().c_str(), scene->getEntityName(top).c_str());
                DeleteEntityCmd::destroyEntities(prefab.registry.get(), regBranch, prefab.registryEntities);
                continue;
            }
            if (prefab.registry->findComponent<Transform>(regBranch[0])) {
                ProjectUtils::moveEntityOrderByTransform(prefab.registry.get(), prefab.registryEntities, regBranch[0], nested.registryEntity, 0, false);
            }
            Prefab::Instance* instance = prefab.getInstanceById(sceneId, holderId);
            if (!instance) {
                return;
            }
            for (size_t i = 0; i < regBranch.size(); i++) {
                instance->members.push_back({localBranch[i], regBranch[i]});
                if (prefab.isNestedRoot(regBranch[i]) && findInstanceRootedAt(sceneProject, localBranch[i])) {
                    all.push_back({localBranch[i], regBranch[i]});
                }
            }
            Prefab::AddedObject added;
            added.childIndex = childIndex;
            if (parentLocal != nested.localEntity) {
                added.parent = makePrefabReference(sceneId, holderRoot, parentLocal, prefabPath);
            }
            prefab.addedObjects[regBranch[0]] = added;
        }
    }

    // 2. What the scene's data of each overrides (at any depth) is the prefab's data for
    //    that nested instance, references in the prefab's ids. A component referring to
    //    something outside stays overridden in the scene.
    std::set<std::pair<Entity, ComponentType>> kept;
    std::function<PrefabInstanceData(Entity, const std::filesystem::path&, bool, size_t)> capture =
            [&](Entity root, const std::filesystem::path& levelPath, bool top, size_t depth) {
        PrefabInstanceData data;
        data.present = true;
        std::filesystem::path path;
        Prefab::Instance* instance = findInstanceAt(sceneProject, root, levelPath, &path);
        if (!instance || depth > 64) {
            return data;
        }
        const Prefab::Instance snapshot = *instance;
        for (const auto& [local, objectOverrides] : snapshot.overrides) {
            if (local == snapshot.rootEntity) {
                if (top) {
                    // the holder's registry entity holds the values (its nested root)
                    data.objects[snapshot.rootRegistryEntity].overrides = objectOverrides;
                }
                continue;
            }
            const Entity registryEntity = snapshot.getRegistryEntity(local);
            if (registryEntity == NULL_ENTITY || !scene->isEntityCreated(local)) {
                continue;
            }
            PrefabInstanceData::Object object;
            object.overrides = objectOverrides;
            YAML::Node values(YAML::NodeType::Map);
            if (objectOverrides.state & PrefabObjectOverrides::StateName) values["name"] = scene->getEntityName(local);
            if (objectOverrides.state & PrefabObjectOverrides::StateActive) values["active"] = scene->isEntityActive(local);
            if (objectOverrides.state & PrefabObjectOverrides::StateTag) values["tag"] = scene->getEntityTag(local);
            if (objectOverrides.state & PrefabObjectOverrides::StateLayer) values["layer"] = scene->getEntityLayer(local);
            std::set<ComponentType> types = objectOverrides.addedComponents;
            for (const auto& [type, names] : objectOverrides.properties) {
                types.insert(type);
            }
            for (ComponentType type : types) {
                bool outside = false;
                YAML::Node value = encodeComponentForPrefab(sceneId, holderRoot, local, type, &outside, prefabPath);
                if (outside) {
                    kept.insert({local, type});
                    object.overrides.properties.erase(type);
                    object.overrides.addedComponents.erase(type);
                    continue;
                }
                if (value) {
                    values["components"][componentKey(type)] = value;
                }
            }
            if (values.size() > 0) {
                object.values = values;
            }
            if (!object.overrides.empty()) {
                data.objects[registryEntity] = object;
            }
        }
        for (const auto& [registryEntity, members] : snapshot.removed) {
            data.removedGameObjects.insert(registryEntity);
        }
        // the instances nested in it (its base's at its root, for a variant)
        for (const NestedInstanceLevel& level : getNestedInstanceLevels(sceneProject, path, snapshot)) {
            PrefabInstanceData entry;
            if (level.instance) {
                entry = capture(level.localRoot, level.path, false, depth + 1);
                entry.nestedPath = level.path.string();
            } else if (const MissingPrefabInstance* missing = getMissingPrefab(sceneId, level.localRoot)) {
                // a Missing Prefab keeps what it was loaded with
                entry = Stream::decodePrefabInstanceData(missing->data);
                entry.memberIds.clear();
                entry.addedGameObjects.clear();
                entry.nestedPath = missing->path;
            } else {
                continue;
            }
            entry.nestedRoot = level.registryEntity;
            if (!entry.empty()) {
                data.nested.push_back(std::move(entry));
            }
        }
        return data;
    };
    // the scene's data goes, what refers outside stays
    std::function<void(Entity, const std::filesystem::path&, size_t)> clear = [&](Entity root, const std::filesystem::path& levelPath, size_t depth) {
        std::filesystem::path path;
        Prefab::Instance* instance = findInstanceAt(sceneProject, root, levelPath, &path);
        if (!instance || depth > 64) {
            return;
        }
        for (auto it = instance->overrides.begin(); it != instance->overrides.end();) {
            Prefab::ObjectOverrides keep;
            for (const auto& [type, names] : it->second.properties) {
                if (kept.count({it->first, type})) keep.properties[type] = names;
            }
            for (ComponentType type : it->second.addedComponents) {
                if (kept.count({it->first, type})) keep.addedComponents.insert(type);
            }
            if (keep.empty() || it->first == instance->rootEntity) {
                it = instance->overrides.erase(it);
            } else {
                it->second = keep;
                ++it;
            }
        }
        instance->removed.clear();
        std::vector<std::pair<Entity, std::filesystem::path>> inner;
        for (const NestedInstanceLevel& level : getNestedInstanceLevels(sceneProject, path, *instance)) {
            if (level.instance) {
                inner.emplace_back(level.localRoot, level.path);
            }
        }
        for (const auto& [member, memberPath] : inner) {
            clear(member, memberPath, depth + 1);
        }
    };
    for (const Prefab::EntityMember& nested : all) {
        PrefabInstanceData data;
        const std::filesystem::path nestedPath = nestedPathOf(nested);
        if (findInstanceAt(sceneProject, nested.localEntity, nestedPath)) {
            data = capture(nested.localEntity, nestedPath, true, 0);
        } else if (const MissingPrefabInstance* missing = getMissingPrefab(sceneId, nested.localEntity)) {
            data = Stream::decodePrefabInstanceData(missing->data);
            data.memberIds.clear();
            data.addedGameObjects.clear();
        }
        std::function<void(PrefabInstanceData&)> strip = [&](PrefabInstanceData& entry) {
            entry.memberIds.clear();
            entry.addedGameObjects.clear();
            entry.legacy = false;
            for (PrefabInstanceData& inner : entry.nested) {
                strip(inner);
            }
        };
        strip(data);
        data.present = true;
        data.nestedRoot = NULL_ENTITY;
        data.nestedPath.clear();
        prefab.nestedInstances[nested.registryEntity] = std::move(data);
    }
    for (const Prefab::EntityMember& nested : all) {
        clear(nested.localEntity, nestedPathOf(nested), 0);
    }
    prefab.isModified = true;
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

bool editor::Project::canAddToPrefab(uint32_t sceneId, Entity entity, const std::filesystem::path& target, std::string* reason) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || target.empty() || !sceneProject->scene->isEntityCreated(entity)) {
        return true;
    }
    Scene* scene = sceneProject->scene;
    const std::string forbidden = target.lexically_normal().generic_string();
    // Whether the prefab at path is target or holds it: its nested instances' prefabs and, for a
    // variant, its bases, at any depth
    std::unordered_set<std::string> visited;
    std::function<bool(const std::filesystem::path&)> holds = [&](const std::filesystem::path& path) -> bool {
        const std::string key = path.lexically_normal().generic_string();
        if (key == forbidden) {
            return true;
        }
        if (!visited.insert(key).second) {
            return false;
        }
        const std::vector<std::filesystem::path> chain = getPrefabVariantChain(path);
        for (size_t i = 1; i < chain.size(); i++) {
            if (holds(chain[i])) {
                return true;
            }
        }
        Prefab* prefab = loadPrefabAsset(path);
        if (!prefab || !prefab->registry) {
            return false;
        }
        auto components = prefab->registry->getComponentArray<PrefabComponent>();
        std::vector<std::filesystem::path> nested;
        for (size_t i = 0; i < components->size(); i++) {
            nested.push_back(components->getComponentFromIndex(i).path);
        }
        for (const std::filesystem::path& nestedPath : nested) {
            if (!nestedPath.empty() && holds(nestedPath)) {
                return true;
            }
        }
        return false;
    };
    // The branch: the GameObject and every one below it (one run of the Transform array)
    std::vector<Entity> branch{entity};
    if (scene->findComponent<Transform>(entity)) {
        auto transforms = scene->getComponentArray<Transform>();
        const size_t first = transforms->getIndex(entity);
        const size_t last = scene->findBranchLastIndex(entity);
        for (size_t i = first + 1; i <= last; i++) {
            branch.push_back(transforms->getEntity(i));
        }
    }
    for (Entity current : branch) {
        const PrefabComponent* component = scene->findComponent<PrefabComponent>(current);
        if (!component || component->path.empty()) {
            continue;
        }
        const std::string path = component->path;
        if (holds(path)) {
            if (reason) {
                *reason = "'" + scene->getEntityName(entity) + "' cannot go into " + target.generic_string() + ": '" +
                    scene->getEntityName(current) + "' in it is an instance of " + path +
                    (std::filesystem::path(path).lexically_normal().generic_string() == forbidden ? "" : ", which holds " + target.generic_string()) +
                    ", so the prefab would contain itself";
            }
            return false;
        }
    }
    return true;
}

bool editor::Project::addEntityToPrefab(uint32_t sceneId, Entity entity, Entity parent, bool createItself, const std::filesystem::path& target){
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene){
        return false;
    }
    // Into a prefab holding the parent's instance: a GameObject it adds to that nested instance.
    // Below the root of an instance of a variant, into any level of it (the variant, its base,
    // ...: the root GameObject of each, PREFABS.md 19).
    fs::path filepath;
    if (!target.empty()) {
        const std::vector<std::filesystem::path> rootLevels = getInstanceRootLevels(sceneId, parent);
        if (std::find(rootLevels.begin(), rootLevels.end(), target) != rootLevels.end()) {
            filepath = target;
        } else {
            PrefabObjectRef ref;
            std::filesystem::path own;
            if (findPrefabObject(sceneId, parent, ref)) {
                own = ref.path;
                if (!ref.isRoot && isPrefabInstanceRoot(sceneId, parent)) {
                    if (const PrefabComponent* component = sceneProject->scene->findComponent<PrefabComponent>(parent)) {
                        own = component->path;
                    }
                }
            }
            if (target != own) {
                return addEntityToOuterPrefab(sceneId, entity, parent, target);
            }
        }
    }

    Scene* scene = sceneProject->scene;

    // Find which prefab the parent belongs to
    // First check if parent is a prefab root (for explicit "Insert to Prefab" targeting): the
    // outermost level of the instances rooted there
    if (filepath.empty()) {
        findInstanceRootedAt(sceneProject, parent, &filepath);
    }
    for (const auto& [prefabPath, prefab] : prefabAssets) {
        if (!filepath.empty()) break;
        auto sceneIt = prefab.instances.find(sceneId);
        if (sceneIt == prefab.instances.end()) continue;
        for (const auto& instance : sceneIt->second) {
            if (instance.rootEntity == parent) {
                filepath = prefabPath;
                break;
            }
        }
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
    if (std::string reason; !canAddToPrefab(sceneId, entity, filepath, &reason)) {
        Out::error("%s", reason.c_str());
        return false;
    }
    uint32_t instanceId = prefab->getInstanceId(sceneId, parent);
    const Prefab::Instance* sourceInstance = prefab->getInstanceById(sceneId, instanceId);
    if (instanceId == 0 || !sourceInstance) {
        Out::error("Failed to find instance for parent entity %u in scene %u", parent, sceneId);
        return false;
    }
    const Entity instanceRoot = sourceInstance->rootEntity;
    if (sourceInstance->getRegistryEntity(entity) != NULL_ENTITY) {
        Out::error("GameObject %u is in the prefab already", entity);
        return false;
    }

    // The parent's registry entity, the prefab root's for the instance root
    Entity registryParent = prefab->getObjectRegistryEntity(sceneId, parent);

    // The branch with the project: the prefab instances in it stay instances (nested in the
    // prefab now), their GameObjects are theirs
    YAML::Node nodeOriginal = Stream::encodeEntity(entity, scene, this, sceneProject);
    YAML::Node nodeRegData = clearEntitiesNode(YAML::Clone(nodeOriginal));

    // Decode into registry
    std::vector<Entity> regEntities = Stream::decodeEntity(nodeRegData, prefab->registry.get(), &prefab->registryEntities);
    std::vector<Entity> srcBranch;
    ProjectUtils::collectEntities(nodeOriginal, srcBranch);
    if (regEntities.empty() || regEntities.size() != srcBranch.size()) {
        Out::error("Failed to decode entity into prefab registry");
        DeleteEntityCmd::destroyEntities(prefab->registry.get(), regEntities, prefab->registryEntities);
        return false;
    }

    // Position in registry if parent has a registry entity
    if (registryParent != NULL_ENTITY && prefab->registry->findComponent<Transform>(regEntities[0])) {
        ProjectUtils::moveEntityOrderByTransform(prefab->registry.get(), prefab->registryEntities, regEntities[0], registryParent, 0, false);
    }

    // The source instance has them already: they are its GameObjects now
    std::vector<Prefab::EntityMember> nestedRoots;
    if (Prefab::Instance* instance = prefab->getInstanceById(sceneId, instanceId)) {
        for (size_t i = 0; i < regEntities.size(); i++) {
            instance->members.push_back({srcBranch[i], regEntities[i]});
            if (prefab->isNestedRoot(regEntities[i])) {
                nestedRoots.push_back({srcBranch[i], regEntities[i]});
            }
        }
    }
    // Place it under its parent in the scene, as in the prefab
    if (scene->findComponent<Transform>(entity) && scene->findComponent<Transform>(parent) && transformParent(scene, entity) != parent) {
        ProjectUtils::moveEntityOrderByTransform(scene, sceneProject->entities, entity, parent, 0, false);
    }

    // Prefab instances put in: what their scene data overrides of them, and the GameObjects
    // added to them, are this prefab's now
    const size_t before = prefab->registryEntities.size();
    moveNestedInstancesToAsset(*prefab, filepath, sceneProject, instanceRoot, nestedRoots);
    std::vector<Entity> newRegistry = regEntities;
    for (size_t i = before; i < prefab->registryEntities.size(); i++) {
        newRegistry.push_back(prefab->registryEntities[i]);
    }

    // References: the scene's ids become the prefab's (its GameObjects, stripped objects for
    // the ones of its nested instances); anything outside is None in the prefab and stays
    // on this instance as an override
    for (Entity registryEntity : newRegistry) {
        if (!prefab->registry->isEntityCreated(registryEntity)) {
            continue;
        }
        for (ComponentType type : Catalog::findComponents(prefab->registry.get(), registryEntity)) {
            if (type != ComponentType::PrefabComponent && Catalog::mayHoldReferences(prefab->registry.get(), registryEntity, type, true, true)) {
                remapReferencesToPrefab(sceneId, instanceRoot, prefab->registry.get(), registryEntity, type, {}, filepath);
            }
        }
    }
    // (the GameObjects put in: what the instance had of its own already is not this one's to
    // change, and for a variant's base level a reference the variant's data gives is no
    // reference outside)
    const std::unordered_set<Entity> putIn(srcBranch.begin(), srcBranch.end());
    overrideOutsideReferences(sceneId, instanceRoot, filepath, &putIn);
    prefab->isModified = true;
    sceneProject->isModified = true;

    // Every instance gets them, from the asset (each keeps its ids, overrides and added
    // GameObjects; the source instance gets them back as they are)
    (void)createItself;
    refreshPrefabInstances(filepath);

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
        YAML::Node node;   // the branch decoded (its local Transform values are put back)
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
            decodeBranch, nodeData});
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
            // the move kept a world placement not computed yet: the local values come back
            const YAML::Node readNode = pending.node;
            const YAML::Node savedTransform = hasTransform && pending.decodeBranch && readNode.IsMap() && readNode["components"] ?
                readNode["components"]["transform"] : YAML::Node();
            Transform* transform = otherScene->findComponent<Transform>(pending.entities[0]);
            if (transform && savedTransform && savedTransform.IsMap()) {
                const Transform saved = Stream::decodeTransform(savedTransform, transform);
                transform->position = saved.position;
                transform->rotation = saved.rotation;
                transform->scale = saved.scale;
                transform->needUpdate = true;
            }
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
                // textures elsewhere in that scene may show a camera or a fluid going with them
                const bool hadLinkSource = std::any_of(allEntities.begin(), allEntities.end(), [&](Entity entity) {
                    return CameraTextureLink::isLinkSource(otherScene->scene, entity);
                });
                DeleteEntityCmd::destroyEntities(otherScene->scene, allEntities,
                    otherScene->entities, this, otherSceneId);
                if (hadLinkSource) {
                    CameraTextureLink::resolve(otherScene->scene);
                }
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

bool editor::Project::addEntityToOuterPrefab(uint32_t sceneId, Entity entity, Entity parent, const std::filesystem::path& target) {
    SceneProject* sceneProject = getScene(sceneId);
    Prefab* prefab = getPrefab(target);
    if (!sceneProject || !sceneProject->scene || !prefab || !prefab->registry) {
        return false;
    }
    Scene* scene = sceneProject->scene;
    if (std::string reason; !canAddToPrefab(sceneId, entity, target, &reason)) {
        Out::error("%s", reason.c_str());
        return false;
    }

    // The instance of `target` holding parent, and its nested root parent is below (at any
    // depth): the GameObject becomes one `target` adds to that nested instance
    PrefabObjectRef ref;
    if (!findPrefabObject(sceneId, parent, ref)) {
        Out::error("GameObject %u is not part of a prefab instance", parent);
        return false;
    }
    Entity owningRoot = ref.instance->rootEntity;
    std::filesystem::path owningPath = ref.path;
    if (ref.isRoot == false && isPrefabInstanceRoot(sceneId, parent)) {
        owningRoot = parent; // a nested root: below it in its own instance
        owningPath.clear();
    }
    const Entity holderRoot = findHolderInstanceRoot(sceneId, owningRoot, target, owningPath);
    Prefab::Instance* holder = holderRoot != NULL_ENTITY ? findInstanceAt(sceneProject, holderRoot, target) : nullptr;
    if (!holder || (holderRoot == owningRoot && (owningPath.empty() || owningPath == target))) {
        Out::error("%s does not hold the instance GameObject %u is in", target.string().c_str(), parent);
        return false;
    }
    // the instance nested in the holder that holds the parent: one of its members' or, for
    // a variant, its base's at its root
    Entity nestedLocal = NULL_ENTITY;
    Entity nestedRegistry = NULL_ENTITY;
    for (const NestedInstanceLevel& level : getNestedInstanceLevels(sceneProject, target, *holder)) {
        if (level.instance && collectInstanceTreeObjects(sceneId, level.localRoot, level.path).count(parent)) {
            nestedLocal = level.localRoot;
            nestedRegistry = level.registryEntity;
            break;
        }
    }
    if (nestedRegistry == NULL_ENTITY) {
        Out::error("%s does not hold the instance GameObject %u is in", target.string().c_str(), parent);
        return false;
    }
    const uint32_t holderId = holder->instanceId;

    // Below its parent in the scene, where the instance shows it
    if (scene->findComponent<Transform>(entity) && scene->findComponent<Transform>(parent) && transformParent(scene, entity) != parent) {
        ProjectUtils::moveEntityOrderByTransform(scene, sceneProject->entities, entity, parent, 0, false);
    }
    const int childIndex = scene->findComponent<Transform>(entity) ? [&]() {
        auto transforms = scene->getComponentArray<Transform>();
        int index = 0;
        for (size_t i = transforms->getIndex(parent) + 1; i < transforms->size(); i++) {
            if (transforms->getEntity(i) == entity) return index;
            if (transforms->getComponentFromIndex(i).parent == parent) index++;
        }
        return -1;
    }() : -1;

    // The branch with the project (its prefab instances stay instances), into the registry
    // below the nested root
    YAML::Node nodeOriginal = Stream::encodeEntity(entity, scene, this, sceneProject);
    YAML::Node nodeRegData = clearEntitiesNode(YAML::Clone(nodeOriginal));
    std::vector<Entity> regEntities = Stream::decodeEntity(nodeRegData, prefab->registry.get(), &prefab->registryEntities);
    std::vector<Entity> srcBranch;
    ProjectUtils::collectEntities(nodeOriginal, srcBranch);
    if (regEntities.empty() || regEntities.size() != srcBranch.size()) {
        Out::error("Failed to decode entity into prefab registry");
        DeleteEntityCmd::destroyEntities(prefab->registry.get(), regEntities, prefab->registryEntities);
        return false;
    }
    if (prefab->registry->findComponent<Transform>(regEntities[0])) {
        ProjectUtils::moveEntityOrderByTransform(prefab->registry.get(), prefab->registryEntities, regEntities[0], nestedRegistry, 0, false);
    }
    std::vector<Prefab::EntityMember> nestedRoots;
    if (Prefab::Instance* instance = prefab->getInstanceById(sceneId, holderId)) {
        for (size_t i = 0; i < regEntities.size(); i++) {
            instance->members.push_back({srcBranch[i], regEntities[i]});
            if (prefab->isNestedRoot(regEntities[i])) {
                nestedRoots.push_back({srcBranch[i], regEntities[i]});
            }
        }
    }
    Prefab::AddedObject added;
    added.childIndex = childIndex;
    if (parent != nestedLocal) {
        added.parent = makePrefabReference(sceneId, holderRoot, parent, target);
    }
    prefab->addedObjects[regEntities[0]] = added;

    const size_t before = prefab->registryEntities.size();
    moveNestedInstancesToAsset(*prefab, target, sceneProject, holderRoot, nestedRoots);
    std::vector<Entity> newRegistry = regEntities;
    for (size_t i = before; i < prefab->registryEntities.size(); i++) {
        newRegistry.push_back(prefab->registryEntities[i]);
    }
    for (Entity registryEntity : newRegistry) {
        if (!prefab->registry->isEntityCreated(registryEntity)) {
            continue;
        }
        for (ComponentType type : Catalog::findComponents(prefab->registry.get(), registryEntity)) {
            if (type != ComponentType::PrefabComponent && Catalog::mayHoldReferences(prefab->registry.get(), registryEntity, type, true, true)) {
                remapReferencesToPrefab(sceneId, holderRoot, prefab->registry.get(), registryEntity, type, {}, target);
            }
        }
    }
    const std::unordered_set<Entity> putIn(srcBranch.begin(), srcBranch.end());
    overrideOutsideReferences(sceneId, holderRoot, target, &putIn);
    prefab->isModified = true;
    sceneProject->isModified = true;
    refreshPrefabInstances(target);
    return true;
}
