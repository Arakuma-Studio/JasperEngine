// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SplineEditCmd.h"
#include "command/CommandNames.h"

#include "Catalog.h"

#include <string>

using namespace jasperengine;

namespace {

    bool sameKnot(const spline::SplineKnot& a, const spline::SplineKnot& b){
        return a.position == b.position && a.tangentIn == b.tangentIn && a.tangentOut == b.tangentOut &&
               a.radius == b.radius && a.rollDeg == b.rollDeg && a.mode == b.mode;
    }

}

bool editor::SplineEditState::operator==(const SplineEditState& other) const{
    if (type != other.type || closed != other.closed || upAxis != other.upAxis || knots.size() != other.knots.size()){
        return false;
    }
    for (size_t i = 0; i < knots.size(); i++){
        if (!sameKnot(knots[i], other.knots[i])){
            return false;
        }
    }
    return true;
}

editor::SplineEditCmd::SplineEditCmd(Project* project, uint32_t sceneId, Entity entity, SplineEditState state, SplineEditKind kind,
                                     bool continuous, std::vector<uint32_t> selection)
    : project(project), sceneId(sceneId), entity(entity), newState(std::move(state)), kind(kind), continuous(continuous),
      selection(std::move(selection)){
    SceneProject* sceneProject = project->getScene(sceneId);
    wasModified = sceneProject && sceneProject->isModified;
}

editor::SplineEditState editor::SplineEditCmd::capture(const SplineComponent& spline){
    SplineEditState state;
    state.type = spline.spline.type();
    state.closed = spline.spline.closed();
    state.upAxis = spline.spline.upAxis();
    state.knots = spline.spline.knots();
    return state;
}

void editor::SplineEditCmd::apply(SplineComponent& spline, const SplineEditState& state){
    spline.spline.setType(state.type);
    spline.spline.setClosed(state.closed);
    spline.spline.setUpAxis(state.upAxis);
    spline.spline.setKnots(state.knots);
}

bool editor::SplineEditCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject){
        return false;
    }
    Scene* scene = sceneProject->scene;
    SplineComponent* spline = scene->findComponent<SplineComponent>(entity);
    if (!spline){
        return false;
    }

    oldState = capture(*spline);
    prefabUndo.capture(project, sceneId, entity);
    apply(*spline, newState);

    // Unity: an edited knot of a prefab instance's spline is an override of that knot's
    // property; a knot added or removed makes the instance's knot array its own
    std::vector<std::string> changed;
    if (oldState.knots.size() != newState.knots.size()){
        changed.push_back("knots");
    }else{
        static const char* const fields[] = {"position", "tangentIn", "tangentOut", "radius", "roll", "mode"};
        for (size_t i = 0; i < newState.knots.size(); i++){
            const spline::SplineKnot& a = oldState.knots[i];
            const spline::SplineKnot& b = newState.knots[i];
            const bool differs[] = {a.position != b.position, a.tangentIn != b.tangentIn, a.tangentOut != b.tangentOut,
                                    a.radius != b.radius, a.rollDeg != b.rollDeg, a.mode != b.mode};
            for (size_t f = 0; f < 6; f++){
                if (differs[f]){
                    changed.push_back("knots[" + std::to_string(i) + "]." + fields[f]);
                }
            }
        }
    }
    if (oldState.type != newState.type) changed.push_back("type");
    if (oldState.closed != newState.closed) changed.push_back("closed");
    if (oldState.upAxis != newState.upAxis) changed.push_back("upAxis");
    if (!changed.empty()){
        project->recordPrefabEdit(sceneId, entity, ComponentType::SplineComponent, changed);
    }

    Catalog::markChanged(scene, entity, ComponentType::SplineComponent);
    sceneProject->isModified = true;
    return true;
}

void editor::SplineEditCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject){
        return;
    }
    SplineComponent* spline = sceneProject->scene->findComponent<SplineComponent>(entity);
    if (!spline){
        return;
    }

    apply(*spline, oldState);
    prefabUndo.restore(project, sceneId);
    Catalog::markChanged(sceneProject->scene, entity, ComponentType::SplineComponent);
    sceneProject->isModified = wasModified;
}

bool editor::SplineEditCmd::mergeWith(Command* otherCommand){
    SplineEditCmd* other = dynamic_cast<SplineEditCmd*>(otherCommand);
    if (!other || !continuous || !other->continuous || kind != other->kind){
        return false;
    }
    if (project != other->project || sceneId != other->sceneId || entity != other->entity || selection != other->selection){
        return false;
    }

    // the older command holds the state a single undo has to restore
    oldState = other->oldState;
    prefabUndo.mergeOlder(other->prefabUndo);
    wasModified = wasModified && other->wasModified;
    return true;
}

std::string editor::SplineEditCmd::describe() const{
    const char* verb = "Edit Spline";
    switch (kind){
        case SplineEditKind::Draw: verb = "Draw Spline"; break;
        case SplineEditKind::MoveKnots: verb = "Move Spline Knots"; break;
        case SplineEditKind::InsertKnot: verb = "Insert Spline Knot"; break;
        case SplineEditKind::DeleteKnots: verb = "Delete Spline Knots"; break;
        case SplineEditKind::EditTangent: verb = "Edit Spline Tangent"; break;
        case SplineEditKind::Edit: verb = "Edit Spline"; break;
    }
    return std::string(verb) + " (" + CommandNames::entity(project, sceneId, entity) + ")";
}

size_t editor::SplineEditCmd::memoryBytes() const{
    return sizeof(*this) + (oldState.knots.capacity() + newState.knots.capacity()) * sizeof(spline::SplineKnot) +
           selection.capacity() * sizeof(uint32_t);
}
