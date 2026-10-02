// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RemoveEntityFromPrefabCmd.h"

using namespace jasperengine;

editor::RemoveEntityFromPrefabCmd::RemoveEntityFromPrefabCmd(Project* project, uint32_t sceneId, Entity entity, Entity parent){
    this->project = project;
    this->sceneId = sceneId;
    this->entity = entity;
    this->parent = parent;

    this->wasModified = project->getScene(sceneId)->isModified;
}

bool editor::RemoveEntityFromPrefabCmd::execute(){
    recovery = project->removeEntityFromPrefab(sceneId, entity, false);

    bool result = !recovery.empty();
    if (result){
        project->getScene(sceneId)->isModified = true;
    }

    return result;
}

void editor::RemoveEntityFromPrefabCmd::undo(){
    project->addEntityToPrefab(sceneId, recovery, parent, false);

    project->getScene(sceneId)->isModified = wasModified;
}
