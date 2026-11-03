// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RemovePreservedComponentCmd.h"

#include "StreamPreserved.h"

using namespace jasperengine;
using namespace jasperengine::editor;

RemovePreservedComponentCmd::RemovePreservedComponentCmd(Project* project, uint32_t sceneId, Entity entity, std::string component):
        project(project), sceneId(sceneId), entity(entity), component(std::move(component)) {}

bool RemovePreservedComponentCmd::execute() {
    SceneProject* sceneProject = project->getScene(sceneId);
    PreservedStore* store = sceneProject ? findPreservedStore(sceneProject->scene) : nullptr;
    if (!store) {
        return false;
    }
    authored = YAML::Clone(store->preservedComponent(entity, component));
    if (!authored) {
        return false;
    }
    index = store->preservedComponentIndex(entity, component);
    if (!store->removePreservedComponent(entity, component)) {
        return false;
    }
    wasModified = sceneProject->isModified;
    sceneProject->isModified = true;
    return true;
}

void RemovePreservedComponentCmd::undo() {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !authored) {
        return;
    }
    preservedStore(sceneProject->scene).restorePreservedComponent(entity, component, authored, index);
    sceneProject->isModified = wasModified;
}
