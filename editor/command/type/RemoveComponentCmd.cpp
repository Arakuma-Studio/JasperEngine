// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RemoveComponentCmd.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/Transform.h"

#include "Out.h"
#include "command/type/DeleteEntityCmd.h"
#include "pose/PoseEditSession.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectUtils.h"
#include "util/YamlFootprint.h"
#include "command/PlayChangeReview.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

using namespace jasperengine;

namespace {

void appendUnique(std::vector<Entity>& entities, Entity entity) {
    if (std::find(entities.begin(), entities.end(), entity) == entities.end()) {
        entities.push_back(entity);
    }
}

bool hasOwnedComponent(Scene* scene, Entity entity, editor::ComponentType componentType) {
    if (!scene || !scene->isEntityCreated(entity)) {
        return false;
    }
    if (componentType == editor::ComponentType::ActionComponent) {
        return scene->findComponent<ActionComponent>(entity) != nullptr;
    }
    if (componentType == editor::ComponentType::AnimationComponent) {
        return scene->findComponent<AnimationComponent>(entity) != nullptr;
    }
    return false;
}

}

editor::RemoveComponentCmd::RemoveComponentCmd(Project* project, size_t sceneId, Entity entity, ComponentType componentType){
    this->project = project;
    this->sceneId = sceneId;
    this->componentType = componentType;

    RemoveComponentData entityData;
    entityData.entity = entity;

    this->entities.push_back(entityData);

    this->wasModified = project->getScene(sceneId)->isModified;
}

editor::RemoveComponentCmd::~RemoveComponentCmd() = default;

void editor::RemoveComponentCmd::recordTransformPlace(Scene* scene, RemoveComponentData& entityData) {
    // the parent read before the index queries, which settle the array
    entityData.transformParent = scene->getComponent<Transform>(entityData.entity).parent;
    auto transforms = scene->getComponentArray<Transform>();
    entityData.transformIndex = transforms->getIndex(entityData.entity);
    entityData.transformChildren.clear();
    const size_t last = scene->findBranchLastIndex(entityData.entity);
    for (size_t i = entityData.transformIndex + 1; i <= last; i++) {
        if (transforms->getComponentFromIndex(i).parent == entityData.entity) {
            entityData.transformChildren.push_back(transforms->getEntity(i));
        }
    }
}

void editor::RemoveComponentCmd::restoreTransformPlace(Scene* scene, const RemoveComponentData& entityData) {
    // Put back as the removal left the rest: below its parent, before the children it had
    // (they went to its parent, keeping their local values, and their places)
    const Entity parent = entityData.transformParent;
    if (parent != NULL_ENTITY && scene->isEntityCreated(parent) && scene->findComponent<Transform>(parent)) {
        scene->addEntityChild(parent, entityData.entity, false);
    }
    scene->moveChildToIndex(entityData.entity, entityData.transformIndex);
    for (Entity child : entityData.transformChildren) {
        Transform* transform = scene->isEntityCreated(child) ? scene->findComponent<Transform>(child) : nullptr;
        if (transform && transform->parent == parent) {
            transform->parent = entityData.entity;
            transform->needUpdate = true;
        }
    }
    if (!entityData.transformChildren.empty()) {
        scene->hierarchyChanged();
    }
}

bool editor::RemoveComponentCmd::execute() {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (sceneProject) {
        Scene* scene = sceneProject->scene;

        // the removed component keeps its rest values for undo (blend shape weights, lattice
        // offsets), not a preview's: those previews restore first (clips 9.R7)
        if (componentType == ComponentType::MeshComponent || componentType == ComponentType::LatticeComponent) {
            std::vector<Entity> removedFrom;
            for (const RemoveComponentData& entityData : entities) removedFrom.push_back(entityData.entity);
            PoseEditSession::suspendActiveFor((uint32_t)sceneId, removedFrom);
        }

        if (!ownedEntitiesCaptured) {
            ownedEntitiesCaptured = true;

            // What the removed component owns goes with it, from this scene only: on a prefab
            // instance that is an instance edit too, so the owned prefab GameObjects become
            // removed GameObject overrides (DeleteEntityCmd's default)
            std::vector<Entity> owners;
            for (const RemoveComponentData& entityData : entities) {
                if (hasOwnedComponent(scene, entityData.entity, componentType)) {
                    appendUnique(owners, entityData.entity);
                }
            }
            std::vector<Entity> targets;
            for (Entity owner : owners) {
                for (Entity target : ProjectUtils::getOwnedCascadeEntities(scene, owner, componentType)) {
                    if (scene->isEntityCreated(target) && std::find(owners.begin(), owners.end(), target) == owners.end()) {
                        appendUnique(targets, target);
                    }
                }
            }
            if (!targets.empty()) {
                ownedEntityDeletes.emplace_back(new DeleteEntityCmd(project, sceneId, targets, true, owners));
            }
        }

        for (const std::unique_ptr<DeleteEntityCmd>& ownedDelete : ownedEntityDeletes) {
            ownedDelete->execute();
        }

        if (componentType == ComponentType::Transform) {
            trackedBefore = sceneProject->entities;
        }
        for (RemoveComponentData& entityData : entities){
            entityData.removed = false;
            if (!scene->isEntityCreated(entityData.entity)) {
                continue;
            }
            PrefabObjectRef ref;
            if (componentType == ComponentType::Transform && project->findPrefabObject(sceneId, entityData.entity, ref)){
                Out::error("Cannot remove Transform component from prefab entity '%s' in scene '%s'", scene->getEntityName(entityData.entity).c_str(), sceneProject->name.c_str());
                continue;
            }
            if (componentType == ComponentType::Transform && scene->findComponent<Transform>(entityData.entity)) {
                recordTransformPlace(scene, entityData);
            }

            prefabUndo.capture(project, sceneId, entityData.entity);
            entityData.oldComponent = ProjectUtils::removeEntityComponent(scene, entityData.entity, componentType, sceneProject->entities, true);
            entityData.removed = true;
            project->recordPrefabComponentRemoved(sceneId, entityData.entity, componentType);
        }

        ProjectUtils::reconcileTrackedEntities(scene, sceneProject->entities, project, sceneId);

        // textures showing this camera's (or fluid's) output follow it
        if (CameraTextureLink::isLinkSourceType(componentType)){
            CameraTextureLink::resolve(sceneProject->scene);
        }

        sceneProject->isModified = true;
    }

    return true;
}

void editor::RemoveComponentCmd::undo() {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (sceneProject) {
        Scene* scene = sceneProject->scene;

        for (RemoveComponentData& entityData : entities){
            if (entityData.removed && scene->isEntityCreated(entityData.entity)){
                ProjectUtils::addEntityComponent(scene, entityData.entity, componentType, sceneProject->entities, entityData.oldComponent);
                if (componentType == ComponentType::Transform) {
                    restoreTransformPlace(scene, entityData);
                }
            }
        }
        if (componentType == ComponentType::Transform) {
            ProjectUtils::sortEntitiesByTransformOrder(scene, sceneProject->entities);
            ProjectUtils::keepUntransformedPlaces(scene, trackedBefore, sceneProject->entities);
        }
        prefabUndo.restore(project, sceneId);

        // Restore owned entities after their owner components are back. DeleteEntityCmd then
        // also repairs those components if an occupied ID forced a replacement allocation.
        for (auto ownedDelete = ownedEntityDeletes.rbegin();
            ownedDelete != ownedEntityDeletes.rend(); ++ownedDelete) {
            (*ownedDelete)->undo();
        }

        // textures showing this camera's (or fluid's) output follow it
        if (CameraTextureLink::isLinkSourceType(componentType)){
            CameraTextureLink::resolve(sceneProject->scene);
        }

        sceneProject->isModified = wasModified;
    }
}

bool editor::RemoveComponentCmd::mergeWith(Command* otherCommand){
    RemoveComponentCmd* otherCmd = dynamic_cast<RemoveComponentCmd*>(otherCommand);
    if (otherCmd != nullptr){
        if (project == otherCmd->project && sceneId == otherCmd->sceneId &&
            componentType == otherCmd->componentType){

            std::vector<std::unique_ptr<DeleteEntityCmd>> mergedDeletes;
            mergedDeletes.reserve(otherCmd->ownedEntityDeletes.size() + ownedEntityDeletes.size());
            for (std::unique_ptr<DeleteEntityCmd>& ownedDelete : otherCmd->ownedEntityDeletes) {
                mergedDeletes.push_back(std::move(ownedDelete));
            }
            for (std::unique_ptr<DeleteEntityCmd>& ownedDelete : ownedEntityDeletes) {
                mergedDeletes.push_back(std::move(ownedDelete));
            }
            ownedEntityDeletes = std::move(mergedDeletes);
            ownedEntitiesCaptured = ownedEntitiesCaptured && otherCmd->ownedEntitiesCaptured;

            for (RemoveComponentData& otherEntityData :  otherCmd->entities){
                entities.push_back(otherEntityData);
            }
            prefabUndo.mergeOlder(otherCmd->prefabUndo);
            // the older step's list is the one from before both
            trackedBefore = otherCmd->trackedBefore;

            wasModified = wasModified && otherCmd->wasModified;

            return true;
        }
    }

    return false;
}

std::string editor::RemoveComponentCmd::describe() const{
    return "Remove Component " + CommandNames::component(componentType) + " (" +
        CommandNames::entities(project, (uint32_t)sceneId, getEntities()) + ")";
}

size_t editor::RemoveComponentCmd::memoryBytes() const{
    size_t bytes = sizeof(*this) + entities.capacity() * sizeof(RemoveComponentData);
    for (const RemoveComponentData& entityData : entities){
        bytes += yamlFootprint(entityData.oldComponent);
    }
    for (const std::unique_ptr<DeleteEntityCmd>& ownedDelete : ownedEntityDeletes){
        bytes += ownedDelete->memoryBytes();
    }
    return bytes;
}

std::vector<Entity> editor::RemoveComponentCmd::getEntities() const{
    std::vector<Entity> result;
    result.reserve(entities.size());
    for (const RemoveComponentData& entityData : entities){
        result.push_back(entityData.entity);
    }
    return result;
}

void editor::RemoveComponentCmd::collectKeepCommands(KeepContext& context) const{
    for (const RemoveComponentData& entityData : entities){
        if (entityData.removed){
            context.addRemovedComponent(entityData.entity, componentType);
        }
    }
}
