// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AddComponentCmd.h"

#include <algorithm>

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

        // a character controller and a rigidbody of its dimension never share a GameObject
        for (Entity entity : entities){
            const std::string conflict = Catalog::getComponentConflict(scene, entity, componentType);
            if (!conflict.empty()){
                Out::error("%s", conflict.c_str());
                return false;
            }
        }

        required.clear();
        for (Entity& entity : entities){
            prefabUndo.capture(project, sceneId, entity);
            // a spline consumer reads its GameObject's Spline; an extrude draws with its Mesh
            if (componentType == ComponentType::SplineExtrudeComponent || componentType == ComponentType::SplineInstantiateComponent){
                std::vector<ComponentType> needs = {ComponentType::SplineComponent};
                if (componentType == ComponentType::SplineExtrudeComponent) needs.push_back(ComponentType::MeshComponent);
                const std::vector<ComponentType> has = Catalog::findComponents(scene, entity);
                for (ComponentType need : needs){
                    if (std::find(has.begin(), has.end(), need) == has.end()){
                        ProjectUtils::addEntityComponent(scene, entity, need, sceneProject->entities);
                        project->recordPrefabComponentAdded(sceneId, entity, need);
                        required.emplace_back(entity, need);
                    }
                }
            }
            ProjectUtils::addEntityComponent(scene, entity, componentType, sceneProject->entities);
            // a 2D scene's splines lie in the XY plane (S18 R13)
            if (componentType == ComponentType::SplineComponent && sceneProject->sceneType == SceneType::SCENE_2D){
                scene->getComponent<SplineComponent>(entity).spline.setUpAxis(Vector3(0.0f, 0.0f, 1.0f));
            }
            for (const auto& [requiredEntity, requiredType] : required){
                if (requiredEntity == entity && requiredType == ComponentType::SplineComponent && sceneProject->sceneType == SceneType::SCENE_2D){
                    scene->getComponent<SplineComponent>(entity).spline.setUpAxis(Vector3(0.0f, 0.0f, 1.0f));
                }
            }

            if (componentType == ComponentType::InstancedMeshComponent &&
                ProjectUtils::hasModelMeshChildrenWithoutRootGeometry(scene, entity)){
                Out::warning(
                    "Instanced Mesh on '%s' will not render: this model stores its geometry in child mesh entities. "
                    "Right-click the model in the Hierarchy and choose 'Merge static model'.",
                    scene->getEntityName(entity).c_str());
            }

            project->recordPrefabComponentAdded(sceneId, entity, componentType);
        }

        // textures that name this fluid ("fluid|<entity>") show its output now
        if (componentType == ComponentType::FluidSimulationComponent){
            CameraTextureLink::resolve(scene);
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
        for (auto it = required.rbegin(); it != required.rend(); ++it){
            ProjectUtils::removeEntityComponent(scene, it->first, it->second, sceneProject->entities);
        }
        prefabUndo.restore(project, sceneId);

        ProjectUtils::reconcileTrackedEntities(scene, sceneProject->entities, project, sceneId);

        if (componentType == ComponentType::CameraComponent || componentType == ComponentType::FluidSimulationComponent){
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
