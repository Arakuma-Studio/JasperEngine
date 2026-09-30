// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PrefabManager.h"
#include "Scene.h"
#include "EntityHandle.h"
#include "Log.h"

#include <exception>
#include <unordered_set>

using namespace jasperengine;

std::vector<PrefabManager::PrefabEntry> PrefabManager::entries;
std::vector<PrefabManager::PrefabInstance> PrefabManager::instances;

namespace {

void rollbackSpawnedEntities(Scene* scene, const std::unordered_set<Entity>& beforeSet, Entity root) {
    if (!scene)
        return;

    std::vector<Entity> afterEntities = scene->getEntityList();
    for (auto it = afterEntities.rbegin(); it != afterEntities.rend(); ++it) {
        Entity e = *it;
        if (e != root && beforeSet.find(e) == beforeSet.end() && scene->isEntityCreated(e))
            scene->destroyEntity(e);
    }

    if (root != NULL_ENTITY && scene->isEntityCreated(root))
        scene->destroyEntity(root);
}

}

PrefabManager::PrefabEntry* PrefabManager::findEntry(uint32_t id) {
    for (auto& entry : entries) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

PrefabManager::PrefabEntry* PrefabManager::findEntry(const std::string& name) {
    for (auto& entry : entries) {
        if (entry.name == name)
            return &entry;
    }
    return nullptr;
}

void PrefabManager::registerPrefab(uint32_t id, const std::string& name, std::function<bool(Scene*, Entity)> factory, std::function<bool(Scene*, Entity)> destroyer) {
    if (PrefabEntry* entry = findEntry(id)) {
        entry->name = name;
        entry->factory = std::move(factory);
        entry->destroyer = std::move(destroyer);
        return;
    }
    entries.push_back({id, name, std::move(factory), std::move(destroyer)});
}

Entity PrefabManager::createPrefab(const std::string& name, Scene* scene) {
    return createPrefab(name, scene, NULL_ENTITY);
}

Entity PrefabManager::createPrefab(uint32_t id, Scene* scene) {
    return instantiate(id, scene, NULL_ENTITY);
}

Entity PrefabManager::createPrefab(const std::string& name, Scene* scene, const std::string& parentName) {
    PrefabEntry* entry = findEntry(name);
    if (!entry) {
        Log::error("PrefabManager: prefab '%s' not found", name.c_str());
        return NULL_ENTITY;
    }
    return createPrefab(entry->id, scene, parentName);
}

Entity PrefabManager::createPrefab(uint32_t id, Scene* scene, const std::string& parentName) {
    if (!scene) {
        Log::error("PrefabManager: scene is null");
        return NULL_ENTITY;
    }
    Entity parent = scene->findEntity(parentName);
    if (parent == NULL_ENTITY) {
        Log::error("PrefabManager: parent entity '%s' not found in the given scene", parentName.c_str());
        return NULL_ENTITY;
    }
    return instantiate(id, scene, parent);
}

Entity PrefabManager::createPrefab(const std::string& name, Scene* scene, Entity parent) {
    PrefabEntry* entry = findEntry(name);
    if (!entry) {
        Log::error("PrefabManager: prefab '%s' not found", name.c_str());
        return NULL_ENTITY;
    }
    return instantiate(entry->id, scene, parent);
}

Entity PrefabManager::createPrefab(uint32_t id, Scene* scene, Entity parent) {
    return instantiate(id, scene, parent);
}

Entity PrefabManager::createPrefab(const std::string& name, const EntityHandle& parent) {
    return createPrefab(name, parent.getScene(), parent.getEntity());
}

Entity PrefabManager::createPrefab(uint32_t id, const EntityHandle& parent) {
    return instantiate(id, parent.getScene(), parent.getEntity());
}

Entity PrefabManager::instantiate(uint32_t id, Scene* scene, Entity parent) {
    if (!scene) {
        Log::error("PrefabManager: scene is null");
        return NULL_ENTITY;
    }
    if (parent != NULL_ENTITY && !scene->isEntityCreated(parent)) {
        Log::error("PrefabManager: parent entity %u does not exist in the given scene", parent);
        return NULL_ENTITY;
    }

    PrefabEntry* entry = findEntry(id);
    if (!entry) {
        Log::error("PrefabManager: prefab id %u not found", id);
        return NULL_ENTITY;
    }

    std::vector<Entity> beforeEntities = scene->getEntityList();
    std::unordered_set<Entity> beforeSet(beforeEntities.begin(), beforeEntities.end());

    Entity root = scene->createEntity();

    bool ok = false;
    try {
        ok = entry->factory && entry->factory(scene, root);
    } catch (const std::exception& e) {
        Log::error("PrefabManager: factory for prefab id %u threw an exception: %s", id, e.what());
    } catch (...) {
        Log::error("PrefabManager: factory for prefab id %u threw an exception", id);
    }

    if (!ok) {
        Log::error("PrefabManager: factory failed for prefab id %u", id);
        rollbackSpawnedEntities(scene, beforeSet, root);
        return NULL_ENTITY;
    }

    // after the factory, which is what gives the root its Transform
    if (parent != NULL_ENTITY)
        scene->addEntityChild(parent, root, false);

    PrefabInstance instance;
    instance.rootEntity = root;
    instance.scene = scene;
    instance.prefabId = id;
    instance.entities.push_back(root);
    for (Entity e : scene->getEntityList()) {
        if (e != root && beforeSet.find(e) == beforeSet.end())
            instance.entities.push_back(e);
    }

    instances.push_back(std::move(instance));
    return root;
}

bool PrefabManager::destroyPrefab(Scene* scene, Entity rootEntity) {
    for (auto it = instances.begin(); it != instances.end(); ++it) {
        if (it->scene == scene && it->rootEntity == rootEntity) {
            uint32_t prefabId = it->prefabId;

            for (auto& entry : entries) {
                if (entry.id == prefabId && entry.destroyer) {
                    bool result = entry.destroyer(scene, rootEntity);
                    instances.erase(it);
                    return result;
                }
            }

            for (auto eit = it->entities.rbegin(); eit != it->entities.rend(); ++eit) {
                if (scene->isEntityCreated(*eit))
                    scene->destroyEntity(*eit);
            }
            instances.erase(it);
            return true;
        }
    }
    Log::error("PrefabManager: prefab instance with root %u not found in scene", rootEntity);
    return false;
}

uint32_t PrefabManager::getPrefabId(const std::string& name) {
    PrefabEntry* entry = findEntry(name);
    return entry ? entry->id : 0;
}

std::string PrefabManager::getPrefabName(uint32_t id) {
    PrefabEntry* entry = findEntry(id);
    return entry ? entry->name : "";
}

std::vector<std::string> PrefabManager::getPrefabNames() {
    std::vector<std::string> names;
    names.reserve(entries.size());
    for (const auto& entry : entries)
        names.push_back(entry.name);
    return names;
}

int PrefabManager::getPrefabCount() {
    return (int)entries.size();
}

void PrefabManager::destroyAllInstances(Scene* scene) {
    std::vector<Entity> roots;
    for (const auto& inst : instances) {
        if (inst.scene == scene)
            roots.push_back(inst.rootEntity);
    }
    for (Entity root : roots)
        destroyPrefab(scene, root);
}

void PrefabManager::clearAll() {
    entries.clear();
    instances.clear();
}
