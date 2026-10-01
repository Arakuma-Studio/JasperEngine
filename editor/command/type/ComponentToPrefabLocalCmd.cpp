// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ComponentToPrefabLocalCmd.h"

#include "Stream.h"

using namespace jasperengine;

editor::ComponentToPrefabLocalCmd::ComponentToPrefabLocalCmd(Project* project, uint32_t sceneId, Entity entity, ComponentType componentType){
    this->project = project;
    this->sceneId = sceneId;
    this->entities.push_back(entity);
    this->componentType = componentType;

    this->wasModified = project->getScene(sceneId)->isModified;
}

bool editor::ComponentToPrefabLocalCmd::execute() {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject) {
        return false;
    }
    changedEntities.clear();
    for (Entity entity : entities){
        // one already overriding it in whole stays as it is
        std::set<std::string> properties = project->getOverriddenProperties(sceneId, entity, componentType);
        if (properties.count(PrefabObjectOverrides::WHOLE_COMPONENT)) {
            continue;
        }
        if (project->recordPropertyOverride(sceneId, entity, componentType, {})) {
            changedEntities.emplace_back(entity, properties);
        }
    }

    if (changedEntities.empty()) {
        return false;
    }
    sceneProject->isModified = true;

    return true;
}

void editor::ComponentToPrefabLocalCmd::undo() {
    SceneProject* sceneProject = project->getScene(sceneId);
    for (const auto& [entity, properties] : changedEntities){
        if (!sceneProject) {
            break;
        }
        // back to what it overrode before (its values were the prefab's for the rest)
        project->clearPropertyOverride(sceneId, entity, componentType, {});
        if (!properties.empty()) {
            project->recordPropertyOverride(sceneId, entity, componentType, std::vector<std::string>(properties.begin(), properties.end()));
        }
    }

    sceneProject->isModified = wasModified;
}

bool editor::ComponentToPrefabLocalCmd::mergeWith(Command* otherCommand){
    ComponentToPrefabLocalCmd* otherCmd = dynamic_cast<ComponentToPrefabLocalCmd*>(otherCommand);
    if (otherCmd != nullptr){
        if (sceneId == otherCmd->sceneId){

            for (Entity& otherEntity :  otherCmd->entities){
                entities.push_back(otherEntity);
            }
            changedEntities.insert(changedEntities.end(), otherCmd->changedEntities.begin(), otherCmd->changedEntities.end());

            wasModified = wasModified && otherCmd->wasModified;

            return true;
        }
    }

    return false;
}
