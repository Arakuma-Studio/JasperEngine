// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MoveEntityOrderCmd.h"

#include "Out.h"
#include "pose/PoseEditSession.h"
#include "util/ProjectUtils.h"

using namespace jasperengine;

editor::MoveEntityOrderCmd::MoveEntityOrderCmd(Project* project, uint32_t sceneId, Entity source, Entity target, InsertionType type){
    this->project = project;
    this->sceneId = sceneId;
    this->source = source;
    this->target = target;
    this->type = type;

    this->wasModified = project->getScene(sceneId)->isModified;
}

std::string editor::MoveEntityOrderCmd::getPrefabRestructureBlock(Project* project, uint32_t sceneId, Entity source,
        std::filesystem::path* prefabPath, Entity* instanceRoot){
    if (project->isEditingPrefabAsset(sceneId, source)){
        return std::string();  // Prefab Mode: the stage's own GameObjects are the asset's
    }
    PrefabObjectRef ref;
    if (!project->findPrefabObject(sceneId, source, ref) || ref.isRoot || !ref.instance){
        return std::string();  // no member of an instance: an added GameObject or an instance root
    }
    if (prefabPath){
        *prefabPath = ref.path;
    }
    if (instanceRoot){
        *instanceRoot = ref.instance->rootEntity;
    }
    return PREFAB_RESTRUCTURE_MESSAGE;
}

bool editor::MoveEntityOrderCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);

    std::string reason;
    if (!ProjectUtils::canMoveLockedEntityOrder(sceneProject->scene, source, target, type, &reason)){
        editor::Out::warning("Cannot move entity '%u': %s", source, reason.c_str());
        return false;
    }

    // Never the prefab asset (Unity): a member of an instance stays where its prefab puts it.
    // An instance root or an added GameObject moves like any GameObject: below an instance
    // it is an added GameObject of it, its place among the siblings saved with it.
    const std::string blocked = getPrefabRestructureBlock(project, sceneId, source);
    if (!blocked.empty()){
        editor::Out::warning("Cannot move '%s': %s", sceneProject->scene->getEntityName(source).c_str(), blocked.c_str());
        return false;
    }

    // the move keeps the world transform: from the rest, not from a preview of the old or the
    // new hierarchy (animation clips 9.R7)
    {
        const Transform* targetTransform = target != NULL_ENTITY && sceneProject->scene->isEntityCreated(target)
            ? sceneProject->scene->findComponent<Transform>(target) : nullptr;
        PoseEditSession::suspendActiveFor(sceneId, {source, target, targetTransform ? targetTransform->parent : NULL_ENTITY});
    }
    ProjectUtils::moveEntityOrderByTarget(sceneProject->scene, sceneProject->entities, source, target, type, oldParent, oldIndex, hasTransform);

    if (!executed){
        PlayerModelLayout::ensureNodeHierarchyForPlayer(project, sceneId, source, layoutReloads);
    }else{
        PlayerModelLayout::redoReloads(layoutReloads);
    }
    executed = true;

    sceneProject->isModified = true;

    return true;
}

void editor::MoveEntityOrderCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);

    // from the rest again (the new parent's preview, the old one's)
    {
        const Transform* sourceTransform = sceneProject->scene->findComponent<Transform>(source);
        PoseEditSession::suspendActiveFor(sceneId, {source, oldParent, sourceTransform ? sourceTransform->parent : NULL_ENTITY});
    }

    // the model reloads ran after the move
    PlayerModelLayout::undoReloads(layoutReloads);

    ProjectUtils::moveEntityOrderByIndex(sceneProject->scene, sceneProject->entities, source, oldParent, oldIndex, hasTransform);

    sceneProject->isModified = wasModified;
}
