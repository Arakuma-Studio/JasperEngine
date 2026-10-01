// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Helpers shared by the Project prefab translation units (editor/project/ProjectPrefab*.cpp).
// Internal to Project: not part of its interface.

#pragma once

#include "Catalog.h"
#include "Stream.h"
#include "registry/EntityRegistry.h"
#include "util/Prefab.h"

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jasperengine::editor::prefabinternal {

    // True when a component holds an entity reference pointing outside the prefab instance
    // (defined in ProjectPrefabRefs.cpp)
    bool componentHasExternalEntityRef(EntityRegistry* registry, Entity entity, editor::ComponentType componentType, const std::unordered_set<Entity>& memberLocals);

    // Some components of a GameObject copied into a registry of its own
    struct ScratchEntity {
        std::unique_ptr<EntityRegistry> registry = std::make_unique<EntityRegistry>();
        Entity entity = NULL_ENTITY;

        ScratchEntity() { entity = registry->createUserEntity(); }

        void takeComponents(const EntityRegistry* from, Entity source, Signature signature) {
            if (signature.none()) {
                return;
            }
            YAML::Node components = Stream::encodeComponents(source, from, signature);
            if (components.IsMap() && components.size() > 0) {
                Stream::decodeComponents(entity, NULL_ENTITY, registry.get(), components);
            }
        }
        void takeState(const EntityRegistry* from, Entity source) {
            registry->setEntityName(entity, from->getEntityName(source));
            registry->setEntityActive(entity, from->isEntityActive(source));
            registry->setEntityTag(entity, from->getEntityTag(source));
            registry->setEntityLayer(entity, from->getEntityLayer(source));
        }
        void decode(const YAML::Node& components) {
            if (components && components.IsMap() && components.size() > 0) {
                Stream::decodeComponents(entity, NULL_ENTITY, registry.get(), components);
            }
        }
        bool has(ComponentType type) const {
            return (registry->getSignature(entity) & Catalog::componentTypeToSignature(registry.get(), type)).any();
        }
    };

    // (defined in ProjectPrefabImport.cpp)
    bool hasComponent(const EntityRegistry* registry, Entity entity, ComponentType type);
    Entity transformParent(EntityRegistry* registry, Entity entity);
    std::string componentKey(ComponentType type);
    YAML::Node singleComponent(ComponentType type, const YAML::Node& value);
    YAML::Node encodeComponent(const EntityRegistry* registry, Entity entity, ComponentType type);
    // Typed property equality; false for the types it cannot compare (materials, textures, ...)
    bool samePropertyValue(const PropertyData& a, const PropertyData& b);
    // What a GameObject differs in from a prefab's root GameObject, as root overrides (the
    // default overrides left out)
    PrefabObjectOverrides diffAgainstRoot(EntityRegistry* own, Entity ownEntity, EntityRegistry* asset, Entity assetRoot);
    // Copies properties of a component from a scratch copy onto a GameObject that has the
    // component: the whole component for "*", else each named property both have (the ones
    // missing are dropped from `names`, logged with `context`)
    void copyProperties(ScratchEntity& from, EntityRegistry* target, Entity entity, ComponentType type,
            std::set<std::string>& names, const std::string& context);
    // The prefab's value of a component onto an instance's GameObject: the requested
    // properties (all for an empty list) but the ones that GameObject overrides and, on an
    // instance root, the default overrides (defined in ProjectPrefabEdit.cpp)
    void copyPrefabValueToInstance(EntityRegistry* registry, Entity registryEntity, EntityRegistry* scene, Entity local,
        ComponentType type, const std::vector<std::string>& properties, const PrefabObjectOverrides* overrides, bool isRoot,
        const std::unordered_map<Entity, Entity>& registryToLocal);

}
