// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Entity reference remapping across prefab boundaries: the remap helpers prefab
// import, edit and override code share, and external-reference override promotion.

#include "Project.h"
#include "ProjectPrefabInternal.h"
#include "util/CameraTextureLink.h"
#include "texture/Texture.h"

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
