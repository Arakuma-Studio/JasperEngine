// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EntityNameCmd.h"

using namespace jasperengine;

editor::EntityNameCmd::EntityNameCmd(Project* project, uint32_t sceneId, Entity entity, std::string name){
    this->project = project;
    this->sceneId = sceneId;
    this->entity = entity;
    this->newName = name;
}

bool editor::EntityNameCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    // A Play-created entity is gone after Stop, and naming it would recreate the id
    if (!sceneProject || !sceneProject->scene->isEntityCreated(entity)){
        return false;
    }

    oldName = sceneProject->scene->getEntityName(entity);
    wasModified = project->getScene(sceneId)->isModified;

    prefabUndo.capture(project, sceneId, entity);
    sceneProject->scene->setEntityName(entity, newName);
    // Unity: a prefab instance's GameObject renamed is a name override (an instance root's
    // name is its own anyway)
    if (oldName != newName){
        project->recordPrefabStateEdit(sceneId, entity, PrefabObjectOverrides::StateName);
    }

    sceneProject->isModified = true;

    return true;
}

void editor::EntityNameCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene->isEntityCreated(entity)){
        return;
    }

    sceneProject->scene->setEntityName(entity, oldName);
    prefabUndo.restore(project, sceneId);

    sceneProject->isModified = wasModified;
}

bool editor::EntityNameCmd::mergeWith(editor::Command* otherCommand){
    EntityNameCmd* otherCmd = dynamic_cast<EntityNameCmd*>(otherCommand);
    if (otherCmd != nullptr){
        if (sceneId == otherCmd->sceneId && entity == otherCmd->entity){
            this->oldName = otherCmd->oldName;
            prefabUndo.mergeOlder(otherCmd->prefabUndo);

            this->wasModified = this->wasModified && otherCmd->wasModified;
            return true;
        }
    }
    return false;
}