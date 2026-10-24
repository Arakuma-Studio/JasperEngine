// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PrefabOverrideCmd.h"
#include "pose/PoseEditSession.h"

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
        // a posed bone applies its rest transform, not the pose
        PoseEditSession::suspendActiveFor(sceneId, {entity});
        if (!project->canApplyPrefabOverrides()){
            return false;
        }
        if (!target.empty()){
            targets.push_back(target);
        }else{
            // the owning prefab (what an empty target means to the primitives)
            PrefabObjectRef ref;
            if (!project->findPrefabObject(sceneId, entity, ref)){
                return false;
            }
            targets.push_back(ref.path);
        }
    }
    if (!project->getScene(sceneId)){
        return false;
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
