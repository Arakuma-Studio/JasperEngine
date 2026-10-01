// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Prefab instance overrides: instance queries, unpack/repack, removed GameObject
// overrides (revert/apply) and component override apply.

#include "Project.h"
#include "ProjectPrefabInternal.h"
#include "Out.h"
#include "Stream.h"
#include "command/type/DeleteEntityCmd.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectUtils.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;
using editor::prefabinternal::componentHasExternalEntityRef;

editor::Prefab* editor::Project::getPrefab(const std::filesystem::path& filepath){
    if (filepath.empty()){
        return nullptr;
    }
    auto it = prefabAssets.find(filepath);
    if (it != prefabAssets.end()) {
        return &it->second;
    }
    return nullptr;
}

const editor::Prefab* editor::Project::getPrefab(const std::filesystem::path& filepath) const{
    if (filepath.empty()){
        return nullptr;
    }
    auto it = prefabAssets.find(filepath);
    if (it != prefabAssets.end()) {
        return &it->second;
    }
    return nullptr;
}

std::map<std::filesystem::path, const editor::Prefab*> editor::Project::getPrefabs(uint32_t sceneId) const {
    std::map<std::filesystem::path, const Prefab*> prefabsInScene;
    for (const auto& [filepath, prefab] : prefabAssets) {
        if (prefab.hasInstances(sceneId)) {
            prefabsInScene[filepath] = &prefab;
        }
    }
    return prefabsInScene;
}

std::filesystem::path editor::Project::findPrefabPathFor(uint32_t sceneId, Entity entity) const {
    // First pass: prefer member matches (so nested prefab roots resolve to the outer prefab)
    for (const auto& [filepath, prefab] : prefabAssets) {
        auto sceneIt = prefab.instances.find(sceneId);
        if (sceneIt == prefab.instances.end()) {
            continue;
        }

        for (const auto& instance : sceneIt->second) {
            for (const auto& member : instance.members) {
                if (member.localEntity == entity) {
                    return filepath;
                }
            }
        }
    }

    // Second pass: root matches
    for (const auto& [filepath, prefab] : prefabAssets) {
        auto sceneIt = prefab.instances.find(sceneId);
        if (sceneIt == prefab.instances.end()) {
            continue;
        }

        for (const auto& instance : sceneIt->second) {
            if (instance.rootEntity == entity) {
                return filepath;
            }
        }
    }

    return std::filesystem::path();
}

bool editor::Project::isEntityInPrefab(uint32_t sceneId, Entity entity) const{
    for (const auto& [filepath, prefab] : prefabAssets){
        if (prefab.containsEntity(sceneId, entity)) {
            return true;
        }
    }
    return false;
}

namespace {

// A scene's prefab instances indexed in one pass, so resolving many entities never
// rescans every prefab and member
struct ScenePrefabIndex {
    struct Root {
        const std::filesystem::path* path;         // key in Project::prefabAssets
        const editor::Prefab::Instance* instance;
    };
    std::unordered_map<Entity, Root> roots;           // instance root -> its instance
    std::unordered_map<Entity, Entity> memberRoots;   // member -> root of its instance
};

ScenePrefabIndex indexScenePrefabs(const std::map<std::filesystem::path, editor::Prefab>& prefabAssets, uint32_t sceneId) {
    ScenePrefabIndex index;
    for (const auto& [path, prefab] : prefabAssets) {
        auto sceneIt = prefab.instances.find(sceneId);
        if (sceneIt == prefab.instances.end()) {
            continue;
        }
        for (const editor::Prefab::Instance& instance : sceneIt->second) {
            if (instance.rootEntity == NULL_ENTITY) {
                continue;
            }
            index.roots[instance.rootEntity] = {&path, &instance};
            for (const editor::Prefab::EntityMember& member : instance.members) {
                index.memberRoots[member.localEntity] = instance.rootEntity;
            }
        }
    }
    return index;
}

Entity outermostPrefabRoot(const ScenePrefabIndex& index, Entity entity) {
    Entity current = entity;
    if (index.roots.find(current) == index.roots.end()) {
        auto memberIt = index.memberRoots.find(current);
        if (memberIt == index.memberRoots.end()) {
            return NULL_ENTITY;
        }
        current = memberIt->second;
    }
    // A nested instance root is a member of the instance holding it; the bound stops
    // a (never expected) membership cycle
    for (size_t depth = 0; depth < index.roots.size(); depth++) {
        auto memberIt = index.memberRoots.find(current);
        if (memberIt == index.memberRoots.end()) {
            break;
        }
        current = memberIt->second;
    }
    return current;
}

} // namespace

std::vector<Entity> editor::Project::getOutermostPrefabRoots(uint32_t sceneId, const std::vector<Entity>& entities) const {
    std::vector<Entity> result(entities.size(), NULL_ENTITY);
    if (entities.empty()) {
        return result;
    }

    const ScenePrefabIndex index = indexScenePrefabs(prefabAssets, sceneId);
    if (index.roots.empty()) {
        return result;
    }
    for (size_t i = 0; i < entities.size(); i++) {
        result[i] = outermostPrefabRoot(index, entities[i]);
    }
    return result;
}

std::vector<editor::PrefabUnpackRecord> editor::Project::unpackPrefabInstances(uint32_t sceneId, const std::vector<Entity>& roots, bool completely) {
    std::vector<PrefabUnpackRecord> records;

    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return records;
    }
    Scene* scene = sceneProject->scene;

    const ScenePrefabIndex index = indexScenePrefabs(prefabAssets, sceneId);

    // Instances to unpack
    std::vector<Entity> unpackRoots;
    std::unordered_set<Entity> queued;
    for (Entity root : roots) {
        if (index.roots.count(root) && queued.insert(root).second) {
            unpackRoots.push_back(root);
        }
    }

    if (completely && !unpackRoots.empty()) {
        // Every GameObject below the roots: one pass over the transforms, where a parent
        // always comes before its children
        std::unordered_set<Entity> below(unpackRoots.begin(), unpackRoots.end());
        auto transforms = scene->getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++) {
            const Transform& transform = transforms->getComponentFromIndex(i);
            if (transform.parent != NULL_ENTITY && below.count(transform.parent)) {
                below.insert(transforms->getEntity(i));
            }
        }

        // Nested instances below in the hierarchy, added ones included, then the members
        // that are instance roots, the only way to reach those without a Transform
        for (size_t i = 0; i < transforms->size(); i++) {
            Entity entity = transforms->getEntity(i);
            if (below.count(entity) && index.roots.count(entity) && queued.insert(entity).second) {
                unpackRoots.push_back(entity);
            }
        }
        for (size_t next = 0; next < unpackRoots.size(); next++) {
            for (const Prefab::EntityMember& member : index.roots.at(unpackRoots[next]).instance->members) {
                if (index.roots.count(member.localEntity) && queued.insert(member.localEntity).second) {
                    unpackRoots.push_back(member.localEntity);
                }
            }
        }
    }

    if (unpackRoots.empty()) {
        return records;
    }

    // One pass over each prefab's instance list takes its instances out. A prefab's
    // records stay together, in ascending list position, for repackPrefabInstances.
    std::vector<const std::filesystem::path*> prefabPaths;
    std::unordered_set<const std::filesystem::path*> seenPaths;
    for (Entity root : unpackRoots) {
        const std::filesystem::path* path = index.roots.at(root).path;
        if (seenPaths.insert(path).second) {
            prefabPaths.push_back(path);
        }
    }

    records.reserve(unpackRoots.size());
    for (const std::filesystem::path* path : prefabPaths) {
        Prefab& prefab = prefabAssets.at(*path);
        auto sceneIt = prefab.instances.find(sceneId);
        std::vector<Prefab::Instance>& instances = sceneIt->second;
        size_t kept = 0;
        for (size_t i = 0; i < instances.size(); i++) {
            if (queued.count(instances[i].rootEntity)) {
                PrefabUnpackRecord record;
                record.path = *path;
                record.index = i;
                record.instance = std::move(instances[i]);
                records.push_back(std::move(record));
            } else {
                if (kept != i) {
                    instances[kept] = std::move(instances[i]);
                }
                kept++;
            }
        }
        instances.erase(instances.begin() + kept, instances.end());
        if (instances.empty()) {
            prefab.instances.erase(sceneIt);
        }
    }

    // The members already hold the instance's current values, overrides applied, so
    // dropping the link is all it takes; only the root carries prefab data of its own
    for (PrefabUnpackRecord& record : records) {
        const Entity root = record.instance.rootEntity;
        if (const PrefabComponent* component = scene->findComponent<PrefabComponent>(root)) {
            record.hadComponent = true;
            record.component = *component;
            scene->removeComponent<PrefabComponent>(root);
        }
    }

    updateScenePrefabs(sceneProject);
    sceneProject->structureVersion++;
    sceneProject->needUpdateRender = true;

    return records;
}

void editor::Project::repackPrefabInstances(uint32_t sceneId, std::vector<PrefabUnpackRecord> records) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || records.empty()) {
        return;
    }
    Scene* scene = sceneProject->scene;

    // The prefab of each record is the asset its GUID names, renamed or moved since or not
    for (PrefabUnpackRecord& record : records) {
        if (record.hadComponent && !record.component.guid.empty()) {
            const fs::path current = resolvePrefabPath(record.component.guid, record.path);
            if (!current.empty() && current != record.path) {
                record.path = current;
                record.component.path = current.string();
            }
        }
    }

    // A prefab's records come together in ascending list position, so each list is
    // rebuilt with one merge that puts every instance back where it was
    for (size_t begin = 0, end = 0; begin < records.size(); begin = end) {
        end = begin + 1;
        while (end < records.size() && records[end].path == records[begin].path) {
            end++;
        }
        const std::filesystem::path& path = records[begin].path;

        Prefab* prefab = getPrefab(path);
        if (!prefab) {
            // Scene cleanup drops an asset left without instances; its file keeps the
            // registry entity IDs the members map to
            try {
                Prefab loaded;
                loaded.loadRegistry(YAML::LoadFile((getProjectPath() / path).string()));
                resolveRegistryPrefabRefs(loaded.registry.get());
                prefab = &prefabAssets.emplace(path, std::move(loaded)).first->second;
            } catch (const std::exception& e) {
                Out::error("Failed to load prefab %s to restore its instances: %s", path.string().c_str(), e.what());
                continue;
            }
        }

        std::vector<Prefab::Instance>& instances = prefab->instances[sceneId];
        std::vector<Prefab::Instance> merged;
        merged.reserve(instances.size() + (end - begin));
        size_t existing = 0;

        for (size_t r = begin; r < end; r++) {
            PrefabUnpackRecord& record = records[r];
            Prefab::Instance& instance = record.instance;
            const Entity root = instance.rootEntity;

            if (!scene->isEntityCreated(root)) {
                Out::warning("Prefab instance root %u of %s is gone, its prefab link was not restored",
                    root, path.string().c_str());
                continue;
            }

            // Members the scene or the prefab lost since the unpack (only an edit made
            // through another scene gets here) stay as plain GameObjects under the instance
            auto lost = [&](const Prefab::EntityMember& member) {
                return !scene->isEntityCreated(member.localEntity) ||
                    !prefab->registry || !prefab->registry->isEntityCreated(member.registryEntity);
            };
            for (const Prefab::EntityMember& member : instance.members) {
                if (lost(member)) {
                    instance.overrides.erase(member.localEntity);
                }
            }
            instance.members.erase(std::remove_if(instance.members.begin(), instance.members.end(), lost), instance.members.end());
            // Removed GameObjects the prefab no longer holds have nothing left to keep out
            for (auto removedIt = instance.removed.begin(); removedIt != instance.removed.end();) {
                if (!prefab->registry || !prefab->registry->isEntityCreated(removedIt->first)) {
                    removedIt = instance.removed.erase(removedIt);
                } else {
                    ++removedIt;
                }
            }

            prefab->nextInstanceId = std::max(prefab->nextInstanceId, instance.instanceId + 1);

            while (merged.size() < record.index && existing < instances.size()) {
                merged.push_back(std::move(instances[existing++]));
            }
            merged.push_back(std::move(instance));

            if (record.hadComponent) {
                if (PrefabComponent* component = scene->findComponent<PrefabComponent>(root)) {
                    *component = record.component;
                } else {
                    scene->addComponent<PrefabComponent>(root, record.component);
                }
            }
        }
        while (existing < instances.size()) {
            merged.push_back(std::move(instances[existing++]));
        }

        if (merged.empty()) {
            prefab->instances.erase(sceneId);
        } else {
            instances = std::move(merged);
        }
    }

    updateScenePrefabs(sceneProject);
    sceneProject->structureVersion++;
    sceneProject->needUpdateRender = true;
}

//=== Removed GameObject overrides ===

namespace {

// The instance of the prefab on `entity`'s PrefabComponent rooted at `entity`, if any
const editor::Prefab::Instance* instanceRootedAt(const std::map<std::filesystem::path, editor::Prefab>& prefabAssets,
        const EntityRegistry* scene, uint32_t sceneId, Entity entity, const std::filesystem::path** path = nullptr) {
    if (entity == NULL_ENTITY || !scene->isEntityCreated(entity) ||
        !scene->getSignature(entity).test(scene->getComponentId<PrefabComponent>())) {
        return nullptr;
    }
    const PrefabComponent* component = &scene->getComponent<PrefabComponent>(entity);
    if (component->path.empty()) {
        return nullptr;
    }
    auto prefabIt = prefabAssets.find(component->path);
    if (prefabIt == prefabAssets.end()) {
        return nullptr;
    }
    const editor::Prefab::Instance* instance = prefabIt->second.getInstance(sceneId, entity);
    if (!instance || instance->rootEntity != entity) {
        return nullptr;
    }
    if (path) {
        *path = &prefabIt->first;
    }
    return instance;
}

}

editor::PrefabBranchRecovery editor::Project::removePrefabBranch(uint32_t sceneId, Entity entity, bool recordRemoval) {
    PrefabBranchRecovery recovery;
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !sceneProject->scene->isEntityCreated(entity)) {
        return recovery;
    }
    Scene* scene = sceneProject->scene;

    const fs::path path = findPrefabPathFor(sceneId, entity);
    Prefab* prefab = getPrefab(path);
    Prefab::Instance* instance = prefab ? prefab->getInstance(sceneId, entity) : nullptr;
    if (!instance || instance->rootEntity == entity) {
        return recovery;
    }
    Entity registryEntity = NULL_ENTITY;
    for (const Prefab::EntityMember& member : instance->members) {
        if (member.localEntity == entity) {
            registryEntity = member.registryEntity;
            break;
        }
    }
    if (registryEntity == NULL_ENTITY) {
        return recovery;
    }

    // The snapshot: a nested instance root as an instance root (its overrides, added and
    // removed GameObjects), anything else as plain GameObjects holding nested instance roots
    const bool isNestedRoot = instanceRootedAt(prefabAssets, scene, sceneId, entity) != nullptr;
    YAML::Node node = Stream::encodeEntity(entity, scene, this, sceneProject, !isNestedRoot);
    if (!node.IsMap()) {
        return recovery;
    }

    recovery.path = path;
    recovery.instanceRoot = instance->rootEntity;
    recovery.registryEntity = registryEntity;
    recovery.node.reset(node);

    Transform* transform = scene->findComponent<Transform>(entity);
    recovery.hasTransform = transform != nullptr;
    if (transform) {
        recovery.parent = transform->parent;
        recovery.index = scene->getComponentArray<Transform>()->getIndex(entity);
    } else {
        recovery.parent = instance->rootEntity;
        auto tracked = std::find(sceneProject->entities.begin(), sceneProject->entities.end(), entity);
        recovery.index = tracked != sceneProject->entities.end() ? std::distance(sceneProject->entities.begin(), tracked) : 0;
    }

    // The branch: its Transform range, and the members of nested instances in it that
    // have no Transform. Containing instances come before the ones they hold.
    std::vector<Entity> branch;
    std::unordered_set<Entity> branchSet;
    auto addToBranch = [&](Entity candidate) {
        if (candidate != NULL_ENTITY && scene->isEntityCreated(candidate) && branchSet.insert(candidate).second) {
            branch.push_back(candidate);
        }
    };
    if (transform) {
        auto transforms = scene->getComponentArray<Transform>();
        const size_t first = transforms->getIndex(entity);
        const size_t last = scene->findBranchLastIndex(entity);
        for (size_t i = first; i <= last && i < transforms->size(); i++) {
            addToBranch(transforms->getEntity(i));
        }
    } else {
        addToBranch(entity);
    }
    for (size_t i = 0; i < branch.size(); i++) {
        const fs::path* nestedPath = nullptr;
        if (const Prefab::Instance* nested = instanceRootedAt(prefabAssets, scene, sceneId, branch[i], &nestedPath)) {
            recovery.nestedPrefabs.push_back({*nestedPath, branch[i], nested->members});
            for (const Prefab::EntityMember& member : nested->members) {
                addToBranch(member.localEntity);
            }
        }
    }

    // Out of the instance's bookkeeping
    for (const Prefab::EntityMember& member : instance->members) {
        if (branchSet.count(member.localEntity)) {
            recovery.members.push_back(member);
        }
    }
    instance->members.erase(std::remove_if(instance->members.begin(), instance->members.end(),
        [&branchSet](const Prefab::EntityMember& member) { return branchSet.count(member.localEntity) != 0; }),
        instance->members.end());
    for (auto it = instance->overrides.begin(); it != instance->overrides.end();) {
        if (branchSet.count(it->first)) {
            recovery.overrides.insert(*it);
            it = instance->overrides.erase(it);
        } else {
            ++it;
        }
    }
    if (recordRemoval) {
        instance->removed[registryEntity] = recovery.members;
        recovery.recorded = true;
    }

    // Out of the scene: what the branch owns elsewhere stays (a delete takes those along
    // itself), and so do the nested instances' records until their roots are gone
    bool hadCamera = false;
    for (Entity branchEntity : branch) {
        if (scene->findComponent<CameraComponent>(branchEntity)) {
            hadCamera = true;
        }
        if (ActionComponent* action = scene->findComponent<ActionComponent>(branchEntity)) {
            action->ownedTarget = false;
        }
        if (AnimationComponent* animation = scene->findComponent<AnimationComponent>(branchEntity)) {
            animation->ownedActions = false;
        }
    }
    for (const NestedPrefabRecovery& nested : recovery.nestedPrefabs) {
        removePrefabInstanceTracking(sceneId, nested.rootEntity);
    }
    DeleteEntityCmd::destroyEntities(scene, branch, sceneProject->entities, this, sceneId);
    if (hadCamera) {
        CameraTextureLink::resolve(scene);
    }

    updateScenePrefabs(sceneProject);
    sceneProject->isModified = true;
    sceneProject->needUpdateRender = true;

    return recovery;
}

bool editor::Project::restorePrefabBranch(uint32_t sceneId, const PrefabBranchRecovery& recovery,
        std::unordered_map<Entity, Entity>* entityRemap, std::vector<Entity>* restoredEntities) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !recovery.valid()) {
        return false;
    }
    Scene* scene = sceneProject->scene;

    std::unordered_map<Entity, Entity> localRemap;
    auto remapped = [&](Entity entity) {
        if (auto it = localRemap.find(entity); it != localRemap.end()) {
            return it->second;
        }
        if (entityRemap) {
            if (auto it = entityRemap->find(entity); it != entityRemap->end()) {
                return it->second;
            }
        }
        return entity;
    };

    const Entity instanceRoot = remapped(recovery.instanceRoot);
    Prefab* prefab = getPrefab(recovery.path);
    const Prefab::Instance* found = prefab ? prefab->getInstance(sceneId, instanceRoot) : nullptr;
    if (!found || found->rootEntity != instanceRoot) {
        Out::error("Could not put back a GameObject of prefab %s: its instance is gone", recovery.path.string().c_str());
        return false;
    }
    const uint32_t instanceId = found->instanceId;

    // Same ids where free; the nested instances are imported again from their saved roots
    std::vector<Entity> decoded = Stream::decodeEntity(recovery.node, scene, &sceneProject->entities, this, sceneProject,
        NULL_ENTITY, true, false, &localRemap);
    if (decoded.empty()) {
        return false;
    }
    ProjectUtils::moveEntityOrderByIndex(scene, sceneProject->entities, decoded[0], remapped(recovery.parent),
        recovery.index, recovery.hasTransform);

    // Nested members got new ids from the import: matched by their registry entity
    for (const NestedPrefabRecovery& saved : recovery.nestedPrefabs) {
        const Entity root = remapped(saved.rootEntity);
        const Prefab* nestedPrefab = getPrefab(saved.path);
        const Prefab::Instance* nested = nestedPrefab ? nestedPrefab->getInstance(sceneId, root) : nullptr;
        if (!nested || nested->rootEntity != root) {
            continue;
        }
        std::unordered_map<Entity, Entity> byRegistry;
        for (const Prefab::EntityMember& member : nested->members) {
            byRegistry[member.registryEntity] = member.localEntity;
        }
        for (const Prefab::EntityMember& member : saved.members) {
            auto it = byRegistry.find(member.registryEntity);
            if (it != byRegistry.end() && it->second != member.localEntity) {
                localRemap[member.localEntity] = it->second;
            }
        }
    }

    // Back into the instance's bookkeeping (found again: an import can move the list). A
    // GameObject the Prefab lost meanwhile (through another scene) stays as the instance's own.
    if (Prefab::Instance* instance = prefab->getInstanceById(sceneId, instanceId)) {
        for (const Prefab::EntityMember& member : recovery.members) {
            const Entity local = remapped(member.localEntity);
            if (scene->isEntityCreated(local) && prefab->registry && prefab->registry->isEntityCreated(member.registryEntity)) {
                instance->members.push_back({local, member.registryEntity});
            }
        }
        for (const auto& [overridden, mask] : recovery.overrides) {
            const Entity local = remapped(overridden);
            if (scene->isEntityCreated(local)) {
                instance->overrides[local] = mask;
            }
        }
        if (recovery.recorded) {
            instance->removed.erase(recovery.registryEntity);
        }
    }

    if (entityRemap) {
        for (const auto& [oldEntity, newEntity] : localRemap) {
            (*entityRemap)[oldEntity] = newEntity;
        }
    } else if (!localRemap.empty()) {
        remapEntityProperties(scene, sceneProject->entities, localRemap, false);
    }
    if (restoredEntities) {
        restoredEntities->insert(restoredEntities->end(), decoded.begin(), decoded.end());
    } else {
        CameraTextureLink::resolve(scene, decoded);
    }

    updateScenePrefabs(sceneProject);
    sceneProject->isModified = true;
    sceneProject->needUpdateRender = true;
    return true;
}

std::vector<editor::PrefabRemovalInfo> editor::Project::getPrefabRemovals(uint32_t sceneId, Entity instanceRoot) const {
    std::vector<PrefabRemovalInfo> removals;
    const SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return removals;
    }
    const Scene* scene = sceneProject->scene;

    auto collect = [&](auto&& self, Entity root, size_t depth) -> void {
        const fs::path* path = nullptr;
        const Prefab::Instance* instance = instanceRootedAt(prefabAssets, scene, sceneId, root, &path);
        if (!instance || depth > 64) {
            return;
        }
        const Prefab& prefab = prefabAssets.at(*path);
        if (!instance->removed.empty() && prefab.registry) {
            for (Entity registryEntity : prefab.registryEntities) {
                if (!instance->removed.count(registryEntity)) {
                    continue;
                }
                PrefabRemovalInfo info;
                info.path = *path;
                info.instanceRoot = root;
                info.registryEntity = registryEntity;
                info.name = prefab.registry->getEntityName(registryEntity);
                info.belowRemoved = prefab.isRegistryEntityAbsent(*instance, prefab.getRegistryParent(registryEntity));
                removals.push_back(std::move(info));
            }
        }
        for (const Prefab::EntityMember& member : instance->members) {
            if (instanceRootedAt(prefabAssets, scene, sceneId, member.localEntity)) {
                self(self, member.localEntity, depth + 1);
            }
        }
    };
    collect(collect, instanceRoot, 0);
    return removals;
}

std::vector<std::pair<Entity, editor::ComponentType>> editor::Project::getPrefabComponentOverrides(uint32_t sceneId, Entity instanceRoot) const {
    std::vector<std::pair<Entity, ComponentType>> overrides;
    const SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return overrides;
    }
    const Scene* scene = sceneProject->scene;

    auto collect = [&](auto&& self, Entity root, size_t depth) -> void {
        const Prefab::Instance* instance = instanceRootedAt(prefabAssets, scene, sceneId, root);
        if (!instance || depth > 64) {
            return;
        }
        for (const Prefab::EntityMember& member : instance->members) {
            auto overrideIt = instance->overrides.find(member.localEntity);
            if (overrideIt == instance->overrides.end()) {
                continue;
            }
            for (int bit = 0; bit < 64; bit++) {
                if (overrideIt->second & (1ULL << bit)) {
                    overrides.push_back({member.localEntity, static_cast<ComponentType>(bit)});
                }
            }
        }
        for (const Prefab::EntityMember& member : instance->members) {
            if (instanceRootedAt(prefabAssets, scene, sceneId, member.localEntity)) {
                self(self, member.localEntity, depth + 1);
            }
        }
    };
    collect(collect, instanceRoot, 0);
    return overrides;
}

std::vector<std::pair<Entity, Entity>> editor::Project::getPrefabAddedGameObjects(uint32_t sceneId, Entity instanceRoot) const {
    std::vector<std::pair<Entity, Entity>> added;
    const SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return added;
    }
    const Scene* scene = sceneProject->scene;

    // The roots and members of the instance and the ones nested in it, and among them the
    // GameObjects an added one hangs from
    std::unordered_set<Entity> prefabEntities;
    std::unordered_set<Entity> parents;
    auto collect = [&](auto&& self, Entity root, size_t depth) -> void {
        const Prefab::Instance* instance = instanceRootedAt(prefabAssets, scene, sceneId, root);
        if (!instance || depth > 64) {
            return;
        }
        prefabEntities.insert(root);
        parents.insert(root);
        for (const Prefab::EntityMember& member : instance->members) {
            prefabEntities.insert(member.localEntity);
            parents.insert(member.localEntity);
        }
        for (const Prefab::EntityMember& member : instance->members) {
            if (instanceRootedAt(prefabAssets, scene, sceneId, member.localEntity)) {
                self(self, member.localEntity, depth + 1);
            }
        }
    };
    collect(collect, instanceRoot, 0);

    const std::unordered_set<Entity> tracked(sceneProject->entities.begin(), sceneProject->entities.end());
    auto transforms = scene->getComponentArray<Transform>();
    for (size_t i = 0; i < transforms->size(); i++) {
        const Entity entity = transforms->getEntity(i);
        const Entity parent = transforms->getComponentFromIndex(i).parent;
        if (parent != NULL_ENTITY && parents.count(parent) && !prefabEntities.count(entity) && tracked.count(entity)) {
            added.push_back({entity, parent});
        }
    }
    return added;
}

bool editor::Project::revertPrefabRemoval(uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot,
        Entity registryEntity, PrefabRemovalRevert& revert) {
    SceneProject* sceneProject = getScene(sceneId);
    Prefab* prefab = getPrefab(path);
    Prefab::Instance* instance = prefab ? prefab->getInstance(sceneId, instanceRoot) : nullptr;
    if (!sceneProject || !sceneProject->scene || !instance || instance->rootEntity != instanceRoot) {
        return false;
    }
    auto entry = instance->removed.find(registryEntity);
    if (entry == instance->removed.end()) {
        return false;
    }
    revert.entry = entry->second;
    revert.local = NULL_ENTITY;
    const uint32_t instanceId = instance->instanceId;
    instance->removed.erase(entry);
    sceneProject->isModified = true;

    // Below another removed GameObject it stays out with it, which a revert of that one
    // now brings back too
    if (!prefab->registry || !prefab->registry->isEntityCreated(registryEntity) ||
        prefab->isRegistryEntityAbsent(*instance, registryEntity)) {
        return true;
    }

    // The ids of a previous run of this revert, else the ones it had when removed
    const std::vector<Prefab::EntityMember>& preferred = revert.created.empty() ? revert.entry : revert.created;
    std::vector<Prefab::EntityMember> created = instantiatePrefabBranch(sceneProject, path, instanceId, registryEntity, preferred);
    if (created.empty()) {
        if (Prefab::Instance* again = prefab->getInstanceById(sceneId, instanceId)) {
            again->removed[registryEntity] = revert.entry;
        }
        return false;
    }
    revert.created = created;
    revert.local = created[0].localEntity;
    return true;
}

void editor::Project::undoRevertPrefabRemoval(uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot,
        Entity registryEntity, const PrefabRemovalRevert& revert) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return;
    }
    if (revert.local != NULL_ENTITY && sceneProject->scene->isEntityCreated(revert.local)) {
        removePrefabBranch(sceneId, revert.local, false);
    }
    Prefab* prefab = getPrefab(path);
    Prefab::Instance* instance = prefab ? prefab->getInstance(sceneId, instanceRoot) : nullptr;
    if (instance && instance->rootEntity == instanceRoot) {
        instance->removed[registryEntity] = revert.entry;
    }
    sceneProject->isModified = true;
}

bool editor::Project::applyPrefabRemoval(uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot,
        Entity registryEntity, PrefabRemovalApply& apply) {
    SceneProject* sceneProject = getScene(sceneId);
    Prefab* prefab = getPrefab(path);
    Prefab::Instance* instance = prefab ? prefab->getInstance(sceneId, instanceRoot) : nullptr;
    if (!sceneProject || !instance || instance->rootEntity != instanceRoot || !prefab->registry ||
        !prefab->registry->isEntityCreated(registryEntity)) {
        return false;
    }
    auto entry = instance->removed.find(registryEntity);
    if (entry == instance->removed.end()) {
        return false;
    }
    apply.entry = entry->second;
    apply.registryParent = prefab->getRegistryParent(registryEntity);
    const uint32_t instanceId = instance->instanceId;
    instance->removed.erase(entry);

    // Out of the Prefab, so out of every instance holding it
    apply.recovery = removeRegistryEntityFromPrefab(path, registryEntity, sceneId, instanceId, true);
    if (apply.recovery.empty()) {
        if (Prefab::Instance* again = prefab->getInstanceById(sceneId, instanceId)) {
            again->removed[registryEntity] = apply.entry;
        }
        return false;
    }
    sceneProject->isModified = true;
    return true;
}

void editor::Project::undoApplyPrefabRemoval(uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot,
        Entity registryEntity, PrefabRemovalApply& apply) {
    Prefab* prefab = getPrefab(path);
    const Prefab::Instance* instance = prefab ? prefab->getInstance(sceneId, instanceRoot) : nullptr;
    if (!instance || instance->rootEntity != instanceRoot) {
        return;
    }
    // The applying instance keeps it removed: its entry rides with the recovery, which
    // then leaves that instance out and maps a registry id the restore had to change
    NodeRecovery recovery = apply.recovery;
    recovery[std::to_string(sceneId) + "_" + std::to_string(instanceRoot)].removed[registryEntity] = apply.entry;
    restoreRegistryEntityToPrefab(path, apply.registryParent, recovery, sceneId, instance->instanceId, true, nullptr, nullptr);
    if (SceneProject* sceneProject = getScene(sceneId)) {
        sceneProject->isModified = true;
    }
}

bool editor::Project::canApplyComponentOverride(uint32_t sceneId, Entity entity, ComponentType componentType) const {
    const SceneProject* sceneProject = getScene(sceneId);
    const Prefab* prefab = getPrefab(findPrefabPathFor(sceneId, entity));
    if (!sceneProject || !sceneProject->scene || !prefab || !prefab->registry ||
        componentType == ComponentType::PrefabComponent || !prefab->hasComponentOverride(sceneId, entity, componentType)) {
        return false;
    }
    const Prefab::Instance* instance = prefab->getInstance(sceneId, entity);
    const Entity registryEntity = prefab->getRegistryEntity(sceneId, entity);
    if (!instance || registryEntity == NULL_ENTITY || !prefab->registry->isEntityCreated(registryEntity) ||
        (prefab->registry->getSignature(registryEntity) & Catalog::componentTypeToSignature(prefab->registry.get(), componentType)).none()) {
        return false;
    }
    std::unordered_set<Entity> memberLocals;
    for (const Prefab::EntityMember& member : instance->members) {
        memberLocals.insert(member.localEntity);
    }
    // A reference outside the instance cannot go into the shared Prefab
    return !componentHasExternalEntityRef(sceneProject->scene, entity, componentType, memberLocals);
}

bool editor::Project::applyComponentOverride(uint32_t sceneId, Entity entity, ComponentType componentType, ComponentRecovery& recovery) {
    if (!canApplyComponentOverride(sceneId, entity, componentType)) {
        return false;
    }
    SceneProject* sceneProject = getScene(sceneId);
    Prefab* prefab = getPrefab(findPrefabPathFor(sceneId, entity));
    const uint32_t instanceId = prefab->getInstanceId(sceneId, entity);
    const Entity registryEntity = prefab->getRegistryEntity(sceneId, entity);
    EntityRegistry* registry = prefab->registry.get();

    recovery.clear();
    const Signature registrySignature = Catalog::componentTypeToSignature(registry, componentType);
    recovery[std::to_string(NULL_PROJECT_SCENE)] = {registryEntity, Stream::encodeComponents(registryEntity, registry, registrySignature)};

    std::unordered_map<Entity, Entity> localToRegistry;
    if (const Prefab::Instance* instance = prefab->getInstanceById(sceneId, instanceId)) {
        for (const Prefab::EntityMember& member : instance->members) {
            localToRegistry[member.localEntity] = member.registryEntity;
        }
    }
    prefab->clearComponentOverride(sceneId, entity, componentType);
    Catalog::copyComponent(sceneProject->scene, entity, registry, registryEntity, componentType);
    remapEntityPropertiesInComponent(registry, registryEntity, componentType, {}, localToRegistry);

    // Every other instance that follows the Prefab on this component takes it
    for (auto& [otherSceneId, sceneInstances] : prefab->instances) {
        SceneProject* otherScene = getScene(otherSceneId);
        if (!otherScene || !otherScene->scene) {
            continue;
        }
        for (const Prefab::Instance& instance : sceneInstances) {
            if (otherSceneId == sceneId && instance.instanceId == instanceId) {
                continue;
            }
            const Entity otherEntity = prefab->getLocalEntity(otherSceneId, instance.instanceId, registryEntity);
            if (otherEntity == NULL_ENTITY || prefab->hasComponentOverride(otherSceneId, otherEntity, componentType)) {
                continue;
            }
            const Signature signature = Catalog::componentTypeToSignature(otherScene->scene, componentType);
            recovery[std::to_string(otherSceneId) + "_" + std::to_string(instance.instanceId)] =
                {otherEntity, Stream::encodeComponents(otherEntity, otherScene->scene, signature)};
            Catalog::copyComponent(registry, registryEntity, otherScene->scene, otherEntity, componentType);
            std::unordered_map<Entity, Entity> registryToLocal;
            for (const Prefab::EntityMember& member : instance.members) {
                registryToLocal[member.registryEntity] = member.localEntity;
            }
            remapEntityPropertiesInComponent(otherScene->scene, otherEntity, componentType, {}, registryToLocal);
            otherScene->isModified = true;
            otherScene->needUpdateRender = true;
        }
    }

    prefab->isModified = true;
    sceneProject->isModified = true;
    return true;
}

void editor::Project::undoApplyComponentOverride(uint32_t sceneId, Entity entity, ComponentType componentType, const ComponentRecovery& recovery) {
    Prefab* prefab = getPrefab(findPrefabPathFor(sceneId, entity));
    if (!prefab || !prefab->registry) {
        return;
    }
    auto parentOf = [](EntityRegistry* registry, Entity target) {
        const Transform* transform = registry->findComponent<Transform>(target);
        return transform ? transform->parent : NULL_ENTITY;
    };
    for (const auto& [key, saved] : recovery) {
        if (key == std::to_string(NULL_PROJECT_SCENE)) {
            if (prefab->registry->isEntityCreated(saved.entity)) {
                Stream::decodeComponents(saved.entity, parentOf(prefab->registry.get(), saved.entity), prefab->registry.get(), saved.node);
            }
            continue;
        }
        const uint32_t otherSceneId = static_cast<uint32_t>(std::stoul(key.substr(0, key.find('_'))));
        SceneProject* otherScene = getScene(otherSceneId);
        if (otherScene && otherScene->scene && otherScene->scene->isEntityCreated(saved.entity)) {
            Stream::decodeComponents(saved.entity, parentOf(otherScene->scene, saved.entity), otherScene->scene, saved.node);
            otherScene->needUpdateRender = true;
        }
    }
    prefab->setComponentOverride(sceneId, entity, componentType);
    prefab->isModified = true;
}
