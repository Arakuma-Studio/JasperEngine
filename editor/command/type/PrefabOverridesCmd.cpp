// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PrefabOverridesCmd.h"

using namespace jasperengine;

editor::RevertPrefabRemovalCmd::RevertPrefabRemovalCmd(Project* project, uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot, Entity registryEntity){
    this->project = project;
    this->sceneId = sceneId;
    this->path = path;
    this->instanceRoot = instanceRoot;
    this->registryEntity = registryEntity;
}

bool editor::RevertPrefabRemovalCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject) {
        return false;
    }
    wasModified = sceneProject->isModified;
    return project->revertPrefabRemoval(sceneId, path, instanceRoot, registryEntity, revert);
}

void editor::RevertPrefabRemovalCmd::undo(){
    project->undoRevertPrefabRemoval(sceneId, path, instanceRoot, registryEntity, revert);
    if (SceneProject* sceneProject = project->getScene(sceneId)) {
        sceneProject->isModified = wasModified;
    }
}

bool editor::RevertPrefabRemovalCmd::mergeWith(editor::Command* otherCommand){
    return false;
}

editor::ApplyPrefabRemovalCmd::ApplyPrefabRemovalCmd(Project* project, uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot, Entity registryEntity){
    this->project = project;
    this->sceneId = sceneId;
    this->path = path;
    this->instanceRoot = instanceRoot;
    this->registryEntity = registryEntity;
}

bool editor::ApplyPrefabRemovalCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject) {
        return false;
    }
    wasModified = sceneProject->isModified;
    return project->applyPrefabRemoval(sceneId, path, instanceRoot, registryEntity, apply);
}

void editor::ApplyPrefabRemovalCmd::undo(){
    project->undoApplyPrefabRemoval(sceneId, path, instanceRoot, registryEntity, apply);
    if (SceneProject* sceneProject = project->getScene(sceneId)) {
        sceneProject->isModified = wasModified;
    }
}

bool editor::ApplyPrefabRemovalCmd::mergeWith(editor::Command* otherCommand){
    return false;
}

editor::ApplyPrefabComponentCmd::ApplyPrefabComponentCmd(Project* project, uint32_t sceneId, Entity entity, ComponentType componentType){
    this->project = project;
    this->sceneId = sceneId;
    this->entity = entity;
    this->componentType = componentType;
}

bool editor::ApplyPrefabComponentCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject) {
        return false;
    }
    wasModified = sceneProject->isModified;
    return project->applyComponentOverride(sceneId, entity, componentType, recovery);
}

void editor::ApplyPrefabComponentCmd::undo(){
    project->undoApplyComponentOverride(sceneId, entity, componentType, recovery);
    if (SceneProject* sceneProject = project->getScene(sceneId)) {
        sceneProject->isModified = wasModified;
    }
}

bool editor::ApplyPrefabComponentCmd::mergeWith(editor::Command* otherCommand){
    return false;
}

editor::PrefabOverridesCmd::PrefabOverridesCmd(Project* project, uint32_t sceneId, std::vector<std::unique_ptr<Command>> steps){
    this->project = project;
    this->sceneId = sceneId;
    this->steps = std::move(steps);
    this->stepDone.assign(this->steps.size(), false);
}

bool editor::PrefabOverridesCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject) {
        return false;
    }
    wasModified = sceneProject->isModified;

    bool any = false;
    for (size_t i = 0; i < steps.size(); i++) {
        // A redo runs again what the first run did
        if (executedOnce && !stepDone[i]) {
            continue;
        }
        stepDone[i] = steps[i]->execute();
        any = any || stepDone[i];
    }
    executedOnce = true;
    if (any) {
        sceneProject->isModified = true;
    }
    return any;
}

void editor::PrefabOverridesCmd::undo(){
    for (size_t i = steps.size(); i-- > 0;) {
        if (stepDone[i]) {
            steps[i]->undo();
        }
    }
    if (SceneProject* sceneProject = project->getScene(sceneId)) {
        sceneProject->isModified = wasModified;
    }
}

bool editor::PrefabOverridesCmd::mergeWith(editor::Command* otherCommand){
    return false;
}
