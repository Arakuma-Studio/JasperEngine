// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EntityPath.h"

#include "Project.h"

using namespace jasperengine;

namespace {

std::string pathOf(EntityRegistry* registry, Entity entity, editor::EntityPath::Cache& cache, int depth) {
    auto it = cache.find(entity);
    if (it != cache.end()) return it->second;
    std::string parentPath;
    Entity parent = NULL_ENTITY;
    if (Transform* transform = registry->findComponent<Transform>(entity)) parent = transform->parent;
    if (parent != NULL_ENTITY && depth < 256) parentPath = pathOf(registry, parent, cache, depth + 1) + "/";
    // index among the earlier siblings of the same name, in Transform order
    std::string name = registry->getEntityName(entity);
    int index = 0;
    if (registry->findComponent<Transform>(entity)) {
        auto transforms = registry->getComponentArray<Transform>();
        size_t own = transforms->getIndex(entity);
        for (size_t i = 0; i < own; i++) {
            Entity other = transforms->getEntity(i);
            if (transforms->getComponentFromIndex(i).parent == parent && registry->getEntityName(other) == name) index++;
        }
    }
    std::string path = parentPath + name + (index ? "#" + std::to_string(index) : "");
    cache[entity] = path;
    return path;
}

}

std::string editor::EntityPath::of(EntityRegistry* registry, Entity entity, Cache* cache) {
    if (!registry || !registry->isEntityCreated(entity)) return std::string();
    Cache local;
    return pathOf(registry, entity, cache ? *cache : local, 0);
}

std::string editor::EntityPath::of(const SceneProject& sceneProject, Entity entity, Cache* cache) {
    return of(sceneProject.scene, entity, cache);
}

Entity editor::EntityPath::find(EntityRegistry* registry, const std::vector<Entity>& entities, const std::string& path) {
    if (!registry) return NULL_ENTITY;
    Cache cache;
    for (Entity entity : entities) {
        if (registry->isEntityCreated(entity) && pathOf(registry, entity, cache, 0) == path) {
            return entity;
        }
    }
    return NULL_ENTITY;
}

Entity editor::EntityPath::find(const SceneProject& sceneProject, const std::string& path) {
    return find(sceneProject.scene, sceneProject.entities, path);
}

std::vector<Entity> editor::EntityPath::findByName(EntityRegistry* registry, const std::vector<Entity>& entities, const std::string& name) {
    std::vector<Entity> found;
    if (!registry) return found;
    for (Entity entity : entities) {
        if (registry->isEntityCreated(entity) && registry->getEntityName(entity) == name) {
            found.push_back(entity);
        }
    }
    return found;
}

std::vector<Entity> editor::EntityPath::resolve(EntityRegistry* registry, const std::vector<Entity>& entities, const std::string& ref) {
    const Entity exact = find(registry, entities, ref);
    if (exact != NULL_ENTITY) return {exact};
    return findByName(registry, entities, ref);
}

std::vector<Entity> editor::EntityPath::resolve(const SceneProject& sceneProject, const std::string& ref) {
    return resolve(sceneProject.scene, sceneProject.entities, ref);
}
