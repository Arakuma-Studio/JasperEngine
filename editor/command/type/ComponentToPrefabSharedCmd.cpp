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

    for (ComponentToPrefabSharedData& entityData : entities){

        fs::path filepath = project->findPrefabPathFor(sceneId, entityData.entity);
        if (sceneProject && !filepath.empty()) {
            Prefab* prefab = project->getPrefab(filepath);

            Signature signature = Catalog::componentTypeToSignature(sceneProject->scene, componentType);
            entityData.recovery = Stream::encodeComponents(entityData.entity, sceneProject->scene, signature);

            if (!prefab->hasComponentOverride(sceneId, entityData.entity, componentType)){
                return false;
            }

            // Clear the override and copy values from the prefab registry
            prefab->clearComponentOverride(sceneProject->id, entityData.entity, componentType);

            Entity registryEntity = prefab->getRegistryEntity(sceneId, entityData.entity);
            Catalog::copyComponent(prefab->registry.get(), registryEntity, sceneProject->scene, entityData.entity, componentType);
            std::unordered_map<Entity, Entity> registryToLocal;
            if (const Prefab::Instance* instance = prefab->getInstance(sceneId, entityData.entity)) {
                for (const auto& member : instance->members) {
                    registryToLocal[member.registryEntity] = member.localEntity;
                }
            }
            Project::remapEntityPropertiesInComponent(sceneProject->scene, entityData.entity, componentType, {}, registryToLocal);
        }

    }

    sceneProject->isModified = true;

    return true;
}

void editor::ComponentToPrefabSharedCmd::undo() {
    SceneProject* sceneProject = project->getScene(sceneId);

    for (ComponentToPrefabSharedData& entityData : entities){

        fs::path filepath = project->findPrefabPathFor(sceneId, entityData.entity);
        if (sceneProject && !filepath.empty()) {
            Prefab* prefab = project->getPrefab(filepath);

            prefab->setComponentOverride(sceneProject->id, entityData.entity, componentType);

            Entity parent = NULL_ENTITY;
            if (componentType == ComponentType::Transform){
                parent = sceneProject->scene->getComponent<Transform>(entityData.entity).parent;
            }

            Stream::decodeComponents(entityData.entity, parent, sceneProject->scene, entityData.recovery);
        }

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
