// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AddEntityToPrefabCmd.h"
#include "component/Transform.h"
#include "pose/PoseEditSession.h"
#include "util/ProjectUtils.h"
#include "Stream.h"

using namespace jasperengine;

editor::AddEntityToPrefabCmd::AddEntityToPrefabCmd(Project* project, uint32_t sceneId, Entity entity, Entity parent, const std::filesystem::path& target){
    this->project = project;
    this->sceneId = sceneId;
    this->entity = entity;
    this->parent = parent;
    this->target = target;
    this->wasModified = project->getScene(sceneId)->isModified;

    this->originalParent = NULL_ENTITY;
    this->originalTransformIndex = 0;
    this->hasTransform = false;

    SceneProject* sceneProject = project->getScene(sceneId);
    if (sceneProject) {
        Transform* transform = sceneProject->scene->findComponent<Transform>(entity);
        if (transform) {
            this->hasTransform = true;
            this->originalParent = transform->parent;
            this->originalTransformIndex = ProjectUtils::getTransformIndex(sceneProject->scene, entity);
        }
    }
}

bool editor::AddEntityToPrefabCmd::execute(){
    nestedRoots.clear();
    // a posed character goes into the prefab with its rest bones
    PoseEditSession::suspendActiveFor(sceneId, {entity});
    if (SceneProject* sceneProject = project->getScene(sceneId); sceneProject && sceneProject->scene) {
        // the instance roots of the branch, its own included (nested ones are in their blocks)
        std::vector<Entity> pending{entity};
        for (size_t next = 0; next < pending.size(); next++) {
            const Entity current = pending[next];
            if (project->isPrefabInstanceRoot(sceneId, current)) {
                nestedRoots.emplace_back(current, Stream::encodeEntity(current, sceneProject->scene, project, sceneProject));
                continue;
            }
            auto transforms = sceneProject->scene->getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++) {
                if (transforms->getComponentFromIndex(i).parent == current) {
                    pending.push_back(transforms->getEntity(i));
                }
            }
        }
    }

    bool result = project->addEntityToPrefab(sceneId, entity, parent, false, target);

    if (result){
        project->getScene(sceneId)->isModified = true;
    }

    return result;
}

void editor::AddEntityToPrefabCmd::undo(){
    project->removeEntityFromPrefab(sceneId, entity, false);
    if (SceneProject* sceneProject = project->getScene(sceneId)) {
        for (const auto& [root, node] : nestedRoots) {
            project->reimportPrefabInstance(sceneProject, root, node);
        }
    }

    if (hasTransform) {
        SceneProject* sceneProject = project->getScene(sceneId);
        if (sceneProject) {
            ProjectUtils::moveEntityOrderByTransform(sceneProject->scene, sceneProject->entities, entity, originalParent, originalTransformIndex, true);
        }
    }

    project->getScene(sceneId)->isModified = wasModified;
}
