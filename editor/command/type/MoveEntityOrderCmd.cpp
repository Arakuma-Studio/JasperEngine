// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MoveEntityOrderCmd.h"

#include "Out.h"
#include "animation/PreviewOwner.h"
#include "pose/PoseEditSession.h"
#include "subsystem/RenderSystem.h"
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

void editor::MoveEntityOrderCmd::prepareReparent(Scene* scene, Entity fromParent, Entity toParent){
    // a reorder among the same siblings recomputes nothing: the previews, the pose (and Record) go on
    if (fromParent == toParent) return;
    // the local transform is recomputed from the world under the new parent: with a previewed or
    // posed bone in the branch, or above the old or the new place (a prop held by a posed hand),
    // that world is the rest's, not the preview's (animation clips 9.R7) nor the pose's
    PreviewOwner::stopFor(sceneId, {source, fromParent, toParent}, nullptr, PoseEditSession::active());
    PoseEditSession::suspendActiveForMove(sceneId, source, fromParent, toParent);
    // the restores put the bones' rest back; what hangs below them takes its world from that rest
    // now, parents first: the moved GameObject's chain (the world it keeps) and the new parent's
    // (the world its new local is taken against)
    std::shared_ptr<RenderSystem> render = scene->getSystem<RenderSystem>();
    if (!render) return;
    for (Entity from : {source, toParent}){
        std::vector<Entity> chain;
        for (Entity entity = from; entity != NULL_ENTITY && chain.size() < 4096;){
            const Transform* transform = scene->findComponent<Transform>(entity);
            if (!transform) break;
            chain.push_back(entity);
            entity = transform->parent;
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it){
            if (Transform* transform = scene->findComponent<Transform>(*it)) render->updateTransform(*transform, *it);
        }
    }
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

    {
        const Entity newParent = type == InsertionType::INTO ? target : ProjectUtils::getEffectiveParent(sceneProject->scene, target);
        const Transform* sourceTransform = sceneProject->scene->findComponent<Transform>(source);
        if (sourceTransform){
            prepareReparent(sceneProject->scene, sourceTransform->parent, newParent);
        }
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

    // back under the old parent, from the rest again (the new parent's preview or pose, the old one's)
    if (hasTransform){
        if (const Transform* sourceTransform = sceneProject->scene->findComponent<Transform>(source)){
            prepareReparent(sceneProject->scene, sourceTransform->parent, oldParent);
        }
    }

    // the model reloads ran after the move
    PlayerModelLayout::undoReloads(layoutReloads);

    ProjectUtils::moveEntityOrderByIndex(sceneProject->scene, sceneProject->entities, source, oldParent, oldIndex, hasTransform);

    sceneProject->isModified = wasModified;
}
