// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The override model v2 APIs (editor/project/PREFABS.md, "Overrides" and "APIs"): which
// level owns a GameObject's overrides, queries, the source value of an overridden property,
// record / revert / apply primitives for properties, GameObject state and added / removed
// components, and the re-import of instances (refreshPrefabInstances) the apply path, Prefab
// Mode saves and external edits use, with the undo snapshots built on it.

#include "Project.h"
#include "component/AnimatorComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "component/Transform.h"
#include "component/VertexAnimatorComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/PrefabComponent.h"
#include "command/CommandHistory.h"
#include "ProjectPrefabInternal.h"
#include "Out.h"
#include "Stream.h"
#include "pose/PoseEditSession.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectUtils.h"
#include "util/YamlNumbers.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::editor::prefabinternal;

namespace {

    using editor::Prefab;
    using editor::PrefabInstanceData;
    using editor::PrefabObjectOverrides;

    // The instances holding the instance of `path` rooted at `root`, outwards: the ones it is
    // nested in (a member) and the variants it is the base of (same root)
    std::vector<editor::PrefabHolderLink> holderChain(editor::Project* project, editor::SceneProject* sceneProject, Entity root,
            const std::filesystem::path& path) {
        std::vector<editor::PrefabHolderLink> links;
        Entity current = root;
        std::filesystem::path currentPath = path;
        if (currentPath.empty()) {
            project->findInstanceRootedAt(sceneProject, root, &currentPath);
        }
        for (size_t depth = 0; depth < 64; depth++) {
            editor::PrefabHolderLink link;
            if (!project->findPrefabHolder(sceneProject, current, currentPath, link)) {
                break;
            }
            links.push_back(link);
            current = link.instance->rootEntity;
            currentPath = link.path;
        }
        return links;
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
    // A root: the outermost level (the root of an instance of a variant roots its bases'
    // instances too, which never own its overrides)
    {
        std::filesystem::path path;
        if (Prefab::Instance* instance = findInstanceRootedAt(getScene(sceneId), entity, &path)) {
            ref.path = path;
            ref.prefab = getPrefab(path);
            ref.instance = instance;
            ref.localEntity = entity;
            ref.registryEntity = instance->rootRegistryEntity;
            ref.isRoot = true;
            return true;
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

    // The asset layers of the instance of `path` rooted at `root`, innermost first: what each
    // prefab holding it (directly or further out, the variants it is the base of included)
    // overrides of it
    std::vector<PrefabInstanceData*> assetLayersOf(editor::Project* project, editor::SceneProject* sceneProject, Entity root,
            const std::filesystem::path& path, std::vector<std::pair<Entity, std::filesystem::path>>* holders = nullptr) {
        const std::vector<editor::PrefabHolderLink> links = holderChain(project, sceneProject, root, path);
        std::vector<PrefabInstanceData*> layers;
        for (size_t j = 0; j < links.size(); j++) {
            PrefabInstanceData* layer = project->nestedAssetLayerFor(*links[j].prefab, links[j].path, links[j].registryEntity);
            for (size_t i = j; i-- > 0 && layer;) {
                layer = layer->findNested(links[i].registryEntity, std::string());
            }
            if (layer) {
                layers.push_back(layer);
                if (holders) {
                    // the layer's values are in the ids of the prefab of this instance
                    holders->emplace_back(links[j].instance->rootEntity, links[j].path);
                }
            }
        }
        return layers;
    }

}

std::vector<editor::PrefabInstanceData*> editor::Project::getPrefabAssetLayers(uint32_t sceneId, Entity instanceRoot, const std::filesystem::path& level) {
    SceneProject* sceneProject = getScene(sceneId);
    return sceneProject ? assetLayersOf(this, sceneProject, instanceRoot, level) : std::vector<PrefabInstanceData*>();
}

std::vector<std::filesystem::path> editor::Project::getInstanceRootLevels(uint32_t sceneId, Entity entity) const {
    std::vector<std::filesystem::path> paths;
    SceneProject* sceneProject = const_cast<Project*>(this)->getScene(sceneId);
    for (const PrefabInstanceLevel& level : const_cast<Project*>(this)->getInstanceLevels(sceneProject, entity)) {
        paths.push_back(level.path);
    }
    return paths;
}

bool editor::Project::buildRootSource(SceneProject* sceneProject, Entity localRoot, const std::filesystem::path& path, ComponentType type,
        ScratchEntity& out, size_t depth) {
    Prefab* prefab = getPrefab(path);
    if (!prefab || !prefab->registry || prefab->root == NULL_ENTITY || depth > 64) {
        return false;
    }
    EntityRegistry* registry = prefab->registry.get();
    if (!hasComponent(registry, prefab->root, type)) {
        return false;
    }
    const std::unordered_map<Entity, Entity> map = getPrefabReferenceMap(sceneProject->id, localRoot, path);
    // A variant's root: its base's root, then what the variant overrides of it (an added
    // component is the variant's own value), never its whole stored copy (stale after the
    // base changed)
    if (prefab->isVariant()) {
        const std::filesystem::path basePath = instancePathAt(*prefab, prefab->root);
        const Prefab* base = getPrefab(basePath);
        PrefabObjectOverrides held;
        if (base && base->root != NULL_ENTITY) {
            PrefabInstanceData* data = nestedAssetLayer(*prefab, path, prefab->root, NULL_ENTITY, nullptr);
            auto entry = data->objects.find(base->root);
            if (entry != data->objects.end()) {
                held = entry->second.overrides;
            }
        }
        if (!held.addedComponents.count(type) && buildRootSource(sceneProject, localRoot, basePath, type, out, depth + 1)) {
            ScratchEntity own;
            own.takeComponents(registry, prefab->root, Catalog::componentTypeToSignature(registry, type));
            remapEntityPropertiesInComponent(own.registry.get(), own.entity, type, {}, map);
            std::set<std::string> names;
            auto overridden = held.properties.find(type);
            if (overridden != held.properties.end()) {
                names = overridden->second;
            }
            if (!names.empty()) {
                copyProperties(own, out.registry.get(), out.entity, type, names, path.string());
            }
            if (held.state & PrefabObjectOverrides::StateActive) out.registry->setEntityActive(out.entity, registry->isEntityActive(prefab->root));
            if (held.state & PrefabObjectOverrides::StateTag) out.registry->setEntityTag(out.entity, registry->getEntityTag(prefab->root));
            if (held.state & PrefabObjectOverrides::StateLayer) out.registry->setEntityLayer(out.entity, registry->getEntityLayer(prefab->root));
            return true;
        }
    }
    out.takeComponents(registry, prefab->root, Catalog::componentTypeToSignature(registry, type));
    out.takeState(registry, prefab->root);
    remapEntityPropertiesInComponent(out.registry.get(), out.entity, type, {}, map);
    return true;
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
    // References in prefab ids (stripped objects included) become this instance's
    const std::unordered_map<Entity, Entity> registryToLocal = getPrefabReferenceMap(sceneId, ref.instance->rootEntity, ref.path);

    // A nested instance's root (or a variant's root, its base's instance): its prefab's
    // root (that prefab's base's for a variant, recursively), then what this prefab
    // overrides of it (the values of its nested root here, the default overrides always),
    // never this prefab's whole copy, which only holds those (PREFABS.md 17, 18)
    std::filesystem::path nestedPath;
    Prefab::Instance* nestedInstance = nullptr;
    if (ref.prefab->isInstanceRoot(ref.registryEntity)) {
        nestedPath = instancePathAt(*ref.prefab, ref.registryEntity);
        nestedInstance = findInstanceAt(sceneProject, ref.localEntity, nestedPath);
    }
    Prefab* nested = nestedInstance ? getPrefab(nestedPath) : nullptr;
    PrefabObjectOverrides held;
    if (nested && nested->registry) {
        PrefabInstanceData* data = nestedAssetLayer(*ref.prefab, ref.path, ref.registryEntity, NULL_ENTITY, nullptr);
        auto entry = data->objects.find(nested->root);
        if (entry != data->objects.end()) {
            held = entry->second.overrides;
        }
    }
    ScratchEntity nestedSource;
    if (nested && nested->registry && !held.addedComponents.count(type) &&
            buildRootSource(sceneProject, ref.localEntity, nestedPath, type, nestedSource)) {
        scratch.takeComponents(nestedSource.registry.get(), nestedSource.entity, Catalog::componentTypeToSignature(nestedSource.registry.get(), type));
        scratch.takeState(nestedSource.registry.get(), nestedSource.entity);
        ScratchEntity own;
        own.takeComponents(registry, ref.registryEntity, Catalog::componentTypeToSignature(registry, type));
        remapEntityPropertiesInComponent(own.registry.get(), own.entity, type, {}, registryToLocal);
        std::set<std::string> names;
        auto overridden = held.properties.find(type);
        if (overridden != held.properties.end()) {
            names = overridden->second;
        }
        if (!names.count(PrefabObjectOverrides::WHOLE_COMPONENT)) {
            for (auto& [name, property] : Catalog::findEntityProperties(own.registry.get(), own.entity, type)) {
                if (isPrefabRootDefaultOverride(type, name)) {
                    names.insert(name);
                }
            }
        }
        if (!names.empty()) {
            copyProperties(own, scratch.registry.get(), scratch.entity, type, names, ref.path.string());
        }
        scratch.registry->setEntityName(scratch.entity, registry->getEntityName(ref.registryEntity));
        if (held.state & PrefabObjectOverrides::StateActive) scratch.registry->setEntityActive(scratch.entity, registry->isEntityActive(ref.registryEntity));
        if (held.state & PrefabObjectOverrides::StateTag) scratch.registry->setEntityTag(scratch.entity, registry->getEntityTag(ref.registryEntity));
        if (held.state & PrefabObjectOverrides::StateLayer) scratch.registry->setEntityLayer(scratch.entity, registry->getEntityLayer(ref.registryEntity));
    } else {
        scratch.takeComponents(registry, ref.registryEntity, Catalog::componentTypeToSignature(registry, type));
        scratch.takeState(registry, ref.registryEntity);
        remapEntityPropertiesInComponent(scratch.registry.get(), scratch.entity, type, {}, registryToLocal);
    }

    // The layers between the prefab and this instance (when it is nested, or the base of a
    // variant), their references in the ids of the prefab holding each made local
    std::vector<std::pair<Entity, std::filesystem::path>> holderRoots;
    const std::vector<PrefabInstanceData*> layers = assetLayersOf(this, sceneProject, ref.instance->rootEntity, ref.path, &holderRoots);
    for (size_t l = 0; l < layers.size(); l++) {
        PrefabInstanceData* layer = layers[l];
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
        if (layerValue.has(type)) {
            remapEntityPropertiesInComponent(layerValue.registry.get(), layerValue.entity, type, {},
                getPrefabReferenceMap(sceneId, holderRoots[l].first, holderRoots[l].second));
        }
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
    // An instance root is the root GameObject of its own prefab, and of that prefab's bases
    // when it is a variant: the innermost one its values can go to (Unity's "Apply to Prefab
    // 'Inner'", "Apply to base Prefab"), then each variant over it, then the holding prefab
    // that keeps them as its override of that prefab (for a nested root)
    const std::vector<std::filesystem::path> levels = getInstanceRootLevels(sceneId, entity);
    for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
        if (*it != ref.path && std::find(targets.begin(), targets.end(), *it) == targets.end()) {
            targets.push_back(*it);
        }
    }
    targets.push_back(ref.path);
    // The instances holding the owning one: the variants it is the base of, the prefabs it
    // is nested in, outwards
    Project* self = const_cast<Project*>(this);
    for (const PrefabHolderLink& link : holderChain(self, self->getScene(sceneId), ref.instance->rootEntity, ref.path)) {
        if (std::find(targets.begin(), targets.end(), link.path) == targets.end()) {
            targets.push_back(link.path);
        }
    }
    return targets;
}

// ---- Primitives ----

editor::PrefabInstanceData* editor::Project::applyTargetLayer(uint32_t sceneId, const PrefabObjectRef& ref, const std::filesystem::path& target, Prefab** targetPrefab) {
    // The chain of holding instances up to the one of `target` (a variant of the owning
    // prefab holds its instance at the same root)
    std::vector<PrefabHolderLink> links;
    for (const PrefabHolderLink& link : holderChain(this, getScene(sceneId), ref.instance->rootEntity, ref.path)) {
        links.push_back(link);
        if (link.path == target) {
            break;
        }
    }
    if (links.empty() || links.back().path != target) {
        return nullptr;
    }
    const size_t j = links.size() - 1;
    PrefabInstanceData* layer = nestedAssetLayer(*links[j].prefab, links[j].path, links[j].registryEntity, NULL_ENTITY, nullptr);
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
    // a reverted clip list or pose animation loads again, as a decode does (the whole-component
    // copy also brings the prefab's own runtime flags)
    if (type == ComponentType::AnimationPlayerComponent) {
        if (AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(entity)) {
            player->needsLoad = true;
        }
    } else if (type == ComponentType::PoseAnimatorComponent) {
        if (PoseAnimatorComponent* animator = scene->findComponent<PoseAnimatorComponent>(entity)) {
            animator->needsLoad = true;
        }
    } else if (type == ComponentType::VertexAnimatorComponent) {
        if (VertexAnimatorComponent* animator = scene->findComponent<VertexAnimatorComponent>(entity)) {
            animator->needsLoad = true;
        }
    } else if (type == ComponentType::AnimatorComponent) {
        if (AnimatorComponent* animator = scene->findComponent<AnimatorComponent>(entity)) {
            animator->needsLoad = true;
        }
    }
    clearPropertyOverride(sceneId, entity, type, whole ? std::vector<std::string>{} : properties);
    if (CameraTextureLink::isLinkSourceType(type)) {
        CameraTextureLink::resolve(scene);
    }
    sceneProject->isModified = true;
    sceneProject->needUpdateRender = true;
    return true;
}

void editor::Project::recordNestedRootAssetOverride(Prefab& prefab, const std::filesystem::path& prefabPath, Entity registryEntity, ComponentType type,
        const std::vector<std::string>& properties) {
    // (a variant's root too: the variant's override of its base's root)
    if (!prefab.isInstanceRoot(registryEntity)) {
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

void editor::Project::recordNestedRootAssetState(Prefab& prefab, const std::filesystem::path& prefabPath, Entity registryEntity, uint8_t stateBits) {
    stateBits &= ~PrefabObjectOverrides::StateName;  // a root's name is its own
    if (!prefab.isInstanceRoot(registryEntity) || stateBits == 0) {
        return;
    }
    const PrefabComponent* component = prefab.registry->findComponent<PrefabComponent>(registryEntity);
    Prefab* nested = component ? loadPrefabAsset(component->path) : nullptr;
    if (!nested || nested == &prefab || nested->root == NULL_ENTITY) {
        return;
    }
    // The values belong to the root's registry entity.
    nestedAssetLayer(prefab, prefabPath, registryEntity, NULL_ENTITY, nullptr)->objects[nested->root].overrides.state |= stateBits;
}

void editor::Project::recordNestedRootAssetRemoval(Prefab& prefab, const std::filesystem::path& prefabPath, Entity registryEntity, ComponentType type) {
    if (!prefab.isInstanceRoot(registryEntity)) {
        return;
    }
    const PrefabComponent* component = prefab.registry->findComponent<PrefabComponent>(registryEntity);
    Prefab* nested = component ? loadPrefabAsset(component->path) : nullptr;
    if (!nested || nested == &prefab || nested->root == NULL_ENTITY) {
        return;
    }
    PrefabObjectOverrides& overrides = nestedAssetLayer(prefab, prefabPath, registryEntity, NULL_ENTITY, nullptr)->objects[nested->root].overrides;
    overrides.properties.erase(type);
    if (!overrides.addedComponents.erase(type) && buildRootHas(component->path, type)) {
        overrides.removedComponents.insert(type);
    }
}

bool editor::Project::buildRootHas(const std::filesystem::path& path, ComponentType type, size_t depth) {
    // Whether the root of `path`, as its instances have it, has the component: a variant's
    // as its base's with what the variant adds and removes
    Prefab* prefab = loadPrefabAsset(path);
    if (!prefab || !prefab->registry || prefab->root == NULL_ENTITY || depth > 64) {
        return false;
    }
    if (!prefab->isVariant()) {
        return hasComponent(prefab->registry.get(), prefab->root, type);
    }
    const std::filesystem::path basePath = instancePathAt(*prefab, prefab->root);
    const Prefab* base = loadPrefabAsset(basePath);
    if (!base || base->root == NULL_ENTITY) {
        return hasComponent(prefab->registry.get(), prefab->root, type);
    }
    const PrefabObjectOverrides& held = nestedAssetLayer(*prefab, path, prefab->root, NULL_ENTITY, nullptr)->objects[base->root].overrides;
    if (held.addedComponents.count(type)) {
        return true;
    }
    if (held.removedComponents.count(type)) {
        return false;
    }
    return buildRootHas(basePath, type, depth + 1);
}

bool editor::Project::applyPropertyOverride(uint32_t sceneId, Entity entity, ComponentType type, const std::vector<std::string>& requested,
        const std::filesystem::path& target) {
    SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || !findPrefabObject(sceneId, entity, ref) ||
        !hasComponent(sceneProject->scene, entity, type) || type == ComponentType::PrefabComponent) {
        return false;
    }
    if (!canApplyPrefabOverrides()) {
        return false;
    }
    Scene* scene = sceneProject->scene;
    const bool whole = requested.empty() ||
        std::find(requested.begin(), requested.end(), PrefabObjectOverrides::WHOLE_COMPONENT) != requested.end();
    // (a nested root going to its own prefab is that prefab's root: its defaults stay out)
    const bool nestedRootTarget = isNestedRootApplyTarget(sceneId, ref, target);
    const std::vector<std::string> properties = whole ? std::vector<std::string>{} :
        withoutDefaults(requested, type, ref.isRoot || nestedRootTarget);
    if (!whole && properties.empty()) {
        return false;
    }

    const Entity instanceRoot = ref.instance->rootEntity;
    if (nestedRootTarget) {
        // the nested root's own prefab (or one of its bases, for a variant): its references
        // in that prefab's ids (stripped objects for the instances nested in it)
        Prefab* nested = getPrefab(target);
        Prefab::Instance* inner = findInstanceAt(sceneProject, entity, target);
        if (!inner || !nested->registry || !hasComponent(nested->registry.get(), nested->root, type)) {
            return false;
        }
        if (componentReferencesOutside(sceneId, entity, entity, type, target)) {
            return false;
        }
        EntityRegistry* registry = nested->registry.get();
        if (whole) {
            ScratchEntity kept;
            kept.takeComponents(registry, nested->root, Catalog::componentTypeToSignature(registry, type));
            Catalog::copyComponent(scene, entity, registry, nested->root, type);
            std::set<std::string> defaults;
            for (auto& [name, data] : Catalog::findEntityProperties(kept.registry.get(), kept.entity, type)) {
                if (isPrefabRootDefaultOverride(type, name)) {
                    defaults.insert(name);
                }
            }
            copyProperties(kept, registry, nested->root, type, defaults, target.string());
        } else {
            for (const std::string& property : properties) {
                Catalog::copyPropertyValue(scene, entity, registry, nested->root, type, property);
            }
        }
        remapReferencesToPrefab(sceneId, entity, registry, nested->root, type, properties, target);
        // a variant (one of the bases of a variant root) keeps it as its override of its base
        recordNestedRootAssetOverride(*nested, target, nested->root, type, properties);
        clearPropertyOverride(sceneId, entity, type, properties);
        clearIntermediateOverrides(sceneId, ref, target, type, properties, 0);
        nested->isModified = true;
        refreshPrefabInstances(target);
        sceneProject->isModified = true;
        return true;
    }

    if (target.empty() || target == ref.path) {
        Prefab& prefab = *ref.prefab;
        EntityRegistry* registry = prefab.registry.get();
        // references into the instances nested in it are the prefab's (stripped objects)
        if (!hasComponent(registry, ref.registryEntity, type) ||
            componentReferencesOutside(sceneId, instanceRoot, entity, type, ref.path)) {
            return false; // a reference outside the instance cannot go into the shared Prefab
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
        remapReferencesToPrefab(sceneId, instanceRoot, registry, ref.registryEntity, type, properties, ref.path);
        recordNestedRootAssetOverride(prefab, ref.path, ref.registryEntity, type, properties);
        clearPropertyOverride(sceneId, entity, type, properties);
        clearIntermediateOverrides(sceneId, ref, ref.path, type, properties, 0);
        prefab.isModified = true;
        const std::filesystem::path path = ref.path;
        refreshPrefabInstances(path);
    } else {
        // the value as the target prefab holds it: references in its ids
        bool outside = false;
        const Entity holderRoot = findHolderInstanceRoot(sceneId, instanceRoot, target, ref.path);
        const YAML::Node value = holderRoot != NULL_ENTITY ? encodeComponentForPrefab(sceneId, holderRoot, entity, type, &outside, target) : YAML::Node();
        if (!value || outside) {
            return false;
        }
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
        object.values["components"][componentKey(type)] = value;
        clearPropertyOverride(sceneId, entity, type, properties);
        clearIntermediateOverrides(sceneId, ref, target, type, properties, 0);
        targetPrefab->isModified = true;
        refreshPrefabInstances(target);
    }
    sceneProject->isModified = true;
    return true;
}

void editor::Project::clearIntermediateOverrides(uint32_t sceneId, const PrefabObjectRef& ref, const std::filesystem::path& target,
        ComponentType type, const std::vector<std::string>& properties, uint8_t stateBits) {
    // Unity: a value applied to a prefab is what the instance shows, so what the prefabs
    // between that one and the instance (the variants over it, the prefabs holding it)
    // override of the same property goes from them
    const std::vector<std::filesystem::path> targets = getPrefabApplyTargets(sceneId, ref.localEntity);
    auto at = std::find(targets.begin(), targets.end(), target.empty() ? ref.path : target);
    if (at == targets.end()) {
        return;
    }
    SceneProject* sceneProject = getScene(sceneId);
    const std::vector<std::filesystem::path> rootLevels = getInstanceRootLevels(sceneId, ref.localEntity);
    for (auto it = std::next(at); it != targets.end(); ++it) {
        const std::filesystem::path path = *it;
        Prefab* prefab = getPrefab(path);
        if (!prefab || !prefab->registry) {
            continue;
        }
        PrefabObjectOverrides* overrides = nullptr;
        auto entryOf = [&](PrefabInstanceData* data, Entity key) {
            auto entry = data ? data->objects.find(key) : std::map<Entity, PrefabInstanceData::Object>::iterator();
            return (data && entry != data->objects.end()) ? &entry->second.overrides : nullptr;
        };
        if (path != ref.path && std::find(rootLevels.begin(), rootLevels.end(), path) != rootLevels.end()) {
            // a variant over the root's prefab: its override of its base's root
            const Prefab* base = prefab->isVariant() ? getPrefab(instancePathAt(*prefab, prefab->root)) : nullptr;
            if (base && base->root != NULL_ENTITY) {
                overrides = entryOf(nestedAssetLayer(*prefab, path, prefab->root, NULL_ENTITY, nullptr), base->root);
            }
        } else if (path == ref.path) {
            // the owning prefab overrides the root of the instance nested at this GameObject
            const Prefab* inner = prefab->isInstanceRoot(ref.registryEntity) ? getPrefab(instancePathAt(*prefab, ref.registryEntity)) : nullptr;
            if (inner && inner->root != NULL_ENTITY) {
                overrides = entryOf(nestedAssetLayer(*prefab, path, ref.registryEntity, NULL_ENTITY, nullptr), inner->root);
            }
        } else {
            overrides = entryOf(applyTargetLayer(sceneId, ref, path, nullptr), ref.registryEntity);
        }
        if (!overrides) {
            continue;
        }
        bool changed = false;
        if (stateBits != 0) {
            if (overrides->state & stateBits) {
                overrides->state &= ~stateBits;
                changed = true;
            }
        } else {
            auto names = overrides->properties.find(type);
            if (names != overrides->properties.end()) {
                if (properties.empty()) {
                    overrides->properties.erase(names);
                } else {
                    if (names->second.count(PrefabObjectOverrides::WHOLE_COMPONENT) && sceneProject && sceneProject->scene) {
                        // the rest of the component stays overridden there, property by property
                        names->second.clear();
                        for (auto& [name, data] : Catalog::findEntityProperties(sceneProject->scene, ref.localEntity, type)) {
                            if (std::find(properties.begin(), properties.end(), name) == properties.end()) {
                                names->second.insert(name);
                            }
                        }
                    } else {
                        for (const std::string& property : properties) {
                            names->second.erase(property);
                        }
                    }
                    if (names->second.empty()) {
                        overrides->properties.erase(names);
                    }
                }
                changed = true;
            }
        }
        if (changed) {
            prefab->isModified = true;
        }
    }
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

bool editor::Project::getPrefabSourceState(uint32_t sceneId, Entity entity, std::string& name, bool& active, std::string& tag, int& layerValue) {
    SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || !findPrefabObject(sceneId, entity, ref) || !ref.prefab->registry) {
        return false;
    }
    EntityRegistry* registry = ref.prefab->registry.get();
    // The prefab's state, then what the layers holding the instance override of it
    name = registry->getEntityName(ref.registryEntity);
    active = registry->isEntityActive(ref.registryEntity);
    tag = registry->getEntityTag(ref.registryEntity);
    layerValue = registry->getEntityLayer(ref.registryEntity);
    // A nested instance's root (or a variant's root: its base's): its prefab's root's state
    // (a variant's: its base's, but what the variant overrides, down the chain) but what
    // this prefab overrides (its name is the nested root's here, a default override)
    std::function<bool(const std::filesystem::path&, bool&, std::string&, int&, size_t)> rootState =
            [&](const std::filesystem::path& path, bool& a, std::string& t, int& l, size_t depth) -> bool {
        Prefab* prefab = getPrefab(path);
        if (!prefab || !prefab->registry || prefab->root == NULL_ENTITY || depth > 64) {
            return false;
        }
        EntityRegistry* own = prefab->registry.get();
        a = own->isEntityActive(prefab->root);
        t = own->getEntityTag(prefab->root);
        l = own->getEntityLayer(prefab->root);
        if (prefab->isVariant()) {
            const std::filesystem::path basePath = instancePathAt(*prefab, prefab->root);
            const Prefab* base = getPrefab(basePath);
            bool baseActive = a;
            std::string baseTag = t;
            int baseLayer = l;
            if (base && rootState(basePath, baseActive, baseTag, baseLayer, depth + 1)) {
                uint8_t held = 0;
                PrefabInstanceData* data = nestedAssetLayer(*prefab, path, prefab->root, NULL_ENTITY, nullptr);
                auto entry = data->objects.find(base->root);
                if (entry != data->objects.end()) {
                    held = entry->second.overrides.state;
                }
                if (!(held & PrefabObjectOverrides::StateActive)) a = baseActive;
                if (!(held & PrefabObjectOverrides::StateTag)) t = baseTag;
                if (!(held & PrefabObjectOverrides::StateLayer)) l = baseLayer;
            }
        }
        return true;
    };
    if (ref.prefab->isInstanceRoot(ref.registryEntity)) {
        const std::filesystem::path nestedPath = instancePathAt(*ref.prefab, ref.registryEntity);
        const Prefab* nested = findInstanceAt(sceneProject, ref.localEntity, nestedPath) ? getPrefab(nestedPath) : nullptr;
        bool nestedActive = active;
        std::string nestedTag = tag;
        int nestedLayer = layerValue;
        if (nested && nested->registry && nested->root != NULL_ENTITY && rootState(nestedPath, nestedActive, nestedTag, nestedLayer, 0)) {
            PrefabObjectOverrides held;
            PrefabInstanceData* data = nestedAssetLayer(*ref.prefab, ref.path, ref.registryEntity, NULL_ENTITY, nullptr);
            auto entry = data->objects.find(nested->root);
            if (entry != data->objects.end()) {
                held = entry->second.overrides;
            }
            if (!(held.state & PrefabObjectOverrides::StateActive)) active = nestedActive;
            if (!(held.state & PrefabObjectOverrides::StateTag)) tag = nestedTag;
            if (!(held.state & PrefabObjectOverrides::StateLayer)) layerValue = nestedLayer;
        }
    }
    for (PrefabInstanceData* layer : assetLayersOf(this, sceneProject, ref.instance->rootEntity, ref.path)) {
        auto entry = layer->objects.find(ref.registryEntity);
        if (entry == layer->objects.end() || !entry->second.values.IsMap()) continue;
        const YAML::Node values = entry->second.values;
        const uint8_t state = entry->second.overrides.state;
        if ((state & PrefabObjectOverrides::StateName) && values["name"]) name = values["name"].as<std::string>();
        if ((state & PrefabObjectOverrides::StateActive) && values["active"]) active = YamlNumbers::as<bool>(values["active"]);
        if ((state & PrefabObjectOverrides::StateTag) && values["tag"]) tag = values["tag"].as<std::string>();
        if ((state & PrefabObjectOverrides::StateLayer) && values["layer"]) layerValue = YamlNumbers::as<int>(values["layer"]);
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
    std::string name;
    bool active = true;
    std::string tag;
    int layerValue = 0;
    if (!getPrefabSourceState(sceneId, entity, name, active, tag, layerValue)) {
        return false;
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
    if (isNestedRootApplyTarget(sceneId, ref, target)) {
        stateBits &= ~PrefabObjectOverrides::StateName; // the nested prefab's root's name is per instance
    }
    if (stateBits == 0 || !canApplyPrefabOverrides()) {
        return false;
    }
    Scene* scene = sceneProject->scene;
    if (isNestedRootApplyTarget(sceneId, ref, target)) {
        Prefab* nested = getPrefab(target);
        if (!nested || !nested->registry) {
            return false;
        }
        EntityRegistry* registry = nested->registry.get();
        if (stateBits & PrefabObjectOverrides::StateActive) registry->setEntityActive(nested->root, scene->isEntityActive(entity));
        if (stateBits & PrefabObjectOverrides::StateTag) registry->setEntityTag(nested->root, scene->getEntityTag(entity));
        if (stateBits & PrefabObjectOverrides::StateLayer) registry->setEntityLayer(nested->root, scene->getEntityLayer(entity));
        recordNestedRootAssetState(*nested, target, nested->root, stateBits);
        clearIntermediateOverrides(sceneId, ref, target, ComponentType::PrefabComponent, {}, stateBits);
        nested->isModified = true;
    } else if (target.empty() || target == ref.path) {
        EntityRegistry* registry = ref.prefab->registry.get();
        if (stateBits & PrefabObjectOverrides::StateName) registry->setEntityName(ref.registryEntity, scene->getEntityName(entity));
        if (stateBits & PrefabObjectOverrides::StateActive) registry->setEntityActive(ref.registryEntity, scene->isEntityActive(entity));
        if (stateBits & PrefabObjectOverrides::StateTag) registry->setEntityTag(ref.registryEntity, scene->getEntityTag(entity));
        if (stateBits & PrefabObjectOverrides::StateLayer) registry->setEntityLayer(ref.registryEntity, scene->getEntityLayer(entity));
        // a nested root's (a variant root's) state is this prefab's override of its prefab's root
        recordNestedRootAssetState(*ref.prefab, ref.path, ref.registryEntity, stateBits);
        clearIntermediateOverrides(sceneId, ref, ref.path, ComponentType::PrefabComponent, {}, stateBits);
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
        clearIntermediateOverrides(sceneId, ref, target, ComponentType::PrefabComponent, {}, stateBits);
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
    if (CameraTextureLink::isLinkSourceType(type)) {
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
    if (!canApplyPrefabOverrides()) {
        return false;
    }
    const Entity instanceRoot = ref.instance->rootEntity;
    if (isNestedRootApplyTarget(sceneId, ref, target)) {
        Prefab* nested = getPrefab(target);
        Prefab::Instance* inner = findInstanceAt(sceneProject, entity, target);
        if (!inner || !nested->registry || hasComponent(nested->registry.get(), nested->root, type)) {
            return false;
        }
        bool outside = false;
        const YAML::Node value = encodeComponentForPrefab(sceneId, entity, entity, type, &outside, target);
        if (!value || outside) {
            return false;
        }
        ProjectUtils::addEntityComponent(nested->registry.get(), nested->root, type, nested->registryEntities, YAML::Clone(value));
        // a variant (a base level) adds it to its base's root
        recordNestedRootAssetOverride(*nested, target, nested->root, type, {});
        nested->isModified = true;
    } else if (target.empty() || target == ref.path) {
        bool outside = false;
        const YAML::Node value = encodeComponentForPrefab(sceneId, instanceRoot, entity, type, &outside, ref.path);
        if (!value || outside) {
            return false;
        }
        Prefab& prefab = *ref.prefab;
        ProjectUtils::addEntityComponent(prefab.registry.get(), ref.registryEntity, type, prefab.registryEntities, YAML::Clone(value));
        recordNestedRootAssetOverride(prefab, ref.path, ref.registryEntity, type, {});
        prefab.isModified = true;
    } else {
        bool outside = false;
        const Entity holderRoot = findHolderInstanceRoot(sceneId, instanceRoot, target, ref.path);
        const YAML::Node value = holderRoot != NULL_ENTITY ? encodeComponentForPrefab(sceneId, holderRoot, entity, type, &outside, target) : YAML::Node();
        if (!value || outside) {
            return false;
        }
        Prefab* targetPrefab = nullptr;
        PrefabInstanceData* layer = applyTargetLayer(sceneId, ref, target, &targetPrefab);
        if (!layer) {
            return false;
        }
        PrefabInstanceData::Object& object = layerObject(layer, ref.registryEntity);
        object.overrides.addedComponents.insert(type);
        object.values["components"][componentKey(type)] = value;
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
    if (CameraTextureLink::isLinkSourceType(type)) {
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
    if (it == ref.instance->overrides.end() || !it->second.removedComponents.count(type) || !canApplyPrefabOverrides()) {
        return false;
    }
    if (isNestedRootApplyTarget(sceneId, ref, target)) {
        Prefab* nested = getPrefab(target);
        if (!nested || !nested->registry || !hasComponent(nested->registry.get(), nested->root, type)) {
            return false;
        }
        ProjectUtils::removeEntityComponent(nested->registry.get(), nested->root, type, nested->registryEntities);
        // a variant (a base level) removes it from its base's root
        recordNestedRootAssetRemoval(*nested, target, nested->root, type);
        nested->isModified = true;
    } else if (target.empty() || target == ref.path) {
        Prefab& prefab = *ref.prefab;
        ProjectUtils::removeEntityComponent(prefab.registry.get(), ref.registryEntity, type, prefab.registryEntities);
        recordNestedRootAssetRemoval(prefab, ref.path, ref.registryEntity, type);
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

// The GameObjects of root's branch (root first; a branch is one run of the Transform array), and
// whether one of them is a camera texture source
static std::vector<Entity> branchEntities(Scene* scene, Entity root, bool* hasLinkSource) {
    std::vector<Entity> branch;
    if (!scene->isEntityCreated(root) || !scene->findComponent<Transform>(root)) {
        return branch;
    }
    auto transforms = scene->getComponentArray<Transform>();
    const size_t first = transforms->getIndex(root);
    const size_t last = scene->findBranchLastIndex(root);
    for (size_t i = first; i <= last && i < transforms->size(); i++) {
        const Entity entity = transforms->getEntity(i);
        branch.push_back(entity);
        if (hasLinkSource && editor::CameraTextureLink::isLinkSource(scene, entity)) {
            *hasLinkSource = true;
        }
    }
    return branch;
}

bool editor::Project::reimportPrefabInstance(SceneProject* sceneProject, Entity root, const YAML::Node& givenNode) {
    if (!sceneProject || !sceneProject->scene) {
        return false;
    }
    Scene* scene = sceneProject->scene;
    // the instance is rewritten from the prefab (same entity ids, no channel notification): a
    // preview of it restores first, or its stop would write the old values back (clips 9.R7)
    PoseEditSession::suspendActiveFor(sceneProject->id, {root});
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
    const std::vector<Entity> order = sceneProject->entities;
    const Entity mainCamera = sceneProject->mainCamera;
    const bool wasModified = sceneProject->isModified;
    // a camera of the instance, before or after: textures anywhere may name it
    bool hadLinkSource = false;
    branchEntities(scene, root, &hadLinkSource);

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
    // the members came back at the end of the list: one without a Transform among them, and the
    // scene's own, go back to their place, so a reimport (an apply, its undo) leaves the list as it was
    ProjectUtils::keepUntransformedPlaces(scene, order, sceneProject->entities);
    // Only the instance's GameObjects were made again, so only their camera textures need binding:
    // resolving the whole scene for each instance made a prefab apply to many instances quadratic
    // (300 instances: 27 s). With a camera among them, before or after, the whole scene
    bool hasLinkSource = hadLinkSource;
    const std::vector<Entity> branch = branchEntities(scene, root, &hasLinkSource);
    if (hasLinkSource || branch.empty()) {
        CameraTextureLink::resolve(scene);
    } else {
        CameraTextureLink::resolve(scene, branch);
    }
    if (mainCamera != NULL_ENTITY && scene->isEntityCreated(mainCamera)) {
        sceneProject->mainCamera = mainCamera;
    }
    std::vector<Entity> kept;
    for (Entity entity : selection) {
        if (scene->isEntityCreated(entity)) {
            kept.push_back(entity);
        }
    }
    SelectionState keptSelection;
    keptSelection.entities = kept;
    setSelection(sceneProject->id, keptSelection, SelectionCause::Forced);
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
        prefab->takeAssetData(std::move(loaded));
        prefab->isModified = false;
        // what the file holds again
        prefab->savedAt = CommandHistory::stampSave();
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
        state.assets.push_back({path, encodePrefabNode(path), prefab->isModified, prefab->needsUpgrade, prefab->savedAt});
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
        state.instances.push_back({rootSceneId, root, Stream::encodeEntity(root, sceneProject->scene, this, sceneProject), sceneProject->isModified,
                                   sceneProject->savedAt});
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
        // written since the capture, its file holds what is undone here: the next save writes it
        const bool savedSince = prefab->savedAt != asset.savedAt;
        prefab->isModified = asset.isModified || savedSince;
        prefab->needsUpgrade = asset.needsUpgrade && !savedSince;
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
        sceneProject->isModified = instance.sceneModified || sceneProject->savedAt != instance.sceneSavedAt;
    }
    for (SceneProject& sceneProject : scenes) {
        if (sceneProject.scene) {
            updateScenePrefabs(&sceneProject);
        }
    }
}
