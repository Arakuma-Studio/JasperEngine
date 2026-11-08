// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MeshChangeCmd.h"
#include "component/MeshComponent.h"

#include "Stream.h"
#include "util/ProjectUtils.h"

using namespace jasperengine;

editor::MeshChangeCmd::MeshChangeCmd(Project* project, uint32_t sceneId, Entity entity, MeshComponent mesh){
    this->project = project;
    this->sceneId = sceneId;
    this->entity = entity;
    this->newMesh = Stream::encodeMeshComponent(mesh);

    this->wasModified = project->getScene(sceneId)->isModified;
}

bool editor::MeshChangeCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);

    oldMesh = Stream::encodeMeshComponent(sceneProject->scene->getComponent<MeshComponent>(entity));
    prefabUndo.capture(project, sceneId, entity);
    MeshComponent& mesh = sceneProject->scene->getComponent<MeshComponent>(entity);
    ProjectUtils::releaseMeshResources(sceneProject->scene, entity, mesh);
    mesh = Stream::decodeMeshComponent(newMesh);

    sceneProject->isModified = true;

    // Unity: a prefab instance's mesh changed is an override of it (the whole mesh)
    project->recordPrefabEdit(sceneId, entity, ComponentType::MeshComponent, {});

    return true;
}

void editor::MeshChangeCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);

    MeshComponent& mesh = sceneProject->scene->getComponent<MeshComponent>(entity);
    ProjectUtils::releaseMeshResources(sceneProject->scene, entity, mesh);
    mesh = Stream::decodeMeshComponent(oldMesh);

    sceneProject->isModified = wasModified;

    prefabUndo.restore(project, sceneId);
}

bool editor::MeshChangeCmd::mergeWith(editor::Command* otherCommand){
    MeshChangeCmd* otherCmd = dynamic_cast<MeshChangeCmd*>(otherCommand);
    if (otherCmd != nullptr){
        if (sceneId == otherCmd->sceneId && entity == otherCmd->entity){
            this->oldMesh = otherCmd->oldMesh;
            prefabUndo.mergeOlder(otherCmd->prefabUndo);

            this->wasModified = this->wasModified && otherCmd->wasModified;
            return true;
        }
    }
    return false;
}