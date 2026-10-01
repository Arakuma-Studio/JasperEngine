// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PrefabOverridesCmd.h"

#include "command/type/AddEntityToPrefabCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/PrefabOverrideCmd.h"

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
    if (!sceneProject || !project->canApplyPrefabOverrides()) {
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

editor::PrefabOverridesCmd* editor::PrefabOverridesCmd::revertAll(Project* project, uint32_t sceneId, Entity root){
    using Kind = PrefabOverrideEntry::Kind;
    using Operation = PrefabOverrideCmd::Operation;
    const std::vector<PrefabOverrideEntry> entries = project->getPrefabOverrideEntries(sceneId, root);
    // Added GameObjects first (their delete reads the scene as it is), removed GameObjects
    // before components (a reverted component pointing at one finds it back)
    std::vector<std::unique_ptr<Command>> steps;
    std::vector<Entity> addedEntities;
    for (const PrefabOverrideEntry& entry : entries) {
        if (entry.kind == Kind::AddedGameObject) addedEntities.push_back(entry.entity);
    }
    if (!addedEntities.empty()) {
        steps.emplace_back(new DeleteEntityCmd(project, sceneId, addedEntities));
    }
    for (const PrefabOverrideEntry& entry : entries) {
        if (entry.kind == Kind::RemovedGameObject) {
            steps.emplace_back(new RevertPrefabRemovalCmd(project, sceneId, entry.removal.path, entry.removal.instanceRoot, entry.removal.registryEntity));
        }
    }
    for (const PrefabOverrideEntry& entry : entries) {
        switch (entry.kind) {
            case Kind::State: steps.emplace_back(new PrefabOverrideCmd(project, sceneId, entry.entity, Operation::RevertState, ComponentType::PrefabComponent, {}, entry.state)); break;
            case Kind::Property: steps.emplace_back(new PrefabOverrideCmd(project, sceneId, entry.entity, Operation::RevertProperty, entry.type, {})); break;
            case Kind::AddedComponent: steps.emplace_back(new PrefabOverrideCmd(project, sceneId, entry.entity, Operation::RevertAddedComponent, entry.type)); break;
            case Kind::RemovedComponent: steps.emplace_back(new PrefabOverrideCmd(project, sceneId, entry.entity, Operation::RevertRemovedComponent, entry.type)); break;
            default: break;
        }
    }
    return new PrefabOverridesCmd(project, sceneId, std::move(steps));
}

editor::PrefabOverridesCmd* editor::PrefabOverridesCmd::applyAll(Project* project, uint32_t sceneId, Entity root){
    using Kind = PrefabOverrideEntry::Kind;
    using Operation = PrefabOverrideCmd::Operation;
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !project->canApplyPrefabOverrides()) {
        return nullptr;
    }
    const PrefabComponent* component = sceneProject->scene->findComponent<PrefabComponent>(root);
    if (!component) {
        return nullptr;
    }
    const std::filesystem::path rootPath = component->path;
    const std::vector<PrefabOverrideEntry> entries = project->getPrefabOverrideEntries(sceneId, root);
    // Added GameObjects first: each reads where it is when made
    std::vector<std::unique_ptr<Command>> steps;
    for (const PrefabOverrideEntry& entry : entries) {
        if (entry.kind == Kind::AddedGameObject) {
            steps.emplace_back(new AddEntityToPrefabCmd(project, sceneId, entry.entity, entry.parent));
        }
    }
    for (const PrefabOverrideEntry& entry : entries) {
        const std::vector<std::string> names(entry.properties.begin(), entry.properties.end());
        switch (entry.kind) {
            case Kind::State:
                steps.emplace_back(new PrefabOverrideCmd(project, sceneId, entry.entity, Operation::ApplyState, ComponentType::PrefabComponent, {}, entry.state, rootPath));
                break;
            case Kind::Property:
                if (project->canApplyPrefabOverride(sceneId, entry.entity, entry.type, names, rootPath)) {
                    steps.emplace_back(new PrefabOverrideCmd(project, sceneId, entry.entity, Operation::ApplyProperty, entry.type, names, 0, rootPath));
                }
                break;
            case Kind::AddedComponent:
                if (project->canApplyPrefabOverride(sceneId, entry.entity, entry.type, {}, rootPath)) {
                    steps.emplace_back(new PrefabOverrideCmd(project, sceneId, entry.entity, Operation::ApplyAddedComponent, entry.type, {}, 0, rootPath));
                }
                break;
            case Kind::RemovedComponent:
                steps.emplace_back(new PrefabOverrideCmd(project, sceneId, entry.entity, Operation::ApplyRemovedComponent, entry.type, {}, 0, rootPath));
                break;
            default:
                break;
        }
    }
    for (const PrefabOverrideEntry& entry : entries) {
        if (entry.kind == Kind::RemovedGameObject) {
            steps.emplace_back(new ApplyPrefabRemovalCmd(project, sceneId, entry.removal.path, entry.removal.instanceRoot, entry.removal.registryEntity));
        }
    }
    return new PrefabOverridesCmd(project, sceneId, std::move(steps));
}
