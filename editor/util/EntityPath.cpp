// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EntityPath.h"
#include "component/Transform.h"

#include "Project.h"
#include "animation/ClipBinding.h"

using namespace jasperengine;

namespace {

// A lookup may visit most of the scene. Count each preceding Transform only once,
// including siblings outside the caller's entity list. Build the index lazily so a
// match near the beginning of Transform order still does only a small amount of work.
struct SiblingIndices {
    std::map<std::pair<Entity, std::string>, int> counts;
    std::vector<int> indices;

    int get(EntityRegistry* registry, size_t own) {
        auto transforms = registry->getComponentArray<Transform>();
        while (indices.size() <= own) {
            const size_t i = indices.size();
            const Entity entity = transforms->getEntity(i);
            const Entity parent = transforms->getComponentFromIndex(i).parent;
            indices.push_back(counts[{parent, registry->getEntityName(entity)}]++);
        }
        return indices[own];
    }
};

std::string pathOf(EntityRegistry* registry, Entity entity, editor::EntityPath::Cache& cache, int depth,
                   SiblingIndices* siblings = nullptr) {
    auto it = cache.find(entity);
    if (it != cache.end()) return it->second;
    std::string parentPath;
    Entity parent = NULL_ENTITY;
    if (Transform* transform = registry->findComponent<Transform>(entity)) parent = transform->parent;
    if (parent != NULL_ENTITY && depth < 256) parentPath = pathOf(registry, parent, cache, depth + 1, siblings) + "/";
    // index among the earlier siblings of the same name, in Transform order
    std::string name = registry->getEntityName(entity);
    int index = 0;
    if (registry->findComponent<Transform>(entity)) {
        auto transforms = registry->getComponentArray<Transform>();
        size_t own = transforms->getIndex(entity);
        // Small prefixes are cheaper to scan than to allocate a sibling index.
        if (siblings && own >= 16) {
            index = siblings->get(registry, own);
        } else {
            for (size_t i = 0; i < own; i++) {
                Entity other = transforms->getEntity(i);
                if (transforms->getComponentFromIndex(i).parent == parent && registry->getEntityName(other) == name) index++;
            }
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
    SiblingIndices siblings;
    // A small candidate subset may lie late in a large Transform array. Avoid
    // indexing that entire prefix when only a handful of paths will be tested.
    SiblingIndices* siblingIndex = entities.size() >= 16 ? &siblings : nullptr;
    for (Entity entity : entities) {
        if (registry->isEntityCreated(entity) && pathOf(registry, entity, cache, 0, siblingIndex) == path) {
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

bool editor::EntityPath::relative(EntityRegistry* registry, Entity root, Entity entity, std::string& out) {
    return anim::relativePath(registry, root, entity, out);
}

Entity editor::EntityPath::findRelative(EntityRegistry* registry, Entity root, const std::string& path) {
    return anim::resolvePath(registry, root, path);
}

bool editor::EntityPath::isAddressableName(const std::string& name) {
    return anim::isAddressableName(name);
}

std::string editor::EntityPath::unaddressableReason(const std::string& name) {
    if (anim::isAddressableName(name)) return std::string();
    if (name.empty()) return "a GameObject without a name can't be addressed by an animation path: name it";
    if (name.find('/') != std::string::npos) {
        return "'" + name + "' contains '/', so an animation path can't address it: rename it";
    }
    return "'" + name + "' ends in '#' and digits, which a path reads as a sibling index: rename it";
}
