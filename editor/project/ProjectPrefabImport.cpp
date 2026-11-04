// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Prefab instantiation: importing/unimporting instances into scenes, instantiating
// removed branches back, prefab order placement, and the Play-mode prefab factories.

#include "Project.h"
#include "component/PrefabComponent.h"
#include "util/FileUtils.h"
#include "util/WorldTransforms.h"
#include "ProjectPrefabInternal.h"
#include "Factory.h"
#include "Out.h"
#include "Stream.h"
#include "PrefabManager.h"
#include "command/type/DeleteEntityCmd.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectUtils.h"
#include "util/YamlNumbers.h"
#include "component/MeshComponent.h"
#include <cstdlib>

#include <algorithm>
#include <functional>
#include <limits>
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

    // Prefabs its scripts can instantiate (Prefab script properties) are built with it
    addScriptPrefabReferences(sceneProject);
}

// collectAllPrefabs: ProjectPrefabRuntime.cpp

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
            case PropertyType::Asset: return *static_cast<AssetRef*>(a.ref) == *static_cast<AssetRef*>(b.ref);
            case PropertyType::Shader: return *static_cast<ShaderRef*>(a.ref) == *static_cast<ShaderRef*>(b.ref);
            case PropertyType::Curve: return *static_cast<Curve*>(a.ref) == *static_cast<Curve*>(b.ref);
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
            case PropertyType::Gradient: return *static_cast<Gradient*>(a.ref) == *static_cast<Gradient*>(b.ref);
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
            // an overridden blend shape weight is the instance's own, also past the targets the
            // asset's weights cover: a model load keeps it (MeshComponent::morphTargetCount)
            if (type == ComponentType::MeshComponent && it->compare(0, 13, "morphWeights[") == 0) {
                if (MeshComponent* mesh = target->findComponent<MeshComponent>(entity)) {
                    const int index = std::atoi(it->c_str() + 13);
                    mesh->morphWeightsSet = true;
                    mesh->morphTargetCount = std::max(mesh->morphTargetCount, std::min(index + 1, MAX_MORPHTARGETS));
                }
            }
            ++it;
        }
        // prefab override applied: a change of the component (the whole-component path is
        // stamped by Stream::decodeComponents)
        if (!names.empty()) {
            Catalog::markChanged(target, entity, type);
        }
    }

}

using namespace jasperengine::editor::prefabinternal;

namespace {

    using editor::Prefab;
    using editor::PrefabInstanceData;
    using editor::PrefabObjectOverrides;
    using Scratch = editor::prefabinternal::ScratchEntity;
    using EntityMap = std::unordered_map<Entity, Entity>;

    // Set by an import right before it imports one of its nested roots: that import builds
    // the GameObjects only, its holder then applies what it and the levels above override
    thread_local bool nextImportDeferred = false;
    // Set right before a variant's import imports its base onto the variant's root: the
    // prefab to import (the root's PrefabComponent names the variant) and how many bases
    // down the chain it is (the key of its member ids, Project::prefabLevelKey)
    thread_local std::filesystem::path nextImportPath;
    thread_local size_t nextImportLevel = 0;
    // ... and what the levels above it (the variant, and what overrides the variant's root)
    // keep of their own on the root: components they add or remove, state they override
    struct RootKept {
        std::set<editor::ComponentType> added;
        std::set<editor::ComponentType> removed;
        uint8_t state = 0;
    };
    thread_local RootKept nextImportRootKept;

    // The prefab a nested data entry of `prefab` is for: the one its nested root (or the
    // root of a variant) instantiates, else the path the oldest entries name it by
    std::filesystem::path nestedEntryPath(const editor::Project* project, const Prefab& prefab, const PrefabInstanceData& entry) {
        if (entry.nestedRoot != NULL_ENTITY) {
            std::filesystem::path path = project->instancePathAt(prefab, entry.nestedRoot);
            if (!path.empty()) {
                return path;
            }
        }
        return entry.nestedPath;
    }

    // A component value of a layer, its references (in the ids of the prefab holding the
    // layer) made local through `remap` (the holder's registry-to-local map, stripped ids
    // included); as given without one (the scene's, in local ids already)
    YAML::Node localValue(editor::ComponentType type, const YAML::Node& value, const EntityMap* remap) {
        if (!remap || !value) {
            return value;
        }
        Scratch scratch;
        scratch.decode(singleComponent(type, value));
        if (!scratch.has(type)) {
            return value;
        }
        editor::Project::remapEntityPropertiesInComponent(scratch.registry.get(), scratch.entity, type, {}, *remap);
        return encodeComponent(scratch.registry.get(), scratch.entity, type);
    }

    // Applies one GameObject's entry of a layer (its values in the entry's YAML) to its local
    // entity, keeping only what still applies to it: a property of a component it has, a
    // removed component of the prefab GameObject, an added one it does not have from the
    // prefab. What does not apply is dropped and logged, never put on another GameObject.
    // remap: the holder's map for an asset layer's values (see localValue)
    PrefabObjectOverrides applyObjectEntry(editor::SceneProject* sceneProject, Entity local, const PrefabInstanceData::Object& object,
            const EntityRegistry* prefabRegistry, Entity registryEntity, const std::string& context, const EntityMap* remap = nullptr) {
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
            const YAML::Node value = components ? localValue(*it, components[componentKey(*it)], remap) : YAML::Node(YAML::NodeType::Undefined);
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
                editor::Stream::decodeComponents(local, transformParent(scene, local), scene, singleComponent(type, localValue(type, value, remap)));
            } else {
                Scratch scratch;
                scratch.decode(singleComponent(type, value));
                if (remap && scratch.has(type)) {
                    std::vector<std::string> names(it->second.begin(), it->second.end());
                    editor::Project::remapEntityPropertiesInComponent(scratch.registry.get(), scratch.entity, type, names, *remap);
                }
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
    // registry ids in decode order. A nested root's children (the GameObjects this prefab
    // adds to that nested instance) are left out: they come once the nested instance exists.
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
        node.remove("addedTo");
        node.remove("childIndex");
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

// ---- Nested instances, stripped objects and the layers of nested data (PREFABS.md 17) ----

editor::Prefab::Instance* editor::Project::findInstanceRootedAt(SceneProject* sceneProject, Entity root, std::filesystem::path* path) {
    if (!sceneProject || !sceneProject->scene || root == NULL_ENTITY || !sceneProject->scene->isEntityCreated(root)) {
        return nullptr;
    }
    const PrefabComponent* component = sceneProject->scene->findComponent<PrefabComponent>(root);
    Prefab* prefab = component ? getPrefab(component->path) : nullptr;
    Prefab::Instance* instance = prefab ? prefab->getInstanceByRoot(sceneProject->id, root) : nullptr;
    if (!instance) {
        return nullptr;
    }
    if (path) {
        *path = component->path;
    }
    return instance;
}

Entity editor::Project::resolveStrippedObject(SceneProject* sceneProject, const std::unordered_map<Entity, Entity>& registryToLocal,
        const std::vector<Entity>& source, const Prefab* holder) {
    if (source.size() < 2) {
        return NULL_ENTITY;
    }
    auto first = registryToLocal.find(source[0]);
    if (first == registryToLocal.end()) {
        return NULL_ENTITY;
    }
    Entity local = first->second;
    // Each step is in the prefab the previous one instantiates, read from the holder's
    // registry: the root of a variant (an instance of its base at the same GameObject) is
    // told apart from the variant's own level
    const Prefab* level = holder;
    for (size_t i = 1; i < source.size() && local != NULL_ENTITY; i++) {
        std::filesystem::path path;
        if (level) {
            path = instancePathAt(*level, source[i - 1]);
        }
        Prefab::Instance* instance = findInstanceAt(sceneProject, local, path, &path);
        if (!instance) {
            return NULL_ENTITY;
        }
        local = instance->getLocalEntity(source[i]);
        level = getPrefab(path);
    }
    return (local != NULL_ENTITY && sceneProject->scene->isEntityCreated(local)) ? local : NULL_ENTITY;
}

void editor::Project::addStrippedToMap(SceneProject* sceneProject, const Prefab& prefab, std::unordered_map<Entity, Entity>& registryToLocal) {
    for (const auto& [id, source] : prefab.stripped) {
        const Entity local = resolveStrippedObject(sceneProject, registryToLocal, source, &prefab);
        if (local != NULL_ENTITY) {
            registryToLocal[id] = local;
        }
    }
}

std::unordered_map<Entity, Entity> editor::Project::getPrefabReferenceMap(uint32_t sceneId, Entity instanceRoot, const std::filesystem::path& level) {
    std::unordered_map<Entity, Entity> map;
    SceneProject* sceneProject = getScene(sceneId);
    std::filesystem::path path;
    Prefab::Instance* instance = findInstanceAt(sceneProject, instanceRoot, level, &path);
    if (!instance) {
        return map;
    }
    map[instance->rootRegistryEntity] = instance->rootEntity;
    for (const Prefab::EntityMember& member : instance->members) {
        map[member.registryEntity] = member.localEntity;
    }
    if (const Prefab* prefab = getPrefab(path)) {
        addStrippedToMap(sceneProject, *prefab, map);
    }
    return map;
}

Entity editor::Project::findNestedLocalRoot(SceneProject* sceneProject, const Prefab& prefab, const Prefab::Instance& instance,
        const PrefabInstanceData& entry) {
    if (entry.nestedRoot != NULL_ENTITY) {
        return instance.getLocalEntity(entry.nestedRoot);
    }
    // the oldest entries name the nested prefab by its path only
    if (entry.nestedPath.empty() || !prefab.registry) {
        return NULL_ENTITY;
    }
    for (const Prefab::EntityMember& member : instance.members) {
        if (!prefab.isNestedRoot(member.registryEntity)) {
            continue;
        }
        const PrefabComponent* component = prefab.registry->findComponent<PrefabComponent>(member.registryEntity);
        if (component && component->path == entry.nestedPath && sceneProject->scene->isEntityCreated(member.localEntity)) {
            return member.localEntity;
        }
    }
    return NULL_ENTITY;
}

void editor::Project::applyHolderData(SceneProject* sceneProject, Entity nestedRoot, const PrefabInstanceData& data,
        const std::unordered_map<Entity, Entity>& holderMap, const EntityRegistry* holderRegistry, Entity holderEntity, bool skipRoot, size_t depth,
        const std::filesystem::path& level) {
    std::filesystem::path path;
    Prefab::Instance* found = findInstanceAt(sceneProject, nestedRoot, level, &path);
    Prefab* prefab = found ? getPrefab(path) : nullptr;
    if (!found || !prefab || !prefab->registry || depth > 64) {
        return;
    }
    Scene* scene = sceneProject->scene;
    EntityRegistry* registry = prefab->registry.get();
    const std::string context = path.string();
    const Entity registryRoot = found->rootRegistryEntity;
    // the instance's maps, before anything below can add instances to the vector holding it
    std::vector<std::pair<Entity, Entity>> objects;
    for (const auto& [registryEntity, object] : data.objects) {
        objects.emplace_back(registryEntity, found->getLocalEntity(registryEntity));
    }
    std::vector<std::tuple<const PrefabInstanceData*, Entity, std::filesystem::path>> nested;
    for (const PrefabInstanceData& entry : data.nested) {
        nested.emplace_back(&entry, findNestedLocalRoot(sceneProject, *prefab, *found, entry), nestedEntryPath(this, *prefab, entry));
    }

    // The root: what the holder overrides of it are the holder's own values of its nested
    // root (its registry entity, references made local), with the root's default overrides
    if (!skipRoot && holderRegistry && holderEntity != NULL_ENTITY && holderRegistry->isEntityCreated(holderEntity)) {
        Scratch values;
        Signature signature = holderRegistry->getSignature(holderEntity);
        signature.reset(holderRegistry->getComponentId<PrefabComponent>());
        values.takeComponents(holderRegistry, holderEntity, signature);
        remapEntityProperties(values.registry.get(), {values.entity}, holderMap);
        PrefabObjectOverrides overrides;
        auto entry = data.objects.find(registryRoot);
        if (entry != data.objects.end()) {
            overrides = entry->second.overrides;
        }
        for (ComponentType type : Catalog::findComponents(values.registry.get(), values.entity)) {
            if (!hasComponent(scene, nestedRoot, type)) {
                continue;
            }
            std::set<std::string> names;
            if (overrides.addedComponents.count(type) || overrides.isWholeComponent(type)) {
                names.insert(PrefabObjectOverrides::WHOLE_COMPONENT);
            } else {
                auto overridden = overrides.properties.find(type);
                if (overridden != overrides.properties.end()) {
                    names = overridden->second;
                }
                for (auto& [name, property] : Catalog::findEntityProperties(values.registry.get(), values.entity, type)) {
                    if (isPrefabRootDefaultOverride(type, name)) {
                        names.insert(name);
                    }
                }
            }
            if (!names.empty()) {
                copyProperties(values, scene, nestedRoot, type, names, context);
            }
        }
    } else if (!skipRoot) {
        auto entry = data.objects.find(registryRoot);
        if (entry != data.objects.end() && entry->second.values.IsMap()) {
            applyObjectEntry(sceneProject, nestedRoot, entry->second, registry, registryRoot, context, &holderMap);
        }
    }

    // Its other GameObjects
    for (const auto& [registryEntity, local] : objects) {
        if (registryEntity == registryRoot || local == NULL_ENTITY || !scene->isEntityCreated(local) || !registry->isEntityCreated(registryEntity)) {
            continue;
        }
        applyObjectEntry(sceneProject, local, data.objects.at(registryEntity), registry, registryEntity, context, &holderMap);
    }

    // The instances nested in it (a variant's base level included: its root's entry is what
    // this level overrides of the base's root, as a holder's of a nested root)
    for (const auto& [entry, local, nestedPath] : nested) {
        if (local != NULL_ENTITY) {
            applyHolderData(sceneProject, local, *entry, holderMap, nullptr, NULL_ENTITY, false, depth + 1, nestedPath);
        }
    }
}

void editor::Project::applySceneData(SceneProject* sceneProject, Entity instanceRoot, const PrefabInstanceData& data, size_t depth,
        const std::filesystem::path& level) {
    std::filesystem::path path;
    Prefab::Instance* found = findInstanceAt(sceneProject, instanceRoot, level, &path);
    Prefab* prefab = found ? getPrefab(path) : nullptr;
    if (!found || !prefab || !prefab->registry || depth > 64) {
        return;
    }
    Scene* scene = sceneProject->scene;
    EntityRegistry* registry = prefab->registry.get();
    const std::string context = path.string();
    const uint32_t instanceId = found->instanceId;
    const Entity registryRoot = found->rootRegistryEntity;
    std::vector<std::pair<Entity, Entity>> objects;
    for (const auto& [registryEntity, object] : data.objects) {
        objects.emplace_back(registryEntity, found->getLocalEntity(registryEntity));
    }
    std::vector<std::tuple<const PrefabInstanceData*, Entity, std::filesystem::path>> nested;
    for (const PrefabInstanceData& entry : data.nested) {
        nested.emplace_back(&entry, findNestedLocalRoot(sceneProject, *prefab, *found, entry), nestedEntryPath(this, *prefab, entry));
    }

    for (const auto& [registryEntity, local] : objects) {
        // the root's own values are its node's (a top-level root) or the holder's entry's
        if (registryEntity == registryRoot) {
            continue;
        }
        if (!registry->isEntityCreated(registryEntity)) {
            Out::warning("Prefab override for GameObject %u of %s: the prefab has no such GameObject any more, dropped",
                registryEntity, context.c_str());
            continue;
        }
        if (local == NULL_ENTITY || !scene->isEntityCreated(local)) {
            continue; // removed from this instance
        }
        PrefabObjectOverrides applied = applyObjectEntry(sceneProject, local, data.objects.at(registryEntity), registry, registryEntity, context);
        if (!applied.empty()) {
            if (Prefab::Instance* instance = prefab->getInstanceById(sceneProject->id, instanceId)) {
                instance->overrides[local].merge(applied);
            }
        }
    }

    for (const auto& [entry, local, nestedPath] : nested) {
        if (local != NULL_ENTITY) {
            applySceneData(sceneProject, local, *entry, depth + 1, nestedPath);
        }
    }
}

void editor::Project::placeAtChildIndex(Scene* scene, Entity parent, Entity local, int childIndex) {
    // Each child is a whole branch in the Transform array, so the slot is where the
    // childIndex-th other child starts, or the end of the parent's branch
    if (!scene->findComponent<Transform>(local) || !scene->findComponent<Transform>(parent)) {
        return;
    }
    auto transforms = scene->getComponentArray<Transform>();
    size_t targetIndex = scene->findBranchLastIndex(parent) + 1;
    if (childIndex >= 0) {
        size_t siblings = 0;
        for (size_t i = transforms->getIndex(parent) + 1; i < transforms->size(); i++) {
            if (transforms->getEntity(i) == local || transforms->getComponentFromIndex(i).parent != parent) {
                continue;
            }
            if (siblings == static_cast<size_t>(childIndex)) {
                targetIndex = i;
                break;
            }
            siblings++;
        }
    }
    scene->moveChildToIndex(local, targetIndex);
}

void editor::Project::decodeSceneAddedGameObjects(SceneProject* sceneProject, std::vector<Entity>* entities, Entity instanceRoot,
        const PrefabInstanceData& data, std::unordered_map<Entity, Entity>* entityRemap, std::vector<Entity>& allResult, size_t depth,
        const std::filesystem::path& level) {
    std::filesystem::path path;
    Prefab::Instance* found = findInstanceAt(sceneProject, instanceRoot, level, &path);
    Prefab* prefab = found ? getPrefab(path) : nullptr;
    if (!found || !prefab || depth > 64) {
        return;
    }
    Scene* scene = sceneProject->scene;
    const std::string context = path.string();
    // where each goes, read before decoding (which can import other instances)
    struct Pending {
        YAML::Node node;
        Entity parent;
        size_t childIndex;
    };
    std::vector<Pending> pending;
    for (const YAML::Node& addedNode : data.addedGameObjects) {
        Entity parentRegEntity = NULL_ENTITY;
        size_t childIndex = 0;
        if (addedNode["parentRegistryEntity"]) {
            parentRegEntity = YamlNumbers::as<Entity>(addedNode["parentRegistryEntity"]);
        }
        if (addedNode["childIndex"]) {
            childIndex = YamlNumbers::as<size_t>(addedNode["childIndex"]);
        }
        // Below its prefab GameObject (the root for NULL), else below the root
        Entity parentEntity = instanceRoot;
        if (parentRegEntity != NULL_ENTITY) {
            const Entity parentLocal = found->getLocalEntity(parentRegEntity);
            if (parentLocal != NULL_ENTITY && scene->isEntityCreated(parentLocal)) {
                parentEntity = parentLocal;
            } else {
                Out::warning("GameObject '%s' added to an instance of %s: its parent is not in the instance, put below the root",
                    addedNode["name"] ? addedNode["name"].as<std::string>().c_str() : "", context.c_str());
            }
        }
        YAML::Node clean = YAML::Clone(addedNode);
        clean.remove("parentRegistryEntity");
        clean.remove("childIndex");
        pending.push_back({clean, parentEntity, childIndex});
    }
    std::vector<std::tuple<const PrefabInstanceData*, Entity, std::filesystem::path>> nested;
    for (const PrefabInstanceData& entry : data.nested) {
        nested.emplace_back(&entry, findNestedLocalRoot(sceneProject, *prefab, *found, entry), nestedEntryPath(this, *prefab, entry));
    }

    for (Pending& added : pending) {
        std::vector<Entity> decoded = Stream::decodeEntity(added.node, scene, entities, this, sceneProject,
            added.parent, true, false, entityRemap);
        if (decoded.empty()) {
            continue;
        }
        scene->addEntityChild(added.parent, decoded[0], true);
        placeAtChildIndex(scene, added.parent, decoded[0], static_cast<int>(added.childIndex));
        allResult.insert(allResult.end(), decoded.begin(), decoded.end());
    }

    for (const auto& [entry, local, nestedPath] : nested) {
        if (local != NULL_ENTITY) {
            decodeSceneAddedGameObjects(sceneProject, entities, local, *entry, entityRemap, allResult, depth + 1, nestedPath);
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
        // what the asset's file holds that this editor cannot read gates its saves
        LoadReport loadReport;
        loadReport.documentPath = FileUtils::pathToUtf8(filepath);
        {
            ScopedLoadReport reportScope(loadReport);
            bool migrated = false;
            const YAML::Node node = loadPrefabFile(getProjectPath() / filepath, &prefab.diskSha1, &migrated);
            std::string rootName = filepath.stem().string();
            prefab.loadRegistry(node, rootName.empty() ? std::string("Prefab") : rootName);
            // the migrations stamped the current version into the node, so its header no longer
            // says the file is older: the file still is, and is written again with its next save
            if (migrated) {
                prefab.needsUpgrade = true;
            }
        }
        prefab.loadReport = std::move(loadReport);
        logLoadReport(prefab.loadReport);
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
        forgetGonePrefabFile(filepath);
        return nullptr;
    }
    if (it != prefabAssets.end()) {
        // an entry without its registry (never expected): the instances it lists stay
        it->second.takeAssetData(std::move(loaded));
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

// Builds the instance in two phases. The GameObjects first (this prefab's, then each nested
// instance's, recursively, then the GameObjects this prefab adds to its nested instances):
// a nested import (`deferred`) stops there. Then the values in layers: each prefab's data for
// the instances nested in it, applied by the import of that prefab once everything below it
// exists (inner prefabs first, so outer ones win), with its references made local through
// its own map (stripped objects included); then, by the outermost import only, the scene's
// data for the whole tree and its added GameObjects.
std::vector<Entity> editor::Project::importPrefab(SceneProject* sceneProject, std::vector<Entity>* entities, const std::filesystem::path& requestedPath, Entity rootEntity,
        bool needSaveScene, const PrefabInstanceData* instanceData, std::unordered_map<Entity, Entity>* entityRemap, const PrefabMemberIds* preferredIds,
        const std::vector<PrefabInstanceData*>& assetLayers) {
    // the instance's world values, computed when the outermost import (or the scene's load) ends
    WorldTransformScope worldTransforms(sceneProject ? sceneProject->scene : nullptr);
    const bool deferred = nextImportDeferred;
    nextImportDeferred = false;
    // The base of a variant imported onto the variant's root (by the variant's import)
    const std::filesystem::path forcedPath = nextImportPath;
    nextImportPath.clear();
    const size_t levelDepth = nextImportLevel;
    nextImportLevel = 0;
    const RootKept keptAbove = nextImportRootKept;
    nextImportRootKept = RootKept();
    const bool variantBaseLevel = !forcedPath.empty();
    std::filesystem::path filepath = variantBaseLevel ? forcedPath : requestedPath;

    // Prefabs being imported up the call stack: nested imports, and cycle detection
    static thread_local std::unordered_set<std::string> loadingPaths;
    const bool nestedImport = !loadingPaths.empty();

    // Which prefab: the root's PrefabComponent names it by GUID, its path is only a hint.
    // One that cannot be found leaves a Missing Prefab instance holding the instance data.
    auto missingPrefab = [&]() -> std::vector<Entity> {
        if (variantBaseLevel) {
            // never expected: the variant's import checked its bases first
            Out::error("The base %s of a Prefab Variant could not be imported", filepath.string().c_str());
            return {};
        }
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
    PrefabComponent* rootComponent = variantBaseLevel ? nullptr : sceneProject->scene->findComponent<PrefabComponent>(rootEntity);
    if (rootComponent) {
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

    // Prefab Mode: an instance of a prefab holding the stage's asset (a paste, a drop) would
    // make the asset contain itself
    if (!nestedImport && isPrefabStage(sceneProject->id) && !canInstantiatePrefabIn(sceneProject->id, filepath)) {
        Out::error("Prefab %s cannot go into this prefab: it would contain itself", filepath.string().c_str());
        return missingPrefab();
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
    // A Prefab Variant needs its whole chain of bases: one missing (or a cycle) makes the
    // instance a Missing Prefab as a whole, keeping its data (PREFABS.md 19)
    std::filesystem::path basePath;
    if (prefabPtr->isVariant()) {
        bool broken = false;
        const std::vector<std::filesystem::path> chain = getPrefabVariantChain(filepath, &broken);
        for (size_t i = 1; i < chain.size() && !broken; i++) {
            if (loadingPaths.count(chain[i].string())) {
                Out::error("Circular prefab reference detected: %s is a base of %s", chain[i].string().c_str(), pathStr.c_str());
                broken = true;
            }
        }
        if (broken || chain.size() < 2) {
            Out::warning("Prefab Variant %s: a base of it cannot be found (or its bases make a cycle)", pathStr.c_str());
            return missingPrefab();
        }
        basePath = chain[1];
        prefabPtr = getPrefab(filepath);
        if (!prefabPtr) {
            return missingPrefab();
        }
    }
    // std::map nodes stay put while nested imports load other prefabs
    Prefab& prefab = *prefabPtr;
    const bool variant = !basePath.empty();
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
    const uint32_t instanceId = newInstance.instanceId;

    // ---- The root: the prefab's root GameObject applied to the instance's root ----

    const bool topLevel = assetLayers.empty();
    const PrefabInstanceData* rootLayer = topLevel ? instanceData : assetLayers.front();
    Entity rootParent = transformParent(scene, rootEntity);

    // What the root has of its own: its values for the default overrides and for its
    // overrides, which come from its own node (a scene's), or for a nested root imported
    // on its own, its current values
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

    // A nested root imported again on its own (a relinked Missing Prefab, a removal
    // reverted): what every level above overrides of it is kept from its current values,
    // what the instance holding it overrides of it too
    const bool standaloneNested = !topLevel && !deferred;
    if (standaloneNested) {
        for (size_t i = 1; i < assetLayers.size(); i++) {
            if (!assetLayers[i]) {
                continue;
            }
            auto entry = assetLayers[i]->objects.find(registryRoot);
            if (entry != assetLayers[i]->objects.end()) {
                rootOverrides.merge(entry->second.overrides);
            }
        }
        for (auto& [otherPath, other] : prefabAssets) {
            auto sceneIt = other.instances.find(sceneProject->id);
            if (sceneIt == other.instances.end()) {
                continue;
            }
            for (const Prefab::Instance& holder : sceneIt->second) {
                const Prefab::ObjectOverrides* held = holder.rootEntity != rootEntity ? holder.findOverrides(rootEntity) : nullptr;
                if (held) {
                    rootOverrides.merge(*held);
                }
            }
        }
    }

    // Without the project: a nested root's children are in it (left out by prepareBranch)
    YAML::Node rootNode = Stream::encodeEntity(registryRoot, registry);
    const YAML::Node rootComponents = rootNode["components"];
    std::vector<ComponentType> rootFromPrefab;
    for (ComponentType type : Catalog::findComponents(registry, registryRoot)) {
        if (type == ComponentType::PrefabComponent) {
            continue;
        }
        if (rootOverrides.removedComponents.count(type) || keptAbove.removed.count(type)) {
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
        if (hasComponent(registry, registryRoot, type) || rootOverrides.addedComponents.count(type) || keptAbove.added.count(type)) {
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
    if (!((rootOverrides.state | keptAbove.state) & PrefabObjectOverrides::StateActive)) {
        scene->setEntityActive(rootEntity, registry->isEntityActive(registryRoot));
    }
    if (!((rootOverrides.state | keptAbove.state) & PrefabObjectOverrides::StateTag)) {
        scene->setEntityTag(rootEntity, registry->getEntityTag(registryRoot));
    }
    if (!((rootOverrides.state | keptAbove.state) & PrefabObjectOverrides::StateLayer)) {
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
        auto asked = preferredIds->find(prefabLevelKey(rootEntity, levelDepth));
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
    std::unordered_map<Entity, Entity> registryToLocal;
    registryToLocal[registryRoot] = rootEntity;
    std::vector<Prefab::EntityMember> pendingNested;
    // Decodes a registry branch below `parent`: its GameObjects become members, its nested
    // roots wait for their import. Returns the first decoded one.
    auto decodeBranch = [&](YAML::Node node, Entity parent) -> Entity {
        std::vector<Entity> registryOrder;
        prepareBranch(node, absent, localId, registryOrder, prefabKey);
        std::vector<Entity> decoded = Stream::decodeEntity(node, scene, &tracked, nullptr, nullptr, parent, true, false, nullptr);
        if (decoded.size() != registryOrder.size()) {
            Out::error("importPrefab(%s): entity count mismatch: decoded=%zu, registry=%zu",
                context.c_str(), decoded.size(), registryOrder.size());
            DeleteEntityCmd::destroyEntities(scene, decoded, tracked, this, sceneProject->id);
            return NULL_ENTITY;
        }
        for (size_t i = 0; i < decoded.size(); i++) {
            newEntities.push_back(decoded[i]);
            newInstance.members.push_back({decoded[i], registryOrder[i]});
            registryToLocal[registryOrder[i]] = decoded[i];
            if (prefab.isNestedRoot(registryOrder[i])) {
                pendingNested.push_back({decoded[i], registryOrder[i]});
            }
        }
        return decoded.empty() ? NULL_ENTITY : decoded[0];
    };
    // (a variant's root children are the GameObjects it adds to its base: below, once the
    // base's GameObjects exist; the root goes first in the nested queue)
    if (variant) {
        pendingNested.push_back({rootEntity, registryRoot});
    }
    if (const YAML::Node children = rootNode["children"]; !variant && children && children.IsSequence()) {
        for (YAML::Node child : children) {
            const YAML::Node readChild = child;
            if (readChild.IsMap() && readChild["entity"] && !absent(YamlNumbers::as<Entity>(readChild["entity"]))) {
                decodeBranch(YAML::Clone(child), rootEntity);
            }
        }
    }
    // GameObjects without a Transform listed next to the root
    for (Entity topLevelEntity : getTopLevelEntities(registry, prefab.registryEntities)) {
        if (topLevelEntity == registryRoot || absent(topLevelEntity)) {
            continue;
        }
        decodeBranch(Stream::encodeEntity(topLevelEntity, registry), NULL_ENTITY);
    }

    std::vector<Entity> allResult;

    // ---- Nested instances, then the GameObjects this prefab adds to each ----
    std::vector<Prefab::EntityMember> nestedRoots;
    std::vector<std::filesystem::path> nestedPaths;   // the prefab each nested root instantiates
    for (size_t next = 0; next < pendingNested.size(); next++) {
        const Prefab::EntityMember member = pendingNested[next];
        if (!scene->isEntityCreated(member.localEntity)) {
            continue;
        }
        // the variant's root: its base, which its PrefabComponent (the variant's) does not name
        const bool baseOfVariant = variant && member.localEntity == rootEntity;
        std::string nestedPath;
        if (baseOfVariant) {
            nestedPath = basePath.string();
        } else {
            const PrefabComponent* component = scene->findComponent<PrefabComponent>(member.localEntity);
            if (!component) {
                continue;
            }
            nestedPath = component->path;
        }
        nestedRoots.push_back(member);
        nestedPaths.push_back(nestedPath);
        std::vector<PrefabInstanceData*> nestedLayers;
        nestedLayers.push_back(nestedAssetLayer(prefab, filepath, member.registryEntity, member.localEntity, sceneProject));
        for (PrefabInstanceData* layer : assetLayers) {
            if (PrefabInstanceData* found = layer ? layer->findNested(member.registryEntity, nestedPath) : nullptr) {
                nestedLayers.push_back(found);
            }
        }
        const PrefabInstanceData* nestedData = instanceData ? instanceData->findNested(member.registryEntity, nestedPath) : nullptr;
        nextImportDeferred = true;
        if (baseOfVariant) {
            nextImportPath = nestedPath;
            nextImportLevel = levelDepth + 1;
            // what this level and the ones above keep of their own on the root (the base's
            // import leaves it), as a holder keeps what it overrides of a nested root
            RootKept kept = keptAbove;
            kept.added.insert(rootOverrides.addedComponents.begin(), rootOverrides.addedComponents.end());
            kept.removed.insert(rootOverrides.removedComponents.begin(), rootOverrides.removedComponents.end());
            kept.state |= rootOverrides.state;
            nextImportRootKept = kept;
        }
        std::vector<Entity> nestedEntities = importPrefab(sceneProject, entities, nestedPath, member.localEntity, false,
            nestedData, entityRemap, preferredIds, nestedLayers);
        nextImportDeferred = false;
        nextImportPath.clear();
        nextImportLevel = 0;
        nextImportRootKept = RootKept();
        allResult.insert(allResult.end(), nestedEntities.begin(), nestedEntities.end());

        // Its added GameObjects: below the nested root, or below the nested GameObject their
        // stripped parent names, at their place among its children
        std::vector<Entity> tops = prefab.getAddedObjectTops(member.registryEntity);
        std::stable_sort(tops.begin(), tops.end(), [&prefab](Entity a, Entity b) {
            auto index = [&prefab](Entity e) {
                auto it = prefab.addedObjects.find(e);
                return (it == prefab.addedObjects.end() || it->second.childIndex < 0) ? std::numeric_limits<int>::max() : it->second.childIndex;
            };
            return index(a) < index(b);
        });
        for (Entity top : tops) {
            if (absent(top)) {
                continue;
            }
            Entity parent = member.localEntity;
            int childIndex = -1;
            auto added = prefab.addedObjects.find(top);
            if (added != prefab.addedObjects.end()) {
                childIndex = added->second.childIndex;
                if (added->second.parent != NULL_ENTITY) {
                    auto source = prefab.stripped.find(added->second.parent);
                    const Entity resolved = source != prefab.stripped.end() ?
                        resolveStrippedObject(sceneProject, registryToLocal, source->second, &prefab) : NULL_ENTITY;
                    if (resolved != NULL_ENTITY) {
                        parent = resolved;
                    } else {
                        Out::warning("GameObject '%s' that %s adds to a nested instance: its parent is not in it, put below the nested root",
                            registry->getEntityName(top).c_str(), context.c_str());
                        childIndex = -1;
                    }
                }
            }
            const Entity decoded = decodeBranch(Stream::encodeEntity(top, registry), parent);
            if (decoded != NULL_ENTITY) {
                scene->addEntityChild(parent, decoded, false);
                placeAtChildIndex(scene, parent, decoded, childIndex);
            }
        }
    }

    // ---- References of this level, stripped objects included, made local ----
    addStrippedToMap(sceneProject, prefab, registryToLocal);
    {
        // a nested root has the nested prefab's values: this prefab's own come below
        std::unordered_set<Entity> nestedLocals;
        for (const Prefab::EntityMember& member : nestedRoots) {
            nestedLocals.insert(member.localEntity);
        }
        std::vector<Entity> remapped;
        for (Entity entity : newEntities) {
            if (!nestedLocals.count(entity)) {
                remapped.push_back(entity);
            }
        }
        // anything else is None
        remapEntityProperties(scene, remapped, registryToLocal);
        // (a variant's root holds its base's values now: its own come below, as a nested
        // root's from its holder)
        for (ComponentType type : variant ? std::vector<ComponentType>() : rootFromPrefab) {
            remapEntityPropertiesInComponent(scene, rootEntity, type, {}, registryToLocal);
        }
    }
    allResult.insert(allResult.begin(), newEntities.begin(), newEntities.end());

    // ---- What this prefab overrides of the instances nested in it (its base for a variant) ----
    for (size_t n = 0; n < nestedRoots.size(); n++) {
        const Prefab::EntityMember& member = nestedRoots[n];
        if (!findInstanceAt(sceneProject, member.localEntity, nestedPaths[n])) {
            continue; // a Missing Prefab: its root keeps this prefab's values as decoded
        }
        PrefabInstanceData* data = nestedAssetLayer(prefab, filepath, member.registryEntity, member.localEntity, sceneProject);
        applyHolderData(sceneProject, member.localEntity, *data, registryToLocal, registry, member.registryEntity, false, 0, nestedPaths[n]);
    }
    for (size_t n = 0; n < nestedRoots.size(); n++) {
        const Prefab::EntityMember& member = nestedRoots[n];
        if (findInstanceAt(sceneProject, member.localEntity, nestedPaths[n]) || !scene->isEntityCreated(member.localEntity)) {
            continue;
        }
        // a Missing nested prefab's root stays with this prefab's values
        for (ComponentType type : Catalog::findComponents(scene, member.localEntity)) {
            remapEntityPropertiesInComponent(scene, member.localEntity, type, {}, registryToLocal);
        }
    }

    prefab.instances[sceneProject->id].push_back(std::move(newInstance));

    if (deferred) {
        // the import holding this one applies the layers above
        updateScenePrefabs(sceneProject);
        return allResult;
    }

    // ---- The layers above a nested root imported on its own ----
    if (standaloneNested) {
        struct Link {
            Prefab* prefab;
            std::filesystem::path path;
            Entity instanceRoot;
            Entity registryEntity;
        };
        std::vector<Link> links;
        Entity current = rootEntity;
        std::filesystem::path currentPath = filepath;
        for (size_t depth = 0; depth < 64; depth++) {
            PrefabHolderLink holder;
            if (!findPrefabHolder(sceneProject, current, currentPath, holder)) {
                break;
            }
            links.push_back({holder.prefab, holder.path, holder.instance->rootEntity, holder.registryEntity});
            current = holder.instance->rootEntity;
            currentPath = holder.path;
        }
        for (size_t j = 0; j < links.size(); j++) {
            PrefabInstanceData* layer = nestedAssetLayer(*links[j].prefab, links[j].path, links[j].registryEntity, NULL_ENTITY, nullptr);
            for (size_t i = j; i-- > 0 && layer;) {
                layer = layer->findNested(links[i].registryEntity, std::string());
            }
            if (!layer) {
                continue;
            }
            const std::unordered_map<Entity, Entity> holderMap = getPrefabReferenceMap(sceneProject->id, links[j].instanceRoot, links[j].path);
            // the root keeps its current values (below)
            applyHolderData(sceneProject, rootEntity, *layer, holderMap, nullptr, NULL_ENTITY, true, 0, filepath);
        }
    }

    // ---- The root takes back its own values: the default overrides, and its overrides ----
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
    // Added components a nested root imported on its own keeps have their current values
    if (standaloneNested) {
        for (ComponentType type : rootOverrides.addedComponents) {
            if (own.has(type) && hasComponent(scene, rootEntity, type)) {
                std::set<std::string> whole{PrefabObjectOverrides::WHOLE_COMPONENT};
                copyProperties(own, scene, rootEntity, type, whole, context);
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
    if (standaloneNested) {
        // the levels above keep their state overrides of it
        if (rootOverrides.state & PrefabObjectOverrides::StateActive) scene->setEntityActive(rootEntity, own.registry->isEntityActive(own.entity));
        if (rootOverrides.state & PrefabObjectOverrides::StateTag) scene->setEntityTag(rootEntity, own.registry->getEntityTag(own.entity));
        if (rootOverrides.state & PrefabObjectOverrides::StateLayer) scene->setEntityLayer(rootEntity, own.registry->getEntityLayer(own.entity));
    }

    // ---- The scene's data, for this instance and the ones nested in it ----
    if (instanceData) {
        applySceneData(sceneProject, rootEntity, *instanceData, 0, filepath);
    }
    if (topLevel && !rootOverrides.empty()) {
        if (Prefab::Instance* instance = prefab.getInstanceById(sceneProject->id, instanceId)) {
            instance->overrides[rootEntity] = rootOverrides;
        }
    }

    // ---- GameObjects added to this instance (and the nested ones) in the scene ----
    if (instanceData) {
        // They are the scene's, not the prefab's: an instance of this prefab among them (one
        // placed below a GameObject of this instance) is no cycle. With this import's path still
        // held it was taken for one and loaded as a Missing Prefab, its GameObjects lost when the
        // scene opened. They import as the scene's own instances do (put back on a throw too).
        struct OuterImports {
            std::unordered_set<std::string>& paths;
            std::unordered_set<std::string> held;
            explicit OuterImports(std::unordered_set<std::string>& paths) : paths(paths) { held.swap(paths); }
            ~OuterImports() { paths.swap(held); }
        } outerImports{loadingPaths};
        decodeSceneAddedGameObjects(sceneProject, entities, rootEntity, *instanceData, entityRemap, allResult, 0, filepath);
    }

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
        if (!scene->findComponent<PrefabComponent>(entity)) {
            return;
        }

        // Every level rooted here: an instance of a variant roots its bases' instances too
        std::vector<Entity> nestedMembers;
        for (auto& [path, nestedPrefab] : prefabAssets) {
            auto sceneIt = nestedPrefab.instances.find(sceneId);
            if (sceneIt == nestedPrefab.instances.end()) {
                continue;
            }
            for (const Prefab::Instance& instance : sceneIt->second) {
                if (instance.rootEntity == entity) {
                    for (const Prefab::EntityMember& member : instance.members) {
                        nestedMembers.push_back(member.localEntity);
                    }
                }
            }
        }

        for (Entity member : nestedMembers) {
            self(self, member);
        }

        for (auto& [path, nestedPrefab] : prefabAssets) {
            auto sceneIt = nestedPrefab.instances.find(sceneId);
            if (sceneIt == nestedPrefab.instances.end()) {
                continue;
            }
            auto& instances = sceneIt->second;
            instances.erase(std::remove_if(instances.begin(), instances.end(),
                [entity](const Prefab::Instance& candidate) {
                    return candidate.rootEntity == entity;
                }), instances.end());
            if (instances.empty()) {
                nestedPrefab.instances.erase(sceneIt);
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

void editor::Project::applyLayersAbove(SceneProject* sceneProject, Entity instanceRoot, const std::unordered_set<Entity>& registryEntities,
        const std::vector<Prefab::EntityMember>& nestedRoots, const std::filesystem::path& level) {
    std::filesystem::path path;
    if (!findInstanceAt(sceneProject, instanceRoot, level, &path)) {
        return;
    }
    // The instances holding this one, outwards (the variants it is the base of included),
    // with the registry entity of its root in each
    struct Link {
        Prefab* prefab;
        std::filesystem::path path;
        Entity instanceRoot;
        Entity registryEntity;
    };
    std::vector<Link> links;
    Entity current = instanceRoot;
    std::filesystem::path currentPath = path;
    for (size_t depth = 0; depth < 64; depth++) {
        PrefabHolderLink holder;
        if (!findPrefabHolder(sceneProject, current, currentPath, holder)) {
            break;
        }
        links.push_back({holder.prefab, holder.path, holder.instance->rootEntity, holder.registryEntity});
        current = holder.instance->rootEntity;
        currentPath = holder.path;
    }
    Prefab* prefab = getPrefab(path);
    for (size_t j = 0; j < links.size() && prefab; j++) {
        PrefabInstanceData* layer = nestedAssetLayer(*links[j].prefab, links[j].path, links[j].registryEntity, NULL_ENTITY, nullptr);
        for (size_t i = j; i-- > 0 && layer;) {
            layer = layer->findNested(links[i].registryEntity, std::string());
        }
        if (!layer) {
            continue;
        }
        const std::unordered_map<Entity, Entity> holderMap = getPrefabReferenceMap(sceneProject->id, links[j].instanceRoot, links[j].path);
        for (const auto& [registryEntity, object] : layer->objects) {
            Prefab::Instance* instance = findInstanceAt(sceneProject, instanceRoot, path);
            const Entity local = instance && registryEntities.count(registryEntity) ? instance->getLocalEntity(registryEntity) : NULL_ENTITY;
            if (local != NULL_ENTITY && sceneProject->scene->isEntityCreated(local)) {
                applyObjectEntry(sceneProject, local, object, prefab->registry.get(), registryEntity, path.string(), &holderMap);
            }
        }
        for (const Prefab::EntityMember& nested : nestedRoots) {
            const PrefabComponent* component = sceneProject->scene->findComponent<PrefabComponent>(nested.localEntity);
            const PrefabInstanceData* entry = layer->findNested(nested.registryEntity, component ? component->path : std::string());
            if (entry) {
                applyHolderData(sceneProject, nested.localEntity, *entry, holderMap, nullptr, NULL_ENTITY, false, 0,
                    instancePathAt(*prefab, nested.registryEntity));
            }
        }
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
    const Entity instanceRoot = instance->rootEntity;

    // Below its registry parent's GameObject; a GameObject this prefab adds to a nested
    // instance goes below the nested GameObject its stripped parent names, at its place
    const Entity registryParent = prefab->getRegistryParent(registryEntity);
    Entity localParent = instance->rootEntity;
    int addedChildIndex = -1;
    bool addedObject = false;
    if (registryParent != NULL_ENTITY) {
        localParent = prefab->getLocalEntity(sceneProject->id, instanceId, registryParent);
        if (localParent == NULL_ENTITY) {
            return {}; // below a GameObject the instance removed
        }
        if (prefab->isInstanceRoot(registryParent)) {
            addedObject = true;
            auto added = prefab->addedObjects.find(registryEntity);
            if (added != prefab->addedObjects.end()) {
                addedChildIndex = added->second.childIndex;
                auto source = added->second.parent != NULL_ENTITY ? prefab->stripped.find(added->second.parent) : prefab->stripped.end();
                if (source != prefab->stripped.end()) {
                    const Entity resolved = resolveStrippedObject(sceneProject, getPrefabReferenceMap(sceneProject->id, instanceRoot, filepath),
                        source->second, prefab);
                    if (resolved != NULL_ENTITY) {
                        localParent = resolved;
                    } else {
                        addedChildIndex = -1;
                    }
                }
            }
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

    if (addedObject) {
        scene->addEntityChild(localParent, decoded[0], false);
        placeAtChildIndex(scene, localParent, decoded[0], addedChildIndex);
        ProjectUtils::sortEntitiesByTransformOrder(scene, sceneProject->entities);
    } else {
        placeInPrefabOrder(sceneProject, *prefab, *instance, registryEntity, decoded[0], localParent, false);
    }
    instance->members.insert(instance->members.end(), created.begin(), created.end());

    // Nested prefab roots bring their own GameObjects, then the GameObjects this prefab adds
    // to them; what this prefab and the levels above override of them come after
    std::vector<Prefab::EntityMember> nestedRoots;
    std::vector<Prefab::EntityMember> pending;
    for (const Prefab::EntityMember& member : created) {
        if (prefab->isNestedRoot(member.registryEntity)) {
            pending.push_back(member);
        }
    }
    for (size_t next = 0; next < pending.size(); next++) {
        const Prefab::EntityMember member = pending[next];
        const PrefabComponent* component = scene->findComponent<PrefabComponent>(member.localEntity);
        if (!component) {
            continue;
        }
        nestedRoots.push_back(member);
        const fs::path nestedPath = component->path;
        nextImportDeferred = true;
        importPrefab(sceneProject, &sceneProject->entities, nestedPath, member.localEntity, false, nullptr, nullptr, nullptr,
            getPrefabAssetLayers(sceneProject->id, member.localEntity));
        nextImportDeferred = false;
        for (Entity top : prefab->getAddedObjectTops(member.registryEntity)) {
            Prefab::Instance* current = prefab->getInstanceById(sceneProject->id, instanceId);
            if (!current || current->removed.count(top)) {
                continue;
            }
            std::vector<Prefab::EntityMember> more = instantiatePrefabBranch(sceneProject, filepath, instanceId, top, preferredIds);
            created.insert(created.end(), more.begin(), more.end());
        }
    }

    const std::unordered_map<Entity, Entity> registryToLocal = getPrefabReferenceMap(sceneProject->id, instanceRoot, filepath);
    {
        std::unordered_set<Entity> nestedLocals;
        for (const Prefab::EntityMember& member : nestedRoots) {
            nestedLocals.insert(member.localEntity);
        }
        std::vector<Entity> remapped;
        for (Entity entity : decoded) {
            if (!nestedLocals.count(entity)) {
                remapped.push_back(entity);
            }
        }
        remapEntityProperties(scene, remapped, registryToLocal);
    }
    for (const Prefab::EntityMember& member : nestedRoots) {
        if (findInstanceRootedAt(sceneProject, member.localEntity)) {
            applyHolderData(sceneProject, member.localEntity, *nestedAssetLayer(*prefab, filepath, member.registryEntity, NULL_ENTITY, nullptr),
                registryToLocal, registry, member.registryEntity, false, 0, instancePathAt(*prefab, member.registryEntity));
        } else {
            for (ComponentType type : Catalog::findComponents(scene, member.localEntity)) {
                remapEntityPropertiesInComponent(scene, member.localEntity, type, {}, registryToLocal);
            }
        }
    }
    // What the prefabs holding this instance override of the GameObjects made again
    {
        std::unordered_set<Entity> registryEntities(registryOrder.begin(), registryOrder.end());
        applyLayersAbove(sceneProject, instanceRoot, registryEntities, nestedRoots, filepath);
    }

    // The instance's GameObjects that the Prefab has pointing at these get them back
    instance = prefab->getInstanceById(sceneProject->id, instanceId);
    if (!instance) {
        return created;
    }
    std::vector<Prefab::EntityMember> objects = instance->members;
    objects.push_back({instance->rootEntity, instance->rootRegistryEntity});
    const std::unordered_set<Entity> createdRegistry(registryOrder.begin(), registryOrder.end());
    const std::unordered_set<Entity> createdLocal(decoded.begin(), decoded.end());
    // a stripped object counts when the nested instance it is in was made again
    std::unordered_set<Entity> createdIds = createdRegistry;
    for (const auto& [id, source] : prefab->stripped) {
        if (!source.empty() && createdRegistry.count(source[0])) {
            createdIds.insert(id);
        }
    }
    for (const Prefab::EntityMember& member : objects) {
        if (createdLocal.count(member.localEntity) || !scene->isEntityCreated(member.localEntity) ||
            !registry->isEntityCreated(member.registryEntity) || prefab->isInstanceRoot(member.registryEntity)) {
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
                auto resolved = registryToLocal.find(target);
                if (target == NULL_ENTITY || !createdIds.count(target) || resolved == registryToLocal.end() ||
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
                    ref->entity = resolved->second;
                    ref->sceneId = 0;
                } else {
                    *static_cast<Entity*>(local->second.ref) = resolved->second;
                }
                updateFlags |= local->second.updateFlags;
            }
            if (updateFlags != 0) {
                Catalog::updateEntity(scene, member.localEntity, updateFlags);
            }
        }
    }

    CameraTextureLink::resolve(scene, decoded);
    updateScenePrefabs(sceneProject);
    sceneProject->isModified = true;
    sceneProject->needUpdateRender = true;
    return created;
}

// registerPrefabManager: ProjectPrefabRuntime.cpp

std::map<std::filesystem::path, std::shared_ptr<editor::BakedPrefab>> editor::Project::bakePrefabsForExport() {
    std::map<std::filesystem::path, std::shared_ptr<BakedPrefab>> baked;
    // a scene id no project scene has: the scratch instances are dropped right after
    static constexpr uint32_t bakeSceneId = 0xFFFFFF01u;

    std::vector<std::filesystem::path> paths;
    for (const auto& [path, prefab] : prefabAssets) {
        if (prefab.registry && prefab.root != NULL_ENTITY) {
            paths.push_back(path);
        }
    }
    for (const std::filesystem::path& path : paths) {
        auto result = std::make_shared<BakedPrefab>();
        result->scene = std::make_shared<Scene>();
        Scene* scene = result->scene.get();

        SceneProject scratch;
        scratch.id = bakeSceneId;
        scratch.name = "Prefab bake";
        scratch.scene = scene;

        const Entity root = scene->createUserEntity();
        PrefabComponent link;
        link.name = path.stem().string();
        link.path = path.string();
        link.guid = getPrefabGuid(path);
        scene->addComponent<PrefabComponent>(root, link);
        scratch.entities.push_back(root);
        result->root = root;

        try {
            importPrefab(&scratch, &scratch.entities, path, root, false);
        } catch (const std::exception& e) {
            Out::error("Export of prefab %s: %s", path.string().c_str(), e.what());
        }

        // The slots, as Generator's subtree sizes count them
        std::unordered_set<std::string> visiting;
        std::function<void(const std::filesystem::path&, Entity)> walk = [&](const std::filesystem::path& prefabPath, Entity localRoot) {
            auto it = prefabAssets.find(prefabPath);
            if (it == prefabAssets.end() || !it->second.registry || !visiting.insert(prefabPath.string()).second) {
                return;
            }
            Prefab& prefab = it->second;
            const Prefab::Instance* instance = localRoot != NULL_ENTITY ? findInstanceAt(&scratch, localRoot, prefabPath) : nullptr;
            const std::vector<Entity> members = Factory::getPrefabMemberEntities(prefab.registry.get(), prefab.registryEntities, prefab.root);
            for (Entity registryEntity : members) {
                result->slots.push_back(instance ? instance->getLocalEntity(registryEntity) : NULL_ENTITY);
            }
            // a variant: its base's GameObjects (the instance at the same root) come next
            // (Generator counts them the same way, Factory::getPrefabBasePath)
            if (const PrefabComponent* base = prefab.getBaseComponent(); base && !base->path.empty()) {
                walk(base->path, instance ? localRoot : NULL_ENTITY);
            }
            for (Entity registryEntity : members) {
                const PrefabComponent* component = prefab.registry->findComponent<PrefabComponent>(registryEntity);
                if (component && !component->path.empty()) {
                    const Prefab::Instance* current = localRoot != NULL_ENTITY ? findInstanceAt(&scratch, localRoot, prefabPath) : nullptr;
                    const Entity nestedLocal = current ? current->getLocalEntity(registryEntity) : NULL_ENTITY;
                    walk(component->path, nestedLocal);
                }
            }
            visiting.erase(prefabPath.string());
        };
        walk(path, findInstanceRootedAt(&scratch, root) ? root : NULL_ENTITY);
        CameraTextureLink::resolve(scene);

        // the scratch instances go; the scene goes with the bake
        for (auto& [otherPath, other] : prefabAssets) {
            other.instances.erase(bakeSceneId);
        }
        baked[path] = std::move(result);
    }
    return baked;
}

void editor::Project::promoteNestedAssetData(uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot,
        std::vector<PrefabNestedState>& before, const std::unordered_set<Entity>& skip, bool includeBase) {
    SceneProject* sceneProject = getScene(sceneId);
    Prefab* prefab = getPrefab(path);
    Prefab::Instance* holder = findInstanceAt(sceneProject, instanceRoot, path);
    if (!sceneProject || !prefab || !prefab->registry || !holder) {
        return;
    }
    // copies: recording below touches other instance lists only
    const Prefab::Instance holderCopy = *holder;

    auto saveState = [&](Entity root, const std::filesystem::path& levelPath) {
        for (const PrefabNestedState& state : before) {
            if (state.root == root && state.path == levelPath) {
                return;
            }
        }
        if (Prefab::Instance* instance = findInstanceAt(sceneProject, root, levelPath)) {
            before.push_back({root, instance->overrides, instance->removed, levelPath});
        }
    };

    // A data entry of the prefab's for the instance (of levelPath) rooted at `root`: its
    // objects' overrides, its removed GameObjects, and its nested entries for the instances
    // nested in it (whose roots' entries are overrides of GameObjects of this one; a
    // variant's base level is one of them, at the same root)
    std::function<void(Entity, const std::filesystem::path&, const PrefabInstanceData&, size_t)> promote =
            [&](Entity root, const std::filesystem::path& levelPath, const PrefabInstanceData& data, size_t depth) {
        Prefab::Instance* instance = findInstanceAt(sceneProject, root, levelPath);
        Prefab* inner = instance ? getPrefab(levelPath) : nullptr;
        if (!instance || !inner || depth > 64) {
            return;
        }
        saveState(root, levelPath);
        const Entity registryRoot = instance->rootRegistryEntity;
        for (const auto& [registryEntity, object] : data.objects) {
            const Entity local = instance->getLocalEntity(registryEntity);
            if (local == NULL_ENTITY || object.overrides.empty() || !sceneProject->scene->isEntityCreated(local)) {
                continue;
            }
            if (registryEntity == registryRoot && depth > 0) {
                continue; // a nested root's overrides are the instance holding it's (below)
            }
            Prefab::ObjectOverrides overrides = object.overrides;
            if (registryEntity == registryRoot) {
                overrides.state &= ~PrefabObjectOverrides::StateName; // the root's own
            }
            instance->overrides[local].merge(overrides);
        }
        for (Entity registryEntity : data.removedGameObjects) {
            if (inner->registry->isEntityCreated(registryEntity)) {
                instance->removed.emplace(registryEntity, std::vector<Prefab::EntityMember>());
            }
        }
        struct Nested {
            const PrefabInstanceData* entry;
            Entity local;
            std::filesystem::path path;
        };
        std::vector<Nested> nested;
        for (const PrefabInstanceData& entry : data.nested) {
            const std::filesystem::path entryPath = entry.nestedRoot != NULL_ENTITY ?
                instancePathAt(*inner, entry.nestedRoot) : std::filesystem::path(entry.nestedPath);
            nested.push_back({&entry, findNestedLocalRoot(sceneProject, *inner, *instance, entry), entryPath});
        }
        for (const Nested& item : nested) {
            if (item.local == NULL_ENTITY || (item.local != root && skip.count(item.local))) {
                continue;
            }
            // the root of an instance nested in this one: its overrides are this one's (a
            // variant's base level shares this root, which this level's entry holds)
            auto rootEntry = item.entry->objects.end();
            Prefab::Instance* nestedInstance = findInstanceAt(sceneProject, item.local, item.path);
            if (nestedInstance) {
                rootEntry = item.entry->objects.find(nestedInstance->rootRegistryEntity);
            }
            if (rootEntry != item.entry->objects.end() && !rootEntry->second.overrides.empty()) {
                if (Prefab::Instance* again = findInstanceAt(sceneProject, root, levelPath)) {
                    Prefab::ObjectOverrides overrides = rootEntry->second.overrides;
                    overrides.state &= ~PrefabObjectOverrides::StateName;
                    again->overrides[item.local].merge(overrides);
                }
            }
            promote(item.local, item.path, *item.entry, depth + 1);
        }
    };

    // An instance of a variant unpacked one level is an instance of its base now (Unity),
    // which takes what the variant overrides of it (the root's entry too: a top-level root
    // now) and what the instance overrode of the root
    if (includeBase && prefab->isVariant()) {
        const std::filesystem::path basePath = instancePathAt(*prefab, prefab->root);
        if (findInstanceAt(sceneProject, instanceRoot, basePath)) {
            PrefabInstanceData* data = nestedAssetLayer(*prefab, path, prefab->root, NULL_ENTITY, nullptr);
            promote(instanceRoot, basePath, *data, 0);
            if (Prefab::Instance* base = findInstanceAt(sceneProject, instanceRoot, basePath)) {
                if (const Prefab::ObjectOverrides* held = holderCopy.findOverrides(instanceRoot)) {
                    Prefab::ObjectOverrides rootOverrides = *held;
                    rootOverrides.state &= ~PrefabObjectOverrides::StateName;
                    if (!rootOverrides.empty()) {
                        base->overrides[instanceRoot].merge(rootOverrides);
                    }
                }
            }
        }
    }

    for (const Prefab::EntityMember& member : holderCopy.members) {
        if (!prefab->isNestedRoot(member.registryEntity) || skip.count(member.localEntity) ||
            !findInstanceRootedAt(sceneProject, member.localEntity)) {
            continue;
        }
        const std::filesystem::path memberPath = instancePathAt(*prefab, member.registryEntity);
        PrefabInstanceData* data = nestedAssetLayer(*prefab, path, member.registryEntity, NULL_ENTITY, nullptr);
        promote(member.localEntity, memberPath, *data, 0);
        // What the prefab and the scene override of its root (a member here) is the root's own
        if (Prefab::Instance* nested = findInstanceAt(sceneProject, member.localEntity, memberPath)) {
            Prefab::ObjectOverrides rootOverrides;
            auto rootEntry = data->objects.find(nested->rootRegistryEntity);
            if (rootEntry != data->objects.end()) {
                rootOverrides.merge(rootEntry->second.overrides);
            }
            if (const Prefab::ObjectOverrides* held = holderCopy.findOverrides(member.localEntity)) {
                rootOverrides.merge(*held);
            }
            rootOverrides.state &= ~PrefabObjectOverrides::StateName;
            if (!rootOverrides.empty()) {
                nested->overrides[member.localEntity].merge(rootOverrides);
            }
        }
    }
}
