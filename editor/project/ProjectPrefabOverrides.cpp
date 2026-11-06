// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Prefab instance overrides: instance queries, unpack/repack, removed GameObject
// overrides (revert/apply) and component override apply.

#include "Project.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/Transform.h"
#include "project/PrefabUnpackRecord.h"
#include "ProjectPrefabInternal.h"
#include "Out.h"
#include "Stream.h"
#include "command/type/DeleteEntityCmd.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectUtils.h"

#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::editor::prefabinternal;

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

    // Second pass: root matches, the outermost level first (the root of an instance of a
    // variant roots its bases' instances too)
    if (const SceneProject* sceneProject = getScene(sceneId); sceneProject && sceneProject->scene && sceneProject->scene->isEntityCreated(entity)) {
        if (const PrefabComponent* component = sceneProject->scene->findComponent<PrefabComponent>(entity)) {
            const Prefab* prefab = getPrefab(component->path);
            const Prefab::Instance* instance = prefab ? prefab->getInstance(sceneId, entity) : nullptr;
            if (instance && instance->rootEntity == entity) {
                return std::filesystem::path(component->path);
            }
        }
    }
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

// Bulk findPrefabPathFor for passes over every entity of a scene (the Hierarchy), with the
// same precedence: a member match wins over every root match, a root's own PrefabComponent
// prefab (the outermost level of a variant) comes before the path order, and the first
// prefab in path order wins the remaining ties.
std::unordered_map<Entity, std::filesystem::path> editor::Project::getPrefabPathsForScene(uint32_t sceneId) const {
    std::unordered_map<Entity, std::filesystem::path> paths;

    for (const auto& [filepath, prefab] : prefabAssets) {
        auto sceneIt = prefab.instances.find(sceneId);
        if (sceneIt == prefab.instances.end()) {
            continue;
        }
        for (const auto& instance : sceneIt->second) {
            for (const auto& member : instance.members) {
                paths.try_emplace(member.localEntity, filepath);
            }
        }
    }

    const SceneProject* sceneProject = getScene(sceneId);
    for (const auto& [filepath, prefab] : prefabAssets) {
        auto sceneIt = prefab.instances.find(sceneId);
        if (sceneIt == prefab.instances.end()) {
            continue;
        }
        for (const auto& instance : sceneIt->second) {
            const Entity root = instance.rootEntity;
            if (paths.count(root)) {
                continue;
            }
            if (sceneProject && sceneProject->scene && sceneProject->scene->isEntityCreated(root)) {
                if (const PrefabComponent* component = sceneProject->scene->findComponent<PrefabComponent>(root)) {
                    const Prefab* own = getPrefab(component->path);
                    const Prefab::Instance* ownInstance = own ? own->getInstance(sceneId, root) : nullptr;
                    if (ownInstance && ownInstance->rootEntity == root) {
                        paths.emplace(root, std::filesystem::path(component->path));
                        continue;
                    }
                }
            }
            paths.emplace(root, filepath);
        }
    }

    // findPrefabPathFor's callers ignore an empty result
    for (auto it = paths.begin(); it != paths.end();) {
        if (it->second.empty()) {
            it = paths.erase(it);
        } else {
            ++it;
        }
    }

    return paths;
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

ScenePrefabIndex indexScenePrefabs(const std::map<std::filesystem::path, editor::Prefab>& prefabAssets, uint32_t sceneId,
        const EntityRegistry* scene = nullptr) {
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
            // A root of several levels (a variant's instance and its bases') is indexed by
            // its outermost one, the prefab its PrefabComponent names
            auto known = index.roots.find(instance.rootEntity);
            bool outermost = known == index.roots.end();
            if (!outermost && scene && scene->isEntityCreated(instance.rootEntity)) {
                const PrefabComponent* component = scene->getSignature(instance.rootEntity).test(scene->getComponentId<PrefabComponent>()) ? &scene->getComponent<PrefabComponent>(instance.rootEntity) : nullptr;
                outermost = component && std::filesystem::path(component->path) == path;
            }
            if (outermost) {
                index.roots[instance.rootEntity] = {&path, &instance};
            }
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

    const SceneProject* sceneProject = getScene(sceneId);
    const ScenePrefabIndex index = indexScenePrefabs(prefabAssets, sceneId, sceneProject ? sceneProject->scene : nullptr);
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

    const ScenePrefabIndex index = indexScenePrefabs(prefabAssets, sceneId, scene);

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
            // every level at the root (a variant's bases have members of their own)
            std::vector<const Prefab::Instance*> levels;
            for (const PrefabInstanceLevel& level : getInstanceLevels(sceneProject, unpackRoots[next])) {
                levels.push_back(level.instance);
            }
            if (levels.empty()) {
                levels.push_back(index.roots.at(unpackRoots[next]).instance);
            }
            for (const Prefab::Instance* level : levels) {
                for (const Prefab::EntityMember& member : level->members) {
                    if (index.roots.count(member.localEntity) && queued.insert(member.localEntity).second) {
                        unpackRoots.push_back(member.localEntity);
                    }
                }
            }
        }
    }

    if (unpackRoots.empty()) {
        return records;
    }

    // The levels unpacked at each root: the outermost one; every level (a variant's bases)
    // when unpacking completely. Unity: unpacking an instance of a Prefab Variant makes it
    // an instance of the variant's base (PREFABS.md 19).
    std::set<std::pair<Entity, std::filesystem::path>> unpackLevels;
    std::unordered_map<Entity, std::filesystem::path> becomesBase;
    for (Entity root : unpackRoots) {
        const std::vector<PrefabInstanceLevel> levels = getInstanceLevels(sceneProject, root);
        if (levels.empty()) {
            unpackLevels.insert({root, *index.roots.at(root).path});
            continue;
        }
        for (size_t i = 0; i < levels.size(); i++) {
            if (i == 0 || completely) {
                unpackLevels.insert({root, levels[i].path});
            }
        }
        if (!completely && levels.size() > 1) {
            becomesBase[root] = levels[1].path;
        }
    }

    // One pass over each prefab's instance list takes its instances out. A prefab's
    // records stay together, in ascending list position, for repackPrefabInstances.
    std::vector<std::filesystem::path> prefabPaths;
    for (const auto& [root, path] : unpackLevels) {
        if (std::find(prefabPaths.begin(), prefabPaths.end(), path) == prefabPaths.end()) {
            prefabPaths.push_back(path);
        }
    }

    // What each prefab overrides of the instances nested in it becomes theirs (kept per root)
    std::unordered_map<Entity, std::vector<PrefabNestedState>> nestedBefore;
    for (Entity root : unpackRoots) {
        promoteNestedAssetData(sceneId, *index.roots.at(root).path, root, nestedBefore[root], queued, !completely);
    }

    records.reserve(unpackRoots.size());
    for (const std::filesystem::path& path : prefabPaths) {
        Prefab& prefab = prefabAssets.at(path);
        auto sceneIt = prefab.instances.find(sceneId);
        if (sceneIt == prefab.instances.end()) {
            continue;
        }
        std::vector<Prefab::Instance>& instances = sceneIt->second;
        size_t kept = 0;
        for (size_t i = 0; i < instances.size(); i++) {
            if (unpackLevels.count({instances[i].rootEntity, path})) {
                PrefabUnpackRecord record;
                record.path = path;
                record.index = i;
                record.instance = std::move(instances[i]);
                // (the nested states once per root: with its outermost level's record)
                if (index.roots.count(record.instance.rootEntity) && *index.roots.at(record.instance.rootEntity).path == path) {
                    record.nestedBefore = std::move(nestedBefore[record.instance.rootEntity]);
                }
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
    // dropping the link is all it takes; only the root carries prefab data of its own. An
    // instance of a variant unpacked one level is an instance of its base now: its root
    // names the base.
    for (PrefabUnpackRecord& record : records) {
        const Entity root = record.instance.rootEntity;
        PrefabComponent* component = scene->findComponent<PrefabComponent>(root);
        if (!component || std::filesystem::path(component->path) != record.path) {
            continue;  // a base level (unpacked completely): the outermost's record has the component
        }
        record.hadComponent = true;
        record.component = *component;
        auto base = becomesBase.find(root);
        if (base != becomesBase.end()) {
            component->name = base->second.stem().string();
            component->path = base->second.string();
            component->guid = getPrefabGuid(base->second);
        } else {
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
            prefab = loadPrefabAsset(path);
            if (!prefab) {
                Out::error("Failed to load prefab %s to restore its instances", path.string().c_str());
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
            // the instances nested in it get back the overrides they had
            for (PrefabNestedState& nested : record.nestedBefore) {
                if (Prefab::Instance* inner = findInstanceAt(sceneProject, nested.root, nested.path)) {
                    inner->overrides = std::move(nested.overrides);
                    inner->removed = std::move(nested.removed);
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
        // every level rooted there (an instance of a variant roots its bases' instances too)
        for (const PrefabInstanceLevel& level : getInstanceLevels(sceneProject, branch[i])) {
            recovery.nestedPrefabs.push_back({level.path, branch[i], level.instance->members});
            for (const Prefab::EntityMember& member : level.instance->members) {
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
    bool hadLinkSource = false;
    for (Entity branchEntity : branch) {
        if (CameraTextureLink::isLinkSource(scene, branchEntity)) {
            hadLinkSource = true;
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
    if (hadLinkSource) {
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
    // The saved values are local to that parent: the move kept a world placement the new
    // GameObject has not computed yet, so they are put back (as DeleteEntityCmd's undo does)
    if (recovery.hasTransform) {
        const YAML::Node readNode = recovery.node;
        const YAML::Node savedTransform = readNode["components"] ? readNode["components"]["transform"] : YAML::Node();
        Transform* transform = scene->findComponent<Transform>(decoded[0]);
        if (transform && savedTransform && savedTransform.IsMap()) {
            const Transform saved = Stream::decodeTransform(savedTransform, transform);
            transform->position = saved.position;
            transform->rotation = saved.rotation;
            transform->scale = saved.scale;
            transform->needUpdate = true;
        }
    }

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

    auto collect = [&](auto&& self, const Prefab::Instance* instance, const fs::path& path, size_t depth) -> void {
        if (!instance || depth > 64) {
            return;
        }
        const Prefab* prefab = getPrefab(path);
        if (!prefab) {
            return;
        }
        const Entity root = instance->rootEntity;
        if (!instance->removed.empty() && prefab->registry) {
            for (Entity registryEntity : prefab->registryEntities) {
                if (!instance->removed.count(registryEntity)) {
                    continue;
                }
                PrefabRemovalInfo info;
                info.path = path;
                info.instanceRoot = root;
                info.registryEntity = registryEntity;
                info.name = prefab->registry->getEntityName(registryEntity);
                info.belowRemoved = prefab->isRegistryEntityAbsent(*instance, prefab->getRegistryParent(registryEntity));
                removals.push_back(std::move(info));
            }
        }
        // the instances nested in it, its variant base level first
        for (const NestedInstanceLevel& nested : getNestedInstanceLevels(sceneProject, path, *instance)) {
            self(self, nested.instance, nested.path, depth + 1);
        }
    };
    const fs::path* rootPath = nullptr;
    const Prefab::Instance* top = instanceRootedAt(prefabAssets, scene, sceneId, instanceRoot, &rootPath);
    if (top) {
        collect(collect, top, *rootPath, 0);
    }
    return removals;
}

std::vector<std::pair<Entity, Entity>> editor::Project::getPrefabAddedGameObjects(uint32_t sceneId, Entity instanceRoot) const {
    std::vector<std::pair<Entity, Entity>> added;
    const SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return added;
    }
    const Scene* scene = sceneProject->scene;

    // The roots and members of the instance and the ones nested in it.
    std::unordered_set<Entity> prefabEntities;
    auto collect = [&](auto&& self, const Prefab::Instance* instance, const fs::path& path, size_t depth) -> void {
        if (!instance || depth > 64) {
            return;
        }
        prefabEntities.insert(instance->rootEntity);
        for (const Prefab::EntityMember& member : instance->members) {
            prefabEntities.insert(member.localEntity);
        }
        for (const NestedInstanceLevel& nested : getNestedInstanceLevels(sceneProject, path, *instance)) {
            self(self, nested.instance, nested.path, depth + 1);
        }
    };
    {
        const fs::path* rootPath = nullptr;
        const Prefab::Instance* top = instanceRootedAt(prefabAssets, scene, sceneId, instanceRoot, &rootPath);
        if (top) {
            collect(collect, top, *rootPath, 0);
        }
    }

    const std::unordered_set<Entity> tracked(sceneProject->entities.begin(), sceneProject->entities.end());
    auto transforms = scene->getComponentArray<Transform>();
    for (size_t i = 0; i < transforms->size(); i++) {
        const Entity entity = transforms->getEntity(i);
        const Entity parent = transforms->getComponentFromIndex(i).parent;
        if (parent != NULL_ENTITY && prefabEntities.count(parent) && !prefabEntities.count(entity) && tracked.count(entity)) {
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

bool editor::Project::applyPrefabRemovalTo(uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot,
        Entity registryEntity, const std::filesystem::path& target) {
    // As an override in a prefab holding the instance (a variant of its prefab, or one it is
    // nested in): that prefab's data for the instance leaves the GameObject out
    SceneProject* sceneProject = getScene(sceneId);
    Prefab* prefab = getPrefab(path);
    Prefab::Instance* instance = findInstanceAt(sceneProject, instanceRoot, path);
    if (!sceneProject || !prefab || !instance || !prefab->registry || !prefab->registry->isEntityCreated(registryEntity) ||
            !instance->removed.count(registryEntity) || !canApplyPrefabOverrides()) {
        return false;
    }
    PrefabObjectRef ref;
    ref.path = path;
    ref.prefab = prefab;
    ref.instance = instance;
    ref.localEntity = instanceRoot;
    ref.registryEntity = instance->rootRegistryEntity;
    ref.isRoot = true;
    Prefab* targetPrefab = nullptr;
    PrefabInstanceData* layer = applyTargetLayer(sceneId, ref, target, &targetPrefab);
    if (!layer || !targetPrefab) {
        return false;
    }
    layer->removedGameObjects.insert(registryEntity);
    if (Prefab::Instance* again = findInstanceAt(sceneProject, instanceRoot, path)) {
        again->removed.erase(registryEntity);
    }
    targetPrefab->isModified = true;
    refreshPrefabInstances(target);
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
