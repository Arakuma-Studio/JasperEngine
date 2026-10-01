// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ImportPrefabCmd.h"

#include "editor/Out.h"
#include "Stream.h"
#include "util/ProjectUtils.h"

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

    // Create root entity with PrefabComponent (Preserve ID for Undo/Redo)
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
    scene->addComponent<PrefabComponent>(rootEntity, prefabComp);

    // Check if prefab has top-level entities with transforms to decide if root needs one
    Prefab* prefab = project->getPrefab(filepath);
    bool hasTopLevelTransform = false;

    if (!prefab || prefab->registryEntities.empty()) {
        // Prefab not loaded yet — load from disk to check
        try {
            std::filesystem::path fullPrefabPath = project->getProjectPath() / filepath;
            YAML::Node node = YAML::LoadFile(fullPrefabPath.string());
            // Temporarily decode to check for transforms
            EntityRegistry tempRegistry;
            std::vector<Entity> tempEntities;
            Stream::decodeEntitySelection(node, &tempRegistry, &tempEntities);
            std::vector<Entity> topLevel = Project::getTopLevelEntities(&tempRegistry, tempEntities);
            for (Entity e : topLevel) {
                if (tempRegistry.getSignature(e).test(tempRegistry.getComponentId<Transform>())) {
                    hasTopLevelTransform = true;
                    break;
                }
            }
        } catch (const std::exception& e) {
            Out::error("Failed to check prefab file: %s", e.what());
            scene->destroyEntity(rootEntity);
            rootEntity = NULL_ENTITY;
            return false;
        }
    } else {
        std::vector<Entity> topLevel = Project::getTopLevelEntities(prefab->registry.get(), prefab->registryEntities);
        for (Entity e : topLevel) {
            if (prefab->registry->getSignature(e).test(prefab->registry->getComponentId<Transform>())) {
                hasTopLevelTransform = true;
                break;
            }
        }
    }

    if (hasTopLevelTransform) {
        scene->addComponent<Transform>(rootEntity, {});
    }

    // Set name
    std::string rootName = filepath.stem().string();
    if (rootName.empty()) {
        rootName = "Prefab";
    }
    scene->setEntityName(rootEntity, ProjectUtils::makeUniqueEntityName(scene, sceneProject->entities, rootName));
    sceneProject->entities.push_back(rootEntity);

    // Reparent under drop target if provided
    if (parent != NULL_ENTITY && hasTopLevelTransform) {
        if (scene->findComponent<Transform>(parent)) {
            scene->addEntityChild(parent, rootEntity, true);
        }
    }

    // Import prefab contents
    importedEntities = project->importPrefab(sceneProject, &sceneProject->entities, filepath, rootEntity, needSaveScene);

    if (importedEntities.empty()){
        // Clean up root if import failed
        scene->destroyEntity(rootEntity);
        auto it = std::find(sceneProject->entities.begin(), sceneProject->entities.end(), rootEntity);
        if (it != sceneProject->entities.end()) {
            sceneProject->entities.erase(it);
        }
        rootEntity = NULL_ENTITY;
        return false;
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

    // If parent is part of a prefab, add the new root as a nested prefab member. A brush
    // placement never does: it drops props into the scene, not into a shared prefab asset.
    addedToParentPrefab = false;
    if (parent != NULL_ENTITY && !hasPlacement && project->isEntityInPrefab(sceneId, parent)) {
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

    // Unimport the prefab (removes instance + destroys entities)
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
