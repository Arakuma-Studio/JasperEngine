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

bool editor::MoveEntityOrderCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);

    std::string reason;
    if (!ProjectUtils::canMoveLockedEntityOrder(sceneProject->scene, source, target, type, &reason)){
        editor::Out::warning("Cannot move entity '%u': %s", source, reason.c_str());
        return false;
    }

    if (project->isEntityInPrefab(sceneId, source)){

        fs::path sourcePrefabPath = project->findPrefabPathFor(sceneId, source);
        fs::path targetPrefabPath = project->findPrefabPathFor(sceneId, target);

        if (type == InsertionType::INTO){
            if (!project->isEntityInPrefab(sceneId, target)){
                Out::error("Cannot move prefab entity %u into target %u", source, target);
                return false;
            }
        }else{
            Transform* transformTarget = sceneProject->scene->findComponent<Transform>(target);
            if (transformTarget){
                fs::path parentPrefabPath = project->findPrefabPathFor(sceneId, transformTarget->parent);

                Prefab* sourcePrefab = project->getPrefab(sourcePrefabPath);
                bool isSourceRoot = sourcePrefab && (sourcePrefab->getRootEntity(sceneId, source) == source);

                if (parentPrefabPath != sourcePrefabPath && targetPrefabPath != sourcePrefabPath && !isSourceRoot){
                    Out::error("Cannot move prefab entity %u outside prefab", source);
                    return false;
                }
            }
        }

        if (targetPrefabPath == sourcePrefabPath){
            prefabMoveRecovery = project->moveEntityFromPrefab(sceneId, source, target, type, false);
        }
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