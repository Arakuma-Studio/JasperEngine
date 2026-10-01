// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PrefabManager.h"
#include "Scene.h"
#include "Log.h"

#include <exception>

using namespace jasperengine;

std::vector<PrefabManager::PrefabEntry> PrefabManager::entries;
std::unordered_map<uint32_t, size_t> PrefabManager::byId;
std::unordered_map<std::string, size_t> PrefabManager::byName;
std::unordered_map<std::string, size_t> PrefabManager::byGuid;
uint32_t PrefabManager::revision = 1;

namespace {

std::string withoutPrefabExtension(const std::string& name) {
    static const std::string extension = ".prefab";
    if (name.size() > extension.size() && name.compare(name.size() - extension.size(), extension.size(), extension) == 0) {
        return name.substr(0, name.size() - extension.size());
    }
    return name;
}

std::string fileStem(const std::string& name) {
    size_t slash = name.find_last_of("/\\");
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

// Entities a failed factory left: user ids are handed out counting up, so whatever it
// made has an id above the counter as it was before the call
void rollbackSpawnedEntities(Scene* scene, Entity lastBefore, Entity root) {
    const Entity lastAfter = scene->getLastEntity();
    for (Entity e = lastAfter; e > lastBefore; e--) {
        if (e != root && scene->isEntityCreated(e)) {
            scene->destroyEntity(e);
        }
    }
    if (root != NULL_ENTITY && scene->isEntityCreated(root)) {
        scene->destroyEntity(root);
    }
}

}

void PrefabManager::rebuildIndex() {
    byId.clear();
    byName.clear();
    byGuid.clear();
    for (size_t i = 0; i < entries.size(); i++) {
        byId[entries[i].id] = i;
        byName[entries[i].name] = i;
        if (!entries[i].guid.empty()) {
            byGuid[entries[i].guid] = i;
        }
    }
    revision++;
    if (revision == 0) {
        revision = 1;
    }
}

const PrefabManager::PrefabEntry* PrefabManager::findEntry(uint32_t id) {
    auto it = byId.find(id);
    return it != byId.end() ? &entries[it->second] : nullptr;
}

void PrefabManager::registerPrefab(uint32_t id, const std::string& name, const std::string& guid, std::function<bool(Scene*, Entity)> factory) {
    const std::string cleanName = withoutPrefabExtension(name);
    auto it = byId.find(id);
    if (it != byId.end()) {
        PrefabEntry& entry = entries[it->second];
        entry.name = cleanName;
        entry.guid = guid;
        entry.factory = std::move(factory);
    } else {
        entries.push_back({id, cleanName, guid, std::move(factory)});
    }
    rebuildIndex();
}

void PrefabManager::registerPrefab(uint32_t id, const std::string& name, std::function<bool(Scene*, Entity)> factory) {
    registerPrefab(id, name, std::string(), std::move(factory));
}

uint32_t PrefabManager::findPrefabByGuid(const std::string& guid) {
    if (guid.empty()) {
        return 0;
    }
    auto it = byGuid.find(guid);
    return it != byGuid.end() ? entries[it->second].id : 0;
}

uint32_t PrefabManager::findPrefab(const std::string& nameOrGuid) {
    if (nameOrGuid.empty()) {
        return 0;
    }
    if (uint32_t id = findPrefabByGuid(nameOrGuid)) {
        return id;
    }

    std::string name = withoutPrefabExtension(nameOrGuid);
    for (char& c : name) {
        if (c == '\\') c = '/';
    }
    while (name.compare(0, 2, "./") == 0) {
        name.erase(0, 2);
    }
    auto it = byName.find(name);
    if (it != byName.end()) {
        return entries[it->second].id;
    }

    // a bare file name, when it names one prefab only
    if (name.find('/') == std::string::npos) {
        uint32_t found = 0;
        for (const PrefabEntry& entry : entries) {
            if (fileStem(entry.name) == name) {
                if (found != 0) {
                    Log::error("PrefabManager: more than one prefab is named '%s', use its path", name.c_str());
                    return 0;
                }
                found = entry.id;
            }
        }
        return found;
    }
    return 0;
}

Entity PrefabManager::instantiate(uint32_t id, Scene* scene) {
    if (!scene) {
        Log::error("PrefabManager: scene is null");
        return NULL_ENTITY;
    }

    const PrefabEntry* entry = findEntry(id);
    if (!entry) {
        Log::error("PrefabManager: prefab id %u not found", id);
        return NULL_ENTITY;
    }
    // the factory may register again (the editor's do not), invalidating the entry
    std::function<bool(Scene*, Entity)> factory = entry->factory;

    const Entity lastBefore = scene->getLastEntity();
    Entity root = scene->createEntity();

    bool ok = false;
    try {
        ok = factory && factory(scene, root);
    } catch (const std::exception& e) {
        Log::error("PrefabManager: factory for prefab id %u threw an exception: %s", id, e.what());
    } catch (...) {
        Log::error("PrefabManager: factory for prefab id %u threw an exception", id);
    }

    if (!ok) {
        Log::error("PrefabManager: factory failed for prefab id %u", id);
        rollbackSpawnedEntities(scene, lastBefore, root);
        return NULL_ENTITY;
    }

    return root;
}

bool PrefabManager::hasPrefab(uint32_t id) {
    return findEntry(id) != nullptr;
}

uint32_t PrefabManager::getPrefabId(const std::string& name) {
    return findPrefab(name);
}

std::string PrefabManager::getPrefabName(uint32_t id) {
    const PrefabEntry* entry = findEntry(id);
    return entry ? entry->name : "";
}

std::string PrefabManager::getPrefabGuid(uint32_t id) {
    const PrefabEntry* entry = findEntry(id);
    return entry ? entry->guid : "";
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

uint32_t PrefabManager::getRevision() {
    return revision;
}

void PrefabManager::clearAll() {
    entries.clear();
    rebuildIndex();
}
