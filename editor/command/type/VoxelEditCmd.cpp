// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelEditCmd.h"

#include "command/CommandNames.h"
#include "voxel/VoxelEdit.h"

#include <map>
#include <utility>

using namespace jasperengine;

namespace {

    int64_t hotBytes(const voxel::Slot& slot){
        return slot.kind() == voxel::Slot::Hot ? static_cast<int64_t>(slot.hotBlock()->payloadBytes()) : 0;
    }

    std::shared_ptr<VoxelSystem> systemOf(editor::Project* project, uint32_t sceneId){
        editor::SceneProject* sceneProject = project->getScene(sceneId);
        return sceneProject && sceneProject->scene ? sceneProject->scene->findSystem<VoxelSystem>() : nullptr;
    }

}

editor::VoxelEditCmd::VoxelEditCmd(Project* project, uint32_t sceneId, Entity entity, std::string displayName,
                                   std::vector<voxel::SlotChange> delta, bool continuous)
    : project(project), sceneId(sceneId), entity(entity), displayName(std::move(displayName)), delta(std::move(delta)), continuous(continuous),
      budget(VoxelAssets::sharedBudget()){
    if (std::shared_ptr<VoxelSystem> system = systemOf(project, sceneId)){
        volume = system->getAsset(entity);
    }
    updateCharge();
}

editor::VoxelEditCmd::~VoxelEditCmd(){
    budget->charge(-charged);
}

void editor::VoxelEditCmd::updateCharge(){
    int64_t bytes = static_cast<int64_t>(delta.size() * sizeof(voxel::SlotChange));
    for (const voxel::SlotChange& change : delta){
        bytes += hotBytes(applied ? change.before : change.after);
    }
    budget->charge(bytes - charged);
    charged = bytes;
}

void editor::VoxelEditCmd::applyDelta(bool undoing){
    if (!volume || !volume->store){
        return;
    }
    std::shared_ptr<VoxelSystem> system = systemOf(project, sceneId);
    const std::shared_ptr<VoxelAsset> current = system ? system->getAsset(entity) : nullptr;
    // the file changed under a step folded into a bigger one: this content is not what it edited
    if (current && current != volume){
        return;
    }
    if (current && system->getStore(entity) == volume->store.get()){
        system->applyDelta(entity, delta, undoing);
    }else{
        // its component is not back in the system yet (an Add Component redone a moment ago)
        voxel::applyDelta(*volume->store, delta, undoing);
    }
    volume->dirty = true;
    applied = !undoing;
    updateCharge();
}

bool editor::VoxelEditCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !volume){
        return false;
    }
    wasModified = sceneProject->isModified;
    if (recorded){
        applyDelta(false);
    }
    // (the edit itself made the first state: VoxelSystem::edit)
    recorded = true;
    if (!delta.empty()){
        volume->dirty = true;
        sceneProject->isModified = true;
    }
    return true;
}

void editor::VoxelEditCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject){
        return;
    }
    applyDelta(true);
    sceneProject->isModified = wasModified;
}

bool editor::VoxelEditCmd::mergeWith(Command* otherCommand){
    VoxelEditCmd* other = dynamic_cast<VoxelEditCmd*>(otherCommand);
    if (!other || !continuous || !other->continuous || project != other->project || sceneId != other->sceneId || entity != other->entity ||
        volume != other->volume || displayName != other->displayName){
        return false;
    }
    // this is the newer one: a block both touched goes back to the older step's state
    std::map<voxel::BlockKey, size_t> at;
    for (size_t i = 0; i < delta.size(); i++){
        at[delta[i].key] = i;
    }
    for (voxel::SlotChange& older : other->delta){
        const auto found = at.find(older.key);
        if (found != at.end()){
            delta[found->second].before = std::move(older.before);
        }else{
            delta.push_back(std::move(older));
        }
    }
    other->delta.clear();
    other->updateCharge();
    updateCharge();
    wasModified = wasModified && other->wasModified;
    return true;
}

std::string editor::VoxelEditCmd::describe() const{
    return displayName + " (" + CommandNames::entity(project, sceneId, entity) + ")";
}

size_t editor::VoxelEditCmd::memoryBytes() const{
    return sizeof(*this) + static_cast<size_t>(charged);
}
