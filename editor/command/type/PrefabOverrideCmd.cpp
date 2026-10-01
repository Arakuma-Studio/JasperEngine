// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PrefabOverrideCmd.h"

using namespace jasperengine;

editor::PrefabOverrideCmd::PrefabOverrideCmd(Project* project, uint32_t sceneId, Entity entity, Operation operation,
        ComponentType componentType, const std::vector<std::string>& properties, uint8_t stateBits, const std::filesystem::path& target){
    this->project = project;
    this->sceneId = sceneId;
    this->entity = entity;
    this->operation = operation;
    this->componentType = componentType;
    this->properties = properties;
    this->stateBits = stateBits;
    this->target = target;
}

bool editor::PrefabOverrideCmd::isApply() const{
    return operation == Operation::ApplyProperty || operation == Operation::ApplyState ||
        operation == Operation::ApplyAddedComponent || operation == Operation::ApplyRemovedComponent;
}

bool editor::PrefabOverrideCmd::execute(){
    std::vector<std::filesystem::path> targets;
    if (isApply()){
        if (!target.empty()){
            targets.push_back(target);
        }else{
            std::vector<std::filesystem::path> candidates = project->getPrefabApplyTargets(sceneId, entity);
            if (candidates.empty()){
                return false;
            }
            targets.push_back(candidates.front());
        }
    }
    before = project->capturePrefabUndoState(sceneId, entity, targets);

    switch (operation){
        case Operation::RecordProperty: return project->recordPropertyOverride(sceneId, entity, componentType, properties);
        case Operation::RevertProperty: return project->revertPropertyOverride(sceneId, entity, componentType, properties);
        case Operation::ApplyProperty: return project->applyPropertyOverride(sceneId, entity, componentType, properties, target);
        case Operation::RecordState: return project->recordStateOverride(sceneId, entity, stateBits);
        case Operation::RevertState: return project->revertStateOverride(sceneId, entity, stateBits);
        case Operation::ApplyState: return project->applyStateOverride(sceneId, entity, stateBits, target);
        case Operation::RecordAddedComponent: return project->recordAddedComponent(sceneId, entity, componentType);
        case Operation::RevertAddedComponent: return project->revertAddedComponent(sceneId, entity, componentType);
        case Operation::ApplyAddedComponent: return project->applyAddedComponent(sceneId, entity, componentType, target);
        case Operation::RecordRemovedComponent: return project->recordRemovedComponent(sceneId, entity, componentType);
        case Operation::RevertRemovedComponent: return project->revertRemovedComponent(sceneId, entity, componentType);
        case Operation::ApplyRemovedComponent: return project->applyRemovedComponent(sceneId, entity, componentType, target);
    }
    return false;
}

void editor::PrefabOverrideCmd::undo(){
    project->restorePrefabUndoState(before);
}

bool editor::PrefabOverrideCmd::mergeWith(Command* otherCommand){
    return false;
}
