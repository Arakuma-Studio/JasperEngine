// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DuplicateEntityCmd.h"
#include "component/Transform.h"
#include "component/PrefabComponent.h"
#include "pose/PoseEditSession.h"

#include "command/PlayChangeReview.h"

#include "Stream.h"
#include "Out.h"
#include "command/type/DeleteEntityCmd.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectUtils.h"
#include "util/BackgroundRelease.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;

editor::DuplicateEntityCmd::DuplicateEntityCmd(Project* project, uint32_t sceneId, const std::vector<Entity>& entities){
    this->project = project;
    this->sceneId = sceneId;
    this->sourceEntities = entities;
    this->wasModified = project->getScene(sceneId)->isModified;
}

bool editor::DuplicateEntityCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);

    if (!sceneProject || sourceEntities.empty()){
        return false;
    }
    // a posed character goes back to its rest bones first: they are what the copies get (also on redo)
    PoseEditSession::suspendActiveFor(sceneId, sourceEntities);

    Scene* scene = sceneProject->scene;

    std::vector<Entity> topLevel = Project::getTopLevelEntities(scene, sourceEntities);
    if (topLevel.empty()){
        return false;
    }

    for (Entity entity : topLevel){
        if (ProjectUtils::getModelBranchOwner(scene, entity) != NULL_ENTITY){
            Out::warning("Cannot duplicate entity '%u'. Duplicate the whole model instead.", entity);
            return false;
        }
    }

    // Record parents of source entities
    std::vector<Entity> sourceParents;
    for (Entity entity : topLevel) {
        Transform* transform = scene->findComponent<Transform>(entity);
        sourceParents.push_back(transform ? transform->parent : NULL_ENTITY);
    }

    // Encode then decode to create duplicates.
    // Prefab members (non-root) are skipped by the encoder when project context is set,
    // so encode them without project context to treat as plain entities.
    // They become prefab local entities automatically at save time.
    auto encodeEntityLocal = [&](Entity e) {
        bool isPrefabMember = !project->findPrefabPathFor(sceneId, e).empty()
                              && !scene->findComponent<PrefabComponent>(e);
        return Stream::encodeEntity(e, scene, isPrefabMember ? nullptr : project, isPrefabMember ? nullptr : sceneProject);
    };

    // A model's animation and track entities have no Transform, so the encoder never reaches
    // them. Copied with the roots, the same set DeleteEntityCmd removes with them.
    std::vector<Entity> virtualChildren = ProjectUtils::getVirtualChildren(scene, topLevel);

    std::vector<Entity> encodedRoots = topLevel;
    encodedRoots.insert(encodedRoots.end(), virtualChildren.begin(), virtualChildren.end());

    YAML::Node encoded;
    if (encodedRoots.size() == 1) {
        encoded = encodeEntityLocal(encodedRoots[0]);
    } else {
        encoded["type"] = "Prefab";
        YAML::Node membersNode(YAML::NodeType::Sequence);
        for (Entity entity : encodedRoots) {
            membersNode.push_back(encodeEntityLocal(entity));
        }
        encoded["members"] = membersNode;
    }

    // Decode keeping the source entity IDs: every source entity is still alive, so recreateEntity
    // fails and a fresh ID is allocated, recording old->new in entityRemap. Stripping the IDs
    // instead (the previous approach) also produced fresh entities but discarded that mapping,
    // leaving internal entity references (e.g. a model's meshNodesMapping/skeleton/bones, joint
    // bodies, script targets) pointing at the source entities. For multi-node models that meant
    // the copy's ModelComponent kept mapping to the source's child meshes, so on reload it
    // reused/reloaded those instead of populating its own children and the duplicate rendered
    // empty (and saving the broken state could hang).
    std::unordered_map<Entity, Entity> entityRemap;
    createdEntities = Stream::decodeEntitySelection(encoded, scene, &sceneProject->entities, project, sceneProject, NULL_ENTITY, true, &entityRemap);
    // Only needed for the decode: freed away from the main thread
    BackgroundRelease::release(encoded);
    if (createdEntities.empty()){
        return false;
    }

    // Repoint internal references from the source entities to their duplicates. clearUnmapped is
    // false so references outside the duplicated set keep pointing at their original target.
    Project::remapEntityProperties(scene, createdEntities, entityRemap, false);

    // bind framebuffers of duplicated camera-linked textures (all of them for a duplicated camera)
    CameraTextureLink::resolve(scene, createdEntities);

    // The remap pairs each source root with its copy: a parentless scan cannot, now that
    // transform-less virtual children are copied too.
    std::vector<Entity> createdTopLevel;
    for (Entity source : topLevel) {
        auto it = entityRemap.find(source);
        if (it != entityRemap.end())
            createdTopLevel.push_back(it->second);
    }

    lastSelected = project->getSelectedEntities(sceneId);

    std::unordered_set<std::string> existingNames;
    for (Entity e : sceneProject->entities) {
        existingNames.insert(scene->getEntityName(e));
    }
    UniqueEntityNames uniqueNames(std::move(existingNames));

    // Re-parent, rename, and select top-level duplicates
    const size_t pairs = std::min(topLevel.size(), createdTopLevel.size());
    std::vector<Entity> selection;
    std::unordered_set<Entity> selected;
    selection.reserve(pairs);
    for (size_t i = 0; i < pairs; i++) {
        Entity duplicated = createdTopLevel[i];

        if (sourceParents[i] != NULL_ENTITY) {
            scene->addEntityChild(sourceParents[i], duplicated, false);
        } else if (scene->findComponent<Transform>(duplicated) && scene->findComponent<Transform>(topLevel[i])) {
            // A copy at the root goes right after its source's branch in the Transform order too,
            // where the tracked list (below) and so the file put it: decoded at the end of the
            // array, the Hierarchy showed it last and the scene opened again with it after its source
            scene->moveChildToIndex(duplicated, scene->findBranchLastIndex(topLevel[i]) + 1);
        }

        scene->setEntityName(duplicated, uniqueNames.make(scene->getEntityName(topLevel[i])));
        // Distinct copies of top-level sources: none holds another, so none drops out of
        // the selection as a selected child would
        if (selected.insert(duplicated).second)
            selection.push_back(duplicated);
    }
    project->replaceSelectedEntities(sceneId, selection);

    // Each duplicate right after its source in the tracked list (dropped when the source
    // is not in it), in one pass
    {
        std::unordered_map<Entity, Entity> duplicateOf;
        std::unordered_set<Entity> placed;
        for (size_t i = 0; i < pairs; i++) {
            duplicateOf[topLevel[i]] = createdTopLevel[i];
            placed.insert(createdTopLevel[i]);
        }
        std::vector<Entity>& entities = sceneProject->entities;
        std::vector<Entity> ordered;
        ordered.reserve(entities.size());
        for (Entity entity : entities) {
            if (placed.count(entity)) {
                continue;
            }
            ordered.push_back(entity);
            auto duplicate = duplicateOf.find(entity);
            if (duplicate != duplicateOf.end()) {
                ordered.push_back(duplicate->second);
            }
        }
        entities.swap(ordered);
    }
    // the copies' descendants were decoded at the end of the list: they join their branch,
    // parents before children as the Transform array has them
    ProjectUtils::sortEntitiesByTransformOrder(scene, sceneProject->entities);

    // Update all created entities (exclude scene-wide flags which reload ALL meshes and recapture ALL probes)
    for (Entity entity : createdEntities) {
        Catalog::updateEntity(scene, entity, ~UpdateFlags_SceneWide);
    }

    sceneProject->isModified = true;

    editor::Out::info("Duplicated %zu %s", topLevel.size(), topLevel.size() == 1 ? "entity" : "entities");

    return true;
}

void editor::DuplicateEntityCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);

    if (sceneProject){
        // Delete created entities in reverse order
        std::vector<Entity> entitiesToDestroy(createdEntities.rbegin(), createdEntities.rend());
        DeleteEntityCmd::destroyEntities(sceneProject->scene, entitiesToDestroy,
            sceneProject->entities, project, sceneId);

        if (!lastSelected.empty()){
            // What adding them one by one gives: each once, without the ones inside the
            // Transform branch of another
            Scene* scene = sceneProject->scene;
            auto transforms = scene->getComponentArray<Transform>();
            std::vector<Entity> selection;
            std::unordered_set<Entity> selected;
            for (Entity entity : lastSelected){
                if (entity != NULL_ENTITY && selected.insert(entity).second){
                    selection.push_back(entity);
                }
            }
            std::unordered_set<Entity> nested;
            for (Entity entity : selection){
                if (scene->getSignature(entity).test(scene->getComponentId<Transform>())){
                    size_t firstIndex = transforms->getIndex(entity);
                    size_t branchIndex = scene->findBranchLastIndex(entity);
                    for (size_t t = firstIndex + 1; t <= branchIndex; t++){
                        Entity child = transforms->getEntity(t);
                        if (selected.count(child)){
                            nested.insert(child);
                        }
                    }
                }
            }
            selection.erase(std::remove_if(selection.begin(), selection.end(), [&nested](Entity entity) {
                return nested.count(entity) != 0;
            }), selection.end());
            project->replaceSelectedEntities(sceneId, selection);
        }

        sceneProject->isModified = wasModified;
    }
}

std::vector<Entity> editor::DuplicateEntityCmd::getCreatedEntities() const{
    return createdEntities;
}

void editor::DuplicateEntityCmd::collectKeepCommands(KeepContext& context) const{
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject){
        return;
    }
    for (Entity root : Project::getTopLevelEntities(sceneProject->scene, createdEntities)){
        context.addCreated(root);
    }
}
