// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PrefabAsset.h"
#include "PrefabManager.h"
#include "Log.h"

using namespace jasperengine;

PrefabAsset::PrefabAsset() = default;

PrefabAsset::PrefabAsset(const std::string& guid, const std::string& path): guid(guid), path(path) {
}

PrefabAsset PrefabAsset::load(const std::string& nameOrPath) {
    uint32_t id = PrefabManager::findPrefab(nameOrPath);
    if (id == 0) {
        Log::error("Prefab '%s' not found: only prefabs of the build can be loaded (instantiated in a scene, referenced by a script property, or listed in Assets > Prefabs)", nameOrPath.c_str());
        return PrefabAsset();
    }
    return load(id);
}

PrefabAsset PrefabAsset::load(uint32_t id) {
    if (!PrefabManager::hasPrefab(id)) {
        Log::error("Prefab id %u not found", id);
        return PrefabAsset();
    }
    PrefabAsset asset(PrefabManager::getPrefabGuid(id), PrefabManager::getPrefabName(id) + ".prefab");
    asset.cachedId = id;
    asset.cachedRevision = PrefabManager::getRevision();
    return asset;
}

uint32_t PrefabAsset::getId() const {
    if (cachedRevision == PrefabManager::getRevision() && cachedId != 0) {
        return cachedId;
    }
    uint32_t id = 0;
    if (!guid.empty()) {
        id = PrefabManager::findPrefabByGuid(guid);
    }
    if (id == 0 && !path.empty()) {
        id = PrefabManager::findPrefab(path);
        // a GUID the build does not know with a path it does is another prefab
        if (id != 0 && !guid.empty() && !PrefabManager::getPrefabGuid(id).empty()) {
            id = 0;
        }
    }
    cachedId = id;
    cachedRevision = PrefabManager::getRevision();
    return id;
}

bool PrefabAsset::isValid() const {
    return getId() != 0;
}

bool PrefabAsset::isEmpty() const {
    return guid.empty() && path.empty();
}

const std::string& PrefabAsset::getGuid() const {
    return guid;
}

const std::string& PrefabAsset::getPath() const {
    return path;
}

std::string PrefabAsset::getName() const {
    std::string name = path;
    size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) {
        name = name.substr(slash + 1);
    }
    size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) {
        name = name.substr(0, dot);
    }
    return name;
}

bool PrefabAsset::operator==(const PrefabAsset& other) const {
    if (!guid.empty() || !other.guid.empty()) {
        return guid == other.guid;
    }
    return path == other.path;
}

bool PrefabAsset::operator!=(const PrefabAsset& other) const {
    return !(*this == other);
}
