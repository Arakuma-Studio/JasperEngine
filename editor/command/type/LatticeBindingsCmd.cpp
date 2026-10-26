// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LatticeBindingsCmd.h"
#include "command/CommandNames.h"

#include "Catalog.h"
#include "util/EntityClipboard.h"
#include "util/ProjectUtils.h"

using namespace jasperengine;

editor::LatticeBindingsCmd::LatticeBindingsCmd(Project* project, uint32_t sceneId, Entity target, const std::vector<LatticeBinding>& bindings, bool includeChildren){
    this->project = project;
    this->sceneId = sceneId;
    this->entity = target;
    this->newBindings = bindings;
    this->newIncludeChildren = includeChildren;
    this->wasModified = project->getScene(sceneId)->isModified;
}

bool editor::LatticeBindingsCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene->isEntityCreated(entity)){
        return false;
    }
    Scene* scene = sceneProject->scene;

    prefabUndo.capture(project, sceneId, entity);

    addedComponent = false;
    YAML::Node before;
    if (!scene->findComponent<LatticeModifierComponent>(entity)){
        ProjectUtils::addEntityComponent(scene, entity, ComponentType::LatticeModifierComponent, sceneProject->entities);
        addedComponent = true;
    }else{
        before = YAML::Node(YAML::NodeType::Map);
        before[Catalog::getComponentName(ComponentType::LatticeModifierComponent, true)] =
            EntityClipboard::encodeComponent(scene, entity, ComponentType::LatticeModifierComponent);
    }

    LatticeModifierComponent* modifier = scene->findComponent<LatticeModifierComponent>(entity);
    if (!modifier){
        return false;
    }

    oldBindings = modifier->bindings;
    oldIncludeChildren = modifier->includeChildren;
    modifier->bindings = newBindings;
    modifier->includeChildren = newIncludeChildren;
    // a new list may fit in LATTICE_MAX_PER_MESH again: warn anew when it does not
    modifier->warnedOverflow = false;

    if (addedComponent){
        project->recordPrefabComponentAdded(sceneId, entity, ComponentType::LatticeModifierComponent);
    }else{
        project->recordPrefabComponentEdit(sceneId, entity, ComponentType::LatticeModifierComponent, before);
    }

    sceneProject->isModified = true;
    return true;
}

void editor::LatticeBindingsCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene->isEntityCreated(entity)){
        return;
    }
    Scene* scene = sceneProject->scene;

    if (addedComponent){
        ProjectUtils::removeEntityComponent(scene, entity, ComponentType::LatticeModifierComponent, sceneProject->entities);
    }else if (LatticeModifierComponent* modifier = scene->findComponent<LatticeModifierComponent>(entity)){
        modifier->bindings = oldBindings;
        modifier->includeChildren = oldIncludeChildren;
        modifier->warnedOverflow = false;
    }
    prefabUndo.restore(project, sceneId);

    sceneProject->isModified = wasModified;
}

std::string editor::LatticeBindingsCmd::describe() const{
    const std::string subject = " (" + CommandNames::entity(project, sceneId, entity) + ")";
    if (newBindings.size() > oldBindings.size() || addedComponent){
        return "Bind Lattice" + subject;
    }
    if (newBindings.size() < oldBindings.size()){
        return "Unbind Lattice" + subject;
    }
    return "Set Lattice Bindings" + subject;
}

bool editor::LatticeBindingsCmd::isNoOp() const{
    if (addedComponent || oldIncludeChildren != newIncludeChildren || oldBindings.size() != newBindings.size()){
        return false;
    }
    for (size_t i = 0; i < oldBindings.size(); i++){
        const LatticeBinding& a = oldBindings[i];
        const LatticeBinding& b = newBindings[i];
        if (a.lattice != b.lattice || a.weight != b.weight || a.stage != b.stage || a.submesh != b.submesh || a.mask != b.mask){
            return false;
        }
    }
    return true;
}

size_t editor::LatticeBindingsCmd::memoryBytes() const{
    return sizeof(*this) + (oldBindings.capacity() + newBindings.capacity()) * sizeof(LatticeBinding);
}
