// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Entity reference remapping across prefab boundaries: the remap helpers prefab
// import, edit and override code share, and external-reference override promotion.

#include "Project.h"
#include "ProjectPrefabInternal.h"
#include "Out.h"
#include "util/CameraTextureLink.h"
#include "texture/Texture.h"

#include <filesystem>

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
        const Entity camera = editor::CameraTextureLink::parse(texture->getId());
        if (camera == NULL_ENTITY || (!clearUnmapped && !entityMap.count(camera))) {
            return false;
        }
        Entity target = camera;
        remapLocalEntity(target, entityMap, clearUnmapped);
        // Bound even when the id stays: a texture copied from another registry still shows
        // the framebuffer of that registry's camera
        CameraComponent* targetCamera = (target != NULL_ENTITY && registry) ? registry->findComponent<CameraComponent>(target) : nullptr;
        if (target == camera && texture->getFramebuffer() == (targetCamera ? targetCamera->framebuffer : nullptr)) {
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
            target = editor::CameraTextureLink::parse(static_cast<Texture*>(property.ref)->getId());
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

// ---- References into nested prefabs: stripped objects (PREFABS.md 16) ----

std::unordered_set<Entity> editor::Project::collectInstanceTreeObjects(uint32_t sceneId, Entity instanceRoot) {
    std::unordered_set<Entity> objects;
    SceneProject* sceneProject = getScene(sceneId);
    std::vector<Entity> pending{instanceRoot};
    for (size_t next = 0; next < pending.size() && next < 100000; next++) {
        const Prefab::Instance* instance = findInstanceRootedAt(sceneProject, pending[next]);
        if (!instance) {
            continue;
        }
        objects.insert(instance->rootEntity);
        for (const Prefab::EntityMember& member : instance->members) {
            if (objects.insert(member.localEntity).second) {
                pending.push_back(member.localEntity);
            }
        }
    }
    return objects;
}

namespace {

    // The way from `instance` down to `target` (registry entities: the nested roots, then
    // the GameObject in the innermost prefab); `instance`'s own GameObjects give a way of one
    bool findWayDown(editor::Project* project, editor::SceneProject* sceneProject, const editor::Prefab::Instance& instance, Entity target,
            std::vector<Entity>& way, size_t depth) {
        const Entity own = instance.getRegistryEntity(target);
        if (own != NULL_ENTITY) {
            way.push_back(own);
            return true;
        }
        if (depth > 64) {
            return false;
        }
        for (const editor::Prefab::EntityMember& member : instance.members) {
            const editor::Prefab::Instance* nested = project->findInstanceRootedAt(sceneProject, member.localEntity);
            if (!nested) {
                continue;
            }
            way.push_back(member.registryEntity);
            if (findWayDown(project, sceneProject, *nested, target, way, depth + 1)) {
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
            return editor::CameraTextureLink::parse(static_cast<const Texture*>(property.ref)->getId());
        }
        return NULL_ENTITY;
    }

}

Entity editor::Project::makePrefabReference(uint32_t sceneId, Entity instanceRoot, Entity localTarget) {
    SceneProject* sceneProject = getScene(sceneId);
    std::filesystem::path path;
    const Prefab::Instance* instance = findInstanceRootedAt(sceneProject, instanceRoot, &path);
    Prefab* prefab = instance ? getPrefab(path) : nullptr;
    if (!prefab || localTarget == NULL_ENTITY) {
        return NULL_ENTITY;
    }
    std::vector<Entity> way;
    if (!findWayDown(this, sceneProject, *instance, localTarget, way, 0)) {
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

Entity editor::Project::resolvePrefabObjectId(uint32_t sceneId, Entity instanceRoot, Entity prefabId) {
    const std::unordered_map<Entity, Entity> map = getPrefabReferenceMap(sceneId, instanceRoot);
    auto it = map.find(prefabId);
    return it != map.end() ? it->second : NULL_ENTITY;
}

bool editor::Project::remapReferencesToPrefab(uint32_t sceneId, Entity instanceRoot, EntityRegistry* registry, Entity entity, ComponentType type,
        const std::vector<std::string>& properties) {
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
        const Entity id = makePrefabReference(sceneId, instanceRoot, target);
        if (id != NULL_ENTITY) {
            map[target] = id;
        } else {
            inside = false;
        }
    }
    remapEntityPropertiesInComponent(registry, entity, type, properties, map);
    return inside;
}

YAML::Node editor::Project::encodeComponentForPrefab(uint32_t sceneId, Entity instanceRoot, Entity entity, ComponentType type, bool* outside) {
    SceneProject* sceneProject = getScene(sceneId);
    if (outside) {
        *outside = false;
    }
    if (!sceneProject || !sceneProject->scene || !prefabinternal::hasComponent(sceneProject->scene, entity, type)) {
        return YAML::Node();
    }
    prefabinternal::ScratchEntity scratch;
    scratch.takeComponents(sceneProject->scene, entity, Catalog::componentTypeToSignature(sceneProject->scene, type));
    const bool inside = remapReferencesToPrefab(sceneId, instanceRoot, scratch.registry.get(), scratch.entity, type);
    if (outside) {
        *outside = !inside;
    }
    return prefabinternal::encodeComponent(scratch.registry.get(), scratch.entity, type);
}

bool editor::Project::componentReferencesOutside(uint32_t sceneId, Entity instanceRoot, Entity entity, ComponentType type) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return false;
    }
    return componentHasExternalEntityRef(sceneProject->scene, entity, type, collectInstanceTreeObjects(sceneId, instanceRoot));
}

Entity editor::Project::findHolderInstanceRoot(uint32_t sceneId, Entity instanceRoot, const std::filesystem::path& target) {
    SceneProject* sceneProject = getScene(sceneId);
    Entity current = instanceRoot;
    for (size_t depth = 0; depth < 64 && current != NULL_ENTITY; depth++) {
        std::filesystem::path path;
        if (!findInstanceRootedAt(sceneProject, current, &path)) {
            return NULL_ENTITY;
        }
        if (path == target) {
            return current;
        }
        Entity holder = NULL_ENTITY;
        for (auto& [otherPath, other] : prefabAssets) {
            auto sceneIt = other.instances.find(sceneId);
            if (sceneIt == other.instances.end()) continue;
            for (const Prefab::Instance& instance : sceneIt->second) {
                for (const Prefab::EntityMember& member : instance.members) {
                    if (member.localEntity == current) {
                        holder = instance.rootEntity;
                        break;
                    }
                }
                if (holder != NULL_ENTITY) break;
            }
            if (holder != NULL_ENTITY) break;
        }
        current = holder;
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
