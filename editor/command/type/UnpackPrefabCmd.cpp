// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UnpackPrefabCmd.h"

#include <algorithm>
#include <unordered_set>
#include "project/PrefabUnpackRecord.h"

using namespace jasperengine;

editor::UnpackPrefabCmd::UnpackPrefabCmd(Project* project, uint32_t sceneId, std::vector<Entity> targets, bool completely){
    this->project = project;
    this->sceneId = sceneId;
    this->targets = std::move(targets);
    this->completely = completely;

    SceneProject* sceneProject = project->getScene(sceneId);
    this->wasModified = sceneProject && sceneProject->isModified;
}

bool editor::UnpackPrefabCmd::canUnpack(const Project* project, uint32_t sceneId, const std::vector<Entity>& targets){
    // A running scene gets its components back on Stop, but not the prefab bookkeeping
    if (project->isAnyScenePlaying()) {
        return false;
    }
    std::vector<Entity> roots = project->getOutermostPrefabRoots(sceneId, targets);
    return std::any_of(roots.begin(), roots.end(), [](Entity root) { return root != NULL_ENTITY; });
}

bool editor::UnpackPrefabCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || project->isAnyScenePlaying()) {
        return false;
    }

    std::vector<Entity> roots;
    std::unordered_set<Entity> seen;
    for (Entity root : project->getOutermostPrefabRoots(sceneId, targets)) {
        if (root != NULL_ENTITY && seen.insert(root).second) {
            roots.push_back(root);
        }
    }

    records = project->unpackPrefabInstances(sceneId, roots, completely);
    if (records.empty()) {
        return false;
    }

    sceneProject->isModified = true;

    return true;
}

void editor::UnpackPrefabCmd::undo(){
    project->repackPrefabInstances(sceneId, std::move(records));
    records.clear();

    if (SceneProject* sceneProject = project->getScene(sceneId)) {
        sceneProject->isModified = wasModified;
    }
}
