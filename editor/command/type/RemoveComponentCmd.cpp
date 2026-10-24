// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RemoveComponentCmd.h"

#include "Out.h"
#include "command/type/DeleteEntityCmd.h"
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

bool editor::RemoveComponentCmd::execute() {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (sceneProject) {
        Scene* scene = sceneProject->scene;

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

            prefabUndo.capture(project, sceneId, entityData.entity);
            entityData.oldComponent = ProjectUtils::removeEntityComponent(scene, entityData.entity, componentType, sceneProject->entities, true);
            entityData.removed = true;
            project->recordPrefabComponentRemoved(sceneId, entityData.entity, componentType);
        }

        ProjectUtils::reconcileTrackedEntities(scene, sceneProject->entities, project, sceneId);

        if (componentType == ComponentType::CameraComponent){
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
            }
        }
        prefabUndo.restore(project, sceneId);

        // Restore owned entities after their owner components are back. DeleteEntityCmd then
        // also repairs those components if an occupied ID forced a replacement allocation.
        for (auto ownedDelete = ownedEntityDeletes.rbegin();
            ownedDelete != ownedEntityDeletes.rend(); ++ownedDelete) {
            (*ownedDelete)->undo();
        }

        if (componentType == ComponentType::CameraComponent){
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
