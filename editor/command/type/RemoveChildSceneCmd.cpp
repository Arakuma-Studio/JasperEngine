// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RemoveChildSceneCmd.h"
#include "Out.h"

#include <algorithm>

using namespace jasperengine;

editor::RemoveChildSceneCmd::RemoveChildSceneCmd(Project* project, uint32_t sceneId, uint32_t childSceneId){
    this->project = project;
    this->sceneId = sceneId;
    this->childSceneId = childSceneId;
    this->index = -1;

    SceneProject* sceneProject = project->getScene(sceneId);
    if (sceneProject){
        this->wasModified = sceneProject->isModified;
        auto& childScenes = sceneProject->childScenes;
        auto it = std::find_if(childScenes.begin(), childScenes.end(),
            [childSceneId](const ChildSceneRef& childScene) {
                return childScene.id == childSceneId;
            });
        if (it != childScenes.end()){
            this->index = std::distance(childScenes.begin(), it);
            this->childSceneRef = *it;
        }
    }
}

bool editor::RemoveChildSceneCmd::execute() {
    if (index == -1) return false;
    project->removeChildScene(sceneId, childSceneId);
    return true;
}

void editor::RemoveChildSceneCmd::undo() {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (sceneProject && index != -1){
        auto& childScenes = sceneProject->childScenes;
        if (index <= childScenes.size()){
            childScenes.insert(childScenes.begin() + index, childSceneRef);
            sceneProject->isModified = wasModified;
            // Restores an inline child, so the Engine layers need rebuilding.
            project->markParentScenesNeedUpdate(childSceneId);
        }
    }
}

// Another scene may have taken this one as a child since the removal
bool editor::RemoveChildSceneCmd::canUndo() const {
    if (index == -1 || !project->wouldCreateChildSceneCycle(sceneId, childSceneId)) {
        return true;
    }

    const SceneProject* sceneProject = project->getScene(sceneId);
    const SceneProject* childScene = project->getScene(childSceneId);
    if (sceneProject && childScene) {
        const char* name = sceneProject->name.c_str();
        const char* childName = childScene->name.c_str();
        Out::error("Cannot restore '%s' in '%s': '%s' now contains '%s'", childName, name, childName, name);
    }
    return false;
}
