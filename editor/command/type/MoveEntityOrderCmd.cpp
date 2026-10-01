// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MoveEntityOrderCmd.h"

#include "Out.h"
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

    ProjectUtils::moveEntityOrderByTarget(sceneProject->scene, sceneProject->entities, source, target, type, oldParent, oldIndex, hasTransform);

    sceneProject->isModified = true;

    return true;
}

void editor::MoveEntityOrderCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);

    // The reverse of execute: the source index was taken after the other instances moved
    // (a GameObject leaving or entering one that removed a parent shifts the scene's
    // indices), and theirs before the source moved
    ProjectUtils::moveEntityOrderByIndex(sceneProject->scene, sceneProject->entities, source, oldParent, oldIndex, hasTransform);
    if (prefabMoveRecovery.size() > 0){
        project->undoMoveEntityInPrefab(sceneId, source, target, prefabMoveRecovery, false);
    }

    sceneProject->isModified = wasModified;
}

bool editor::MoveEntityOrderCmd::mergeWith(Command* otherCommand){
    return false;
}
