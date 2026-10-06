// Benchmark adapter: real metadata, Transform and component storage, without a
// graphics/audio Scene. EntityPath.cpp is compiled unchanged against this owner.
#pragma once
#include "ecs/EntityManager.h"
#include "ecs/ComponentArray.h"
#include "component/Transform.h"

namespace jasperengine {
class EntityRegistry {
public:
    EntityManager metadata;
    ComponentArray<Transform> transforms;
    bool isEntityCreated(Entity entity) const { return metadata.isCreated(entity); }
    std::string getEntityName(Entity entity) const { return metadata.getName(entity); }
    template<class T> ComponentArray<T>* getComponentArray() { return &transforms; }
    template<class T> T* findComponent(Entity entity) { return transforms.findComponent(entity); }
    Entity add(const std::string& name, Entity parent = NULL_ENTITY, bool transformed = true) {
        const Entity entity = metadata.createUserEntity();
        metadata.setName(entity, name);
        if (transformed) {
            Transform transform;
            transform.parent = parent;
            transforms.insert(entity, transform);
        }
        return entity;
    }
};
}
