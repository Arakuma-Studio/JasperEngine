// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ComponentToPrefabSharedCmd.h"

#include "Stream.h"

using namespace jasperengine;

editor::ComponentToPrefabSharedCmd::ComponentToPrefabSharedCmd(Project* project, uint32_t sceneId, Entity entity, ComponentType componentType){
    this->project = project;
    this->sceneId = sceneId;
    this->componentType = componentType;

    ComponentToPrefabSharedData entityData;
    entityData.entity = entity;
    entities.push_back(entityData);

    this->wasModified = project->getScene(sceneId)->isModified;
}

bool editor::ComponentToPrefabSharedCmd::execute() {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject) {
        return false;
    }

    bool changed = false;
    for (ComponentToPrefabSharedData& entityData : entities){
        entityData.reverted = false;
        if (!project->isComponentOverridden(sceneId, entityData.entity, componentType)) {
            continue; // already following the prefab
        }
        Signature signature = Catalog::componentTypeToSignature(sceneProject->scene, componentType);
        entityData.recovery = Stream::encodeComponents(entityData.entity, sceneProject->scene, signature);
        entityData.properties = project->getOverriddenProperties(sceneId, entityData.entity, componentType);

        // The prefab's values back (with what the prefabs holding the instance override),
        // the override gone
        entityData.reverted = project->revertPropertyOverride(sceneId, entityData.entity, componentType, {});
        changed = changed || entityData.reverted;
    }

    if (changed) {
        sceneProject->isModified = true;
    }

    return changed;
}

void editor::ComponentToPrefabSharedCmd::undo() {
    SceneProject* sceneProject = project->getScene(sceneId);

    for (ComponentToPrefabSharedData& entityData : entities){
        if (!sceneProject || !entityData.reverted) {
            continue;
        }
        Entity parent = NULL_ENTITY;
        if (Transform* transform = sceneProject->scene->findComponent<Transform>(entityData.entity)){
            parent = transform->parent;
        }
        Stream::decodeComponents(entityData.entity, parent, sceneProject->scene, entityData.recovery);
        project->recordPropertyOverride(sceneId, entityData.entity, componentType,
            std::vector<std::string>(entityData.properties.begin(), entityData.properties.end()));
    }

    sceneProject->isModified = wasModified;
}

bool editor::ComponentToPrefabSharedCmd::mergeWith(Command* otherCommand){
    ComponentToPrefabSharedCmd* otherCmd = dynamic_cast<ComponentToPrefabSharedCmd*>(otherCommand);
    if (otherCmd != nullptr){
        if (sceneId == otherCmd->sceneId){

            for (ComponentToPrefabSharedData& otherEntityData :  otherCmd->entities){
                entities.push_back(otherEntityData);
            }

            wasModified = wasModified && otherCmd->wasModified;

            return true;
        }
    }

    return false;
}
