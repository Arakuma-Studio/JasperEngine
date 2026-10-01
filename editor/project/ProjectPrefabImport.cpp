// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Prefab instantiation: importing/unimporting instances into scenes, instantiating
// removed branches back, prefab order placement, and the Play-mode prefab factories.

#include "Project.h"
#include "ProjectPrefabInternal.h"
#include "Factory.h"
#include "Out.h"
#include "Stream.h"
#include "PrefabManager.h"
#include "command/type/DeleteEntityCmd.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectUtils.h"
#include "util/YamlNumbers.h"

#include <algorithm>
#include <functional>
#include <set>
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
            info.guid = getPrefabGuid(prefabPath);
            sceneProject->prefabs.push_back(std::move(info));
        }
    }

    // Missing Prefabs stay listed (by their GUID): the scene still uses them, and its
    // generated source is written again when they come back (no file, no build entry)
    for (const auto& [root, missing] : sceneProject->missingPrefabs) {
        if (missing.path.empty() || !getMissingPrefab(sceneProject->id, root)) {
            continue;
        }
        const bool listed = std::any_of(sceneProject->prefabs.begin(), sceneProject->prefabs.end(),
            [&missing](const PrefabSceneInfo& info) {
                return (!missing.guid.empty() && info.guid == missing.guid) || info.prefabPath == fs::path(missing.path);
            });
        if (!listed) {
            PrefabSceneInfo info;
            info.prefabPath = missing.path;
            info.functionName = Factory::prefabToFunctionName(info.prefabPath);
            info.guid = missing.guid;
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

namespace jasperengine::editor::prefabinternal {

    bool hasComponent(const EntityRegistry* registry, Entity entity, ComponentType type) {
        return registry->isEntityCreated(entity) &&
            (registry->getSignature(entity) & Catalog::componentTypeToSignature(registry, type)).any();
    }

    Entity transformParent(EntityRegistry* registry, Entity entity) {
        const Transform* transform = registry->findComponent<Transform>(entity);
        return transform ? transform->parent : NULL_ENTITY;
    }

    std::string componentKey(ComponentType type) {
        return Catalog::getComponentName(type, true);
    }

    YAML::Node singleComponent(ComponentType type, const YAML::Node& value) {
        YAML::Node node(YAML::NodeType::Map);
        node[componentKey(type)] = value;
        return node;
    }

    YAML::Node encodeComponent(const EntityRegistry* registry, Entity entity, ComponentType type) {
        YAML::Node components = Stream::encodeComponents(entity, registry, Catalog::componentTypeToSignature(registry, type));
        return components.IsMap() ? components[componentKey(type)] : YAML::Node(YAML::NodeType::Undefined);
    }

    bool samePropertyValue(const PropertyData& a, const PropertyData& b) {
        if (!a.ref || !b.ref || a.type != b.type) {
            return false;
        }
        switch (a.type) {
            case PropertyType::Bool: return *static_cast<bool*>(a.ref) == *static_cast<bool*>(b.ref);
            case PropertyType::String: return *static_cast<std::string*>(a.ref) == *static_cast<std::string*>(b.ref);
            case PropertyType::Float: return *static_cast<float*>(a.ref) == *static_cast<float*>(b.ref);
            case PropertyType::Double: return *static_cast<double*>(a.ref) == *static_cast<double*>(b.ref);
            case PropertyType::Vector2: return *static_cast<Vector2*>(a.ref) == *static_cast<Vector2*>(b.ref);
            case PropertyType::Vector3: return *static_cast<Vector3*>(a.ref) == *static_cast<Vector3*>(b.ref);
            case PropertyType::Vector4: return *static_cast<Vector4*>(a.ref) == *static_cast<Vector4*>(b.ref);
            case PropertyType::Quat: return *static_cast<Quaternion*>(a.ref) == *static_cast<Quaternion*>(b.ref);
            case PropertyType::Int: return *static_cast<int*>(a.ref) == *static_cast<int*>(b.ref);
            case PropertyType::UInt: return *static_cast<unsigned int*>(a.ref) == *static_cast<unsigned int*>(b.ref);
            case PropertyType::Enum: return *static_cast<int*>(a.ref) == *static_cast<int*>(b.ref);
            case PropertyType::Entity: return *static_cast<Entity*>(a.ref) == *static_cast<Entity*>(b.ref);
            case PropertyType::EntityReference: {
                const EntityReference* ra = static_cast<EntityReference*>(a.ref);
                const EntityReference* rb = static_cast<EntityReference*>(b.ref);
                return ra->entity == rb->entity && ra->sceneId == rb->sceneId;
            }
            default: return false;
        }
    }

    // An instance root written before the real-root model (its components and state were
    // the per-instance wrapper's), or a nested root of such a prefab file, against the
    // prefab's root GameObject. The default overrides (name, position, rotation,
    // RectTransform placement) are the instance's own anyway.
    PrefabObjectOverrides diffAgainstRoot(EntityRegistry* own, Entity ownEntity, EntityRegistry* asset, Entity assetRoot) {
        PrefabObjectOverrides overrides;
        const ComponentType prefabType = ComponentType::PrefabComponent;
        std::vector<ComponentType> assetTypes = Catalog::findComponents(asset, assetRoot);
        for (ComponentType type : assetTypes) {
            if (type == prefabType) {
                continue;
            }
            if (!hasComponent(own, ownEntity, type)) {
                overrides.removedComponents.insert(type);
                continue;
            }
            if (type == ComponentType::Transform || type == ComponentType::RectTransformComponent) {
                // property by property, these only hold plain values
                for (auto& [name, assetProperty] : Catalog::findEntityProperties(asset, assetRoot, type)) {
                    if (Project::isPrefabRootDefaultOverride(type, name)) {
                        continue;
                    }
                    PropertyData ownProperty = Catalog::findProperty(own, ownEntity, type, name);
                    if (!samePropertyValue(ownProperty, assetProperty)) {
                        overrides.addProperty(type, name);
                    }
                }
                continue;
            }
            const std::string ownText = YAML::Dump(encodeComponent(own, ownEntity, type));
            const std::string assetText = YAML::Dump(encodeComponent(asset, assetRoot, type));
            if (ownText != assetText) {
                overrides.addWholeComponent(type);
            }
        }
        for (ComponentType type : Catalog::findComponents(own, ownEntity)) {
            if (type != prefabType && std::find(assetTypes.begin(), assetTypes.end(), type) == assetTypes.end()) {
                overrides.addedComponents.insert(type);
            }
        }
        if (own->isEntityActive(ownEntity) != asset->isEntityActive(assetRoot)) {
            overrides.state |= PrefabObjectOverrides::StateActive;
        }
        if (own->getEntityTag(ownEntity) != asset->getEntityTag(assetRoot)) {
            overrides.state |= PrefabObjectOverrides::StateTag;
        }
        if (own->getEntityLayer(ownEntity) != asset->getEntityLayer(assetRoot)) {
            overrides.state |= PrefabObjectOverrides::StateLayer;
        }
        return overrides;
    }

    void copyProperties(ScratchEntity& from, EntityRegistry* target, Entity entity, ComponentType type,
            std::set<std::string>& names, const std::string& context) {
        if (!from.has(type) || !hasComponent(target, entity, type)) {
            names.clear();
            return;
        }
        if (names.count(PrefabObjectOverrides::WHOLE_COMPONENT)) {
            Stream::decodeComponents(entity, transformParent(target, entity), target,
                singleComponent(type, encodeComponent(from.registry.get(), from.entity, type)));
            return;
        }
        for (auto it = names.begin(); it != names.end();) {
            const PropertyData source = Catalog::findProperty(from.registry.get(), from.entity, type, *it);
            const PropertyData destination = Catalog::findProperty(target, entity, type, *it);
            if (!source.ref || !destination.ref) {
                Out::warning("Prefab override of %s.%s on '%s' (%s) no longer matches a property, dropped",
                    componentKey(type).c_str(), it->c_str(), target->getEntityName(entity).c_str(), context.c_str());
                it = names.erase(it);
                continue;
            }
            Catalog::copyPropertyValue(from.registry.get(), from.entity, target, entity, type, *it);
            ++it;
        }
    }

}

using namespace jasperengine::editor::prefabinternal;

namespace {

    using editor::PrefabInstanceData;
    using editor::PrefabObjectOverrides;
    using Scratch = editor::prefabinternal::ScratchEntity;

    // Applies one GameObject's entry of a layer (its values in the entry's YAML) to its local
    // entity, keeping only what still applies to it: a property of a component it has, a
    // removed component of the prefab GameObject, an added one it does not have from the
    // prefab. What does not apply is dropped and logged, never put on another GameObject.
    PrefabObjectOverrides applyObjectEntry(editor::SceneProject* sceneProject, Entity local, const PrefabInstanceData::Object& object,
            const EntityRegistry* prefabRegistry, Entity registryEntity, const std::string& context) {
        Scene* scene = sceneProject->scene;
        PrefabObjectOverrides overrides = object.overrides;
        const YAML::Node values = object.values;
        const YAML::Node components = values.IsMap() ? values["components"] : YAML::Node(YAML::NodeType::Undefined);

        for (auto it = overrides.removedComponents.begin(); it != overrides.removedComponents.end();) {
            if (!hasComponent(prefabRegistry, registryEntity, *it)) {
                editor::Out::warning("Removed component override %s on '%s' (%s): the prefab GameObject has no such component, dropped",
                    componentKey(*it).c_str(), scene->getEntityName(local).c_str(), context.c_str());
                it = overrides.removedComponents.erase(it);
                continue;
            }
            if (hasComponent(scene, local, *it) && *it != editor::ComponentType::Transform) {
                editor::ProjectUtils::removeEntityComponent(scene, local, *it, sceneProject->entities);
            }
            ++it;
        }

        for (auto it = overrides.addedComponents.begin(); it != overrides.addedComponents.end();) {
            const YAML::Node value = components ? components[componentKey(*it)] : YAML::Node(YAML::NodeType::Undefined);
            if (!value) {
                editor::Out::warning("Added component override %s on '%s' (%s) has no value, dropped",
                    componentKey(*it).c_str(), scene->getEntityName(local).c_str(), context.c_str());
                it = overrides.addedComponents.erase(it);
                continue;
            }
            if (hasComponent(scene, local, *it)) {
                // the prefab has it now: the instance's values stay, as an override of all of it
                editor::Stream::decodeComponents(local, transformParent(scene, local), scene, singleComponent(*it, value));
                overrides.addWholeComponent(*it);
                it = overrides.addedComponents.erase(it);
                continue;
            }
            editor::ProjectUtils::addEntityComponent(scene, local, *it, sceneProject->entities, YAML::Clone(value));
            ++it;
        }

        for (auto it = overrides.properties.begin(); it != overrides.properties.end();) {
            const editor::ComponentType type = it->first;
            const YAML::Node value = components ? components[componentKey(type)] : YAML::Node(YAML::NodeType::Undefined);
            if (!hasComponent(scene, local, type) || !value) {
                editor::Out::warning("Override of %s on '%s' (%s): %s, dropped", componentKey(type).c_str(),
                    scene->getEntityName(local).c_str(), context.c_str(),
                    value ? "the GameObject has no such component" : "no value was saved");
                it = overrides.properties.erase(it);
                continue;
            }
            if (it->second.count(PrefabObjectOverrides::WHOLE_COMPONENT)) {
                // as files before property overrides had it: the saved component decoded over it
                editor::Stream::decodeComponents(local, transformParent(scene, local), scene, singleComponent(type, value));
            } else {
                Scratch scratch;
                scratch.decode(singleComponent(type, value));
                copyProperties(scratch, scene, local, type, it->second, context);
            }
            it = it->second.empty() ? overrides.properties.erase(it) : std::next(it);
        }

        const uint8_t requested = overrides.state;
        overrides.state = 0;
        if (values.IsMap()) {
            if ((requested & PrefabObjectOverrides::StateName) && values["name"]) {
                scene->setEntityName(local, values["name"].as<std::string>());
                overrides.state |= PrefabObjectOverrides::StateName;
            }
            if ((requested & PrefabObjectOverrides::StateActive) && values["active"]) {
                scene->setEntityActive(local, editor::YamlNumbers::as<bool>(values["active"]));
                overrides.state |= PrefabObjectOverrides::StateActive;
            }
            if ((requested & PrefabObjectOverrides::StateTag) && values["tag"]) {
                scene->setEntityTag(local, values["tag"].as<std::string>());
                overrides.state |= PrefabObjectOverrides::StateTag;
            }
            if ((requested & PrefabObjectOverrides::StateLayer) && values["layer"]) {
                scene->setEntityLayer(local, editor::YamlNumbers::as<int>(values["layer"]));
                overrides.state |= PrefabObjectOverrides::StateLayer;
            }
        }
        return overrides;
    }

    // The registry branch of a node with what the instance leaves out removed, every node's
    // entity id replaced by the local id it is to take (or removed: a new one), and the
    // registry ids in decode order
    void prepareBranch(YAML::Node node, const std::function<bool(Entity)>& absent, const std::function<Entity(Entity)>& localId,
            std::vector<Entity>& registryOrder, const std::string& prefabKey) {
        const YAML::Node readNode = node;
        const Entity registryEntity = editor::YamlNumbers::as<Entity>(readNode["entity"]);
        registryOrder.push_back(registryEntity);
        const Entity local = localId(registryEntity);
        if (local != NULL_ENTITY) {
            node["entity"] = local;
        } else {
            node.remove("entity");
        }
        const YAML::Node components = readNode["components"];
        if (components && components.IsMap() && components[prefabKey]) {
            // a nested prefab root brings its own GameObjects
            node.remove("children");
            node.remove("prefabInstance");
            return;
        }
        const YAML::Node children = readNode["children"];
        if (children && children.IsSequence()) {
            YAML::Node kept(YAML::NodeType::Sequence);
            for (YAML::Node child : children) {
                const YAML::Node readChild = child;
                if (!readChild.IsMap() || !readChild["entity"] || absent(editor::YamlNumbers::as<Entity>(readChild["entity"]))) {
                    continue;
                }
                prepareBranch(child, absent, localId, registryOrder, prefabKey);
                kept.push_back(child);
            }
            if (kept.size() > 0) {
                node["children"] = kept;
            } else {
                node.remove("children");
            }
        }
    }

}

bool editor::Project::isPrefabRootDefaultOverride(ComponentType type, const std::string& property) {
    if (type == ComponentType::Transform) {
        return property == "position" || property == "rotation";
    }
    if (type == ComponentType::RectTransformComponent) {
        static const std::unordered_set<std::string> placement = {
            "width", "height", "anchorPointLeft", "anchorPointTop", "anchorPointRight", "anchorPointBottom",
            "anchorOffsetLeft", "anchorOffsetTop", "anchorOffsetRight", "anchorOffsetBottom",
            "positionOffset", "pivot", "anchorPreset", "usingAnchors"
        };
        return placement.count(property) != 0;
    }
    return false;
}

bool editor::Project::readPrefabFile(const std::filesystem::path& filepath, Prefab& prefab) {
    try {
        const YAML::Node node = loadPrefabFile(getProjectPath() / filepath);
        std::string rootName = filepath.stem().string();
        prefab.loadRegistry(node, rootName.empty() ? std::string("Prefab") : rootName);
        resolveRegistryPrefabRefs(prefab.registry.get());
        if (prefab.root == NULL_ENTITY) {
            Out::error("Prefab %s has no GameObject", filepath.string().c_str());
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        Out::error("Failed to load prefab file %s: %s", filepath.string().c_str(), e.what());
        return false;
    }
}

editor::Prefab* editor::Project::loadPrefabAsset(const std::filesystem::path& filepath) {
    if (filepath.empty()) {
        return nullptr;
    }
    auto it = prefabAssets.find(filepath);
    if (it != prefabAssets.end() && it->second.registry && it->second.root != NULL_ENTITY) {
        return &it->second;
    }
    Prefab loaded;
    if (!readPrefabFile(filepath, loaded)) {
        return nullptr;
    }
    if (it != prefabAssets.end()) {
        // an entry without its registry (never expected): the instances it lists stay
        it->second.registry = std::move(loaded.registry);
        it->second.registryEntities = std::move(loaded.registryEntities);
        it->second.root = loaded.root;
        it->second.nestedInstances = std::move(loaded.nestedInstances);
        it->second.needsUpgrade = loaded.needsUpgrade;
        return &it->second;
    }
    return &prefabAssets.emplace(filepath, std::move(loaded)).first->second;
}

editor::PrefabInstanceData* editor::Project::nestedAssetLayer(Prefab& prefab, const std::filesystem::path& prefabPath, Entity registryRoot,
        Entity localRoot, SceneProject* sceneProject) {
    (void)localRoot;
    (void)sceneProject;
    auto it = prefab.nestedInstances.find(registryRoot);
    if (it == prefab.nestedInstances.end()) {
        PrefabInstanceData data;
        data.present = true;
        data.legacy = prefab.needsUpgrade;
        it = prefab.nestedInstances.emplace(registryRoot, std::move(data)).first;
    }
    PrefabInstanceData& data = it->second;
    if (!data.legacy) {
        return &data;
    }
    // Written before the real-root model: the nested root's own values in this prefab were
    // a wrapper's, so what differs from the nested prefab's root is what this prefab
    // overrides of it
    data.legacy = false;
    const PrefabComponent* component = prefab.registry->findComponent<PrefabComponent>(registryRoot);
    Prefab* nested = component ? loadPrefabAsset(component->path) : nullptr;
    if (nested && nested->registry && nested->root != NULL_ENTITY && nested != &prefab) {
        PrefabObjectOverrides diff = diffAgainstRoot(prefab.registry.get(), registryRoot, nested->registry.get(), nested->root);
        if (!diff.empty()) {
            data.objects[nested->root].overrides.merge(diff);
        }
    } else {
        Out::warning("Prefab %s: the prefab nested at GameObject %u is not available to upgrade its data", prefabPath.string().c_str(), registryRoot);
    }
    prefab.needsUpgrade = true;
    return &data;
}

std::vector<Entity> editor::Project::importPrefab(SceneProject* sceneProject, std::vector<Entity>* entities, const std::filesystem::path& requestedPath, Entity rootEntity,
        bool needSaveScene, const PrefabInstanceData* instanceData, std::unordered_map<Entity, Entity>* entityRemap, const PrefabMemberIds* preferredIds,
        const std::vector<PrefabInstanceData*>& assetLayers) {
    std::filesystem::path filepath = requestedPath;

    // Prefabs being imported up the call stack: nested imports, and cycle detection
    static thread_local std::unordered_set<std::string> loadingPaths;
    const bool nestedImport = !loadingPaths.empty();

    // Which prefab: the root's PrefabComponent names it by GUID, its path is only a hint.
    // One that cannot be found leaves a Missing Prefab instance holding the instance data.
    auto missingPrefab = [&]() -> std::vector<Entity> {
        YAML::Node data(YAML::NodeType::Map);
        if (instanceData && instanceData->present) {
            YAML::Node block = Stream::encodePrefabInstanceBlock(*instanceData);
            block["version"] = Prefab::FILE_VERSION;
            if (instanceData->legacy) {
                block["legacyRoot"] = true;
            }
            data["prefabInstance"] = block;
        }
        setMissingPrefab(sceneProject, rootEntity, data, preferredIds);
        return {};
    };
    if (!sceneProject || !sceneProject->scene) {
        return {};
    }
    if (PrefabComponent* rootComponent = sceneProject->scene->findComponent<PrefabComponent>(rootEntity)) {
        bool pathChanged = false;
        if (!resolvePrefabReference(*rootComponent, &pathChanged)) {
            return missingPrefab();
        }
        // A path the scene itself wrote (not one inside a prefab file) makes it modified
        if (pathChanged && !nestedImport) {
            Out::info("Prefab \"%s\" is now at \"%s\"", filepath.string().c_str(), rootComponent->path.c_str());
            sceneProject->prefabRefsChanged = true;
        }
        filepath = rootComponent->path;
    }

    if (!filepath.is_relative()) {
        Out::error("Prefab filepath must be relative: %s", filepath.string().c_str());
        return {};
    }

    // Cycle detection for nested prefabs
    std::string pathStr = filepath.string();
    if (loadingPaths.count(pathStr)) {
        Out::error("Circular prefab reference detected: %s", pathStr.c_str());
        return missingPrefab();
    }
    loadingPaths.insert(pathStr);
    struct LoadingPathGuard {
        std::unordered_set<std::string>& paths;
        const std::string& path;
        ~LoadingPathGuard() { paths.erase(path); }
    } loadingGuard{loadingPaths, pathStr};

    Prefab* prefabPtr = loadPrefabAsset(filepath);
    if (!prefabPtr) {
        return missingPrefab();
    }
    // std::map nodes stay put while nested imports load other prefabs
    Prefab& prefab = *prefabPtr;
    EntityRegistry* registry = prefab.registry.get();
    const Entity registryRoot = prefab.root;

    Scene* scene = sceneProject->scene;
    std::vector<Entity>& tracked = entities ? *entities : sceneProject->entities;
    const std::string context = filepath.string();
    const std::string prefabKey = Catalog::getComponentName(ComponentType::PrefabComponent, true);

    Prefab::Instance newInstance;
    newInstance.instanceId = prefab.nextInstanceId++;
    newInstance.rootEntity = rootEntity;
    newInstance.rootRegistryEntity = registryRoot;

    // ---- The root: the prefab's root GameObject applied to the instance's root ----

    const bool topLevel = assetLayers.empty();
    const PrefabInstanceData* rootLayer = topLevel ? instanceData : assetLayers.front();
    Entity rootParent = transformParent(scene, rootEntity);

    // What the root has of its own: its values for the default overrides and for its
    // overrides, which come from its own node (the scene's, or the containing prefab's)
    Scratch own;
    std::set<ComponentType> rootHad;
    for (ComponentType type : Catalog::findComponents(scene, rootEntity)) {
        if (type != ComponentType::PrefabComponent) {
            rootHad.insert(type);
        }
    }
    {
        Signature signature = scene->getSignature(rootEntity);
        signature.reset(scene->getComponentId<PrefabComponent>());
        own.takeComponents(scene, rootEntity, signature);
        own.takeState(scene, rootEntity);
    }

    PrefabObjectOverrides rootOverrides;
    if (rootLayer && rootLayer->legacy) {
        if (!rootHad.empty()) {
            rootOverrides = diffAgainstRoot(own.registry.get(), own.entity, registry, registryRoot);
        }
    } else if (rootLayer) {
        auto rootEntry = rootLayer->objects.find(registryRoot);
        if (rootEntry != rootLayer->objects.end()) {
            rootOverrides = rootEntry->second.overrides;
        }
    }

    YAML::Node rootNode = Stream::encodeEntity(registryRoot, registry, this);
    const YAML::Node rootComponents = rootNode["components"];
    std::vector<ComponentType> rootFromPrefab;
    for (ComponentType type : Catalog::findComponents(registry, registryRoot)) {
        if (type == ComponentType::PrefabComponent) {
            continue;
        }
        if (rootOverrides.removedComponents.count(type)) {
            if (hasComponent(scene, rootEntity, type) && type != ComponentType::Transform) {
                ProjectUtils::removeEntityComponent(scene, rootEntity, type, sceneProject->entities);
            }
            continue;
        }
        const YAML::Node value = rootComponents ? rootComponents[componentKey(type)] : YAML::Node(YAML::NodeType::Undefined);
        if (!value) {
            continue;
        }
        Stream::decodeComponents(rootEntity, rootParent, scene, singleComponent(type, value));
        rootFromPrefab.push_back(type);
    }
    for (auto it = rootOverrides.removedComponents.begin(); it != rootOverrides.removedComponents.end();) {
        it = hasComponent(registry, registryRoot, *it) ? std::next(it) : rootOverrides.removedComponents.erase(it);
    }
    // What the root has that the prefab's root has not: kept when added by the instance
    for (ComponentType type : rootHad) {
        if (hasComponent(registry, registryRoot, type) || rootOverrides.addedComponents.count(type)) {
            continue;
        }
        if (type == ComponentType::Transform) {
            continue; // never taken from under the hierarchy
        }
        ProjectUtils::removeEntityComponent(scene, rootEntity, type, sceneProject->entities);
    }
    for (auto it = rootOverrides.addedComponents.begin(); it != rootOverrides.addedComponents.end();) {
        it = (rootHad.count(*it) && !hasComponent(registry, registryRoot, *it)) ? std::next(it) : rootOverrides.addedComponents.erase(it);
    }
    if (!(rootOverrides.state & PrefabObjectOverrides::StateActive)) {
        scene->setEntityActive(rootEntity, registry->isEntityActive(registryRoot));
    }
    if (!(rootOverrides.state & PrefabObjectOverrides::StateTag)) {
        scene->setEntityTag(rootEntity, registry->getEntityTag(registryRoot));
    }
    if (!(rootOverrides.state & PrefabObjectOverrides::StateLayer)) {
        scene->setEntityLayer(rootEntity, registry->getEntityLayer(registryRoot));
    }
    rootOverrides.state &= ~PrefabObjectOverrides::StateName; // the name is the root's own
    if (scene->getEntityName(rootEntity).empty()) {
        scene->setEntityName(rootEntity, registry->getEntityName(registryRoot));
    }

    // ---- The other GameObjects ----

    // Removed GameObject overrides: the scene's (recorded) and the containing prefabs'
    std::unordered_set<Entity> removedHere;
    auto addRemovals = [&](const PrefabInstanceData* layer, bool record) {
        if (!layer) {
            return;
        }
        for (Entity registryEntity : layer->removedGameObjects) {
            // An entry whose GameObject left the Prefab since has nothing to remove
            if (registryEntity == registryRoot || !registry->isEntityCreated(registryEntity)) {
                continue;
            }
            removedHere.insert(registryEntity);
            if (record) {
                newInstance.removed.emplace(registryEntity, std::vector<Prefab::EntityMember>());
            }
        }
    };
    for (const PrefabInstanceData* layer : assetLayers) {
        addRemovals(layer, false);
    }
    addRemovals(instanceData, true);
    auto absent = [&](Entity registryEntity) {
        return removedHere.count(registryEntity) != 0;
    };

    // The ids members take: the ones asked for, else the ones they had (a scene file's
    // memberIds), else, where free and not needed by the scene file being read, their
    // registry id (what files without memberIds had), else new ones
    const std::unordered_map<Entity, Entity>* askedIds = nullptr;
    if (preferredIds) {
        auto asked = preferredIds->find(rootEntity);
        if (asked != preferredIds->end()) {
            askedIds = &asked->second;
        }
    }
    std::unordered_set<Entity> takenIds;
    auto localId = [&](Entity registryEntity) -> Entity {
        auto usable = [&](Entity id) {
            return id != NULL_ENTITY && !scene->isEntityCreated(id) && !takenIds.count(id);
        };
        Entity id = NULL_ENTITY;
        if (askedIds) {
            auto it = askedIds->find(registryEntity);
            if (it != askedIds->end() && usable(it->second)) id = it->second;
        }
        if (id == NULL_ENTITY && instanceData) {
            auto it = instanceData->memberIds.find(registryEntity);
            if (it != instanceData->memberIds.end() && usable(it->second)) id = it->second;
        }
        if (id == NULL_ENTITY && usable(registryEntity) && !sceneProject->reservedEntityIds.count(registryEntity)) {
            id = registryEntity;
        }
        if (id != NULL_ENTITY) {
            takenIds.insert(id);
        }
        return id;
    };

    std::vector<Entity> newEntities;
    std::vector<Entity> regEntities;
    auto decodeBranch = [&](YAML::Node node, Entity parent) {
        std::vector<Entity> registryOrder;
        prepareBranch(node, absent, localId, registryOrder, prefabKey);
        std::vector<Entity> decoded = Stream::decodeEntity(node, scene, &tracked, nullptr, nullptr, parent, true, false, nullptr);
        if (decoded.size() != registryOrder.size()) {
            Out::error("importPrefab(%s): entity count mismatch: decoded=%zu, registry=%zu",
                context.c_str(), decoded.size(), registryOrder.size());
            DeleteEntityCmd::destroyEntities(scene, decoded, tracked, this, sceneProject->id);
            return;
        }
        newEntities.insert(newEntities.end(), decoded.begin(), decoded.end());
        regEntities.insert(regEntities.end(), registryOrder.begin(), registryOrder.end());
    };
    if (const YAML::Node children = rootNode["children"]; children && children.IsSequence()) {
        for (YAML::Node child : children) {
            const YAML::Node readChild = child;
            if (readChild.IsMap() && readChild["entity"] && !absent(YamlNumbers::as<Entity>(readChild["entity"]))) {
                decodeBranch(YAML::Clone(child), rootEntity);
            }
        }
    }
    // GameObjects without a Transform listed next to the root
    for (Entity topLevel : getTopLevelEntities(registry, prefab.registryEntities)) {
        if (topLevel == registryRoot || absent(topLevel)) {
            continue;
        }
        decodeBranch(Stream::encodeEntity(topLevel, registry, this), NULL_ENTITY);
    }

    std::unordered_map<Entity, Entity> registryToLocal;
    registryToLocal[registryRoot] = rootEntity;
    for (size_t i = 0; i < newEntities.size(); i++) {
        newInstance.members.push_back({newEntities[i], regEntities[i]});
        registryToLocal[regEntities[i]] = newEntities[i];
    }
    // References in prefab ids become this instance's; anything else is None
    remapEntityProperties(scene, newEntities, registryToLocal);
    for (ComponentType type : rootFromPrefab) {
        remapEntityPropertiesInComponent(scene, rootEntity, type, {}, registryToLocal);
    }

    // The root takes back its own values: the default overrides, and its overrides
    for (ComponentType type : rootFromPrefab) {
        if (!own.has(type)) {
            if (rootOverrides.properties.erase(type)) {
                Out::warning("Override of %s on prefab root '%s' (%s): it had no such component, dropped",
                    componentKey(type).c_str(), scene->getEntityName(rootEntity).c_str(), context.c_str());
            }
            continue;
        }
        std::set<std::string> names;
        auto overridden = rootOverrides.properties.find(type);
        if (overridden != rootOverrides.properties.end()) {
            names = overridden->second;
        }
        if (!names.count(PrefabObjectOverrides::WHOLE_COMPONENT)) {
            for (auto& [name, property] : Catalog::findEntityProperties(own.registry.get(), own.entity, type)) {
                if (isPrefabRootDefaultOverride(type, name)) {
                    names.insert(name);
                }
            }
        }
        if (names.empty()) {
            continue;
        }
        copyProperties(own, scene, rootEntity, type, names, context);
        if (overridden != rootOverrides.properties.end()) {
            std::set<std::string> kept;
            for (const std::string& name : overridden->second) {
                if (names.count(name) && !isPrefabRootDefaultOverride(type, name)) {
                    kept.insert(name);
                }
            }
            if (kept.empty()) {
                rootOverrides.properties.erase(overridden);
            } else {
                overridden->second = kept;
            }
        }
    }
    // Overrides of components the prefab root (or the instance root) does not have
    for (auto it = rootOverrides.properties.begin(); it != rootOverrides.properties.end();) {
        if (std::find(rootFromPrefab.begin(), rootFromPrefab.end(), it->first) == rootFromPrefab.end()) {
            Out::warning("Override of %s on prefab root '%s' (%s): no such component, dropped",
                componentKey(it->first).c_str(), scene->getEntityName(rootEntity).c_str(), context.c_str());
            it = rootOverrides.properties.erase(it);
        } else {
            ++it;
        }
    }
    // Overrides of the root kept by layers further out, with their values
    auto applyRootEntry = [&](const PrefabInstanceData* layer, bool record) {
        if (!layer || layer == rootLayer) {
            return;
        }
        auto entry = layer->objects.find(registryRoot);
        if (entry == layer->objects.end() || !entry->second.values.IsMap()) {
            return;
        }
        PrefabObjectOverrides applied = applyObjectEntry(sceneProject, rootEntity, entry->second, registry, registryRoot, context);
        if (record) {
            rootOverrides.merge(applied);
        }
    };
    for (size_t i = 1; i < assetLayers.size(); i++) {
        applyRootEntry(assetLayers[i], false);
    }
    if (!topLevel) {
        applyRootEntry(instanceData, true);
    }

    // Decoded children of nested prefab roots (kept by files of old versions) go: the nested
    // import brings them
    std::unordered_set<Entity> nestedRoots;
    for (const Prefab::EntityMember& member : newInstance.members) {
        if (prefab.isNestedRoot(member.registryEntity)) {
            nestedRoots.insert(member.localEntity);
        }
    }
    if (!nestedRoots.empty()) {
        std::unordered_set<Entity> below;
        for (const Prefab::EntityMember& member : newInstance.members) {
            if (nestedRoots.count(member.localEntity)) continue;
            for (Entity ancestor = transformParent(scene, member.localEntity); ancestor != NULL_ENTITY; ancestor = transformParent(scene, ancestor)) {
                if (nestedRoots.count(ancestor)) {
                    below.insert(member.localEntity);
                    break;
                }
            }
        }
        if (!below.empty()) {
            std::vector<Entity> doomed(below.begin(), below.end());
            DeleteEntityCmd::destroyEntities(scene, doomed, tracked, this, sceneProject->id);
            newEntities.erase(std::remove_if(newEntities.begin(), newEntities.end(),
                [&below](Entity e) { return below.count(e) != 0; }), newEntities.end());
            newInstance.members.erase(std::remove_if(newInstance.members.begin(), newInstance.members.end(),
                [&below](const Prefab::EntityMember& m) { return below.count(m.localEntity) != 0; }), newInstance.members.end());
        }
    }

    std::vector<Entity> allResult = newEntities;

    // ---- Nested instances, each with what this prefab and the layers above override of it ----
    for (const Prefab::EntityMember member : std::vector<Prefab::EntityMember>(newInstance.members)) {
        if (!prefab.isNestedRoot(member.registryEntity) || !scene->isEntityCreated(member.localEntity)) {
            continue;
        }
        const PrefabComponent* component = scene->findComponent<PrefabComponent>(member.localEntity);
        if (!component) {
            continue;
        }
        const std::string nestedPath = component->path;
        std::vector<PrefabInstanceData*> nestedLayers;
        nestedLayers.push_back(nestedAssetLayer(prefab, filepath, member.registryEntity, member.localEntity, sceneProject));
        for (PrefabInstanceData* layer : assetLayers) {
            if (PrefabInstanceData* found = layer ? layer->findNested(member.registryEntity, nestedPath) : nullptr) {
                nestedLayers.push_back(found);
            }
        }
        const PrefabInstanceData* nestedData = instanceData ? instanceData->findNested(member.registryEntity, nestedPath) : nullptr;
        std::vector<Entity> nestedEntities = importPrefab(sceneProject, entities, nestedPath, member.localEntity, false,
            nestedData, entityRemap, preferredIds, nestedLayers);
        allResult.insert(allResult.end(), nestedEntities.begin(), nestedEntities.end());
    }

    // ---- This level's overrides of its GameObjects: the containing prefabs', then the scene's ----
    auto applyLayer = [&](const PrefabInstanceData* layer, bool record) {
        if (!layer) {
            return;
        }
        for (const auto& [registryEntity, object] : layer->objects) {
            if (registryEntity == registryRoot) {
                continue; // done with the root
            }
            if (!registry->isEntityCreated(registryEntity)) {
                if (record) {
                    Out::warning("Prefab override for GameObject %u of %s: the prefab has no such GameObject any more, dropped",
                        registryEntity, context.c_str());
                }
                continue;
            }
            auto local = registryToLocal.find(registryEntity);
            if (local == registryToLocal.end() || !scene->isEntityCreated(local->second)) {
                continue; // removed from this instance
            }
            PrefabObjectOverrides applied = applyObjectEntry(sceneProject, local->second, object, registry, registryEntity, context);
            if (record && !applied.empty()) {
                newInstance.overrides[local->second].merge(applied);
            }
        }
    };
    for (const PrefabInstanceData* layer : assetLayers) {
        applyLayer(layer, false);
    }
    applyLayer(instanceData, true);
    if (topLevel && !rootOverrides.empty()) {
        newInstance.overrides[rootEntity] = rootOverrides;
    } else if (!topLevel && !rootOverrides.empty() && instanceData) {
        // the containing prefab's overrides of the root are its own; only what the scene
        // layer added is the instance's
    }

    // ---- GameObjects added to this instance ----
    if (instanceData) {
        for (const YAML::Node& addedNode : instanceData->addedGameObjects) {
            Entity parentRegEntity = NULL_ENTITY;
            size_t childIndex = 0;
            if (addedNode["parentRegistryEntity"]) {
                parentRegEntity = YamlNumbers::as<Entity>(addedNode["parentRegistryEntity"]);
            }
            if (addedNode["childIndex"]) {
                childIndex = YamlNumbers::as<size_t>(addedNode["childIndex"]);
            }

            // Below its prefab GameObject (the root for NULL), else below the root
            Entity parentEntity = rootEntity;
            if (parentRegEntity != NULL_ENTITY) {
                auto parentLocal = registryToLocal.find(parentRegEntity);
                if (parentLocal != registryToLocal.end() && scene->isEntityCreated(parentLocal->second)) {
                    parentEntity = parentLocal->second;
                } else {
                    Out::warning("GameObject '%s' added to an instance of %s: its parent is not in the instance, put below the root",
                        addedNode["name"] ? addedNode["name"].as<std::string>().c_str() : "", context.c_str());
                }
            }

            YAML::Node clean = YAML::Clone(addedNode);
            clean.remove("parentRegistryEntity");
            clean.remove("childIndex");
            std::vector<Entity> decoded = Stream::decodeEntity(clean, scene, entities, this, sceneProject,
                parentEntity, true, false, entityRemap);
            if (decoded.empty()) {
                continue;
            }
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
                                     bool reconcileTracked, bool keepRoot) {
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
    if (rootEntity != NULL_ENTITY && !keepRoot) {
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
    std::vector<Prefab::EntityMember> objects = instance->members;
    objects.push_back({instance->rootEntity, instance->rootRegistryEntity});
    for (const Prefab::EntityMember& member : objects) {
        registryToLocal[member.registryEntity] = member.localEntity;
    }
    remapEntityProperties(scene, decoded, registryToLocal);

    // The instance's GameObjects that the Prefab has pointing at these get them back
    const std::unordered_set<Entity> createdRegistry(registryOrder.begin(), registryOrder.end());
    const std::unordered_set<Entity> createdLocal(decoded.begin(), decoded.end());
    for (const Prefab::EntityMember& member : objects) {
        if (createdLocal.count(member.localEntity) || !scene->isEntityCreated(member.localEntity) ||
            !registry->isEntityCreated(member.registryEntity)) {
            continue;
        }
        const Prefab::ObjectOverrides* overridden = instance->findOverrides(member.localEntity);
        for (ComponentType componentType : Catalog::findComponents(registry, member.registryEntity)) {
            if ((overridden && overridden->isWholeComponent(componentType)) ||
                !Catalog::mayHoldReferences(registry, member.registryEntity, componentType, true, false)) {
                continue;
            }
            std::map<std::string, PropertyData> localProperties;
            bool localRead = false;
            uint64_t updateFlags = 0;
            for (auto& [propertyName, registryProperty] : Catalog::findEntityProperties(registry, member.registryEntity, componentType)) {
                const Entity target = propertyEntityTarget(registryProperty);
                if (target == NULL_ENTITY || !createdRegistry.count(target) ||
                    (overridden && overridden->hasProperty(componentType, propertyName))) {
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

    // Nested prefab roots bring their own GameObjects, with what this prefab overrides of them
    for (const Prefab::EntityMember& member : created) {
        const PrefabComponent* component = scene->findComponent<PrefabComponent>(member.localEntity);
        if (component && prefab->isNestedRoot(member.registryEntity)) {
            const fs::path nestedPath = component->path;
            std::vector<PrefabInstanceData*> layers{nestedAssetLayer(*prefab, filepath, member.registryEntity, member.localEntity, sceneProject)};
            importPrefab(sceneProject, &sceneProject->entities, nestedPath, member.localEntity, false, nullptr, nullptr, nullptr, layers);
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

        PrefabManager::registerPrefab(prefabId, prefabName,
            // Factory: the bare root PrefabManager made becomes the prefab's root GameObject
            // (its components, Transform included, come from importPrefab), the export's way
            [this, capturablePrefabPath](Scene* scene, Entity root) -> bool {
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
                const std::vector<ComponentType> oldComponents = Catalog::findComponents(scene, root);
                const std::vector<Entity> entitiesSnapshot = sceneProject->entities;

                // PrefabManager destroys the entities created by the import, this only has to
                // undo what the factory and the import changed on the root plus the instance
                // metadata
                auto restoreRoot = [&]() {
                    removePrefabInstanceTracking(sceneProject->id, root);
                    if (scene->isEntityCreated(root)) {
                        scene->setEntityName(root, oldName);
                        std::vector<Entity> unused;
                        for (ComponentType type : Catalog::findComponents(scene, root)) {
                            if (std::find(oldComponents.begin(), oldComponents.end(), type) == oldComponents.end()) {
                                ProjectUtils::removeEntityComponent(scene, root, type, unused);
                            }
                        }
                    }
                    sceneProject->entities = entitiesSnapshot;
                };

                if (oldName.empty()) {
                    // the prefab root's name, not a unique one: a "Prefab 2" is a name the same
                    // script would not find in the export, which names the root the same way
                    const Prefab* asset = loadPrefabAsset(capturablePrefabPath);
                    std::string rootName = asset ? asset->registry->getEntityName(asset->root) : capturablePrefabPath.stem().string();
                    if (rootName.empty())
                        rootName = "Prefab";
                    scene->setEntityName(root, rootName);
                }

                if (addedPrefab) {
                    PrefabComponent prefabComp;
                    prefabComp.name = capturablePrefabPath.stem().string();
                    prefabComp.path = capturablePrefabPath.string();
                    prefabComp.guid = getPrefabGuid(capturablePrefabPath);
                    scene->addComponent<PrefabComponent>(root, prefabComp);
                }

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
