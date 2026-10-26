// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LatticePointsCmd.h"
#include "command/CommandNames.h"

#include "Catalog.h"
#include "util/EntityClipboard.h"

#include <string>

using namespace jasperengine;

editor::LatticePointsCmd::LatticePointsCmd(Project* project, uint32_t sceneId, Entity lattice, const std::vector<Vector3>& offsets, bool continuous){
    this->project = project;
    this->sceneId = sceneId;
    this->entity = lattice;
    this->newState.offsets = offsets;
    this->wholeState = false;
    this->continuous = continuous;
    this->wasModified = project->getScene(sceneId)->isModified;
}

editor::LatticePointsCmd::LatticePointsCmd(Project* project, uint32_t sceneId, Entity lattice, const LatticePointsState& state){
    this->project = project;
    this->sceneId = sceneId;
    this->entity = lattice;
    this->newState = state;
    this->wholeState = true;
    this->continuous = false;
    this->wasModified = project->getScene(sceneId)->isModified;
}

editor::LatticePointsState editor::LatticePointsCmd::capture(const LatticeComponent& lattice){
    LatticePointsState state;
    state.resolutionX = lattice.resolutionX;
    state.resolutionY = lattice.resolutionY;
    state.resolutionZ = lattice.resolutionZ;
    state.offsets = lattice.offsets;
    state.poses = lattice.poses;
    return state;
}

void editor::LatticePointsCmd::apply(LatticeComponent& lattice, const LatticePointsState& state, bool wholeState){
    if (wholeState){
        lattice.resolutionX = state.resolutionX;
        lattice.resolutionY = state.resolutionY;
        lattice.resolutionZ = state.resolutionZ;
        lattice.poses = state.poses;
        // jiggle and tweens run on the old layout: they restart from the new one
        lattice.dynamicOffsets.clear();
        lattice.jiggleVelocities.clear();
        lattice.tweening = false;
    }
    lattice.offsets = state.offsets;
    lattice.revision++;
}

bool editor::LatticePointsCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject){
        return false;
    }
    Scene* scene = sceneProject->scene;
    LatticeComponent* lattice = scene->findComponent<LatticeComponent>(entity);
    if (!lattice){
        return false;
    }

    const size_t expected = wholeState
        ? (size_t)newState.resolutionX * (size_t)newState.resolutionY * (size_t)newState.resolutionZ
        : (size_t)lattice->resolutionX * (size_t)lattice->resolutionY * (size_t)lattice->resolutionZ;
    if (newState.offsets.size() != expected){
        return false;
    }

    oldState = capture(*lattice);
    prefabUndo.capture(project, sceneId, entity);

    const bool sameLayout = !wholeState || (oldState.resolutionX == newState.resolutionX &&
        oldState.resolutionY == newState.resolutionY && oldState.resolutionZ == newState.resolutionZ);
    // the component as it was, for the overrides of an edit that is more than offsets
    YAML::Node before;
    if (wholeState){
        before = YAML::Node(YAML::NodeType::Map);
        before[Catalog::getComponentName(ComponentType::LatticeComponent, true)] =
            EntityClipboard::encodeComponent(scene, entity, ComponentType::LatticeComponent);
    }

    apply(*lattice, newState, wholeState);

    if (sameLayout){
        // Unity: an edited point of a prefab instance's lattice is an override of that point
        std::vector<std::string> changed;
        for (size_t i = 0; i < newState.offsets.size() && i < oldState.offsets.size(); i++){
            if (newState.offsets[i] != oldState.offsets[i]){
                changed.push_back("offsets[" + std::to_string(i) + "]");
            }
        }
        if (!changed.empty()){
            project->recordPrefabEdit(sceneId, entity, ComponentType::LatticeComponent, changed);
        }
    }
    if (wholeState){
        // resolution and poses: what differs from before (a resampled layout, a saved pose)
        project->recordPrefabComponentEdit(sceneId, entity, ComponentType::LatticeComponent, before);
        if (!sameLayout){
            // the instance's grid is its own as a whole: with only the changed axis overridden,
            // the prefab changing another axis later would reach the instance and leave it with
            // offsets laid out for another grid (a broken layout: no cage, no edits)
            project->recordPrefabEdit(sceneId, entity, ComponentType::LatticeComponent,
                                      {"offsets", "resolutionX", "resolutionY", "resolutionZ"});
        }
    }

    sceneProject->isModified = true;
    return true;
}

void editor::LatticePointsCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject){
        return;
    }
    LatticeComponent* lattice = sceneProject->scene->findComponent<LatticeComponent>(entity);
    if (!lattice){
        return;
    }

    apply(*lattice, oldState, wholeState);
    prefabUndo.restore(project, sceneId);

    sceneProject->isModified = wasModified;
}

bool editor::LatticePointsCmd::mergeWith(Command* otherCommand){
    LatticePointsCmd* otherCmd = dynamic_cast<LatticePointsCmd*>(otherCommand);
    if (!otherCmd || !continuous || !otherCmd->continuous || wholeState || otherCmd->wholeState){
        return false;
    }
    if (project != otherCmd->project || sceneId != otherCmd->sceneId || entity != otherCmd->entity){
        return false;
    }
    if (otherCmd->oldState.offsets.size() != newState.offsets.size()){
        return false;
    }

    // The older command holds the state a single undo has to restore
    oldState = otherCmd->oldState;
    prefabUndo.mergeOlder(otherCmd->prefabUndo);
    wasModified = wasModified && otherCmd->wasModified;
    return true;
}

std::string editor::LatticePointsCmd::describe() const{
    const std::string subject = " (" + CommandNames::entity(project, sceneId, entity) + ")";
    return (wholeState ? "Edit Lattice" : "Move Lattice Points") + subject;
}

bool editor::LatticePointsCmd::isNoOp() const{
    if (oldState.offsets != newState.offsets){
        return false;
    }
    if (!wholeState){
        return true;
    }
    if (oldState.resolutionX != newState.resolutionX || oldState.resolutionY != newState.resolutionY ||
        oldState.resolutionZ != newState.resolutionZ || oldState.poses.size() != newState.poses.size()){
        return false;
    }
    for (size_t i = 0; i < oldState.poses.size(); i++){
        if (oldState.poses[i].name != newState.poses[i].name || oldState.poses[i].offsets != newState.poses[i].offsets){
            return false;
        }
    }
    return true;
}

size_t editor::LatticePointsCmd::memoryBytes() const{
    size_t bytes = sizeof(*this);
    for (const LatticePointsState* state : {&oldState, &newState}){
        bytes += state->offsets.capacity() * sizeof(Vector3);
        for (const LatticePose& pose : state->poses){
            bytes += sizeof(LatticePose) + pose.name.capacity() + pose.offsets.capacity() * sizeof(Vector3);
        }
    }
    return bytes;
}
