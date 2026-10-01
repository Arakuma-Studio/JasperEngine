// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <map>
#include <unordered_map>
#include <vector>
#include <memory>
#include <cstdint>
#include <algorithm>

#include "Scene.h"     // Entity, EntityRegistry, NULL_ENTITY
#include "Catalog.h"   // ComponentType

namespace jasperengine::editor {

class Prefab {
public:
    struct EntityMember {
        Entity localEntity;     // Entity in the scene
        Entity registryEntity;  // Corresponding entity in the shared registry
    };

    struct Instance {
        uint32_t instanceId;
        Entity rootEntity = NULL_ENTITY; // Root entity of this instance in the scene
        std::vector<EntityMember> members;
        std::map<Entity, uint64_t> overrides; // entityId → bitmask of overridden ComponentTypes
        // Unity's removed GameObject overrides: registry entities taken out of this instance
        // only, each with the members its branch had then (whose ids a revert takes back;
        // empty after a reload). A Prefab GameObject is absent from the instance when it or
        // one of its registry ancestors is listed, so what the Prefab adds below a removed
        // GameObject stays out too. Members only ever hold the GameObjects present.
        std::unordered_map<Entity, std::vector<EntityMember>> removed;
    };

    // sceneId → list of instances
    std::map<uint32_t, std::vector<Instance>> instances;

    std::unique_ptr<EntityRegistry> registry;
    std::vector<Entity> registryEntities;

    bool isModified = false;
    uint32_t nextInstanceId = 1;

    uint32_t getInstanceId(uint32_t sceneId, Entity entity) const;
    Entity getRegistryEntity(uint32_t sceneId, Entity entity) const;
    Entity getLocalEntity(uint32_t sceneId, uint32_t instanceId, Entity registryEntity) const;
    Entity getRootEntity(uint32_t sceneId, Entity entity) const;

    // Membership and instance access
    bool hasInstances(uint32_t sceneId) const;
    std::vector<Entity> getAllEntities(uint32_t sceneId, uint32_t instanceId) const;
    bool containsEntity(uint32_t sceneId, Entity entity) const;
    Instance* getInstance(uint32_t sceneId, Entity entity);
    const Instance* getInstance(uint32_t sceneId, Entity entity) const;
    Instance* getInstanceById(uint32_t sceneId, uint32_t instanceId);
    const Instance* getInstanceById(uint32_t sceneId, uint32_t instanceId) const;

    // Override management
    void setComponentOverride(uint32_t sceneId, Entity entity, ComponentType componentType);
    void clearComponentOverride(uint32_t sceneId, Entity entity, ComponentType componentType);
    bool hasComponentOverride(uint32_t sceneId, Entity entity, ComponentType componentType) const;

    // Removed GameObjects: whether the registry entity is left out of the instance, being
    // removed itself or below a removed one. One hash lookup per registry ancestor.
    bool isRegistryEntityAbsent(const Instance& instance, Entity registryEntity) const;
    // The registry Transform parent of a registry entity, NULL_ENTITY for a top-level one
    Entity getRegistryParent(Entity registryEntity) const;

};

} // namespace jasperengine::editor

