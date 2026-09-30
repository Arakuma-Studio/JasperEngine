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
    for (Entity entity : entities){
        fs::path filepath = project->findPrefabPathFor(sceneId, entity);
        if (sceneProject && !filepath.empty()) {
            Prefab* prefab = project->getPrefab(filepath);

            if (prefab->hasComponentOverride(sceneId, entity, componentType)){
                return false;
            }

            prefab->setComponentOverride(sceneProject->id, entity, componentType);
        }
    }

    sceneProject->isModified = true;

    return true;
}

void editor::ComponentToPrefabLocalCmd::undo() {
    SceneProject* sceneProject = project->getScene(sceneId);
    for (Entity entity : entities){
        fs::path filepath = project->findPrefabPathFor(sceneId, entity);
        if (sceneProject && !filepath.empty()) {
            Prefab* prefab = project->getPrefab(filepath);

            prefab->clearComponentOverride(sceneProject->id, entity, componentType);

            Entity registryEntity = prefab->getRegistryEntity(sceneId, entity);
            Catalog::copyComponent(prefab->registry.get(), registryEntity, sceneProject->scene, entity, componentType);
            std::unordered_map<Entity, Entity> registryToLocal;
            if (const Prefab::Instance* instance = prefab->getInstance(sceneId, entity)) {
                for (const auto& member : instance->members) {
                    registryToLocal[member.registryEntity] = member.localEntity;
                }
            }
            Project::remapEntityPropertiesInComponent(sceneProject->scene, entity, componentType, {}, registryToLocal);
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

            wasModified = wasModified && otherCmd->wasModified;

            return true;
        }
    }

    return false;
}
