// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Entity reference remapping across prefab boundaries: the remap helpers prefab
// import, edit and override code share, and external-reference override promotion.

#include "Project.h"
#include "component/Transform.h"
#include "ProjectPrefabInternal.h"
#include "Out.h"
#include "Stream.h"
#include "util/ProjectUtils.h"
#include "util/YamlNumbers.h"
#include "util/CameraTextureLink.h"
#include "texture/Texture.h"

#include <filesystem>
#include <functional>

#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;
using editor::prefabinternal::componentHasExternalEntityRef;

// Translate a local entity through the supplied map. Prefab-boundary remaps clear values
// outside the map; recovery remaps leave those unrelated values untouched.
static bool remapLocalEntity(Entity& value, const std::unordered_map<Entity, Entity>& entityMap, bool clearUnmapped) {
    if (value == NULL_ENTITY) {
        return false;
    }
    auto it = entityMap.find(value);
    if (it == entityMap.end() && !clearUnmapped) {
        return false;
    }
    Entity remapped = (it != entityMap.end()) ? it->second : NULL_ENTITY;
    if (remapped == value) {
        return false;
    }
    value = remapped;
    return true;
}

// Remap one entity-typed property in place. Cross-scene references are cleared only when
// crossing a prefab boundary; recovery leaves them untouched. A render-to-texture link names
// its camera like any other reference, and is bound to that camera of `registry`.
static bool remapEntityRef(EntityRegistry* registry, const editor::PropertyData& property, const std::unordered_map<Entity, Entity>& entityMap, bool clearUnmapped) {
    if (!property.ref) {
        return false;
    }
    if (property.type == editor::PropertyType::Texture) {
        Texture* texture = static_cast<Texture*>(property.ref);
        // a camera's render texture, or a fluid's output ("fluid|<e>")
        const Entity camera = editor::CameraTextureLink::parseLink(texture->getId());
        if (camera == NULL_ENTITY || (!clearUnmapped && !entityMap.count(camera))) {
            return false;
        }
        Entity target = camera;
        remapLocalEntity(target, entityMap, clearUnmapped);
        // Bound even when the id stays: a texture copied from another registry still shows
        // the framebuffer of that registry's camera
        Framebuffer* targetFramebuffer = editor::CameraTextureLink::linkFramebuffer(registry, texture->getId(), target);
        if (target == camera && texture->getFramebuffer() == targetFramebuffer) {
            return false;
        }
        editor::CameraTextureLink::retarget(registry, *texture, target);
        return true;
    }
    if (property.type == editor::PropertyType::EntityReference) {
        EntityReference* ref = static_cast<EntityReference*>(property.ref);
        if (ref->sceneId != 0) {
            if (!clearUnmapped) {
                return false;
            }
            if (ref->entity == NULL_ENTITY) {
                ref->sceneId = 0;
                return false;
            }
            ref->entity = NULL_ENTITY;
            ref->sceneId = 0;
            return true;
        }
        return remapLocalEntity(ref->entity, entityMap, clearUnmapped);
    }
    if (property.type == editor::PropertyType::Entity) {
        return remapLocalEntity(*static_cast<Entity*>(property.ref), entityMap, clearUnmapped);
    }
    return false;
}

// A requested property matches its own name and, for an aggregate property such as
// "scripts", any indexed field expanded from it (e.g. "scripts[0].target"). This
// lets a whole-"scripts" update still reach the nested entity references it holds.
static bool propertyRequested(const std::string& propertyName, const std::vector<std::string>& requested) {
    for (const std::string& req : requested) {
        if (propertyName == req) {
            return true;
        }
        if (propertyName.size() > req.size() && propertyName.compare(0, req.size(), req) == 0) {
            char next = propertyName[req.size()];
            if (next == '[' || next == '.') {
                return true;
            }
        }
    }
    return false;
}

namespace jasperengine::editor::prefabinternal {

// True when a component holds an entity reference pointing outside the prefab
// instance: a cross-scene reference, or a local entity that is not one of the
// instance's members. Such references cannot be shared through the registry.
bool componentHasExternalEntityRef(EntityRegistry* registry, Entity entity, editor::ComponentType componentType, const std::unordered_set<Entity>& memberLocals) {
    for (auto& [propertyName, property] : editor::Catalog::findEntityProperties(registry, entity, componentType)) {
        if (!property.ref) {
            continue;
        }
        Entity target = NULL_ENTITY;
        if (property.type == editor::PropertyType::EntityReference) {
            EntityReference* ref = static_cast<EntityReference*>(property.ref);
            if (ref->sceneId != 0) {
                return true; // cross-scene reference
            }
            target = ref->entity;
        } else if (property.type == editor::PropertyType::Entity) {
            target = *static_cast<Entity*>(property.ref);
        } else if (property.type == editor::PropertyType::Texture) {
            // Render to texture: the texture names its camera
            target = editor::CameraTextureLink::parseLink(static_cast<Texture*>(property.ref)->getId());
        } else {
            continue;
        }
        if (target != NULL_ENTITY && !memberLocals.count(target)) {
            return true;
        }
    }
    return false;
}

}

void editor::Project::remapEntityProperties(EntityRegistry* registry, const std::vector<Entity>& entities, const std::unordered_map<Entity, Entity>& entityMap, bool clearUnmapped) {
    if (!clearUnmapped && entityMap.empty()) {
        return;
    }
    for (Entity entity : entities) {
        for (ComponentType componentType : Catalog::findComponents(registry, entity)) {
            if (!Catalog::mayHoldReferences(registry, entity, componentType, true, true)) {
                continue;
            }
            uint64_t updateFlags = 0;

            for (auto& [propertyName, property] : Catalog::findEntityProperties(registry, entity, componentType)) {
                if (remapEntityRef(registry, property, entityMap, clearUnmapped)) {
                    updateFlags |= property.updateFlags;
                }
            }

            if (updateFlags != 0) {
                Catalog::updateEntity(registry, entity, updateFlags);
            }
        }
    }
}

void editor::Project::remapEntityPropertiesInComponent(EntityRegistry* registry, Entity entity, ComponentType componentType, const std::vector<std::string>& properties, const std::unordered_map<Entity, Entity>& entityMap) {
    for (auto& [propertyName, property] : Catalog::findEntityProperties(registry, entity, componentType)) {
        // If specific properties were requested, only remap those.
        if (!properties.empty() && !propertyRequested(propertyName, properties)) {
            continue;
        }

        remapEntityRef(registry, property, entityMap, true);
    }
}

// ---- References into nested prefabs: stripped objects (PREFABS.md 17) ----

std::unordered_set<Entity> editor::Project::collectInstanceTreeObjects(uint32_t sceneId, Entity instanceRoot, const std::filesystem::path& level) {
    std::unordered_set<Entity> objects;
    SceneProject* sceneProject = getScene(sceneId);
    std::filesystem::path path;
    const Prefab::Instance* top = findInstanceAt(sceneProject, instanceRoot, level, &path);
    if (!top) {
        return objects;
    }
    // the instance, the ones nested in it and, for a variant, its bases' (at its root)
    std::vector<std::pair<const Prefab::Instance*, std::filesystem::path>> pending{{top, path}};
    for (size_t next = 0; next < pending.size() && next < 100000; next++) {
        const Prefab::Instance* instance = pending[next].first;
        objects.insert(instance->rootEntity);
        for (const Prefab::EntityMember& member : instance->members) {
            objects.insert(member.localEntity);
        }
        for (const NestedInstanceLevel& nested : getNestedInstanceLevels(sceneProject, pending[next].second, *instance)) {
            if (nested.instance) {
                pending.emplace_back(nested.instance, nested.path);
            }
        }
    }
    return objects;
}

namespace {

    // The way from `instance` (of `path`) down to `target` (registry entities: the nested
    // roots, a variant's root for its base level, then the GameObject in the innermost
    // prefab); `instance`'s own GameObjects give a way of one
    bool findWayDown(editor::Project* project, editor::SceneProject* sceneProject, const std::filesystem::path& path,
            const editor::Prefab::Instance& instance, Entity target, std::vector<Entity>& way, size_t depth) {
        const Entity own = instance.getRegistryEntity(target);
        if (own != NULL_ENTITY) {
            way.push_back(own);
            return true;
        }
        if (depth > 64) {
            return false;
        }
        for (const editor::NestedInstanceLevel& nested : project->getNestedInstanceLevels(sceneProject, path, instance)) {
            if (!nested.instance) {
                continue;
            }
            // (a variant's root is the base's root too: a way to it stays a way of one)
            if (nested.variantBase && target == nested.localRoot) {
                continue;
            }
            way.push_back(nested.registryEntity);
            if (findWayDown(project, sceneProject, nested.path, *nested.instance, target, way, depth + 1)) {
                return true;
            }
            way.pop_back();
        }
        return false;
    }

    Entity referenceTarget(const editor::PropertyData& property, bool& crossScene) {
        crossScene = false;
        if (!property.ref) {
            return NULL_ENTITY;
        }
        if (property.type == editor::PropertyType::EntityReference) {
            const EntityReference* ref = static_cast<const EntityReference*>(property.ref);
            if (ref->sceneId != 0) {
                crossScene = ref->entity != NULL_ENTITY;
                return NULL_ENTITY;
            }
            return ref->entity;
        }
        if (property.type == editor::PropertyType::Entity) {
            return *static_cast<const Entity*>(property.ref);
        }
        if (property.type == editor::PropertyType::Texture) {
            return editor::CameraTextureLink::parseLink(static_cast<const Texture*>(property.ref)->getId());
        }
        return NULL_ENTITY;
    }

}

Entity editor::Project::makePrefabReference(uint32_t sceneId, Entity instanceRoot, Entity localTarget, const std::filesystem::path& level) {
    SceneProject* sceneProject = getScene(sceneId);
    std::filesystem::path path;
    const Prefab::Instance* instance = findInstanceAt(sceneProject, instanceRoot, level, &path);
    Prefab* prefab = instance ? getPrefab(path) : nullptr;
    if (!prefab || localTarget == NULL_ENTITY) {
        return NULL_ENTITY;
    }
    std::vector<Entity> way;
    if (!findWayDown(this, sceneProject, path, *instance, localTarget, way, 0)) {
        return NULL_ENTITY;
    }
    if (way.size() == 1) {
        return way[0];
    }
    Entity id = prefab->findStripped(way);
    if (id == NULL_ENTITY) {
        id = prefab->addStripped(way);
        prefab->isModified = true;
    }
    return id;
}

bool editor::Project::remapReferencesToPrefab(uint32_t sceneId, Entity instanceRoot, EntityRegistry* registry, Entity entity, ComponentType type,
        const std::vector<std::string>& properties, const std::filesystem::path& level) {
    if (!registry || !registry->isEntityCreated(entity)) {
        return true;
    }
    bool inside = true;
    std::unordered_map<Entity, Entity> map;
    for (auto& [propertyName, property] : Catalog::findEntityProperties(registry, entity, type)) {
        if (!properties.empty() && !propertyRequested(propertyName, properties)) {
            continue;
        }
        bool crossScene = false;
        const Entity target = referenceTarget(property, crossScene);
        if (crossScene) {
            inside = false;
        }
        if (target == NULL_ENTITY || map.count(target)) {
            continue;
        }
        const Entity id = makePrefabReference(sceneId, instanceRoot, target, level);
        if (id != NULL_ENTITY) {
            map[target] = id;
        } else {
            inside = false;
        }
    }
    remapEntityPropertiesInComponent(registry, entity, type, properties, map);
    return inside;
}

YAML::Node editor::Project::encodeComponentForPrefab(uint32_t sceneId, Entity instanceRoot, Entity entity, ComponentType type, bool* outside,
        const std::filesystem::path& level) {
    SceneProject* sceneProject = getScene(sceneId);
    if (outside) {
        *outside = false;
    }
    if (!sceneProject || !sceneProject->scene || !prefabinternal::hasComponent(sceneProject->scene, entity, type)) {
        return YAML::Node();
    }
    prefabinternal::ScratchEntity scratch;
    scratch.takeComponents(sceneProject->scene, entity, Catalog::componentTypeToSignature(sceneProject->scene, type));
    const bool inside = remapReferencesToPrefab(sceneId, instanceRoot, scratch.registry.get(), scratch.entity, type, {}, level);
    if (outside) {
        *outside = !inside;
    }
    return prefabinternal::encodeComponent(scratch.registry.get(), scratch.entity, type);
}

bool editor::Project::componentReferencesOutside(uint32_t sceneId, Entity instanceRoot, Entity entity, ComponentType type,
        const std::filesystem::path& level) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return false;
    }
    return componentHasExternalEntityRef(sceneProject->scene, entity, type, collectInstanceTreeObjects(sceneId, instanceRoot, level));
}

Entity editor::Project::findHolderInstanceRoot(uint32_t sceneId, Entity instanceRoot, const std::filesystem::path& target,
        const std::filesystem::path& level) {
    SceneProject* sceneProject = getScene(sceneId);
    Entity current = instanceRoot;
    std::filesystem::path path;
    if (!findInstanceAt(sceneProject, current, level, &path)) {
        return NULL_ENTITY;
    }
    for (size_t depth = 0; depth < 64 && current != NULL_ENTITY; depth++) {
        if (path == target) {
            return current;
        }
        PrefabHolderLink holder;
        if (!findPrefabHolder(sceneProject, current, path, holder)) {
            return NULL_ENTITY;
        }
        current = holder.instance->rootEntity;
        path = holder.path;
    }
    return NULL_ENTITY;
}

void editor::Project::overrideExternalRefComponents(EntityRegistry* scene, Prefab::Instance& instance) {
    std::unordered_set<Entity> memberLocals;
    std::vector<Entity> objects;
    if (instance.rootEntity != NULL_ENTITY) {
        memberLocals.insert(instance.rootEntity);
        objects.push_back(instance.rootEntity);
    }
    for (const auto& member : instance.members) {
        memberLocals.insert(member.localEntity);
        objects.push_back(member.localEntity);
    }

    // An entity reference pointing outside the prefab is per-instance data that cannot
    // be shared through the registry. Promote each component that holds one to a
    // per-instance override so the value is kept on this instance and persisted.
    for (Entity object : objects) {
        for (ComponentType componentType : Catalog::findComponents(scene, object)) {
            if (componentType != ComponentType::PrefabComponent &&
                componentHasExternalEntityRef(scene, object, componentType, memberLocals)) {
                instance.overrides[object].addWholeComponent(componentType);
            }
        }
    }
}

// ---- Prefab Mode: a stage reads and writes the nested data of its asset (PREFABS.md 17) ----

namespace {

    // The nested entry of a `prefabInstance` block for the nested root `registryRoot`, made
    // when missing
    YAML::Node nestedBlockEntry(YAML::Node block, Entity registryRoot) {
        if (block["nested"] && block["nested"].IsSequence()) {
            for (YAML::Node entry : block["nested"]) {
                const YAML::Node readEntry = entry;
                if (readEntry["prefabRootRegistryEntity"] && editor::YamlNumbers::as<Entity>(readEntry["prefabRootRegistryEntity"]) == registryRoot) {
                    return entry;
                }
            }
        }
        YAML::Node entry(YAML::NodeType::Map);
        entry["prefabRootRegistryEntity"] = registryRoot;
        block["nested"].push_back(entry);
        return block["nested"][block["nested"].size() - 1];
    }

}

void editor::Project::prefabNodeToStage(const Prefab& prefab, YAML::Node entities) {
    const std::string prefabKey = Catalog::getComponentName(ComponentType::PrefabComponent, true);
    std::function<void(YAML::Node, size_t)> walk = [&](YAML::Node node, size_t depth) {
        if (!node.IsMap() || depth > 256) {
            return;
        }
        const YAML::Node readNode = node;
        const YAML::Node components = readNode["components"];
        const bool nestedRoot = components && components.IsMap() && components[prefabKey] && readNode["entity"];
        const YAML::Node children = readNode["children"];
        if (!children || !children.IsSequence()) {
            return;
        }
        if (!nestedRoot) {
            for (YAML::Node child : node["children"]) {
                walk(child, depth + 1);
            }
            return;
        }
        // The GameObjects this prefab adds to the nested instance: in the stage they are
        // GameObjects added to that instance, below the nested GameObject they go below
        const Entity rootId = YamlNumbers::as<Entity>(readNode["entity"]);
        YAML::Node block = readNode["prefabInstance"] ? node["prefabInstance"] : YAML::Node(YAML::NodeType::Map);
        for (const YAML::Node& child : children) {
            if (!child.IsMap()) {
                continue;
            }
            YAML::Node entry = YAML::Clone(child);
            walk(entry, depth + 1);
            YAML::Node target = block;
            Entity parentRegistry = NULL_ENTITY;
            if (entry["addedTo"]) {
                auto source = prefab.stripped.find(YamlNumbers::as<Entity>(entry["addedTo"]));
                if (source != prefab.stripped.end() && source->second.size() >= 2 && source->second.front() == rootId) {
                    for (size_t i = 1; i + 1 < source->second.size(); i++) {
                        target = nestedBlockEntry(target, source->second[i]);
                    }
                    parentRegistry = source->second.back();
                }
            }
            // last when the place is not known
            const long long childIndex = entry["childIndex"] ? YamlNumbers::as<long long>(entry["childIndex"]) : -1;
            entry.remove("addedTo");
            entry.remove("childIndex");
            entry["parentRegistryEntity"] = parentRegistry;
            entry["childIndex"] = childIndex >= 0 ? childIndex : 1000000;
            target["addedGameObjects"].push_back(entry);
        }
        node.remove("children");
        node["prefabInstance"] = block;
    };
    for (YAML::Node member : entities) {
        walk(member, 0);
    }
}

void editor::Project::resolveStageStrippedReferences(SceneProject* stage, const Prefab& prefab) {
    if (!stage || !stage->scene || prefab.stripped.empty()) {
        return;
    }
    // The asset's GameObjects keep their file ids in a stage
    std::unordered_map<Entity, Entity> own;
    for (Entity entity : prefab.registryEntities) {
        own[entity] = entity;
    }
    std::unordered_map<Entity, Entity> strippedToLocal;
    for (const auto& [id, source] : prefab.stripped) {
        const Entity local = resolveStrippedObject(stage, own, source);
        if (local != NULL_ENTITY) {
            strippedToLocal[id] = local;
        }
    }
    // ids above nextEntity for the nested GameObjects: only stripped ids change
    remapEntityProperties(stage->scene, stage->entities, strippedToLocal, false);
    CameraTextureLink::resolve(stage->scene);
}

void editor::Project::stageToPrefabAsset(SceneProject* stage, Prefab& prefab, const std::map<Entity, std::vector<Entity>>& previousStripped) {
    if (!stage || !stage->scene || !prefab.registry) {
        return;
    }
    // stripped ids stay what they were (a new one only for a new way down)
    for (const auto& [id, source] : previousStripped) {
        prefab.stripped.emplace(id, source);
    }
    prefab.addedObjects.clear();
    EntityRegistry* registry = prefab.registry.get();
    const std::string prefabKey = Catalog::getComponentName(ComponentType::PrefabComponent, true);

    // 1. The GameObjects added to the nested instances in the stage become the prefab's
    std::vector<Entity> pending;
    for (const auto& [nestedRoot, data] : prefab.nestedInstances) {
        pending.push_back(nestedRoot);
    }
    std::function<void(Entity, PrefabInstanceData&, std::vector<Entity>)> convert = [&](Entity nestedRoot, PrefabInstanceData& data, std::vector<Entity> chain) {
        for (const YAML::Node& addedNode : data.addedGameObjects) {
            const Entity parentRegistry = addedNode["parentRegistryEntity"] ? YamlNumbers::as<Entity>(addedNode["parentRegistryEntity"]) : NULL_ENTITY;
            const int childIndex = addedNode["childIndex"] ? static_cast<int>(YamlNumbers::as<long long>(addedNode["childIndex"])) : -1;
            YAML::Node clean = YAML::Clone(addedNode);
            clean.remove("parentRegistryEntity");
            clean.remove("childIndex");
            std::vector<Entity> decoded = Stream::decodeEntity(clean, registry, &prefab.registryEntities);
            if (decoded.empty()) {
                continue;
            }
            if (registry->findComponent<Transform>(decoded[0])) {
                ProjectUtils::moveEntityOrderByTransform(registry, prefab.registryEntities, decoded[0], nestedRoot, 0, false);
            }
            std::vector<Entity> source{nestedRoot};
            source.insert(source.end(), chain.begin(), chain.end());
            if (parentRegistry != NULL_ENTITY) {
                source.push_back(parentRegistry);
            }
            Prefab::AddedObject added;
            added.childIndex = childIndex < 1000000 ? childIndex : -1;
            if (source.size() >= 2) {
                added.parent = prefab.addStripped(source);
            }
            prefab.addedObjects[decoded[0]] = added;
            // prefab instances among them: nested roots of the prefab, with their data
            std::function<void(const YAML::Node&)> nestedIn = [&](const YAML::Node& node) {
                if (!node.IsMap()) {
                    return;
                }
                const YAML::Node components = node["components"];
                if (node["entity"] && components && components.IsMap() && components[prefabKey]) {
                    const Entity id = YamlNumbers::as<Entity>(node["entity"]);
                    if (registry->isEntityCreated(id) && prefab.isNestedRoot(id)) {
                        PrefabInstanceData nestedData = Stream::decodePrefabInstanceData(node);
                        nestedData.present = true;
                        nestedData.legacy = false;
                        prefab.nestedInstances[id] = std::move(nestedData);
                        pending.push_back(id);
                    }
                    return;
                }
                if (node["children"] && node["children"].IsSequence()) {
                    for (const YAML::Node& child : node["children"]) {
                        nestedIn(child);
                    }
                }
            };
            nestedIn(clean);
        }
        data.addedGameObjects.clear();
        data.memberIds.clear();
        for (PrefabInstanceData& entry : data.nested) {
            if (entry.nestedRoot == NULL_ENTITY) {
                continue;
            }
            std::vector<Entity> deeper = chain;
            deeper.push_back(entry.nestedRoot);
            convert(nestedRoot, entry, deeper);
        }
    };
    for (size_t next = 0; next < pending.size(); next++) {
        auto it = prefab.nestedInstances.find(pending[next]);
        if (it != prefab.nestedInstances.end()) {
            convert(pending[next], it->second, {});
        }
    }

    // 2. References: the stage's ids, the prefab's own GameObjects' being their file ids,
    //    into the instances nested in it stripped objects, anything else None
    std::unordered_set<Entity> own(prefab.registryEntities.begin(), prefab.registryEntities.end());
    std::unordered_map<Entity, Entity> resolved;
    auto prefabId = [&](Entity target) -> Entity {
        // (a stripped id the stage could not resolve, into a Missing nested prefab: kept as it is;
        // the nested GameObjects of the stage take ids above every id of the asset)
        if (target == NULL_ENTITY || own.count(target) || (previousStripped.count(target) && !stage->scene->isEntityCreated(target))) {
            return target;
        }
        auto known = resolved.find(target);
        if (known != resolved.end()) {
            return known->second;
        }
        Entity id = NULL_ENTITY;
        for (Entity entity : prefab.registryEntities) {
            std::filesystem::path nestedPath;
            const Prefab::Instance* nested = prefab.isInstanceRoot(entity) ? findInstanceRootedAt(stage, entity, &nestedPath) : nullptr;
            std::vector<Entity> way;
            if (nested && findWayDown(this, stage, nestedPath, *nested, target, way, 0)) {
                way.insert(way.begin(), entity);
                id = prefab.addStripped(way);
                break;
            }
        }
        resolved[target] = id;
        return id;
    };
    auto remapComponent = [&](EntityRegistry* values, Entity entity, ComponentType type) {
        std::unordered_map<Entity, Entity> map;
        for (auto& [name, property] : Catalog::findEntityProperties(values, entity, type)) {
            bool crossScene = false;
            const Entity target = referenceTarget(property, crossScene);
            if (target != NULL_ENTITY && !map.count(target)) {
                const Entity id = prefabId(target);
                if (id != NULL_ENTITY) {
                    map[target] = id;
                }
            }
        }
        remapEntityPropertiesInComponent(values, entity, type, {}, map);
    };
    for (Entity entity : std::vector<Entity>(prefab.registryEntities)) {
        for (ComponentType type : Catalog::findComponents(registry, entity)) {
            if (type != ComponentType::PrefabComponent && Catalog::mayHoldReferences(registry, entity, type, true, true)) {
                remapComponent(registry, entity, type);
            }
        }
    }
    std::function<void(PrefabInstanceData&, size_t)> remapData = [&](PrefabInstanceData& data, size_t depth) {
        for (auto& [registryEntity, object] : data.objects) {
            if (!object.values.IsMap() || !object.values["components"] || !object.values["components"].IsMap()) {
                continue;
            }
            YAML::Node components = object.values["components"];
            for (auto it = components.begin(); it != components.end(); ++it) {
                ComponentType type;
                try {
                    type = Catalog::getComponentType(it->first.as<std::string>());
                } catch (const std::exception&) {
                    continue;
                }
                prefabinternal::ScratchEntity scratch;
                scratch.decode(prefabinternal::singleComponent(type, it->second));
                if (!scratch.has(type)) {
                    continue;
                }
                remapComponent(scratch.registry.get(), scratch.entity, type);
                it->second = prefabinternal::encodeComponent(scratch.registry.get(), scratch.entity, type);
            }
        }
        if (depth < 64) {
            for (PrefabInstanceData& entry : data.nested) {
                remapData(entry, depth + 1);
            }
        }
    };
    for (auto& [nestedRoot, data] : prefab.nestedInstances) {
        remapData(data, 0);
    }
}
