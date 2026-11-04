// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DeleteEntityCmd.h"
#include "component/CameraComponent.h"
#include "component/FogComponent.h"
#include "component/LightComponent.h"
#include "component/Transform.h"
#include "component/PrefabComponent.h"
#include "pose/PoseEditSession.h"

#include "command/CommandMemory.h"

#include "Stream.h"
#include "Out.h"
#include "util/CameraTextureLink.h"
#include "component/FluidSimulationComponent.h"
#include "component/VideoPlayerComponent.h"
#include "util/ProjectUtils.h"
#include "util/BackgroundRelease.h"
#include "util/EntityClipboard.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using namespace jasperengine;

namespace {

void destroyEntityWithoutReconcile(EntityRegistry* registry, Entity entity) {
    uint64_t structuralFlags = editor::UpdateFlags_None;
    if (registry->isEntityCreated(entity)) { // locked children are deleted by systems with their parent
        if (registry->findComponent<LightComponent>(entity)) {
            structuralFlags |= editor::Catalog::getComponentStructuralUpdateFlags(editor::ComponentType::LightComponent);
        }
        if (registry->findComponent<FogComponent>(entity)) {
            structuralFlags |= editor::Catalog::getComponentStructuralUpdateFlags(editor::ComponentType::FogComponent);
        }
        registry->destroyEntity(entity);
    }
    if (structuralFlags != editor::UpdateFlags_None) {
        editor::Catalog::updateEntity(registry, NULL_ENTITY, structuralFlags);
    }
}

// The Transform index where a restored branch goes back: right after the nearest GameObject
// before it in its recorded order that exists again, or rather after that one's ancestor that is
// the branch's sibling (the whole sibling branch), or right after the parent. NULL_POSITION when
// there is none to go by, or it is not where the hierarchy says (the branch is left where it is).
constexpr size_t NULL_POSITION = static_cast<size_t>(-1);

size_t findRestoredTransformPosition(Scene* scene, const editor::DeleteEntityData& entityData,
        const std::unordered_map<Entity, Entity>& entityRemap) {
    const Entity source = entityData.entity;
    const Transform* sourceTransform = scene->findComponent<Transform>(source);
    if (!sourceTransform || !entityData.transformOrder) {
        return NULL_POSITION;
    }
    const std::vector<Entity>& order = *entityData.transformOrder;
    if (entityData.transformOrderPos >= order.size()) {
        return NULL_POSITION;
    }
    const Entity parent = sourceTransform->parent;
    auto transforms = scene->getComponentArray<Transform>();

    auto existing = [&](Entity entity) -> Entity {
        auto remapped = entityRemap.find(entity);
        if (remapped != entityRemap.end()) {
            entity = remapped->second;
        }
        if (entity == source || !scene->isEntityCreated(entity) || !scene->findComponent<Transform>(entity) ||
            scene->isParentOf(source, entity)) {
            return NULL_ENTITY;
        }
        return entity;
    };

    // The one before, and its ancestor that is a sibling of the branch (or the parent itself)
    for (size_t pos = entityData.transformOrderPos; pos-- > 0;) {
        Entity anchor = existing(order[pos]);
        if (anchor == NULL_ENTITY) {
            continue;
        }
        for (int depth = 0; anchor != NULL_ENTITY && anchor != parent && depth < 1024; depth++) {
            const Transform* transform = scene->findComponent<Transform>(anchor);
            if (!transform) {
                anchor = NULL_ENTITY;
            } else if (transform->parent == parent) {
                break;
            } else {
                anchor = transform->parent;
            }
        }
        if (anchor == NULL_ENTITY || anchor == source) {
            return NULL_POSITION; // not under the same parent as recorded
        }
        if (anchor == parent) {
            return transforms->getIndex(parent) + 1;
        }
        return scene->findBranchLastIndex(anchor) + 1;
    }

    // First of its parent: right after it
    if (parent != NULL_ENTITY) {
        return transforms->getIndex(parent) + 1;
    }
    // First root: before the root of the nearest one after it that exists
    for (size_t pos = entityData.transformOrderPos + 1; pos < order.size(); pos++) {
        Entity anchor = existing(order[pos]);
        if (anchor == NULL_ENTITY) {
            continue;
        }
        for (int depth = 0; depth < 1024; depth++) {
            const Transform* transform = scene->findComponent<Transform>(anchor);
            if (!transform || transform->parent == NULL_ENTITY) {
                break;
            }
            anchor = transform->parent;
        }
        if (anchor == source || !scene->findComponent<Transform>(anchor)) {
            return NULL_POSITION;
        }
        return transforms->getIndex(anchor);
    }
    return NULL_POSITION;
}

}

editor::DeleteEntityCmd::DeleteEntityCmd(Project* project, uint32_t sceneId, Entity entity, bool allowLockedRoots)
    : DeleteEntityCmd(project, sceneId, std::vector<Entity>{entity}, allowLockedRoots){

}

editor::DeleteEntityCmd::DeleteEntityCmd(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, bool allowLockedRoots)
    : DeleteEntityCmd(project, sceneId, entities, allowLockedRoots, {}){

}

editor::DeleteEntityCmd::DeleteEntityCmd(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, bool allowLockedRoots, const std::vector<Entity>& preservedEntities){
    this->project = project;
    this->sceneId = sceneId;
    this->requestedEntities = entities;
    this->preservedEntities = preservedEntities;
    this->allowLockedRoots = allowLockedRoots;

    // a posed character (Pose Animation window) goes back to its rest bones first: they are what
    // the undo snapshots keep
    PoseEditSession::suspendActiveFor(sceneId, entities);
    for (const Entity& entity : entities){
        if (std::find(preservedEntities.begin(), preservedEntities.end(), entity) == preservedEntities.end()) {
            appendEntityData(entity);
        }
    }

    this->wasModified = project->getScene(sceneId)->isModified;
}

editor::DeleteEntityCmd::~DeleteEntityCmd(){
    // The snapshots of a large delete take long to free: the background thread does it
    std::vector<YAML::Node> snapshots;
    snapshots.reserve(entities.size() + preEncoded.size());
    for (DeleteEntityData& entityData : entities) {
        snapshots.emplace_back();
        snapshots.back().reset(entityData.data);
        entityData.data.reset(YAML::Node());
    }
    for (auto& [entity, node] : preEncoded) {
        snapshots.emplace_back();
        snapshots.back().reset(node);
        node.reset(YAML::Node());
    }
    BackgroundRelease::release(std::move(snapshots));
}

bool editor::DeleteEntityCmd::appendEntityData(Entity entity){
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene || !sceneProject->scene->isEntityCreated(entity)) {
        return false;
    }
    if (std::find_if(entities.begin(), entities.end(), [entity](const DeleteEntityData& data) {
            return data.entity == entity;
        }) != entities.end()) {
        return false;
    }

    DeleteEntityData entityData;
    entityData.entity = entity;

    Scene* scene = sceneProject->scene;
    Signature signature = scene->getSignature(entity);
    if (signature.test(scene->getComponentId<Transform>())) {
        entityData.hasTransform = true;
        entityData.parent = scene->getComponent<Transform>(entity).parent;
        entityData.entityIndex = scene->getComponentArray<Transform>()->getIndex(entity);
    } else {
        auto it = std::find(sceneProject->entities.begin(), sceneProject->entities.end(), entity);
        if (it != sceneProject->entities.end()) {
            entityData.entityIndex = std::distance(sceneProject->entities.begin(), it);
        }
    }

    entities.push_back(std::move(entityData));
    return true;
}

std::vector<size_t> editor::DeleteEntityCmd::prepareDeleteSet(){
    SceneProject* sceneProject = project->getScene(sceneId);
    Scene* scene = sceneProject->scene;
    const std::unordered_set<Entity> preserved(preservedEntities.begin(), preservedEntities.end());
    const std::unordered_set<Entity> tracked(sceneProject->entities.begin(), sceneProject->entities.end());

    std::unordered_map<Entity, std::vector<Entity>> transformChildren;
    auto transforms = scene->getComponentArray<Transform>();
    for (size_t i = 0; i < transforms->size(); ++i) {
        transformChildren[transforms->getComponentFromIndex(i).parent].push_back(transforms->getEntity(i));
    }

    struct DeleteCoverage {
        std::unordered_map<Entity, Entity> entityToRoot;
        std::vector<Entity> allEntities;
    };

    auto collectCoverage = [&](const std::vector<Entity>& roots) {
        DeleteCoverage coverage;

        auto addEntity = [&](Entity entity, Entity root) {
            if (entity == NULL_ENTITY || !scene->isEntityCreated(entity) || coverage.entityToRoot.find(entity) != coverage.entityToRoot.end()) {
                return;
            }
            coverage.entityToRoot[entity] = root;
            coverage.allEntities.push_back(entity);
        };

        for (Entity root : roots) {
            addEntity(root, root);
        }

        for (size_t i = 0; i < coverage.allEntities.size(); ++i) {
            Entity entity = coverage.allEntities[i];
            Entity root = coverage.entityToRoot.at(entity);

            // Prefab roots are opaque hierarchy nodes, and nested roots may themselves be
            // members of an already covered prefab instance.
            if (const PrefabComponent* component = scene->findComponent<PrefabComponent>(entity);
                component && !component->path.empty()) {
                // every level rooted here (an instance of a variant roots its bases' too)
                for (const PrefabInstanceLevel& level : project->getInstanceLevels(project->getScene(sceneId), entity)) {
                    for (const Prefab::EntityMember& member : level.instance->members) {
                        addEntity(member.localEntity, root);
                    }
                }
            }

            auto children = transformChildren.find(entity);
            if (children != transformChildren.end()) {
                for (Entity child : children->second) {
                    addEntity(child, root);
                }
            }
        }

        return coverage;
    };

    auto rootEntities = [&]() {
        std::vector<Entity> roots;
        roots.reserve(entities.size());
        for (const DeleteEntityData& data : entities) {
            roots.push_back(data.entity);
        }
        return roots;
    };

    auto forEachOwnedDependency = [&](Entity owner, auto&& callback) {
        for (ComponentType componentType : {ComponentType::ActionComponent, ComponentType::AnimationComponent}) {
            for (Entity dependency : ProjectUtils::getOwnedCascadeEntities(scene, owner, componentType)) {
                callback(dependency);
            }
        }
    };

    auto cascadeTouchesPreserved = [&](Entity root) {
        if (preserved.empty()) {
            return false;
        }

        std::unordered_set<Entity> visited;
        std::function<bool(Entity)> visit = [&](Entity entity) {
            if (!visited.insert(entity).second) {
                return false;
            }

            DeleteCoverage coverage = collectCoverage({entity});
            for (Entity covered : coverage.allEntities) {
                if (preserved.count(covered)) {
                    return true;
                }

                bool touchesPreserved = false;
                forEachOwnedDependency(covered, [&](Entity dependency) {
                    if (!touchesPreserved && dependency != NULL_ENTITY &&
                        scene->isEntityCreated(dependency) && visit(dependency)) {
                        touchesPreserved = true;
                    }
                });
                if (touchesPreserved) {
                    return true;
                }
            }
            return false;
        };

        return visit(root);
    };

    // Removing an owning component must keep its owner alive. If an inconsistent ownership
    // graph points back into a preserved branch, retaining that target is safer than letting
    // an engine callback delete the entity that the outer command still needs to update.
    if (!preserved.empty()) {
        entities.erase(std::remove_if(entities.begin(), entities.end(), [&](const DeleteEntityData& data) {
            return cascadeTouchesPreserved(data.entity);
        }), entities.end());
    }

    // Virtual and owned relationships can introduce new structural roots recursively.
    while (true) {
        DeleteCoverage coverage = collectCoverage(rootEntities());
        bool changed = false;

        for (Entity virtualChild : ProjectUtils::getVirtualChildren(scene, coverage.allEntities, preserved)) {
            if (!cascadeTouchesPreserved(virtualChild) && appendEntityData(virtualChild)) {
                changed = true;
            }
        }

        auto includeDependency = [&](Entity dependency) {
            if (dependency == NULL_ENTITY || preserved.count(dependency) || !scene->isEntityCreated(dependency) ||
                coverage.entityToRoot.find(dependency) != coverage.entityToRoot.end() ||
                !tracked.count(dependency) || cascadeTouchesPreserved(dependency)) {
                return;
            }
            if (appendEntityData(dependency)) {
                changed = true;
            }
        };

        for (Entity owner : coverage.allEntities) {
            forEachOwnedDependency(owner, includeDependency);
        }

        if (!changed) {
            break;
        }
    }

    // Keep only roots not already contained by another root's hierarchy or prefab instance.
    std::unordered_set<Entity> coveredRoots;
    std::vector<Entity> roots = rootEntities();
    const std::unordered_set<Entity> rootSet(roots.begin(), roots.end());
    for (Entity root : roots) {
        DeleteCoverage rootCoverage = collectCoverage({root});
        for (Entity covered : rootCoverage.allEntities) {
            if (covered != root && rootSet.count(covered)) {
                coveredRoots.insert(covered);
            }
        }
    }
    entities.erase(std::remove_if(entities.begin(), entities.end(), [&](const DeleteEntityData& data) {
        return coveredRoots.count(data.entity) != 0;
    }), entities.end());

    // owner root -> dependency roots that must be deleted first.
    DeleteCoverage coverage = collectCoverage(rootEntities());

    prefabInstanceSnapshots.clear();
    auto rememberPrefabInstance = [&](const fs::path& path, Entity root) {
        if (path.empty() || root == NULL_ENTITY ||
            std::find_if(prefabInstanceSnapshots.begin(), prefabInstanceSnapshots.end(), [&](const PrefabInstanceSnapshot& saved) {
                return saved.path == path && saved.rootEntity == root;
            }) != prefabInstanceSnapshots.end()) {
            return;
        }
        const Prefab* prefab = project->getPrefab(path);
        const Prefab::Instance* instance = prefab ? prefab->getInstance(sceneId, root) : nullptr;
        if (instance && instance->rootEntity == root) {
            prefabInstanceSnapshots.push_back({path, root, instance->members});
        }
    };

    for (Entity entity : coverage.allEntities) {
        if (scene->findComponent<PrefabComponent>(entity)) {
            for (const PrefabInstanceLevel& level : project->getInstanceLevels(project->getScene(sceneId), entity)) {
                rememberPrefabInstance(level.path, entity);
            }
        }
    }

    std::unordered_map<Entity, std::vector<Entity>> dependencies;
    auto addDependency = [&](Entity ownerRoot, Entity dependency) {
        auto dependencyIt = coverage.entityToRoot.find(dependency);
        if (dependencyIt == coverage.entityToRoot.end() || dependencyIt->second == ownerRoot) {
            return;
        }
        std::vector<Entity>& roots = dependencies[ownerRoot];
        if (std::find(roots.begin(), roots.end(), dependencyIt->second) == roots.end()) {
            roots.push_back(dependencyIt->second);
        }
    };
    for (Entity owner : coverage.allEntities) {
        Entity ownerRoot = coverage.entityToRoot.at(owner);
        forEachOwnedDependency(owner, [&](Entity dependency) {
            addDependency(ownerRoot, dependency);
        });
    }

    std::unordered_map<Entity, size_t> dataIndex;
    for (size_t i = 0; i < entities.size(); ++i) {
        dataIndex[entities[i].entity] = i;
    }

    std::unordered_set<Entity> visiting;
    std::unordered_set<Entity> emitted;
    std::vector<size_t> deleteOrder;
    deleteOrder.reserve(entities.size());

    std::function<void(Entity)> emit = [&](Entity entity) {
        if (emitted.count(entity) || visiting.count(entity)) {
            return;
        }
        visiting.insert(entity);
        auto depIt = dependencies.find(entity);
        if (depIt != dependencies.end()) {
            for (Entity dependency : depIt->second) {
                emit(dependency);
            }
        }
        visiting.erase(entity);
        if (emitted.insert(entity).second) {
            deleteOrder.push_back(dataIndex.at(entity));
        }
    };

    // Preserve the command's original reverse-delete/forward-undo behavior for unrelated
    // roots while ensuring owned dependencies are explicitly removed before their owners.
    for (auto it = entities.rbegin(); it != entities.rend(); ++it) {
        emit(it->entity);
    }

    return deleteOrder;
}

void editor::DeleteEntityCmd::snapshotEntities(){
    SceneProject* sceneProject = project->getScene(sceneId);
    snapshotsShared = !preEncoded.empty();
    for (DeleteEntityData& entityData : entities) {
        // Rebound, not assigned: an assignment would write through to the node it held,
        // which may be shared (a Cut's clipboard copy)
        entityData.data.reset(YAML::Node());
        entityData.recoveryPrefabData.clear();
        entityData.removedFromInstance = PrefabBranchRecovery();
        entityData.isPrefabRoot = false;
        entityData.prefabPath.clear();

        if (!sceneProject->scene->isEntityCreated(entityData.entity)) {
            continue;
        }

        entityData.prefabPath = project->findPrefabPathFor(sceneId, entityData.entity);
        Prefab* prefab = project->getPrefab(entityData.prefabPath);

        if (prefab) {
            entityData.isPrefabRoot = entityData.entity == prefab->getRootEntity(sceneId, entityData.entity);
        }
        if (!prefab || entityData.isPrefabRoot) {
            auto encoded = preEncoded.find(entityData.entity);
            if (encoded != preEncoded.end()) {
                entityData.data.reset(encoded->second);
            } else {
                entityData.data.reset(Stream::encodeEntity(entityData.entity, sceneProject->scene, project, sceneProject));
            }
        }
    }
    // Only true for the first run: a redo snapshots the scene as it is then
    preEncoded.clear();
}

void editor::DeleteEntityCmd::recordTransformOrder(){
    SceneProject* sceneProject = project->getScene(sceneId);
    Scene* scene = sceneProject->scene;
    // Tracked only: the editor's gizmo entities in the array come and go
    const std::unordered_set<Entity> tracked(sceneProject->entities.begin(), sceneProject->entities.end());
    auto transforms = scene->getComponentArray<Transform>();

    auto order = std::make_shared<std::vector<Entity>>();
    order->reserve(tracked.size());
    std::unordered_map<Entity, size_t> positions;
    for (size_t i = 0; i < transforms->size(); i++) {
        Entity entity = transforms->getEntity(i);
        if (tracked.count(entity)) {
            positions[entity] = order->size();
            order->push_back(entity);
        }
    }

    std::shared_ptr<const std::vector<Entity>> sharedOrder = std::move(order);
    for (DeleteEntityData& entityData : entities) {
        entityData.transformOrder.reset();
        entityData.transformOrderPos = 0;
        if (!entityData.hasTransform || !transforms->hasEntity(entityData.entity)) {
            continue;
        }
        // as it is now, not as when the command was made (a redo runs on the restored scene)
        entityData.entityIndex = transforms->getIndex(entityData.entity);
        auto position = positions.find(entityData.entity);
        if (position != positions.end()) {
            entityData.transformOrder = sharedOrder;
            entityData.transformOrderPos = position->second;
        }
    }
}

void editor::DeleteEntityCmd::destroyEntity(EntityRegistry* registry, Entity entity, std::vector<Entity>& entities, Project* project, uint32_t sceneId){
    destroyEntityWithoutReconcile(registry, entity);
    ProjectUtils::reconcileTrackedEntities(registry, entities, project, sceneId);
    // the holes it left closed before the step ends (ComponentArray)
    registry->compactComponents();
}

void editor::DeleteEntityCmd::destroyEntities(EntityRegistry* registry,
        const std::vector<Entity>& entitiesToDestroy, std::vector<Entity>& entities,
        Project* project, uint32_t sceneId, bool reconcile) {
    if (entitiesToDestroy.empty()) {
        return;
    }
    for (Entity entity : entitiesToDestroy) {
        destroyEntityWithoutReconcile(registry, entity);
    }
    if (reconcile) {
        ProjectUtils::reconcileTrackedEntities(registry, entities, project, sceneId);
    }
    registry->compactComponents();
}

bool editor::DeleteEntityCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);

    if (deletedNames.empty() && sceneProject && sceneProject->scene) {
        // one per requested GameObject, in that order
        for (Entity requested : requestedEntities) {
            deletedNames.push_back(CommandNames::entity(project, sceneId, requested));
        }
    }

    // Prefab Mode: a prefab has its root (Unity refuses to delete it there)
    for (Entity requested : requestedEntities) {
        if (project->isPrefabStageRoot(sceneId, requested)) {
            Out::warning("The root GameObject of a prefab cannot be deleted in Prefab Mode");
            return false;
        }
    }

    std::vector<Entity> entitiesToDelete;
    std::unordered_set<Entity> entitiesToDeleteSet;
    const std::unordered_set<Entity> requestedSet(requestedEntities.begin(), requestedEntities.end());
    auto isRequestedEntity = [&](Entity entity) {
        return requestedSet.count(entity) != 0;
    };

    // indexed once: a scan of the scene's models for each entity made deleting many quadratic
    const editor::LockedParentIndex lockedParents(sceneProject->scene);
    auto it = entities.begin();
    while (it != entities.end()) {
        Entity lockedParent = lockedParents.get(it->entity);
        bool canDelete = true;
        if (lockedParent != NULL_ENTITY){
            canDelete = (allowLockedRoots && isRequestedEntity(it->entity))
                || isRequestedEntity(lockedParent)
                || entitiesToDeleteSet.count(lockedParent);
        }

        // Imported parts and the groups holding them only go away with their own model
        if (canDelete && !allowLockedRoots){
            Entity modelOwner = ProjectUtils::getModelBranchOwner(sceneProject->scene, it->entity);
            canDelete = modelOwner == NULL_ENTITY
                || isRequestedEntity(modelOwner)
                || entitiesToDeleteSet.count(modelOwner);
        }

        if (!canDelete){
             Out::warning("Cannot delete entity '%u'. It is a locked child of another component.", it->entity);
             it = entities.erase(it);
        } else {
             entitiesToDelete.push_back(it->entity);
             entitiesToDeleteSet.insert(it->entity);
             ++it;
        }
    }

    if (entities.empty()){
        return false;
    }

    lastSelected = project->getSelectedEntities(sceneId);
    trackedBeforeDelete = sceneProject->entities;
    std::vector<size_t> deleteOrder = prepareDeleteSet();
    snapshotEntities();
    recordTransformOrder();

    Scene* scene = sceneProject->scene;
    // Only a deleted link source (a camera, a fluid, a Video Player) changes what texture links
    // resolve to
    std::vector<Entity> linkSources;
    auto cameraArray = scene->getComponentArray<CameraComponent>();
    for (size_t i = 0; i < cameraArray->size(); i++) {
        linkSources.push_back(cameraArray->getEntity(i));
    }
    if (scene->isComponentRegistered<FluidSimulationComponent>()) {
        auto fluidArray = scene->getComponentArray<FluidSimulationComponent>();
        for (size_t i = 0; i < fluidArray->size(); i++) {
            linkSources.push_back(fluidArray->getEntity(i));
        }
    }
    if (scene->isComponentRegistered<VideoPlayerComponent>()) {
        auto videoArray = scene->getComponentArray<VideoPlayerComponent>();
        for (size_t i = 0; i < videoArray->size(); i++) {
            linkSources.push_back(videoArray->getEntity(i));
        }
    }

    // The tracked list is reconciled once after a run of destroyed GameObjects and instances,
    // and always before removing a prefab member, whose bookkeeping reads it
    bool reconcilePending = false;
    auto reconcile = [&]() {
        if (reconcilePending) {
            ProjectUtils::reconcileTrackedEntities(scene, sceneProject->entities, project, sceneId);
            reconcilePending = false;
        }
    };

    for (size_t index : deleteOrder){
        DeleteEntityData& entityData = entities[index];
        // A previously processed structural root (notably a complete prefab instance) may
        // already have removed this member; that root's snapshot is sufficient for undo.
        if (!sceneProject->scene->isEntityCreated(entityData.entity)) {
            continue;
        }

        Prefab* prefab = project->getPrefab(entityData.prefabPath);

        if (prefab) {
            if (entityData.isPrefabRoot) {
                const auto* inst = prefab->getInstance(sceneId, entityData.entity);
                std::vector<Entity> memberEntities;
                if (inst) {
                    for (const auto& member : inst->members) {
                        memberEntities.push_back(member.localEntity);
                    }
                }
                project->unimportPrefab(sceneId, entityData.prefabPath, entityData.entity, memberEntities, false);
                reconcilePending = true;
            } else if (prefabMemberDelete == PrefabMemberDelete::RemoveFromInstance) {
                // Unity's removed GameObject: this instance only, the Prefab keeps it
                reconcile();
                entityData.removedFromInstance = project->removePrefabBranch(sceneId, entityData.entity, true);
            } else {
                reconcile();
                if (!entityData.hasTransform) {
                    entityData.parent = prefab->getRootEntity(sceneId, entityData.entity);
                }
                entityData.recoveryPrefabData = project->removeEntityFromPrefab(sceneId, entityData.entity, true);
            }
        } else {
            std::vector<Entity> allEntities;
            ProjectUtils::collectEntities(entityData.data, allEntities);

            // A prefab nested below a normal hierarchy root must go through the prefab
            // bookkeeping path before its encoded root is destroyed with the hierarchy.
            for (auto it = allEntities.rbegin(); it != allEntities.rend(); ++it) {
                Entity nestedRoot = *it;
                const PrefabComponent* component = sceneProject->scene->findComponent<PrefabComponent>(nestedRoot);
                if (!component || component->path.empty()) {
                    continue;
                }
                fs::path nestedPath = component->path;
                Prefab* nestedPrefab = project->getPrefab(nestedPath);
                const Prefab::Instance* instance = nestedPrefab ? nestedPrefab->getInstance(sceneId, nestedRoot) : nullptr;
                if (!instance || instance->rootEntity != nestedRoot) {
                    continue;
                }
                std::vector<Entity> members;
                members.reserve(instance->members.size());
                for (const Prefab::EntityMember& member : instance->members) {
                    members.push_back(member.localEntity);
                }
                project->unimportPrefab(sceneId, nestedPath, nestedRoot, members, false);
                reconcilePending = true;
            }

            for (Entity entity : allEntities) {
                destroyEntityWithoutReconcile(scene, entity);
                reconcilePending = true;
            }
        }

        sceneProject->isModified = true;
    }
    reconcile();

    // unbind textures linked to deleted render-to-texture cameras and fluids
    if (std::any_of(linkSources.begin(), linkSources.end(), [scene](Entity source) { return !scene->isEntityCreated(source); })) {
        CameraTextureLink::resolve(scene);
    }
    survivingEntities = sceneProject->entities;
    // a delete leaves no holes behind it (ComponentArray): the next step finds packed arrays
    scene->compactComponents();

    return true;
}

void editor::DeleteEntityCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (snapshotsShared) {
        // Decoding is not only reads: it runs once the clipboard's worker is done with them
        EntityClipboard::finishPendingWrite();
    }
    std::unordered_map<Entity, Entity> entityRemap;
    std::vector<Entity> restoredEntities;

    // A GameObject put back at its Transform index leaves the tracked list to be sorted
    // once, before the steps that read its order and at the end, instead of per GameObject
    bool sortPending = false;
    auto sortTracked = [&]() {
        if (sortPending) {
            ProjectUtils::sortEntitiesByTransformOrder(sceneProject->scene, sceneProject->entities);
            sortPending = false;
        }
    };

    for (DeleteEntityData& entityData : entities){
        Entity restoredParent = entityData.parent;
        if (auto parentRemap = entityRemap.find(restoredParent); parentRemap != entityRemap.end()) {
            restoredParent = parentRemap->second;
        }

        if (entityData.removedFromInstance.valid()) {
            sortTracked();
            if (!project->restorePrefabBranch(sceneId, entityData.removedFromInstance, &entityRemap, &restoredEntities)) {
                Out::error("Could not restore removed prefab GameObject %u in scene %u", entityData.entity, sceneId);
            }
        } else if (entityData.recoveryPrefabData.size() == 0){
            if (!entityData.data.IsMap() || !entityData.data["type"]) {
                continue;
            }

            std::vector<Entity> allEntities = Stream::decodeEntity(entityData.data, sceneProject->scene,
                &sceneProject->entities, project, sceneProject, NULL_ENTITY, true, false, &entityRemap);
            if (allEntities.empty()) {
                continue;
            }
            restoredEntities.insert(restoredEntities.end(), allEntities.begin(), allEntities.end());
            entityData.entity = allEntities[0];

            if (entityData.hasTransform) {
                // Under its parent (last of its children for now), then back after the GameObject
                // it followed. Not by its old Transform index: what was before it in the array
                // may not be there now (a deleted camera's gizmo entities, a GameObject of this
                // delete put back after it), and that index can be past the end of the array.
                Scene* scene = sceneProject->scene;
                auto transforms = scene->getComponentArray<Transform>();
                if (transforms->hasEntity(entityData.entity)) {
                    scene->addEntityChild(restoredParent, entityData.entity, true);
                    size_t position = findRestoredTransformPosition(scene, entityData, entityRemap);
                    if (position == NULL_POSITION && !entityData.transformOrder) {
                        // Nothing recorded to go by (it was not tracked): its old index, only
                        // ever earlier than where it is and never before its parent
                        const size_t current = transforms->getIndex(entityData.entity);
                        position = std::min(entityData.entityIndex, current);
                        const Transform& transform = scene->getComponent<Transform>(entityData.entity);
                        if (transform.parent != NULL_ENTITY && transforms->hasEntity(transform.parent)) {
                            position = std::max(position, transforms->getIndex(transform.parent) + 1);
                        }
                    }
                    if (position != NULL_POSITION && position <= transforms->size()) {
                        scene->moveChildToIndex(entityData.entity, position);
                    }
                }
                // The saved values are local to that parent: the move kept a world placement
                // that, for a prefab instance root, is still the prefab's (its world matrix is
                // the prefab root's until the next update), so they are put back
                const YAML::Node savedTransform = entityData.data["components"] ? entityData.data["components"]["transform"] : YAML::Node();
                Transform* transform = sceneProject->scene->findComponent<Transform>(entityData.entity);
                if (transform && savedTransform && savedTransform.IsMap()) {
                    const Transform saved = Stream::decodeTransform(savedTransform, transform);
                    transform->position = saved.position;
                    transform->rotation = saved.rotation;
                    transform->scale = saved.scale;
                    transform->needUpdate = true;
                }
                sortPending = true;
            } else {
                // Its index is a position in the sorted list
                sortTracked();
                ProjectUtils::moveEntityOrderByIndex(sceneProject->scene, sceneProject->entities,
                    entityData.entity, restoredParent, entityData.entityIndex, false);
            }

        }else{

            sortTracked();
            if (!project->addEntityToPrefab(sceneId, entityData.recoveryPrefabData, restoredParent,
                true, &entityRemap, &restoredEntities)) {
                Out::error("Could not restore deleted prefab entity %u in scene %u; undo stopped before remapping",
                    entityData.entity, sceneId);
                CameraTextureLink::resolve(sceneProject->scene);
                return;
            }
        }
    }

    sortTracked();
    // Prefab imports allocate scene-local member IDs outside Stream's YAML decoder. Snapshots
    // are stored outer-first, so each nested root is mapped before its own instance is visited.
    for (const PrefabInstanceSnapshot& saved : prefabInstanceSnapshots) {
        Entity root = saved.rootEntity;
        auto rootRemap = entityRemap.find(root);
        if (rootRemap != entityRemap.end()) {
            root = rootRemap->second;
        }

        const Prefab* prefab = project->getPrefab(saved.path);
        const Prefab::Instance* instance = prefab ? prefab->getInstance(sceneId, root) : nullptr;
        if (!instance || instance->rootEntity != root) {
            continue;
        }

        for (const Prefab::EntityMember& oldMember : saved.members) {
            auto newMember = std::find_if(instance->members.begin(), instance->members.end(),
                [&](const Prefab::EntityMember& member) {
                    return member.registryEntity == oldMember.registryEntity;
                });
            if (newMember != instance->members.end() && newMember->localEntity != oldMember.localEntity) {
                entityRemap[oldMember.localEntity] = newMember->localEntity;
            }
        }
    }

    // recreateEntity normally reclaims the original IDs. If an ID was occupied, decodeEntity
    // allocated a replacement; repair links across the separately decoded snapshots without
    // clearing references to entities that were never part of this delete command.
    if (!entityRemap.empty()) {
        std::vector<Entity> referenceEntities;
        std::unordered_set<Entity> seenReferences;
        auto addReferenceEntity = [&](Entity entity) {
            if (sceneProject->scene->isEntityCreated(entity) && seenReferences.insert(entity).second) {
                referenceEntities.push_back(entity);
            }
        };
        for (Entity entity : restoredEntities) {
            addReferenceEntity(entity);
        }
        for (Entity entity : survivingEntities) {
            addReferenceEntity(entity);
        }
        Project::remapEntityProperties(sceneProject->scene, referenceEntities, entityRemap, false);

        auto remapEntity = [&](Entity& entity) {
            auto it = entityRemap.find(entity);
            if (it != entityRemap.end()) {
                entity = it->second;
            }
        };
        for (DeleteEntityData& entityData : entities) {
            remapEntity(entityData.parent);
            if (!entityData.recoveryPrefabData.empty() || entityData.removedFromInstance.valid()) {
                remapEntity(entityData.entity);
            }
        }
        for (Entity& entity : requestedEntities) remapEntity(entity);
        for (Entity& entity : preservedEntities) remapEntity(entity);
        for (Entity& entity : lastSelected) remapEntity(entity);
        for (Entity& entity : trackedBeforeDelete) remapEntity(entity);
    }

    // The tracked list back as it was: placing each GameObject back by index keeps the
    // Transform order, but moves the Transform-less ones (sorted first, or left where the
    // last one was put). Whatever is tracked now and was not then keeps its current order.
    if (!trackedBeforeDelete.empty()) {
        std::vector<Entity>& tracked = sceneProject->entities;
        std::unordered_set<Entity> current(tracked.begin(), tracked.end());
        std::vector<Entity> restored;
        restored.reserve(tracked.size());
        for (Entity entity : trackedBeforeDelete) {
            if (current.erase(entity)) {
                restored.push_back(entity);
            }
        }
        for (Entity entity : tracked) {
            if (current.count(entity)) {
                restored.push_back(entity);
            }
        }
        tracked.swap(restored);
    }

    if (lastSelected.size() > 0){
        project->replaceSelectedEntities(sceneId, lastSelected);
    }

    // recreated cameras get a new framebuffer, recreated textures are bound to theirs
    CameraTextureLink::resolve(sceneProject->scene, restoredEntities);
    // (a restore can replace entities, e.g. a prefab instance's)
    sceneProject->scene->compactComponents();

    sceneProject->isModified = wasModified;
}

bool editor::DeleteEntityCmd::mergeWith(editor::Command* otherCommand){
    DeleteEntityCmd* otherCmd = dynamic_cast<DeleteEntityCmd*>(otherCommand);
    if (otherCmd != nullptr){
        if (project == otherCmd->project && sceneId == otherCmd->sceneId && allowLockedRoots == otherCmd->allowLockedRoots &&
            prefabMemberDelete == otherCmd->prefabMemberDelete){

            lastSelected = otherCmd->lastSelected;
            // Undone together, back to before the older one
            trackedBeforeDelete = otherCmd->trackedBeforeDelete;

            for (DeleteEntityData& otherEntityData :  otherCmd->entities){
                entities.push_back(otherEntityData);
            }
            for (Entity entity : otherCmd->preservedEntities) {
                if (std::find(preservedEntities.begin(), preservedEntities.end(), entity) == preservedEntities.end()) {
                    preservedEntities.push_back(entity);
                }
            }
            for (Entity entity : otherCmd->requestedEntities) {
                if (std::find(requestedEntities.begin(), requestedEntities.end(), entity) == requestedEntities.end()) {
                    requestedEntities.push_back(entity);
                }
            }
            // in the order requestedEntities got them
            for (size_t i = 0; i < otherCmd->requestedEntities.size() && i < otherCmd->deletedNames.size(); i++) {
                if (deletedNames.size() < requestedEntities.size()) {
                    deletedNames.push_back(otherCmd->deletedNames[i]);
                }
            }
            for (const PrefabInstanceSnapshot& instance : otherCmd->prefabInstanceSnapshots) {
                if (std::find_if(prefabInstanceSnapshots.begin(), prefabInstanceSnapshots.end(), [&](const PrefabInstanceSnapshot& saved) {
                        return saved.path == instance.path && saved.rootEntity == instance.rootEntity;
                    }) == prefabInstanceSnapshots.end()) {
                    prefabInstanceSnapshots.push_back(instance);
                }
            }

            wasModified = wasModified && otherCmd->wasModified;

            return true;
        }
    }

    return false;
}

size_t editor::DeleteEntityCmd::memoryBytes() const{
    size_t bytes = sizeof(*this) + entities.capacity() * sizeof(DeleteEntityData) +
        (requestedEntities.capacity() + preservedEntities.capacity() + lastSelected.capacity() +
         survivingEntities.capacity() + trackedBeforeDelete.capacity()) * sizeof(Entity);
    for (const DeleteEntityData& entityData : entities) {
        bytes += yamlFootprint(entityData.data) + CommandMemory::of(entityData.recoveryPrefabData);
        if (!entityData.removedFromInstance.node.IsNull() && entityData.removedFromInstance.node.IsDefined()) {
            bytes += CommandMemory::of(entityData.removedFromInstance);
        }
    }
    for (const auto& [entity, node] : preEncoded) {
        bytes += yamlFootprint(node);
    }
    for (const PrefabInstanceSnapshot& instance : prefabInstanceSnapshots) {
        bytes += sizeof(instance) + instance.members.size() * sizeof(Prefab::EntityMember);
    }
    return bytes;
}
