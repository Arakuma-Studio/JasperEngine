// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The override model v2 APIs (editor/project/PREFABS.md, "Overrides" and "APIs"): which
// level owns a GameObject's overrides, queries, the source value of an overridden property,
// record / revert / apply primitives for properties, GameObject state and added / removed
// components, and the re-import of instances (refreshPrefabInstances) the apply path, Prefab
// Mode saves and external edits use, with the undo snapshots built on it.

#include "Project.h"
#include "ProjectPrefabInternal.h"
#include "Out.h"
#include "Stream.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectUtils.h"
#include "util/YamlNumbers.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::editor::prefabinternal;

namespace {

    using editor::Prefab;
    using editor::PrefabInstanceData;
    using editor::PrefabObjectOverrides;

    // The instance holding a root as one of its members, the way up a nesting chain
    struct OuterLink {
        const std::filesystem::path* path = nullptr;
        Prefab* prefab = nullptr;
        Prefab::Instance* instance = nullptr;
        Entity registryEntity = NULL_ENTITY;   // the inner root's registry entity in `prefab`
    };

    bool findContainingInstance(std::map<std::filesystem::path, Prefab>& prefabAssets, uint32_t sceneId, Entity root, OuterLink& link) {
        for (auto& [path, prefab] : prefabAssets) {
            auto sceneIt = prefab.instances.find(sceneId);
            if (sceneIt == prefab.instances.end()) {
                continue;
            }
            for (Prefab::Instance& instance : sceneIt->second) {
                for (const Prefab::EntityMember& member : instance.members) {
                    if (member.localEntity == root) {
                        link = {&path, &prefab, &instance, member.registryEntity};
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // The instance rooted at `root`, of the prefab its PrefabComponent names
    Prefab::Instance* instanceRootedAt(editor::Project* project, Scene* scene, uint32_t sceneId, Entity root, std::filesystem::path* path = nullptr) {
        const PrefabComponent* component = scene ? scene->findComponent<PrefabComponent>(root) : nullptr;
        Prefab* prefab = component ? project->getPrefab(component->path) : nullptr;
        Prefab::Instance* instance = prefab ? prefab->getInstance(sceneId, root) : nullptr;
        if (!instance || instance->rootEntity != root) {
            return nullptr;
        }
        if (path) {
            *path = component->path;
        }
        return instance;
    }

    std::vector<std::string> withoutDefaults(const std::vector<std::string>& properties, editor::ComponentType type, bool isRoot) {
        std::vector<std::string> kept;
        for (const std::string& property : properties) {
            if (!isRoot || !editor::Project::isPrefabRootDefaultOverride(type, property)) {
                kept.push_back(property);
            }
        }
        return kept;
    }

}

// ---- Owning level and queries ----

bool editor::Project::findPrefabObject(uint32_t sceneId, Entity entity, PrefabObjectRef& ref) {
    ref = PrefabObjectRef();
    if (entity == NULL_ENTITY) {
        return false;
    }
    // A member first: a nested root's overrides belong to the instance holding it
    for (auto& [path, prefab] : prefabAssets) {
        auto sceneIt = prefab.instances.find(sceneId);
        if (sceneIt == prefab.instances.end()) {
            continue;
        }
        for (Prefab::Instance& instance : sceneIt->second) {
            for (const Prefab::EntityMember& member : instance.members) {
                if (member.localEntity == entity) {
                    ref.path = path;
                    ref.prefab = &prefab;
                    ref.instance = &instance;
                    ref.localEntity = entity;
                    ref.registryEntity = member.registryEntity;
                    ref.isRoot = false;
                    return true;
                }
            }
        }
    }
    for (auto& [path, prefab] : prefabAssets) {
        auto sceneIt = prefab.instances.find(sceneId);
        if (sceneIt == prefab.instances.end()) {
            continue;
        }
        for (Prefab::Instance& instance : sceneIt->second) {
            if (instance.rootEntity == entity) {
                ref.path = path;
                ref.prefab = &prefab;
                ref.instance = &instance;
                ref.localEntity = entity;
                ref.registryEntity = instance.rootRegistryEntity;
                ref.isRoot = true;
                return true;
            }
        }
    }
    return false;
}

bool editor::Project::findPrefabObject(uint32_t sceneId, Entity entity, PrefabObjectRef& ref) const {
    return const_cast<Project*>(this)->findPrefabObject(sceneId, entity, ref);
}

bool editor::Project::isPrefabInstanceRoot(uint32_t sceneId, Entity entity) const {
    const SceneProject* sceneProject = getScene(sceneId);
    return sceneProject && sceneProject->scene &&
        instanceRootedAt(const_cast<Project*>(this), sceneProject->scene, sceneId, entity) != nullptr;
}

editor::Prefab::ObjectOverrides editor::Project::getPrefabObjectOverrides(uint32_t sceneId, Entity entity) const {
    PrefabObjectRef ref;
    if (!findPrefabObject(sceneId, entity, ref)) {
        return {};
    }
    const Prefab::ObjectOverrides* overrides = ref.instance->findOverrides(entity);
    return overrides ? *overrides : Prefab::ObjectOverrides();
}

bool editor::Project::isPropertyOverridden(uint32_t sceneId, Entity entity, ComponentType type, const std::string& property) const {
    return getPrefabObjectOverrides(sceneId, entity).hasProperty(type, property);
}

bool editor::Project::isComponentOverridden(uint32_t sceneId, Entity entity, ComponentType type) const {
    return getPrefabObjectOverrides(sceneId, entity).hasComponent(type);
}

std::set<std::string> editor::Project::getOverriddenProperties(uint32_t sceneId, Entity entity, ComponentType type) const {
    Prefab::ObjectOverrides overrides = getPrefabObjectOverrides(sceneId, entity);
    auto it = overrides.properties.find(type);
    return it != overrides.properties.end() ? it->second : std::set<std::string>();
}

// ---- Asset layers and source values ----

namespace {

    // The asset layers of the instance rooted at `root`, innermost first: what each prefab
    // holding it (directly or further out) overrides of it
    std::vector<PrefabInstanceData*> assetLayersOf(editor::Project* project, std::map<std::filesystem::path, Prefab>& prefabAssets,
            editor::SceneProject* sceneProject, Entity root) {
        std::vector<OuterLink> links;
        Entity current = root;
        for (size_t depth = 0; depth < 64; depth++) {
            OuterLink link;
            if (!findContainingInstance(prefabAssets, sceneProject->id, current, link)) {
                break;
            }
            links.push_back(link);
            current = link.instance->rootEntity;
        }
        std::vector<PrefabInstanceData*> layers;
        for (size_t j = 0; j < links.size(); j++) {
            PrefabInstanceData* layer = project->nestedAssetLayerFor(*links[j].prefab, *links[j].path, links[j].registryEntity);
            for (size_t i = j; i-- > 0 && layer;) {
                layer = layer->findNested(links[i].registryEntity, std::string());
            }
            if (layer) {
                layers.push_back(layer);
            }
        }
        return layers;
    }

}

editor::PrefabInstanceData* editor::Project::nestedAssetLayerFor(Prefab& prefab, const std::filesystem::path& prefabPath, Entity registryRoot) {
    return nestedAssetLayer(prefab, prefabPath, registryRoot, NULL_ENTITY, nullptr);
}

bool editor::Project::buildPrefabSource(uint32_t sceneId, Entity entity, ComponentType type, ScratchEntity& scratch, PrefabObjectRef* refOut) {
    SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || !findPrefabObject(sceneId, entity, ref) || !ref.prefab->registry) {
        return false;
    }
    EntityRegistry* registry = ref.prefab->registry.get();
    if (!hasComponent(registry, ref.registryEntity, type)) {
        return false;
    }
    scratch.takeComponents(registry, ref.registryEntity, Catalog::componentTypeToSignature(registry, type));
    scratch.takeState(registry, ref.registryEntity);

    // References in prefab ids become this instance's
    std::unordered_map<Entity, Entity> registryToLocal;
    registryToLocal[ref.instance->rootRegistryEntity] = ref.instance->rootEntity;
    for (const Prefab::EntityMember& member : ref.instance->members) {
        registryToLocal[member.registryEntity] = member.localEntity;
    }
    remapEntityPropertiesInComponent(scratch.registry.get(), scratch.entity, type, {}, registryToLocal);

    // The layers between the prefab and this instance (when it is nested)
    for (PrefabInstanceData* layer : assetLayersOf(this, prefabAssets, sceneProject, ref.instance->rootEntity)) {
        auto entry = layer->objects.find(ref.registryEntity);
        if (entry == layer->objects.end() || !entry->second.values.IsMap()) {
            continue;
        }
        auto names = entry->second.overrides.properties.find(type);
        const YAML::Node value = entry->second.values["components"] ? entry->second.values["components"][componentKey(type)] : YAML::Node(YAML::NodeType::Undefined);
        if (names == entry->second.overrides.properties.end() || !value) {
            continue;
        }
        ScratchEntity layerValue;
        layerValue.decode(singleComponent(type, value));
        std::set<std::string> copied = names->second;
        copyProperties(layerValue, scratch.registry.get(), scratch.entity, type, copied, ref.path.string());
    }
    if (refOut) {
        *refOut = ref;
    }
    return true;
}

bool editor::Project::getPrefabSourceValue(uint32_t sceneId, Entity entity, ComponentType type, const std::string& property, PrefabSourceValue& value) {
    value = PrefabSourceValue();
    ScratchEntity scratch;
    if (!buildPrefabSource(sceneId, entity, type, scratch)) {
        return false;
    }
    value.component = encodeComponent(scratch.registry.get(), scratch.entity, type);
    value.entity = scratch.entity;
    if (!property.empty() && property != PrefabObjectOverrides::WHOLE_COMPONENT) {
        value.property = Catalog::findProperty(scratch.registry.get(), scratch.entity, type, property);
    }
    value.registry = std::move(scratch.registry);
    return property.empty() || property == PrefabObjectOverrides::WHOLE_COMPONENT || value.property.ref != nullptr;
}

YAML::Node editor::Project::getPrefabSourceComponent(uint32_t sceneId, Entity entity, ComponentType type) {
    ScratchEntity scratch;
    if (!buildPrefabSource(sceneId, entity, type, scratch)) {
        return YAML::Node();
    }
    return encodeComponent(scratch.registry.get(), scratch.entity, type);
}

std::vector<std::filesystem::path> editor::Project::getPrefabApplyTargets(uint32_t sceneId, Entity entity) const {
    std::vector<std::filesystem::path> targets;
    PrefabObjectRef ref;
    if (!findPrefabObject(sceneId, entity, ref)) {
        return targets;
    }
    targets.push_back(ref.path);
    auto& assets = const_cast<std::map<std::filesystem::path, Prefab>&>(prefabAssets);
    Entity current = ref.instance->rootEntity;
    for (size_t depth = 0; depth < 64; depth++) {
        OuterLink link;
        if (!findContainingInstance(assets, sceneId, current, link)) {
            break;
        }
        targets.push_back(*link.path);
        current = link.instance->rootEntity;
    }
    return targets;
}

// ---- Primitives ----

editor::PrefabInstanceData* editor::Project::applyTargetLayer(uint32_t sceneId, const PrefabObjectRef& ref, const std::filesystem::path& target, Prefab** targetPrefab) {
    // The chain of containing instances up to the one of `target`
    std::vector<OuterLink> links;
    Entity current = ref.instance->rootEntity;
    for (size_t depth = 0; depth < 64; depth++) {
        OuterLink link;
        if (!findContainingInstance(prefabAssets, sceneId, current, link)) {
            break;
        }
        links.push_back(link);
        if (*link.path == target) {
            break;
        }
        current = link.instance->rootEntity;
    }
    if (links.empty() || *links.back().path != target) {
        return nullptr;
    }
    const size_t j = links.size() - 1;
    PrefabInstanceData* layer = nestedAssetLayer(*links[j].prefab, *links[j].path, links[j].registryEntity, NULL_ENTITY, nullptr);
    for (size_t i = j; i-- > 0;) {
        layer = &layer->nestedFor(links[i].registryEntity, std::string());
    }
    if (targetPrefab) {
        *targetPrefab = links[j].prefab;
    }
    return layer;
}

namespace {

    PrefabInstanceData::Object& layerObject(PrefabInstanceData* layer, Entity registryEntity) {
        PrefabInstanceData::Object& object = layer->objects[registryEntity];
        if (!object.values) {
            object.values = YAML::Node(YAML::NodeType::Map);
        }
        return object;
    }

    void eraseEmpty(Prefab::Instance& instance, Entity local) {
        auto it = instance.overrides.find(local);
        if (it != instance.overrides.end() && it->second.empty()) {
            instance.overrides.erase(it);
        }
    }

}

bool editor::Project::recordPropertyOverride(uint32_t sceneId, Entity entity, ComponentType type, const std::vector<std::string>& properties) {
    SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || !findPrefabObject(sceneId, entity, ref) ||
        type == ComponentType::PrefabComponent || !hasComponent(sceneProject->scene, entity, type)) {
        return false;
    }
    Prefab::ObjectOverrides& overrides = ref.instance->overrides[entity];
    // a component the instance added is its own already
    if (overrides.addedComponents.count(type) || !hasComponent(ref.prefab->registry.get(), ref.registryEntity, type)) {
        eraseEmpty(*ref.instance, entity);
        return false;
    }
    bool recorded = false;
    for (const std::string& property : properties.empty() ? std::vector<std::string>{PrefabObjectOverrides::WHOLE_COMPONENT} : properties) {
        if (ref.isRoot && isPrefabRootDefaultOverride(type, property)) {
            continue;
        }
        overrides.addProperty(type, property);
        recorded = true;
    }
    eraseEmpty(*ref.instance, entity);
    if (recorded) {
        sceneProject->isModified = true;
    }
    return recorded;
}

bool editor::Project::clearPropertyOverride(uint32_t sceneId, Entity entity, ComponentType type, const std::vector<std::string>& properties) {
    PrefabObjectRef ref;
    if (!findPrefabObject(sceneId, entity, ref)) {
        return false;
    }
    auto it = ref.instance->overrides.find(entity);
    if (it == ref.instance->overrides.end()) {
        return false;
    }
    auto names = it->second.properties.find(type);
    if (names == it->second.properties.end()) {
        return false;
    }
    if (properties.empty() || std::find(properties.begin(), properties.end(), PrefabObjectOverrides::WHOLE_COMPONENT) != properties.end()) {
        it->second.properties.erase(names);
    } else {
        if (names->second.count(PrefabObjectOverrides::WHOLE_COMPONENT)) {
            // the rest of the component stays overridden, property by property
            names->second.clear();
            for (auto& [name, data] : Catalog::findEntityProperties(getScene(sceneId)->scene, entity, type)) {
                if (std::find(properties.begin(), properties.end(), name) == properties.end() &&
                    !(ref.isRoot && isPrefabRootDefaultOverride(type, name))) {
                    names->second.insert(name);
                }
            }
        } else {
            for (const std::string& property : properties) {
                names->second.erase(property);
            }
        }
        if (names->second.empty()) {
            it->second.properties.erase(names);
        }
    }
    eraseEmpty(*ref.instance, entity);
    if (SceneProject* sceneProject = getScene(sceneId)) {
        sceneProject->isModified = true;
    }
    return true;
}

bool editor::Project::revertPropertyOverride(uint32_t sceneId, Entity entity, ComponentType type, const std::vector<std::string>& properties) {
    SceneProject* sceneProject = getScene(sceneId);
    ScratchEntity source;
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || !buildPrefabSource(sceneId, entity, type, source, &ref) ||
        !hasComponent(sceneProject->scene, entity, type)) {
        return false;
    }
    Scene* scene = sceneProject->scene;
    std::set<std::string> names;
    const bool whole = properties.empty() ||
        std::find(properties.begin(), properties.end(), PrefabObjectOverrides::WHOLE_COMPONENT) != properties.end();
    if (whole) {
        // a root keeps its default overrides
        ScratchEntity kept;
        if (ref.isRoot) {
            kept.takeComponents(scene, entity, Catalog::componentTypeToSignature(scene, type));
        }
        names.insert(PrefabObjectOverrides::WHOLE_COMPONENT);
        copyProperties(source, scene, entity, type, names, ref.path.string());
        if (ref.isRoot) {
            std::set<std::string> defaults;
            for (auto& [name, data] : Catalog::findEntityProperties(kept.registry.get(), kept.entity, type)) {
                if (isPrefabRootDefaultOverride(type, name)) {
                    defaults.insert(name);
                }
            }
            copyProperties(kept, scene, entity, type, defaults, ref.path.string());
        }
    } else {
        for (const std::string& property : withoutDefaults(properties, type, ref.isRoot)) {
            names.insert(property);
        }
        copyProperties(source, scene, entity, type, names, ref.path.string());
    }
    Catalog::updateEntity(scene, entity, Catalog::getComponentStructuralUpdateFlags(type));
    clearPropertyOverride(sceneId, entity, type, whole ? std::vector<std::string>{} : properties);
    if (type == ComponentType::CameraComponent) {
        CameraTextureLink::resolve(scene);
    }
    sceneProject->isModified = true;
    sceneProject->needUpdateRender = true;
    return true;
}

void editor::Project::recordNestedRootAssetOverride(Prefab& prefab, const std::filesystem::path& prefabPath, Entity registryEntity, ComponentType type,
        const std::vector<std::string>& properties) {
    if (!prefab.isNestedRoot(registryEntity)) {
        return;
    }
    const PrefabComponent* component = prefab.registry->findComponent<PrefabComponent>(registryEntity);
    Prefab* nested = component ? loadPrefabAsset(component->path) : nullptr;
    if (!nested || nested == &prefab) {
        return;
    }
    PrefabInstanceData* layer = nestedAssetLayer(prefab, prefabPath, registryEntity, NULL_ENTITY, nullptr);
    PrefabObjectOverrides& overrides = layer->objects[nested->root].overrides;
    if (!hasComponent(nested->registry.get(), nested->root, type)) {
        overrides.addedComponents.insert(type);
        return;
    }
    for (const std::string& property : properties.empty() ? std::vector<std::string>{PrefabObjectOverrides::WHOLE_COMPONENT} : properties) {
        if (!isPrefabRootDefaultOverride(type, property)) {
            overrides.addProperty(type, property);
        }
    }
}

bool editor::Project::applyPropertyOverride(uint32_t sceneId, Entity entity, ComponentType type, const std::vector<std::string>& requested,
        const std::filesystem::path& target) {
    SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || !findPrefabObject(sceneId, entity, ref) ||
        !hasComponent(sceneProject->scene, entity, type) || type == ComponentType::PrefabComponent) {
        return false;
    }
    Scene* scene = sceneProject->scene;
    const bool whole = requested.empty() ||
        std::find(requested.begin(), requested.end(), PrefabObjectOverrides::WHOLE_COMPONENT) != requested.end();
    const std::vector<std::string> properties = whole ? std::vector<std::string>{} : withoutDefaults(requested, type, ref.isRoot);
    if (!whole && properties.empty()) {
        return false;
    }

    std::unordered_set<Entity> memberLocals;
    memberLocals.insert(ref.instance->rootEntity);
    for (const Prefab::EntityMember& member : ref.instance->members) {
        memberLocals.insert(member.localEntity);
    }

    if (target.empty() || target == ref.path) {
        Prefab& prefab = *ref.prefab;
        EntityRegistry* registry = prefab.registry.get();
        if (!hasComponent(registry, ref.registryEntity, type) ||
            componentHasExternalEntityRef(scene, entity, type, memberLocals)) {
            return false; // a reference outside the instance cannot go into the shared Prefab
        }
        std::unordered_map<Entity, Entity> localToRegistry;
        for (Entity local : memberLocals) {
            localToRegistry[local] = ref.instance->getRegistryEntity(local);
        }
        if (whole) {
            ScratchEntity kept;
            if (ref.isRoot) {
                kept.takeComponents(registry, ref.registryEntity, Catalog::componentTypeToSignature(registry, type));
            }
            Catalog::copyComponent(scene, entity, registry, ref.registryEntity, type);
            if (ref.isRoot) {
                // the prefab root keeps its own placement
                std::set<std::string> defaults;
                for (auto& [name, data] : Catalog::findEntityProperties(kept.registry.get(), kept.entity, type)) {
                    if (isPrefabRootDefaultOverride(type, name)) {
                        defaults.insert(name);
                    }
                }
                copyProperties(kept, registry, ref.registryEntity, type, defaults, ref.path.string());
            }
        } else {
            for (const std::string& property : properties) {
                Catalog::copyPropertyValue(scene, entity, registry, ref.registryEntity, type, property);
            }
        }
        remapEntityPropertiesInComponent(registry, ref.registryEntity, type, properties, localToRegistry);
        recordNestedRootAssetOverride(prefab, ref.path, ref.registryEntity, type, properties);
        clearPropertyOverride(sceneId, entity, type, properties);
        prefab.isModified = true;
        const std::filesystem::path path = ref.path;
        refreshPrefabInstances(path);
    } else {
        Prefab* targetPrefab = nullptr;
        PrefabInstanceData* layer = applyTargetLayer(sceneId, ref, target, &targetPrefab);
        if (!layer) {
            return false;
        }
        PrefabInstanceData::Object& object = layerObject(layer, ref.registryEntity);
        if (whole) {
            object.overrides.addWholeComponent(type);
        } else {
            for (const std::string& property : properties) {
                object.overrides.addProperty(type, property);
            }
        }
        object.values["components"][componentKey(type)] = encodeComponent(scene, entity, type);
        clearPropertyOverride(sceneId, entity, type, properties);
        targetPrefab->isModified = true;
        refreshPrefabInstances(target);
    }
    sceneProject->isModified = true;
    return true;
}

bool editor::Project::recordStateOverride(uint32_t sceneId, Entity entity, uint8_t stateBits) {
    PrefabObjectRef ref;
    if (!findPrefabObject(sceneId, entity, ref)) {
        return false;
    }
    if (ref.isRoot) {
        stateBits &= ~PrefabObjectOverrides::StateName;
    }
    if (stateBits == 0) {
        return false;
    }
    ref.instance->overrides[entity].state |= stateBits;
    if (SceneProject* sceneProject = getScene(sceneId)) {
        sceneProject->isModified = true;
    }
    return true;
}

bool editor::Project::revertStateOverride(uint32_t sceneId, Entity entity, uint8_t stateBits) {
    SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || !findPrefabObject(sceneId, entity, ref)) {
        return false;
    }
    Scene* scene = sceneProject->scene;
    EntityRegistry* registry = ref.prefab->registry.get();
    // The prefab's state, then what the layers holding the instance override of it
    std::string name = registry->getEntityName(ref.registryEntity);
    bool active = registry->isEntityActive(ref.registryEntity);
    std::string tag = registry->getEntityTag(ref.registryEntity);
    int layerValue = registry->getEntityLayer(ref.registryEntity);
    for (PrefabInstanceData* layer : assetLayersOf(this, prefabAssets, sceneProject, ref.instance->rootEntity)) {
        auto entry = layer->objects.find(ref.registryEntity);
        if (entry == layer->objects.end() || !entry->second.values.IsMap()) continue;
        const YAML::Node values = entry->second.values;
        const uint8_t state = entry->second.overrides.state;
        if ((state & PrefabObjectOverrides::StateName) && values["name"]) name = values["name"].as<std::string>();
        if ((state & PrefabObjectOverrides::StateActive) && values["active"]) active = YamlNumbers::as<bool>(values["active"]);
        if ((state & PrefabObjectOverrides::StateTag) && values["tag"]) tag = values["tag"].as<std::string>();
        if ((state & PrefabObjectOverrides::StateLayer) && values["layer"]) layerValue = YamlNumbers::as<int>(values["layer"]);
    }
    if ((stateBits & PrefabObjectOverrides::StateName) && !ref.isRoot) scene->setEntityName(entity, name);
    if (stateBits & PrefabObjectOverrides::StateActive) scene->setEntityActive(entity, active);
    if (stateBits & PrefabObjectOverrides::StateTag) scene->setEntityTag(entity, tag);
    if (stateBits & PrefabObjectOverrides::StateLayer) scene->setEntityLayer(entity, layerValue);
    auto it = ref.instance->overrides.find(entity);
    if (it != ref.instance->overrides.end()) {
        it->second.state &= ~stateBits;
        eraseEmpty(*ref.instance, entity);
    }
    sceneProject->isModified = true;
    sceneProject->structureVersion++;
    return true;
}

bool editor::Project::applyStateOverride(uint32_t sceneId, Entity entity, uint8_t stateBits, const std::filesystem::path& target) {
    SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || !findPrefabObject(sceneId, entity, ref)) {
        return false;
    }
    if (ref.isRoot) {
        stateBits &= ~PrefabObjectOverrides::StateName;
    }
    if (stateBits == 0) {
        return false;
    }
    Scene* scene = sceneProject->scene;
    if (target.empty() || target == ref.path) {
        EntityRegistry* registry = ref.prefab->registry.get();
        if (stateBits & PrefabObjectOverrides::StateName) registry->setEntityName(ref.registryEntity, scene->getEntityName(entity));
        if (stateBits & PrefabObjectOverrides::StateActive) registry->setEntityActive(ref.registryEntity, scene->isEntityActive(entity));
        if (stateBits & PrefabObjectOverrides::StateTag) registry->setEntityTag(ref.registryEntity, scene->getEntityTag(entity));
        if (stateBits & PrefabObjectOverrides::StateLayer) registry->setEntityLayer(ref.registryEntity, scene->getEntityLayer(entity));
        ref.prefab->isModified = true;
    } else {
        Prefab* targetPrefab = nullptr;
        PrefabInstanceData* layer = applyTargetLayer(sceneId, ref, target, &targetPrefab);
        if (!layer) {
            return false;
        }
        PrefabInstanceData::Object& object = layerObject(layer, ref.registryEntity);
        object.overrides.state |= stateBits;
        if (stateBits & PrefabObjectOverrides::StateName) object.values["name"] = scene->getEntityName(entity);
        if (stateBits & PrefabObjectOverrides::StateActive) object.values["active"] = scene->isEntityActive(entity);
        if (stateBits & PrefabObjectOverrides::StateTag) object.values["tag"] = scene->getEntityTag(entity);
        if (stateBits & PrefabObjectOverrides::StateLayer) object.values["layer"] = scene->getEntityLayer(entity);
        targetPrefab->isModified = true;
    }
    auto it = ref.instance->overrides.find(entity);
    if (it != ref.instance->overrides.end()) {
        it->second.state &= ~stateBits;
        eraseEmpty(*ref.instance, entity);
    }
    refreshPrefabInstances(target.empty() ? ref.path : target);
    sceneProject->isModified = true;
    return true;
}

bool editor::Project::recordAddedComponent(uint32_t sceneId, Entity entity, ComponentType type) {
    SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || !findPrefabObject(sceneId, entity, ref) || type == ComponentType::PrefabComponent ||
        !hasComponent(sceneProject->scene, entity, type) || hasComponent(ref.prefab->registry.get(), ref.registryEntity, type)) {
        return false;
    }
    Prefab::ObjectOverrides& overrides = ref.instance->overrides[entity];
    overrides.addedComponents.insert(type);
    overrides.properties.erase(type);
    overrides.removedComponents.erase(type);
    sceneProject->isModified = true;
    return true;
}

bool editor::Project::revertAddedComponent(uint32_t sceneId, Entity entity, ComponentType type) {
    SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || !findPrefabObject(sceneId, entity, ref)) {
        return false;
    }
    auto it = ref.instance->overrides.find(entity);
    if (it == ref.instance->overrides.end() || !it->second.addedComponents.erase(type)) {
        return false;
    }
    eraseEmpty(*ref.instance, entity);
    if (hasComponent(sceneProject->scene, entity, type)) {
        ProjectUtils::removeEntityComponent(sceneProject->scene, entity, type, sceneProject->entities);
        ProjectUtils::reconcileTrackedEntities(sceneProject->scene, sceneProject->entities, this, sceneId);
    }
    if (type == ComponentType::CameraComponent) {
        CameraTextureLink::resolve(sceneProject->scene);
    }
    sceneProject->isModified = true;
    sceneProject->needUpdateRender = true;
    return true;
}

bool editor::Project::applyAddedComponent(uint32_t sceneId, Entity entity, ComponentType type, const std::filesystem::path& target) {
    SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || !findPrefabObject(sceneId, entity, ref)) {
        return false;
    }
    auto it = ref.instance->overrides.find(entity);
    if (it == ref.instance->overrides.end() || !it->second.addedComponents.count(type) || !hasComponent(sceneProject->scene, entity, type)) {
        return false;
    }
    Scene* scene = sceneProject->scene;
    if (target.empty() || target == ref.path) {
        std::unordered_set<Entity> memberLocals{ref.instance->rootEntity};
        std::unordered_map<Entity, Entity> localToRegistry{{ref.instance->rootEntity, ref.instance->rootRegistryEntity}};
        for (const Prefab::EntityMember& member : ref.instance->members) {
            memberLocals.insert(member.localEntity);
            localToRegistry[member.localEntity] = member.registryEntity;
        }
        if (componentHasExternalEntityRef(scene, entity, type, memberLocals)) {
            return false;
        }
        Prefab& prefab = *ref.prefab;
        ProjectUtils::addEntityComponent(prefab.registry.get(), ref.registryEntity, type, prefab.registryEntities,
            YAML::Clone(encodeComponent(scene, entity, type)));
        remapEntityPropertiesInComponent(prefab.registry.get(), ref.registryEntity, type, {}, localToRegistry);
        recordNestedRootAssetOverride(prefab, ref.path, ref.registryEntity, type, {});
        prefab.isModified = true;
    } else {
        Prefab* targetPrefab = nullptr;
        PrefabInstanceData* layer = applyTargetLayer(sceneId, ref, target, &targetPrefab);
        if (!layer) {
            return false;
        }
        PrefabInstanceData::Object& object = layerObject(layer, ref.registryEntity);
        object.overrides.addedComponents.insert(type);
        object.values["components"][componentKey(type)] = encodeComponent(scene, entity, type);
        targetPrefab->isModified = true;
    }
    it = ref.instance->overrides.find(entity);
    if (it != ref.instance->overrides.end()) {
        it->second.addedComponents.erase(type);
        eraseEmpty(*ref.instance, entity);
    }
    refreshPrefabInstances(target.empty() ? ref.path : target);
    sceneProject->isModified = true;
    return true;
}

bool editor::Project::recordRemovedComponent(uint32_t sceneId, Entity entity, ComponentType type) {
    SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || !findPrefabObject(sceneId, entity, ref) || type == ComponentType::PrefabComponent ||
        type == ComponentType::Transform || hasComponent(sceneProject->scene, entity, type) ||
        !hasComponent(ref.prefab->registry.get(), ref.registryEntity, type)) {
        return false;
    }
    Prefab::ObjectOverrides& overrides = ref.instance->overrides[entity];
    overrides.removedComponents.insert(type);
    overrides.properties.erase(type);
    overrides.addedComponents.erase(type);
    sceneProject->isModified = true;
    return true;
}

bool editor::Project::revertRemovedComponent(uint32_t sceneId, Entity entity, ComponentType type) {
    SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || !findPrefabObject(sceneId, entity, ref)) {
        return false;
    }
    auto it = ref.instance->overrides.find(entity);
    if (it == ref.instance->overrides.end() || !it->second.removedComponents.count(type)) {
        return false;
    }
    const YAML::Node source = getPrefabSourceComponent(sceneId, entity, type);
    it = ref.instance->overrides.find(entity);
    it->second.removedComponents.erase(type);
    eraseEmpty(*ref.instance, entity);
    if (source && !hasComponent(sceneProject->scene, entity, type)) {
        ProjectUtils::addEntityComponent(sceneProject->scene, entity, type, sceneProject->entities, YAML::Clone(source));
    }
    if (type == ComponentType::CameraComponent) {
        CameraTextureLink::resolve(sceneProject->scene);
    }
    sceneProject->isModified = true;
    sceneProject->needUpdateRender = true;
    return true;
}

bool editor::Project::applyRemovedComponent(uint32_t sceneId, Entity entity, ComponentType type, const std::filesystem::path& target) {
    SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || !findPrefabObject(sceneId, entity, ref)) {
        return false;
    }
    auto it = ref.instance->overrides.find(entity);
    if (it == ref.instance->overrides.end() || !it->second.removedComponents.count(type)) {
        return false;
    }
    if (target.empty() || target == ref.path) {
        Prefab& prefab = *ref.prefab;
        ProjectUtils::removeEntityComponent(prefab.registry.get(), ref.registryEntity, type, prefab.registryEntities);
        if (prefab.isNestedRoot(ref.registryEntity)) {
            const PrefabComponent* component = prefab.registry->findComponent<PrefabComponent>(ref.registryEntity);
            Prefab* nested = component ? loadPrefabAsset(component->path) : nullptr;
            if (nested && nested != &prefab && hasComponent(nested->registry.get(), nested->root, type)) {
                nestedAssetLayer(prefab, ref.path, ref.registryEntity, NULL_ENTITY, nullptr)->objects[nested->root].overrides.removedComponents.insert(type);
            }
        }
        prefab.isModified = true;
    } else {
        Prefab* targetPrefab = nullptr;
        PrefabInstanceData* layer = applyTargetLayer(sceneId, ref, target, &targetPrefab);
        if (!layer) {
            return false;
        }
        layerObject(layer, ref.registryEntity).overrides.removedComponents.insert(type);
        targetPrefab->isModified = true;
    }
    it = ref.instance->overrides.find(entity);
    if (it != ref.instance->overrides.end()) {
        it->second.removedComponents.erase(type);
        eraseEmpty(*ref.instance, entity);
    }
    refreshPrefabInstances(target.empty() ? ref.path : target);
    sceneProject->isModified = true;
    return true;
}

// ---- Re-import ----

std::vector<std::pair<uint32_t, Entity>> editor::Project::findOutermostInstancesOf(const std::filesystem::path& filepath) const {
    std::vector<std::pair<uint32_t, Entity>> result;
    const Prefab* prefab = getPrefab(filepath);
    if (!prefab) {
        return result;
    }
    for (const auto& [sceneId, instances] : prefab->instances) {
        std::vector<Entity> roots;
        for (const Prefab::Instance& instance : instances) {
            roots.push_back(instance.rootEntity);
        }
        std::vector<Entity> outermost = getOutermostPrefabRoots(sceneId, roots);
        for (Entity root : outermost) {
            if (root != NULL_ENTITY && std::find(result.begin(), result.end(), std::make_pair(sceneId, root)) == result.end()) {
                result.emplace_back(sceneId, root);
            }
        }
    }
    return result;
}

bool editor::Project::reimportPrefabInstance(SceneProject* sceneProject, Entity root, const YAML::Node& givenNode) {
    if (!sceneProject || !sceneProject->scene) {
        return false;
    }
    Scene* scene = sceneProject->scene;
    std::filesystem::path path;
    Prefab::Instance* instance = instanceRootedAt(this, scene, sceneProject->id, root, &path);
    if (!instance) {
        return false;
    }
    const YAML::Node rootNode = givenNode && givenNode.IsMap() ? givenNode : Stream::encodeEntity(root, scene, this, sceneProject);

    PrefabMemberIds memberIds;
    collectPrefabMemberIds(sceneProject->id, *instance, memberIds);
    std::vector<Entity> members;
    for (const Prefab::EntityMember& member : instance->members) {
        members.push_back(member.localEntity);
    }
    const std::vector<Entity> selection = sceneProject->selectedEntities;
    const Entity mainCamera = sceneProject->mainCamera;
    const bool wasModified = sceneProject->isModified;

    unimportPrefab(sceneProject->id, path, root, members, true, true);

    // The root as the node has it (its name, state and components; the import then gives
    // it the prefab root's values where it does not override them)
    if (rootNode["name"]) {
        scene->setEntityName(root, rootNode["name"].as<std::string>());
    }
    Stream::decodeEntityState(root, scene, rootNode);
    const YAML::Node components = rootNode["components"];
    for (ComponentType type : Catalog::findComponents(scene, root)) {
        if (type == ComponentType::PrefabComponent || type == ComponentType::Transform) {
            continue;
        }
        if (!components || !components[componentKey(type)]) {
            ProjectUtils::removeEntityComponent(scene, root, type, sceneProject->entities);
        }
    }
    if (components && components.IsMap()) {
        YAML::Node own = YAML::Clone(components);
        own.remove(componentKey(ComponentType::PrefabComponent));
        Stream::decodeComponents(root, transformParent(scene, root), scene, own);
    }

    const PrefabInstanceData data = Stream::decodePrefabInstanceData(rootNode);
    std::unordered_map<Entity, Entity> entityRemap;
    importPrefab(sceneProject, &sceneProject->entities, path, root, false, &data, &entityRemap, &memberIds);
    if (!entityRemap.empty()) {
        remapEntityProperties(scene, sceneProject->entities, entityRemap, false);
    }

    ProjectUtils::sortEntitiesByTransformOrder(scene, sceneProject->entities);
    CameraTextureLink::resolve(scene);
    if (mainCamera != NULL_ENTITY && scene->isEntityCreated(mainCamera)) {
        sceneProject->mainCamera = mainCamera;
    }
    std::vector<Entity> kept;
    for (Entity entity : selection) {
        if (scene->isEntityCreated(entity)) {
            kept.push_back(entity);
        }
    }
    sceneProject->selectedEntities = kept;
    sceneProject->isModified = wasModified;
    sceneProject->needUpdateRender = true;
    sceneProject->structureVersion++;
    return true;
}

void editor::Project::refreshPrefabInstances(const std::filesystem::path& filepath, bool reloadFromDisk) {
    Prefab* prefab = getPrefab(filepath);
    if (!prefab) {
        return;
    }
    if (reloadFromDisk) {
        Prefab loaded;
        if (!readPrefabFile(filepath, loaded)) {
            return;
        }
        // the ids are the file's, so the instances' members keep their registry entities
        loaded.registry->setLastEntity(std::max(loaded.registry->getLastEntity(), prefab->registry ? prefab->registry->getLastEntity() : NULL_ENTITY));
        prefab->registry = std::move(loaded.registry);
        prefab->registryEntities = std::move(loaded.registryEntities);
        prefab->root = loaded.root;
        prefab->nestedInstances = std::move(loaded.nestedInstances);
        prefab->needsUpgrade = loaded.needsUpgrade;
        prefab->isModified = false;
        for (auto& [sceneId, instances] : prefab->instances) {
            for (Prefab::Instance& instance : instances) {
                instance.rootRegistryEntity = prefab->root;
            }
        }
    }
    for (const auto& [sceneId, root] : findOutermostInstancesOf(filepath)) {
        SceneProject* sceneProject = getScene(sceneId);
        if (sceneProject && sceneProject->scene && sceneProject->playState == ScenePlayState::STOPPED) {
            reimportPrefabInstance(sceneProject, root, YAML::Node());
        }
    }
    for (SceneProject& sceneProject : scenes) {
        if (sceneProject.scene) {
            updateScenePrefabs(&sceneProject);
        }
    }
}

// ---- Undo ----

editor::PrefabUndoState editor::Project::capturePrefabUndoState(uint32_t sceneId, Entity entity, const std::vector<std::filesystem::path>& targets) {
    PrefabUndoState state;
    std::vector<std::pair<uint32_t, Entity>> roots;
    if (entity != NULL_ENTITY) {
        std::vector<Entity> outermost = getOutermostPrefabRoots(sceneId, {entity});
        if (!outermost.empty() && outermost[0] != NULL_ENTITY) {
            roots.emplace_back(sceneId, outermost[0]);
        }
    }
    // The targets and the prefabs holding them (their nested data and caches follow)
    std::vector<std::filesystem::path> assets = targets;
    for (size_t i = 0; i < assets.size(); i++) {
        for (const auto& [path, prefab] : prefabAssets) {
            if (!prefab.registry || std::find(assets.begin(), assets.end(), path) != assets.end()) {
                continue;
            }
            auto components = prefab.registry->getComponentArray<PrefabComponent>();
            for (size_t c = 0; c < components->size(); c++) {
                if (std::filesystem::path(components->getComponentFromIndex(c).path) == assets[i]) {
                    assets.push_back(path);
                    break;
                }
            }
        }
    }
    for (const std::filesystem::path& path : assets) {
        const Prefab* prefab = getPrefab(path);
        if (!prefab || !prefab->registry) {
            continue;
        }
        state.assets.push_back({path, encodePrefabNode(path), prefab->isModified, prefab->needsUpgrade});
        for (const auto& pair : findOutermostInstancesOf(path)) {
            if (std::find(roots.begin(), roots.end(), pair) == roots.end()) {
                roots.push_back(pair);
            }
        }
    }
    for (const auto& [rootSceneId, root] : roots) {
        SceneProject* sceneProject = getScene(rootSceneId);
        if (!sceneProject || !sceneProject->scene) {
            continue;
        }
        state.instances.push_back({rootSceneId, root, Stream::encodeEntity(root, sceneProject->scene, this, sceneProject), sceneProject->isModified});
    }
    return state;
}

void editor::Project::restorePrefabUndoState(const PrefabUndoState& state) {
    for (const PrefabUndoState::Asset& asset : state.assets) {
        Prefab* prefab = getPrefab(asset.path);
        if (!prefab) {
            continue;
        }
        std::string rootName = asset.path.stem().string();
        prefab->loadRegistry(asset.node, rootName);
        resolveRegistryPrefabRefs(prefab->registry.get());
        prefab->isModified = asset.isModified;
        prefab->needsUpgrade = asset.needsUpgrade;
        for (auto& [sceneId, instances] : prefab->instances) {
            for (Prefab::Instance& instance : instances) {
                instance.rootRegistryEntity = prefab->root;
            }
        }
    }
    for (const PrefabUndoState::InstanceState& instance : state.instances) {
        SceneProject* sceneProject = getScene(instance.sceneId);
        if (!sceneProject || !sceneProject->scene) {
            continue;
        }
        reimportPrefabInstance(sceneProject, instance.root, instance.rootNode);
        sceneProject->isModified = instance.sceneModified;
    }
    for (SceneProject& sceneProject : scenes) {
        if (sceneProject.scene) {
            updateScenePrefabs(&sceneProject);
        }
    }
}
