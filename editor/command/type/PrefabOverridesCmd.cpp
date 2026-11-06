// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PrefabOverridesCmd.h"
#include "component/PrefabComponent.h"
#include "pose/PoseEditSession.h"

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

editor::ApplyPrefabRemovalCmd::ApplyPrefabRemovalCmd(Project* project, uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot, Entity registryEntity,
        const std::filesystem::path& target){
    this->project = project;
    this->sceneId = sceneId;
    this->path = path;
    this->instanceRoot = instanceRoot;
    this->registryEntity = registryEntity;
    this->target = target;
}

bool editor::ApplyPrefabRemovalCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !project->canApplyPrefabOverrides()) {
        return false;
    }
    wasModified = sceneProject->isModified;
    if (toOuter()) {
        before = project->capturePrefabUndoState(sceneId, instanceRoot, {target});
        return project->applyPrefabRemovalTo(sceneId, path, instanceRoot, registryEntity, target);
    }
    return project->applyPrefabRemoval(sceneId, path, instanceRoot, registryEntity, apply);
}

void editor::ApplyPrefabRemovalCmd::undo(){
    if (toOuter()) {
        project->restorePrefabUndoState(before);
    } else {
        project->undoApplyPrefabRemoval(sceneId, path, instanceRoot, registryEntity, apply);
    }
    if (SceneProject* sceneProject = project->getScene(sceneId)) {
        sceneProject->isModified = wasModified;
    }
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

editor::PrefabOverridesCmd* editor::PrefabOverridesCmd::revertAll(Project* project, uint32_t sceneId, Entity root){
    using Kind = PrefabOverrideEntry::Kind;
    using Operation = PrefabOverrideCmd::Operation;
    // a posed character's bones are no overrides: back to rest before they are listed and reverted
    PoseEditSession::suspendActiveFor(sceneId, {root});
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
    auto* cmd = new PrefabOverridesCmd(project, sceneId, std::move(steps));
    cmd->label = "Revert All Overrides (" + CommandNames::entity(project, sceneId, root) + ")";
    return cmd;
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
    // a posed character's bones are no overrides: back to rest before they are listed
    PoseEditSession::suspendActiveFor(sceneId, {root});
    const std::vector<PrefabOverrideEntry> entries = project->getPrefabOverrideEntries(sceneId, root);
    // Added GameObjects first: each reads where it is when made
    std::vector<std::unique_ptr<Command>> steps;
    for (const PrefabOverrideEntry& entry : entries) {
        // into the instance's own prefab, also below its nested instances (PREFABS.md 17); not one
        // holding an instance of that prefab, which would make it contain itself: it stays an override
        if (entry.kind == Kind::AddedGameObject && project->canAddToPrefab(sceneId, entry.entity, rootPath)) {
            steps.emplace_back(new AddEntityToPrefabCmd(project, sceneId, entry.entity, entry.parent, rootPath));
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
            // to the instance's prefab too: as its override of a nested instance or of its base
            steps.emplace_back(new ApplyPrefabRemovalCmd(project, sceneId, entry.removal.path, entry.removal.instanceRoot, entry.removal.registryEntity,
                rootPath));
        }
    }
    auto* cmd = new PrefabOverridesCmd(project, sceneId, std::move(steps));
    cmd->label = "Apply All Overrides (" + CommandNames::entity(project, sceneId, root) + ")";
    return cmd;
}

std::string editor::PrefabOverridesCmd::describe() const{
    if (!label.empty()) {
        return label;
    }
    for (size_t i = 0; i < steps.size(); i++) {
        if (i < stepDone.size() && stepDone[i]) {
            return steps[i]->describe();
        }
    }
    return "Prefab Overrides";
}

size_t editor::PrefabOverridesCmd::memoryBytes() const{
    size_t bytes = sizeof(*this);
    for (const std::unique_ptr<Command>& step : steps) {
        bytes += step->memoryBytes();
    }
    return bytes;
}

std::vector<editor::Command::Touched> editor::RevertPrefabRemovalCmd::touchedPaths(bool) const{
    return {};   // the instance in the scene only
}

std::vector<editor::Command::Touched> editor::ApplyPrefabRemovalCmd::touchedPaths(bool) const{
    std::vector<Touched> out;
    for (const std::filesystem::path& file : {path, target}){
        if (file.empty()) continue;
        out.push_back({file.is_absolute() ? file : project->getProjectPath() / file, false});
    }
    return out;
}

std::vector<editor::Command::Touched> editor::PrefabOverridesCmd::touchedPaths(bool undo) const{
    std::vector<Touched> out;
    for (const std::unique_ptr<Command>& step : steps){
        std::vector<Touched> some = step->touchedPaths(undo);
        out.insert(out.end(), some.begin(), some.end());
    }
    return out;
}
