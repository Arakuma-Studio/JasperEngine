// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "AudioSourceSlotCmd.h"

#include "command/CommandNames.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "component/AudioSourceComponent.h"
#include "component/Transform.h"
#include "object/sound/AudioSourceSlots.h"

#include <algorithm>
#include <vector>

using namespace jasperengine;

editor::AddAudioSourceSlotCmd::AddAudioSourceSlotCmd(Project* project, uint32_t sceneId, Entity owner)
    : project(project), sceneId(sceneId), owner(owner){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (sceneProject && sceneProject->scene && sceneProject->scene->isEntityCreated(owner)){
        ownerName = sceneProject->scene->getEntityName(owner);
    }
}

editor::AddAudioSourceSlotCmd::~AddAudioSourceSlotCmd() = default;

std::string editor::AddAudioSourceSlotCmd::refusal(Scene* scene, Entity entity){
    if (!scene || !scene->isEntityCreated(entity)){
        return "The GameObject does not exist.";
    }
    if (!scene->findComponent<AudioSourceComponent>(entity)){
        return std::string();
    }
    if (AudioSourceSlots::isSlot(scene, entity)){
        return "This is a further Audio Source of its parent: add the Audio Source to the parent GameObject.";
    }
    if (!scene->findComponent<Transform>(entity)){
        return "A second Audio Source needs a Transform on the GameObject: add a Transform first.";
    }
    return std::string();
}

bool editor::AddAudioSourceSlotCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene){
        return false;
    }
    Scene* scene = sceneProject->scene;
    if (!create){
        if (!refusal(scene, owner).empty() || !scene->findComponent<AudioSourceComponent>(owner)){
            return false;
        }
        slot = AudioSourceSlots::nextSlot(scene, owner);
        // Spatial Blend 1 at the local origin: the same source Add Component gives the GameObject
        create = std::make_unique<CreateEntityCmd>(project, sceneId, "Audio Source " + std::to_string(slot + 1),
                                                   EntityCreationType::AUDIO_SOURCE_3D, owner);
        // the GameObject stays selected: the slot shows in its Inspector, not as a row of its own
        create->setQuiet(true);
    }
    if (!create->execute()){
        return false;
    }
    scene->getComponent<AudioSourceComponent>(create->getEntity()).slot = slot;
    return true;
}

void editor::AddAudioSourceSlotCmd::undo(){
    if (create){
        create->undo();
    }
}

std::string editor::AddAudioSourceSlotCmd::describe() const{
    return "Add Component " + CommandNames::component(ComponentType::AudioSourceComponent) + " (" + ownerName + ")";
}

Entity editor::AddAudioSourceSlotCmd::getSlotEntity() const{
    return create ? create->getEntity() : NULL_ENTITY;
}

editor::RemoveAudioSourceSlotCmd::RemoveAudioSourceSlotCmd(Project* project, uint32_t sceneId, Entity slot)
    : remove(std::make_unique<DeleteEntityCmd>(project, sceneId, slot)){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (sceneProject && sceneProject->scene){
        const Entity owner = AudioSourceSlots::ownerOf(sceneProject->scene, slot);
        if (owner != NULL_ENTITY) ownerName = sceneProject->scene->getEntityName(owner);
    }
}

editor::RemoveAudioSourceSlotCmd::~RemoveAudioSourceSlotCmd() = default;

bool editor::RemoveAudioSourceSlotCmd::execute(){
    return remove->execute();
}

void editor::RemoveAudioSourceSlotCmd::undo(){
    remove->undo();
}

std::string editor::RemoveAudioSourceSlotCmd::describe() const{
    return "Remove Component " + CommandNames::component(ComponentType::AudioSourceComponent) + " (" + ownerName + ")";
}

editor::Command* editor::buildMoveAudioSourceSlot(Project* project, uint32_t sceneId, Entity slot, int direction){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || (direction != -1 && direction != 1)){
        return nullptr;
    }
    Scene* scene = sceneProject->scene;
    const Entity owner = AudioSourceSlots::ownerOf(scene, slot);
    if (owner == NULL_ENTITY){
        return nullptr;
    }
    const std::vector<Entity> slots = AudioSourceSlots::getSlots(scene, owner);
    const auto it = std::find(slots.begin(), slots.end(), slot);
    const long index = (long)(it - slots.begin()) + direction;
    if (it == slots.end() || index < 0 || index >= (long)slots.size()){
        return nullptr;
    }
    const Entity other = slots[(size_t)index];
    const int mine = scene->getComponent<AudioSourceComponent>(slot).slot;
    const int theirs = scene->getComponent<AudioSourceComponent>(other).slot;
    auto* multi = new MultiPropertyCmd();
    multi->addPropertyCmd<int>(project, sceneId, slot, ComponentType::AudioSourceComponent, "slot", theirs);
    multi->addPropertyCmd<int>(project, sceneId, other, ComponentType::AudioSourceComponent, "slot", mine);
    return multi;
}
