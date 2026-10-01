// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AddComponentCmd.h"

#include "Out.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectUtils.h"

using namespace jasperengine;

editor::AddComponentCmd::AddComponentCmd(Project* project, size_t sceneId, Entity entity, ComponentType componentType){
    this->project = project;
    this->sceneId = sceneId;
    this->entities.push_back(entity);
    this->componentType = componentType;

    this->wasModified = project->getScene(sceneId)->isModified;
}

bool editor::AddComponentCmd::execute() {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (sceneProject) {
        Scene* scene = sceneProject->scene;

        for (Entity& entity : entities){
            prefabUndo.capture(project, sceneId, entity);
            ProjectUtils::addEntityComponent(scene, entity, componentType, sceneProject->entities);

            if (componentType == ComponentType::InstancedMeshComponent &&
                ProjectUtils::hasModelMeshChildrenWithoutRootGeometry(scene, entity)){
                Out::warning(
                    "Instanced Mesh on '%s' will not render: this model stores its geometry in child mesh entities. "
                    "Right-click the model in the Hierarchy and choose 'Merge static model'.",
                    scene->getEntityName(entity).c_str());
            }

            project->recordPrefabComponentAdded(sceneId, entity, componentType);
        }

        sceneProject->isModified = true;
    }

    return true;
}

void editor::AddComponentCmd::undo() {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (sceneProject) {
        Scene* scene = sceneProject->scene;

        for (Entity& entity : entities){
            // Undoing an add removes only the component introduced by this command. Any
            // ownership configured later belongs to later commands and must not broaden undo.
            ProjectUtils::removeEntityComponent(scene, entity, componentType, sceneProject->entities);
        }
        prefabUndo.restore(project, sceneId);

        ProjectUtils::reconcileTrackedEntities(scene, sceneProject->entities, project, sceneId);

        if (componentType == ComponentType::CameraComponent){
            CameraTextureLink::resolve(scene);
        }

        sceneProject->isModified = wasModified;
    }
}

bool editor::AddComponentCmd::mergeWith(Command* otherCommand){
    AddComponentCmd* otherCmd = dynamic_cast<AddComponentCmd*>(otherCommand);
    if (otherCmd != nullptr){
        if (project == otherCmd->project && sceneId == otherCmd->sceneId && componentType == otherCmd->componentType){

            for (Entity& otherEntity :  otherCmd->entities){
                entities.push_back(otherEntity);
            }
            prefabUndo.mergeOlder(otherCmd->prefabUndo);

            wasModified = wasModified && otherCmd->wasModified;

            return true;
        }
    }

    return false;
}
