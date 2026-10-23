// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LatticeBindingsCmd.h"

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
