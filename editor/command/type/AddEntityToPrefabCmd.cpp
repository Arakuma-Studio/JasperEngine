// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AddEntityToPrefabCmd.h"
#include "util/ProjectUtils.h"

using namespace jasperengine;

editor::AddEntityToPrefabCmd::AddEntityToPrefabCmd(Project* project, uint32_t sceneId, Entity entity, Entity parent){
    this->project = project;
    this->sceneId = sceneId;
    this->entity = entity;
    this->parent = parent;
    this->wasModified = project->getScene(sceneId)->isModified;

    this->originalParent = NULL_ENTITY;
    this->originalTransformIndex = 0;
    this->hasTransform = false;

    SceneProject* sceneProject = project->getScene(sceneId);
    if (sceneProject) {
        Transform* transform = sceneProject->scene->findComponent<Transform>(entity);
        if (transform) {
            this->hasTransform = true;
            this->originalParent = transform->parent;
            this->originalTransformIndex = ProjectUtils::getTransformIndex(sceneProject->scene, entity);
        }
    }
}

bool editor::AddEntityToPrefabCmd::execute(){
    bool result = project->addEntityToPrefab(sceneId, entity, parent, false);

    if (result){
        project->getScene(sceneId)->isModified = true;
    }

    return result;
}

void editor::AddEntityToPrefabCmd::undo(){
    project->removeEntityFromPrefab(sceneId, entity, false);

    if (hasTransform) {
        SceneProject* sceneProject = project->getScene(sceneId);
        if (sceneProject) {
            ProjectUtils::moveEntityOrderByTransform(sceneProject->scene, sceneProject->entities, entity, originalParent, originalTransformIndex, true);
        }
    }

    project->getScene(sceneId)->isModified = wasModified;
}

bool editor::AddEntityToPrefabCmd::mergeWith(editor::Command* otherCommand){

    return false;
}
