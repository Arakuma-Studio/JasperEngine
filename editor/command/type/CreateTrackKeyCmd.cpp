// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CreateTrackKeyCmd.h"

#include "Catalog.h"
#include "component/KeyframeTracksComponent.h"
#include "component/LatticeTracksComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/TranslateTracksComponent.h"

using namespace jasperengine;

editor::CreateTrackKeyCmd::CreateTrackKeyCmd(Project* project, uint32_t sceneId, Entity animEntity, Entity target,
                                             EntityCreationType creationType, std::string name, float localTime,
                                             Vector3 value)
    : project(project), sceneId(sceneId), animEntity(animEntity){
    createCmd = std::make_unique<CreateEntityCmd>(project, sceneId, name, creationType, target);
    createCmd->addProperty<std::vector<float>>(ComponentType::KeyframeTracksComponent, "times", {localTime});
    const ComponentType component = creationType == EntityCreationType::SCALE_TRACKS
        ? ComponentType::ScaleTracksComponent : ComponentType::TranslateTracksComponent;
    createCmd->addProperty<std::vector<Vector3>>(component, "values", {value});
    wasModified = project->getScene(sceneId)->isModified;
}

editor::CreateTrackKeyCmd::CreateTrackKeyCmd(Project* project, uint32_t sceneId, Entity animEntity, Entity target,
                                             EntityCreationType creationType, std::string name, float localTime,
                                             Quaternion value)
    : project(project), sceneId(sceneId), animEntity(animEntity){
    createCmd = std::make_unique<CreateEntityCmd>(project, sceneId, name, creationType, target);
    createCmd->addProperty<std::vector<float>>(ComponentType::KeyframeTracksComponent, "times", {localTime});
    createCmd->addProperty<std::vector<Quaternion>>(ComponentType::RotateTracksComponent, "values", {value});
    wasModified = project->getScene(sceneId)->isModified;
}

editor::CreateTrackKeyCmd::CreateTrackKeyCmd(Project* project, uint32_t sceneId, Entity animEntity, Entity target,
                                             std::string name, float localTime, std::vector<int> points,
                                             std::vector<Vector3> values)
    : project(project), sceneId(sceneId), animEntity(animEntity){
    createCmd = std::make_unique<CreateEntityCmd>(project, sceneId, name, EntityCreationType::LATTICE_TRACKS, target);
    createCmd->addProperty<std::vector<float>>(ComponentType::KeyframeTracksComponent, "times", {localTime});
    createCmd->addProperty<std::vector<int>>(ComponentType::LatticeTracksComponent, "points", points);
    createCmd->addProperty<std::vector<std::vector<Vector3>>>(ComponentType::LatticeTracksComponent, "values", {values});
    wasModified = project->getScene(sceneId)->isModified;
}

bool editor::CreateTrackKeyCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !createCmd){
        return false;
    }
    Scene* scene = sceneProject->scene;
    if (!scene->findComponent<AnimationComponent>(animEntity)){
        return false;
    }

    if (!createCmd->execute()){
        return false;
    }
    trackEntity = createCmd->getEntity();
    if (trackEntity == NULL_ENTITY){
        return false;
    }

    AnimationComponent& anim = scene->getComponent<AnimationComponent>(animEntity);
    oldActions = anim.actions;
    std::vector<ActionFrame> newActions = oldActions;
    // Duration 0 = auto: the block grows as keys extend.
    newActions.push_back({0.0f, 0.0f, trackEntity, 0});

    PropertyData prop = Catalog::getProperty(scene, animEntity, ComponentType::AnimationComponent, "actions");
    auto* actionsRef = static_cast<std::vector<ActionFrame>*>(prop.ref);
    if (!actionsRef){
        createCmd->undo();
        trackEntity = NULL_ENTITY;
        return false;
    }
    prefabUndo.capture(project, sceneId, animEntity);
    *actionsRef = newActions;
    Catalog::updateEntity(scene, animEntity, prop.updateFlags);
    // Unity: the animation of a prefab instance's GameObject edited is an override of it
    project->recordPrefabEdit(sceneId, animEntity, ComponentType::AnimationComponent, {"actions"});

    sceneProject->isModified = true;
    return true;
}

void editor::CreateTrackKeyCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene){
        return;
    }
    Scene* scene = sceneProject->scene;

    if (AnimationComponent* anim = scene->findComponent<AnimationComponent>(animEntity)){
        PropertyData prop = Catalog::getProperty(scene, animEntity, ComponentType::AnimationComponent, "actions");
        if (auto* actionsRef = static_cast<std::vector<ActionFrame>*>(prop.ref)){
            *actionsRef = oldActions;
            Catalog::updateEntity(scene, animEntity, prop.updateFlags);
        }else{
            anim->actions = oldActions;
        }
    }

    if (createCmd){
        createCmd->undo();
    }
    prefabUndo.restore(project, sceneId);
    trackEntity = NULL_ENTITY;
    sceneProject->isModified = wasModified;
}
