// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AddActionFrameCmd.h"

#include "component/AudioSnapshotComponent.h"
#include "component/AudioSourceComponent.h"
#include "subsystem/ActionSystem.h"

#include <algorithm>

using namespace jasperengine;

editor::AddActionFrameCmd::AddActionFrameCmd(Project* project, uint32_t sceneId, Entity animEntity, Entity target,
                                             EntityCreationType creationType, std::string name, uint32_t track)
    : project(project), sceneId(sceneId), animEntity(animEntity), track(track){
    if (creationType != EntityCreationType::EMPTY){
        createCmd = std::make_unique<CreateEntityCmd>(project, sceneId, name, creationType, target);
    }
}

bool editor::AddActionFrameCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene){
        return false;
    }
    Scene* scene = sceneProject->scene;
    if (!scene->findComponent<AnimationComponent>(animEntity)){
        return false;
    }

    Entity actionEntity = NULL_ENTITY;
    if (createCmd){
        if (!createCmd->execute()){
            return false;
        }
        actionEntity = createCmd->getEntity();
        // an audio or snapshot action's content, set before the frame measures its duration
        if (AudioSourceComponent* audio = scene->findComponent<AudioSourceComponent>(actionEntity); audio && sound.isSet()){
            if (soundIsEvent) audio->event = sound;
            else audio->filename = sound;
            audio->needUpdate = true;
        }
        if (AudioSnapshotComponent* mixerSnapshot = scene->findComponent<AudioSnapshotComponent>(actionEntity); mixerSnapshot && !snapshot.empty()){
            mixerSnapshot->snapshot = snapshot;
        }
    }

    // Duration 0 = auto: the frame follows the action's own duration.
    // Empty frames have no action to resolve, so give a visible default.
    float newDuration = (actionEntity != NULL_ENTITY) ? 0.0f : 1.0f;
    ActionFrame newFrame = {std::max(0.0f, startTime), newDuration, actionEntity, track};

    // Read after the create: an Animation action adds an AnimationComponent, which can
    // move the other AnimationComponents in memory
    std::vector<ActionFrame> newActions = scene->getComponent<AnimationComponent>(animEntity).actions;

    // Find non-overlapping track (zero-length frames floored so they still
    // spread onto free tracks)
    ActionSystem* actionSystem = scene->getSystem<ActionSystem>().get();
    float newFrameDur = std::max(actionSystem->getFrameDuration(newFrame), 0.01f);
    bool overlap;
    do {
        overlap = false;
        for (const auto& a : newActions){
            if (a.track == newFrame.track){
                float startA = a.startTime;
                float endA = a.startTime + std::max(actionSystem->getFrameDuration(a), 0.01f);
                float startB = newFrame.startTime;
                float endB = newFrame.startTime + newFrameDur;
                if (std::max(startA, startB) < std::min(endA, endB)){
                    overlap = true;
                    newFrame.track++;
                    break;
                }
            }
        }
    } while (overlap);

    newActions.push_back(newFrame);
    actionsCmd = std::make_unique<PropertyCmd<std::vector<ActionFrame>>>(
        project, sceneId, animEntity, ComponentType::AnimationComponent, "actions", newActions);
    if (!actionsCmd->execute()){
        actionsCmd.reset();
        if (createCmd){
            createCmd->undo();
        }
        return false;
    }

    return true;
}

void editor::AddActionFrameCmd::undo(){
    if (actionsCmd){
        actionsCmd->undo();
        actionsCmd.reset();
    }
    if (createCmd){
        createCmd->undo();
    }
}

std::string editor::AddActionFrameCmd::describe() const{
    return (undoLabel.empty() ? std::string("Add Action") : undoLabel) + " (" + CommandNames::entity(project, sceneId, animEntity) + ")";
}

size_t editor::AddActionFrameCmd::memoryBytes() const{
    return sizeof(*this) + (createCmd ? createCmd->memoryBytes() : 0) + (actionsCmd ? actionsCmd->memoryBytes() : 0);
}
