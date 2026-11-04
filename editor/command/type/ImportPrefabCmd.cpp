// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ImportPrefabCmd.h"
#include "component/PrefabComponent.h"
#include "sceneview/SurfacePlacement.h"

#include "editor/Out.h"
#include "Stream.h"
#include "util/ProjectUtils.h"
#include "util/WorldTransforms.h"

using namespace jasperengine;

editor::ImportPrefabCmd::ImportPrefabCmd(Project* project, uint32_t sceneId, const fs::path& filepath, Entity parent, bool needSaveScene){
    this->project = project;
    this->sceneId = sceneId;
    this->filepath = filepath;
    this->parent = parent;
    this->needSaveScene = needSaveScene;
    this->rootEntity = NULL_ENTITY;
    this->wasModified = project->getScene(sceneId)->isModified;
    this->addedToParentPrefab = false;
}

bool editor::ImportPrefabCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);

    if (!sceneProject){
        return false;
    }

    lastSelected = project->getSelectedEntities(sceneId);

    Scene* scene = sceneProject->scene;
    // the instance's world values once it is in and placed (WorldTransforms.h)
    WorldTransformScope worldTransforms(scene);

    // The prefab is the asset with this GUID: a redo after it was renamed finds it
    if (guid.empty()) {
        guid = project->getPrefabGuid(filepath);
    } else {
        std::filesystem::path current = project->resolvePrefabPath(guid, filepath);
        if (!current.empty()) {
            filepath = current;
        }
    }

    // Prefab Mode: a prefab cannot hold itself, nor one that holds it
    if (!project->canInstantiatePrefabIn(sceneId, filepath)) {
        Out::error("Prefab %s cannot go into this prefab: it would contain itself", filepath.string().c_str());
        return false;
    }

    // The prefab, read now: the instance's root takes its root GameObject's name
    Prefab* prefab = project->loadPrefabAsset(filepath);
    if (!prefab) {
        Out::error("Failed to read prefab %s", filepath.string().c_str());
        return false;
    }
    std::string rootName = prefab->registry->getEntityName(prefab->root);
    if (rootName.empty()) {
        rootName = filepath.stem().string();
    }
    if (rootName.empty()) {
        rootName = "Prefab";
    }

    // Create root entity with PrefabComponent (Preserve ID for Undo/Redo); the import gives
    // it the prefab root's components, Transform included
    if (rootEntity == NULL_ENTITY){
        rootEntity = scene->createUserEntity();
    }else{
        if (!scene->recreateEntity(rootEntity)){
            rootEntity = scene->createUserEntity();
        }
    }

    PrefabComponent prefabComp;
    prefabComp.name = filepath.stem().string();
    prefabComp.path = filepath.string();
    prefabComp.guid = guid;
    scene->addComponent<PrefabComponent>(rootEntity, prefabComp);

    scene->setEntityName(rootEntity, ProjectUtils::makeUniqueEntityName(scene, sceneProject->entities, rootName));
    sceneProject->entities.push_back(rootEntity);

    // Import prefab contents
    importedEntities = project->importPrefab(sceneProject, &sceneProject->entities, filepath, rootEntity, needSaveScene);

    prefab = project->getPrefab(filepath);
    const Prefab::Instance* instance = prefab ? prefab->getInstance(sceneId, rootEntity) : nullptr;
    if (!instance || instance->rootEntity != rootEntity){
        // Clean up root if import failed
        project->clearMissingPrefab(sceneId, rootEntity);
        scene->destroyEntity(rootEntity);
        scene->compactComponents();
        auto it = std::find(sceneProject->entities.begin(), sceneProject->entities.end(), rootEntity);
        if (it != sceneProject->entities.end()) {
            sceneProject->entities.erase(it);
        }
        rootEntity = NULL_ENTITY;
        return false;
    }

    // Below the drop target, at the prefab root's own local placement (Unity's way)
    if (parent != NULL_ENTITY && scene->findComponent<Transform>(parent) && scene->findComponent<Transform>(rootEntity)) {
        scene->addEntityChild(parent, rootEntity, false);
        ProjectUtils::sortEntitiesByTransformOrder(scene, sceneProject->entities);
    }

    // Applied after the import so nothing the prefab brings in overwrites it.
    if (hasPlacement){
        if (Transform* rootTransform = scene->findComponent<Transform>(rootEntity)){
            rootTransform->position = placementPosition;
            rootTransform->rotation = placementRotation;
            rootTransform->scale = placementScale;
            rootTransform->needUpdate = true;
        }
    }

    if (Transform* rootTransform = scene->findComponent<Transform>(rootEntity)){
        importedRootRotation = rootTransform->rotation;
    }
    if (hasDropPosition){
        if (Transform* rootTransform = scene->findComponent<Transform>(rootEntity)){
            const float z = rootTransform->position.z;
            rootTransform->position = dropPosition;
            if (dropKeepsZ){
                rootTransform->position.z = z;
            }
            if (hasDropSurface){
                Vector3 position;
                Quaternion rotation;
                sceneview::restOnSurface(dropPosition, dropNormal, sceneview::familyLocalBounds(scene, rootEntity), rootTransform->scale,
                                         rootTransform->rotation, dropAlign, position, rotation);
                rootTransform->position = position;
                rootTransform->rotation = rotation;
            }
            rootTransform->needUpdate = true;
        }
    }

    // Prefab Mode nests the imported prefab in the edited asset. Scene and brush
    // placements remain instance edits.
    addedToParentPrefab = false;
    if ((addToParentPrefab || project->isEditingPrefabAsset(sceneId, parent)) && parent != NULL_ENTITY && !hasPlacement &&
            project->isEntityInPrefab(sceneId, parent)) {
        addedToParentPrefab = project->addEntityToPrefab(sceneId, rootEntity, parent, false);
    }

    if (!quiet){
        // Select the root entity
        project->setSelectedEntity(sceneId, rootEntity);

        if (ImGui::GetCurrentContext()){
            ImGui::SetWindowFocus(("###Scene" + std::to_string(sceneId)).c_str());
        }

        editor::Out::info("Imported prefab from '%s' to scene '%s'", filepath.string().c_str(), sceneProject->name.c_str());
    }

    return true;
}

void editor::ImportPrefabCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject) {
        return;
    }

    // Remove from parent prefab first if it was added
    if (addedToParentPrefab) {
        project->removeEntityFromPrefab(sceneId, rootEntity, false);
        addedToParentPrefab = false;
    }

    // Unimport the prefab (removes instance + destroys entities), at its path now
    if (sceneProject->scene) {
        if (const PrefabComponent* component = sceneProject->scene->findComponent<PrefabComponent>(rootEntity)) {
            if (!component->path.empty()) {
                filepath = component->path;
            }
        }
    }
    project->unimportPrefab(sceneId, filepath, rootEntity, importedEntities);

    // Restore previous selection
    if (!quiet && !lastSelected.empty()) {
        project->replaceSelectedEntities(sceneId, lastSelected);
    }

    // Restore modified state
    sceneProject->isModified = wasModified;
}

std::vector<Entity> editor::ImportPrefabCmd::getImportedEntities() const{
    return importedEntities;
}

void editor::ImportPrefabCmd::setPlacement(const Vector3& position, const Quaternion& rotation, const Vector3& scale){
    hasPlacement = true;
    placementPosition = position;
    placementRotation = rotation;
    placementScale = scale;
}

void editor::ImportPrefabCmd::setQuiet(bool quiet){
    this->quiet = quiet;
}

void editor::ImportPrefabCmd::setDropSurface(const Vector3& normal, bool align){
    hasDropSurface = true;
    dropNormal = normal;
    dropAlign = align;
}

void editor::ImportPrefabCmd::setDropPosition(const Vector3& position, bool keepZ){
    hasDropPosition = true;
    dropKeepsZ = keepZ;
    dropPosition = position;
}

Entity editor::ImportPrefabCmd::getRootEntity() const{
    return rootEntity;
}

void editor::ImportPrefabCmd::setAddToParentPrefab(bool add){
    this->addToParentPrefab = add;
}
