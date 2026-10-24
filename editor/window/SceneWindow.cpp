// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SceneWindow.h"
#include "DocumentFile.h"
#include "util/BidiText.h"
#include "imgui_internal.h"

#include "Engine.h"
#include "Input.h"
#include "external/IconsFontAwesome6.h"
#include "Backend.h"
#include "util/Util.h"
#include "util/EntityPayload.h"
#include "command/CommandHandle.h"
#include "command/type/PropertyCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/LinkMaterialCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "command/type/DuplicateEntityCmd.h"
#include "command/type/ImportPrefabCmd.h"
#include "util/ProjectUtils.h"
#include "render/SceneRender2D.h"
#include "render/SceneRender3D.h"
#include "render/gizmo/ViewportGizmo.h"
#include "subsystem/MeshSystem.h"
#include "Stream.h"
#include "Out.h"
#include "App.h"
#include "EditorHost.h"
#include "Theme.h"
#include "Widgets.h"
#include "window/GameWindow.h"

#include "math/Vector2.h"
#include "util/Angle.h"
#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

constexpr float VIEWPORT_GIZMO_SIZE = 100.0f;

}

editor::SceneWindow::SceneWindow(Project* project) {
    this->project = project;
    this->mouseLeftDraggedInside = false;
    this->windowFocused = false;
}

void editor::SceneWindow::handleCloseScene(uint32_t sceneId) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (sceneProject) {
        project->checkUnsavedAndExecute(sceneId, [this, sceneId]() {
            closeSceneInternal(sceneId);
        });
    }
}

void editor::SceneWindow::closeSceneInternal(uint32_t sceneId) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (sceneProject) {
        // If we're closing the currently selected scene
        if (project->getSelectedSceneId() == sceneId) {
            // Find another scene to select
            for (const auto& otherScene : project->getScenes()) {
                if (otherScene.id != sceneId && otherScene.opened) {
                    project->setSelectedSceneId(otherScene.id);
                    ImGui::SetWindowFocus(("###Scene" + std::to_string(otherScene.id)).c_str());
                    break;
                }
            }
        }
        closeSceneQueue.push_back(sceneId);
    }
}

bool editor::SceneWindow::isFocused() const {
    return windowFocused;
}

void editor::SceneWindow::resetProjectState() {
    cancelPrefabDrag();
    for (const auto& entry : lookActive) {
        // Only one look can be active, and endLook only rewrites keys that exist
        if (entry.second) { endLook(entry.first); break; }
    }
    windowFocused = false;
    mouseLeftDown = false;
    mouseLeftDraggedInside = false;
    draggingMouse.clear();
    suppressLeftMouseUntilRelease.clear();
    subSelectionClickConsumesRelease.clear();
    resyncLookDelta.clear();
    lookActive.clear();
    lookReturnPos.clear();
    focusCanvasOnNextFrame.clear();
    width.clear();
    height.clear();
    framebufferScale.clear();
    canvasDpiScale.clear();
    hasNotification.clear();
    structureSelectionParents.clear();
    closeSceneQueue.clear();
}

void editor::SceneWindow::clearSceneState(uint32_t sceneId) {
    if (prefabDragSceneId == sceneId) {
        cancelPrefabDrag();
    }
    if (lookActive[sceneId]) {
        endLook(sceneId);
    }
    draggingMouse.erase(sceneId);
    suppressLeftMouseUntilRelease.erase(sceneId);
    subSelectionClickConsumesRelease.erase(sceneId);
    resyncLookDelta.erase(sceneId);
    lookActive.erase(sceneId);
    lookReturnPos.erase(sceneId);
    focusCanvasOnNextFrame.erase(sceneId);
    width.erase(sceneId);
    height.erase(sceneId);
    framebufferScale.erase(sceneId);
    canvasDpiScale.erase(sceneId);
    hasNotification.erase(sceneId);
    structureSelectionParents.erase(sceneId);

    closeSceneQueue.erase(
        std::remove(closeSceneQueue.begin(), closeSceneQueue.end(), sceneId),
        closeSceneQueue.end());
}

void editor::SceneWindow::beginStructureVisibilityUpdate(uint32_t sceneId) {
    structureSelectionParents.erase(sceneId);
}

void editor::SceneWindow::setStructureSelectionParent(uint32_t sceneId, Entity entity, Entity selectionParent) {
    if (entity == NULL_ENTITY || selectionParent == NULL_ENTITY || entity == selectionParent) {
        return;
    }
    structureSelectionParents[sceneId][entity] = selectionParent;
}

Entity editor::SceneWindow::resolveStructureSelection(uint32_t viewportSceneId, uint32_t sceneId, Entity entity) const {
    auto sceneIt = structureSelectionParents.find(sceneId);
    if (sceneIt == structureSelectionParents.end()) {
        return entity;
    }

    const SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return entity;
    }

    // The 2D gizmo is drawn from the entity's own bounds, so a group parent without any
    // would be an invisible selection. The other gizmos outline the whole family.
    const SceneProject* viewportScene = project->getScene(viewportSceneId);
    bool outlinesFamily = viewportScene && viewportScene->sceneRender
        && viewportScene->sceneRender->getToolsLayer()->getGizmoSelected() != GizmoSelected::OBJECT2D;

    // Keep the outermost collapsed ancestor that can be selected, else the entity itself
    Entity resolved = entity;
    auto entityIt = sceneIt->second.find(entity);
    while (entityIt != sceneIt->second.end()) {
        Entity ancestor = entityIt->second;
        if (!sceneProject->scene->isEntityCreated(ancestor)) {
            break;
        }
        if (outlinesFamily || project->isEntityPickable(sceneId, ancestor)) {
            resolved = ancestor;
        }
        entityIt = sceneIt->second.find(ancestor);
    }

    return resolved;
}

// The GameObject drawn on top at (x, y): in the scene itself or in a scene layered over
// it (outSceneId tells which), resolved to the collapsed Hierarchy row that holds it
Entity editor::SceneWindow::findSelectableObjectByRay(uint32_t sceneId, float x, float y, uint32_t* outSceneId) {
    uint32_t hitSceneId = sceneId;
    Entity hitEntity = project->findObjectByRay(sceneId, x, y, &hitSceneId);
    if (outSceneId) {
        *outSceneId = hitSceneId;
    }
    return resolveStructureSelection(sceneId, hitSceneId, hitEntity);
}

bool editor::SceneWindow::selectObjectByRay(uint32_t sceneId, float x, float y, bool shiftPressed) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->sceneRender) {
        return false;
    }

    uint32_t hitSceneId = sceneId;
    Entity hitEntity = findSelectableObjectByRay(sceneId, x, y, &hitSceneId);

    if (!sceneProject->sceneRender->isAnyGizmoSideSelected()) {
        if (hitEntity != NULL_ENTITY) {
            // a selection is of one scene: Shift adds to it only within that scene
            if (!shiftPressed || project->getSelectedSceneForProperties() != hitSceneId) {
                project->clearAllSelections(sceneId);
            }
            project->addSelectedEntity(hitSceneId, hitEntity);
            project->setSelectedSceneForProperties(hitSceneId);
            if (HierarchyWindow* structure = Backend::getApp().getHierarchyWindow()) {
                structure->revealEntity(hitSceneId, hitEntity);
            }
            project->syncSceneRenderEditLayer(sceneId);
            return true;
        }

        project->clearAllSelections(sceneId);
        project->setSelectedSceneForProperties(sceneId);
        project->syncSceneRenderEditLayer(sceneId);
    }

    return false;
}

void editor::SceneWindow::requestPlayFocus(uint32_t sceneId) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->opened) {
        return;
    }

    project->setSelectedSceneId(sceneId);
    focusCanvasOnNextFrame[sceneId] = true;
    focusSceneWindow(*sceneProject);
}

bool editor::SceneWindow::viewThroughCamera(uint32_t sceneId, Entity cameraEntity) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !sceneProject->sceneRender) {
        return false;
    }
    if (sceneProject->playState != ScenePlayState::STOPPED) {
        return false;
    }
    if (!sceneProject->sceneRender->setPreviewCamera(cameraEntity)) {
        return false;
    }

    project->setSelectedSceneId(sceneId);
    sceneProject->needUpdateRender = true;
    focusSceneWindow(*sceneProject);
    return true;
}

void editor::SceneWindow::stopViewingCamera(uint32_t sceneId) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->sceneRender) {
        return;
    }

    sceneProject->sceneRender->clearPreviewCamera();
    sceneProject->needUpdateRender = true;
}

void editor::SceneWindow::focusSceneWindow(const SceneProject& sceneProject) const {
    ImGui::SetWindowFocus(("###Scene" + std::to_string(sceneProject.id)).c_str());
}

void editor::SceneWindow::requestOpenParentScene(uint32_t childSceneId, uint32_t parentSceneId) {
    // Defer until the current viewport image has been submitted.
    editor::getEditorHost().enqueueMainThreadTask([this, childSceneId, parentSceneId]() {
        if (project->isAnyScenePlaying()) {
            Out::warning("Cannot open a new scene while a scene is playing.");
            return;
        }
        if (!project->getScene(childSceneId) || !project->getScene(parentSceneId)) {
            return;
        }

        project->checkUnsavedAndExecute(childSceneId, [this, childSceneId, parentSceneId]() {
            if (project->isAnyScenePlaying()) {
                Out::warning("Cannot open a new scene while a scene is playing.");
                return;
            }

            SceneProject* parentScene = project->getScene(parentSceneId);
            if (!parentScene) {
                return;
            }

            const fs::path parentPath = parentScene->filepath;
            project->closeScene(childSceneId, true);

            if (parentPath.empty()) {
                project->setSelectedSceneId(parentSceneId);
            } else {
                project->openScene(parentPath, false);
            }

            parentScene = project->getScene(parentSceneId);
            if (parentScene && parentScene->opened) {
                focusSceneWindow(*parentScene);
            }
        });
    });
}

void editor::SceneWindow::drawParentSceneButton(const SceneProject& sceneProject) {
    std::vector<const SceneProject*> parentScenes;
    for (const SceneProject& possibleParent : project->getScenes()) {
        if (project->hasChildScene(possibleParent.id, sceneProject.id)) {
            parentScenes.push_back(&possibleParent);
        }
    }

    if (parentScenes.empty()) {
        return;
    }

    const std::string popupId = "ParentScenePopup##" + std::to_string(sceneProject.id);
    const std::string buttonId = std::string(ICON_FA_ARROW_TURN_UP) + " Parent##ParentScene" + std::to_string(sceneProject.id);

    ImGui::SameLine(0, Theme::dpi(10.0f));
    ImGui::BeginDisabled(project->isAnyScenePlaying());
    if (ImGui::Button(buttonId.c_str())) {
        if (parentScenes.size() == 1) {
            requestOpenParentScene(sceneProject.id, parentScenes.front()->id);
        } else {
            ImGui::OpenPopup(popupId.c_str());
        }
    }

    if (parentScenes.size() == 1) {
        const std::string parentName = BidiText::toVisual(parentScenes.front()->name);
        ImGui::SetItemTooltip("Return to parent scene '%s'", parentName.c_str());
    } else {
        ImGui::SetItemTooltip("Choose a parent scene");
    }

    if (ImGui::BeginPopup(popupId.c_str())) {
        for (const SceneProject* parentScene : parentScenes) {
            const std::string label = BidiText::toVisual(parentScene->name)
                + "##ParentScene" + std::to_string(parentScene->id);
            if (ImGui::MenuItem(label.c_str())) {
                requestOpenParentScene(sceneProject.id, parentScene->id);
            }
        }
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
}

std::string editor::SceneWindow::getWindowTitle(const SceneProject& sceneProject) const {
    std::string icon;
    if (sceneProject.sceneType == SceneType::SCENE_3D){
        icon = ICON_FA_CUBES + std::string("  ");
    }else if (sceneProject.sceneType == SceneType::SCENE_2D){
        icon = ICON_FA_CUBES_STACKED + std::string("  ");
    }else if (sceneProject.sceneType == SceneType::SCENE_UI){
        icon = ICON_FA_WINDOW_RESTORE + std::string("  ");
    }
    // everything before "###" is drawn, the id after it keeps docking state stable
    // a scene whose load dropped values (docs/data-safety.md): its saves ask first
    const bool partial = sceneProject.loadReport.dropped() > 0;
    return icon + BidiText::toVisual(sceneProject.name) + (partial ? " (Partial Load)" : "") +
        ((project->hasSceneUnsavedChanges(sceneProject.id)) ? " *" : "") + "###Scene" + std::to_string(sceneProject.id);
}

Vector3 editor::SceneWindow::getModelDropPosition(SceneProject* sceneProject, float x, float y, Entity hitEntity) {
    Ray ray = sceneProject->sceneRender->screenToRay(x, y);

    if (hitEntity != NULL_ENTITY) {
        AABB entityAABB = sceneProject->sceneRender->getEntitiesAABB({hitEntity});
        RayReturn entityHit = ray.intersects(entityAABB);
        if (entityHit) {
            // If the hit entity is terrain, use its precise surface height instead of the AABB top
            Vector3 terrainPosition;
            TerrainComponent* terrain = sceneProject->scene->findComponent<TerrainComponent>(hitEntity);
            Transform* transform = sceneProject->scene->findComponent<Transform>(hitEntity);
            if (terrain && transform && sceneProject->scene->getSystem<MeshSystem>()->raycastTerrainSurface(ray, *terrain, *transform, terrainPosition)) {
                return terrainPosition;
            }
            return entityHit.point;
        }
    }

    RayReturn hit = ray.intersects(Plane(Vector3(0, 1, 0), Vector3(0, 0, 0)));
    if (hit) {
        return hit.point;
    }

    return sceneProject->sceneRender->getCamera()->getTarget();
}

void editor::SceneWindow::handleResourceFileDragDrop(SceneProject* sceneProject) {
    static bool draggingResourceFile = false;
    static Image* tempImage = nullptr;
    static MeshComponent* selMesh = nullptr;
    static UIComponent* selUI = nullptr;
    static TextComponent* selText = nullptr;
    static std::vector<Material> originalMeshMaterials;
    static unsigned int originalMeshNumSubmeshes = 0;
    static Material cachedDragMaterial;
    static std::string cachedDragPath;
    static Texture originalTex;
    static FontArray originalFont;
    static Entity lastSelEntity = NULL_ENTITY;

    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* peekPayload = ImGui::GetDragDropPayload();
        if (peekPayload && peekPayload->IsDataType("resource_files")) {
            std::vector<std::string> receivedStrings = editor::Util::getStringsFromPayload(peekPayload);
            if (receivedStrings.size() > 0) {
                const std::string droppedRelativePath = std::filesystem::relative(receivedStrings[0], project->getProjectPath()).generic_string();
                // Fonts, images and models are stored relative to the assets root, material
                // files stay project-relative. Outside the root a drop previews but is refused.
                const bool insideAssets = project->isInsideAssetsPath(receivedStrings[0]);
                const std::string droppedAssetPath = insideAssets
                    ? project->normalizeToAssetsRelative(receivedStrings[0]).generic_string()
                    : std::filesystem::path(receivedStrings[0]).generic_string();
                bool isFont = Util::isFontFile(droppedRelativePath);
                bool isImage = Util::isImageFile(droppedRelativePath);
                bool isMaterial = Util::isMaterialFile(droppedRelativePath);
                bool isModel = Util::isModelFile(droppedRelativePath);

                draggingResourceFile = true;
                ImVec2 windowPos = ImGui::GetWindowPos();
                ImGuiIO& io = ImGui::GetIO();
                ImVec2 mousePos = io.MousePos;
                float x = mousePos.x - windowPos.x;
                float y = mousePos.y - windowPos.y;
                toEngineCanvas(sceneProject->id, x, y);
                Entity selEntity = project->findObjectByRay(sceneProject->id, x, y);

                if (selEntity == NULL_ENTITY || lastSelEntity != selEntity) {
                    if (selMesh) {
                        for (unsigned int s = 0; s < originalMeshNumSubmeshes && s < selMesh->numSubmeshes; s++) {
                            if (selMesh->submeshes[s].material != originalMeshMaterials[s]) {
                                selMesh->submeshes[s].material = originalMeshMaterials[s];
                                selMesh->submeshes[s].needUpdateTexture = true;
                            }
                        }
                        selMesh = nullptr;
                        originalMeshMaterials.clear();
                    }
                    if (selUI) {
                        if (selUI->texture != originalTex) {
                            selUI->texture = originalTex;
                            selUI->needUpdateTexture = true;
                        }
                        selUI = nullptr;
                    }
                    if (selText) {
                        if (selText->font != originalFont) {
                            selText->font = originalFont;
                            selText->needReloadAtlas = true;
                            selText->needUpdateText = true;
                        }
                        selText = nullptr;
                    }
                    cachedDragPath.clear();
                }

                const bool isPrefab = Util::isPrefabFile(droppedRelativePath);

                if (isPrefab) {
                    if (sceneProject->playState == ScenePlayState::STOPPED && !project->isAnyScenePlaying()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
                            updatePrefabDrag(sceneProject, receivedStrings, x, y, payload->IsDelivery());
                        }
                    }
                } else if (isModel && sceneProject->sceneType == SceneType::SCENE_3D) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
                        if (payload->IsDelivery() && !insideAssets) {
                            Backend::getApp().registerOutsideAssetsAlert(receivedStrings[0]);
                            focusSceneWindow(*sceneProject);
                        } else if (payload->IsDelivery()) {
                            std::string modelEntityName = std::filesystem::path(droppedAssetPath).stem().string();
                            if (modelEntityName.empty()) {
                                modelEntityName = "Model";
                            }

                            Vector3 dropPosition = getModelDropPosition(sceneProject, x, y, selEntity);
                            CommandHandle::get(sceneProject->id)->addCommandNoMerge(
                                new ModelLoadCmd(project, sceneProject->id, modelEntityName, dropPosition, droppedAssetPath));

                            focusSceneWindow(*sceneProject);
                        }
                    }
                } else if (selEntity != NULL_ENTITY) {
                    bool accept = false;
                    if (isFont && sceneProject->scene->findComponent<TextComponent>(selEntity)) {
                        accept = true;
                    }
                    if (isImage && (sceneProject->scene->findComponent<MeshComponent>(selEntity) || sceneProject->scene->findComponent<UIComponent>(selEntity))) {
                        accept = true;
                    }
                    if (isMaterial && sceneProject->scene->findComponent<MeshComponent>(selEntity)) {
                        accept = true;
                    }

                    if (accept) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
                            lastSelEntity = selEntity;

                            if (tempImage != nullptr) {
                                delete tempImage;
                                tempImage = nullptr;
                            }

                            if (TextComponent* text = sceneProject->scene->findComponent<TextComponent>(selEntity)) {
                                if (isFont) {
                                    if (!selText) {
                                        selText = text;
                                        originalFont = text->font;
                                    }
                                    // the drop replaces the main font, the fallbacks stay
                                    if (text->font[0] != droppedAssetPath) {
                                        text->font[0] = droppedAssetPath;
                                        text->needReloadAtlas = true;
                                        text->needUpdateText = true;
                                    }
                                    if (payload->IsDelivery()) {
                                        std::string propName = "font";
                                        text->font = originalFont;
                                        text->needReloadAtlas = true;
                                        text->needUpdateText = true;

                                        if (insideAssets) {
                                            FontArray newFonts = originalFont;
                                            newFonts[0] = droppedAssetPath;
                                            PropertyCmd<FontArray>* cmd = new PropertyCmd<FontArray>(project, sceneProject->id, selEntity, ComponentType::TextComponent, propName, newFonts);
                                            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(cmd);
                                        } else {
                                            Backend::getApp().registerOutsideAssetsAlert(receivedStrings[0]);
                                        }

                                        selText = nullptr;

                                        focusSceneWindow(*sceneProject);
                                    }
                                }
                            } else if (isImage) {
                                if (MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(selEntity)) {
                                    if (!selMesh) {
                                        selMesh = mesh;
                                        originalMeshMaterials.clear();
                                        originalMeshNumSubmeshes = mesh->numSubmeshes;
                                        for (unsigned int s = 0; s < mesh->numSubmeshes; s++) {
                                            originalMeshMaterials.push_back(mesh->submeshes[s].material);
                                        }
                                        originalTex = mesh->submeshes[0].material.baseColorTexture;
                                    }
                                    Texture newTex(droppedAssetPath);
                                    if (mesh->submeshes[0].material.baseColorTexture != newTex) {
                                        mesh->submeshes[0].material.baseColorTexture = newTex;
                                        mesh->submeshes[0].needUpdateTexture = true;
                                    }
                                    if (payload->IsDelivery()) {
                                        std::string propName = "submeshes[0].material.baseColorTexture";
                                        for (unsigned int s = 0; s < originalMeshNumSubmeshes && s < selMesh->numSubmeshes; s++) {
                                            selMesh->submeshes[s].material = originalMeshMaterials[s];
                                            selMesh->submeshes[s].needUpdateTexture = true;
                                        }

                                        if (insideAssets) {
                                            PropertyCmd<Texture>* cmd = new PropertyCmd<Texture>(project, sceneProject->id, selEntity, ComponentType::MeshComponent, propName, newTex);
                                            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(cmd);
                                        } else {
                                            Backend::getApp().registerOutsideAssetsAlert(receivedStrings[0]);
                                        }

                                        selMesh = nullptr;
                                        originalMeshMaterials.clear();

                                        focusSceneWindow(*sceneProject);
                                    }
                                } else if (UIComponent* ui = sceneProject->scene->findComponent<UIComponent>(selEntity)) {
                                    if (!selUI) {
                                        selUI = ui;
                                        originalTex = ui->texture;
                                    }
                                    Texture newTex(droppedAssetPath);
                                    if (ui->texture != newTex) {
                                        ui->texture = newTex;
                                        ui->needUpdateTexture = true;
                                    }
                                    if (payload->IsDelivery()) {
                                        std::string propName = "texture";
                                        selUI->texture = originalTex;

                                        if (insideAssets) {
                                            PropertyCmd<Texture>* cmd = new PropertyCmd<Texture>(project, sceneProject->id, selEntity, ComponentType::UIComponent, propName, newTex);
                                            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(cmd);
                                        } else {
                                            selUI->needUpdateTexture = true;
                                            Backend::getApp().registerOutsideAssetsAlert(receivedStrings[0]);
                                        }

                                        selUI = nullptr;

                                        focusSceneWindow(*sceneProject);
                                    }
                                }
                            } else if (isMaterial) {
                                if (MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(selEntity)) {
                                    try {
                                        // Cache the decoded material — only reload when the path changes
                                        if (cachedDragPath != droppedRelativePath) {
                                            fs::path materialPath = project->getProjectPath() / droppedRelativePath;
                                            YAML::Node materialNode = DocumentFiles::read(materialPath, DocumentKind::Material).root;
                                            cachedDragMaterial = Stream::decodeMaterial(materialNode);
                                            cachedDragMaterial.name = fs::path(droppedRelativePath).lexically_normal().generic_string();
                                            cachedDragPath = droppedRelativePath;
                                        }

                                        if (!selMesh) {
                                            selMesh = mesh;
                                            originalMeshMaterials.clear();
                                            originalMeshNumSubmeshes = mesh->numSubmeshes;
                                            for (unsigned int s = 0; s < mesh->numSubmeshes; s++) {
                                                originalMeshMaterials.push_back(mesh->submeshes[s].material);
                                            }
                                        }

                                        // Preview: apply to all submeshes
                                        for (unsigned int s = 0; s < mesh->numSubmeshes; s++) {
                                            if (mesh->submeshes[s].material != cachedDragMaterial) {
                                                mesh->submeshes[s].material = cachedDragMaterial;
                                                mesh->submeshes[s].needUpdateTexture = true;
                                            }
                                        }

                                        if (payload->IsDelivery()) {
                                            // Restore originals before issuing commands
                                            for (unsigned int s = 0; s < originalMeshNumSubmeshes && s < selMesh->numSubmeshes; s++) {
                                                selMesh->submeshes[s].material = originalMeshMaterials[s];
                                                selMesh->submeshes[s].needUpdateTexture = true;
                                            }

                                            // Create a multi-command for all submeshes (link + property)
                                            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                                            for (unsigned int s = 0; s < mesh->numSubmeshes; s++) {
                                                std::string propName = "submeshes[" + std::to_string(s) + "].material";
                                                multiCmd->addCommand(std::make_unique<LinkMaterialCmd>(
                                                    project, sceneProject->id, selEntity, ComponentType::MeshComponent, propName, s, cachedDragMaterial));
                                            }
                                            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(multiCmd);

                                            selMesh = nullptr;
                                            originalMeshMaterials.clear();
                                            cachedDragPath.clear();

                                            focusSceneWindow(*sceneProject);
                                        }
                                    } catch (const std::exception& e) {
                                        Out::error("Error loading material file '%s': %s", droppedRelativePath.c_str(), e.what());
                                    }
                                }
                            }
                        }
                    }
                } else if (sceneProject->sceneType != SceneType::SCENE_3D) {
                    if (isImage) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files", ImGuiDragDropFlags_AcceptBeforeDelivery)) {

                            if (!tempImage) {
                                tempImage = new Image(sceneProject->scene);
                                tempImage->setTexture(droppedAssetPath);
                                tempImage->setAlpha(0.5f);
                            }
                            Ray ray = sceneProject->sceneRender->screenToRay(x, y);
                            RayReturn rreturn = ray.intersects(Plane(Vector3(0, 0, 1), Vector3(0, 0, 0)));
                            if (rreturn) {
                                tempImage->setPosition(rreturn.point);
                            }
                            if (payload->IsDelivery() && !insideAssets) {
                                Backend::getApp().registerOutsideAssetsAlert(receivedStrings[0]);

                                delete tempImage;
                                tempImage = nullptr;

                                focusSceneWindow(*sceneProject);
                            } else if (payload->IsDelivery()) {
                                CreateEntityCmd* cmd = nullptr;

                                if (sceneProject->sceneType == SceneType::SCENE_2D) {
                                    cmd = new CreateEntityCmd(project, sceneProject->id, "Sprite", EntityCreationType::SPRITE);

                                    cmd->addProperty<Vector3>(ComponentType::Transform, "position", rreturn.point);
                                    cmd->addProperty<Texture>(ComponentType::MeshComponent, "submeshes[0].material.baseColorTexture", Texture(droppedAssetPath));
                                    cmd->addProperty<unsigned int>(ComponentType::SpriteComponent, "width", tempImage->getWidth());
                                    cmd->addProperty<unsigned int>(ComponentType::SpriteComponent, "height", tempImage->getHeight());
                                } else {
                                    cmd = new CreateEntityCmd(project, sceneProject->id, "Image", EntityCreationType::IMAGE);

                                    cmd->addProperty<Vector3>(ComponentType::Transform, "position", rreturn.point);
                                    cmd->addProperty<Texture>(ComponentType::UIComponent, "texture", Texture(droppedAssetPath));
                                    cmd->addProperty<unsigned int>(ComponentType::RectTransformComponent, "width", tempImage->getWidth());
                                    cmd->addProperty<unsigned int>(ComponentType::RectTransformComponent, "height", tempImage->getHeight());
                                }

                                CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(cmd);

                                delete tempImage;
                                tempImage = nullptr;

                                focusSceneWindow(*sceneProject);
                            }

                        }
                    }
                }
            }
        }
        ImGui::EndDragDropTarget();
    } else {
        // the dragged prefab left this view (or the drag ended elsewhere)
        if (prefabDragSceneId == sceneProject->id) {
            cancelPrefabDrag();
        }
        if (draggingResourceFile) {
            // If user released mouse (ended drag) and we have a temp texture applied, revert it
            if (selMesh) {
                for (unsigned int s = 0; s < originalMeshNumSubmeshes && s < selMesh->numSubmeshes; s++) {
                    if (selMesh->submeshes[s].material != originalMeshMaterials[s]) {
                        selMesh->submeshes[s].material = originalMeshMaterials[s];
                        selMesh->submeshes[s].needUpdateTexture = true;
                    }
                }
                selMesh = nullptr;
                originalMeshMaterials.clear();
            }
            if (selUI) {
                if (selUI->texture != originalTex) {
                    selUI->texture = originalTex;
                    selUI->needUpdateTexture = true;
                }
                selUI = nullptr;
            }
            if (selText) {
                if (selText->font != originalFont) {
                    selText->font = originalFont;
                    selText->needReloadAtlas = true;
                    selText->needUpdateText = true;
                }
                selText = nullptr;
            }
            draggingResourceFile = false; // Reset flag
            lastSelEntity = NULL_ENTITY;
            cachedDragPath.clear();
        }
        if (tempImage != nullptr) {
            delete tempImage;
            tempImage = nullptr;
        }
    }
}

Vector3 editor::SceneWindow::getPrefabDropPosition(SceneProject* sceneProject, float x, float y, bool& keepZ) {
    keepZ = false;
    if (sceneProject->sceneType == SceneType::SCENE_3D) {
        // On what the pointer is over (the preview itself left out), else the ground plane,
        // else in front of the camera
        Entity hit = project->findObjectByRayIgnoring(sceneProject->id, x, y, prefabDragEntities);
        return getModelDropPosition(sceneProject, x, y, hit);
    }
    // 2D and UI: the pointer's world position, the prefab keeping its own depth
    keepZ = true;
    Ray ray = sceneProject->sceneRender->screenToRay(x, y);
    RayReturn hit = ray.intersects(Plane(Vector3(0, 0, 1), Vector3(0, 0, 0)));
    return hit ? hit.point : Vector3(0, 0, 0);
}

void editor::SceneWindow::cancelPrefabDrag() {
    if (prefabDragPreview) {
        prefabDragPreview->undo();
        delete prefabDragPreview;
        prefabDragPreview = nullptr;
        if (SceneProject* sceneProject = project->getScene(prefabDragSceneId)) {
            sceneProject->needUpdateRender = true;
        }
    }
    prefabDragSceneId = NULL_PROJECT_SCENE;
    prefabDragPath.clear();
    prefabDragEntities.clear();
}

void editor::SceneWindow::updatePrefabDrag(SceneProject* sceneProject, const std::vector<std::string>& paths, float x, float y, bool delivery) {
    std::vector<fs::path> prefabs;
    for (const std::string& path : paths) {
        if (Util::isPrefabFile(path)) {
            fs::path relative = fs::path(path).lexically_relative(project->getProjectPath());
            if (!relative.empty() && relative.begin()->string() != "..") {
                prefabs.push_back(relative);
            }
        }
    }
    if (prefabs.empty() || !sceneProject->sceneRender) {
        cancelPrefabDrag();
        return;
    }

    // The preview: an instance of the first prefab, made once and moved with the pointer
    if (prefabDragSceneId != NULL_PROJECT_SCENE &&
            (prefabDragSceneId != sceneProject->id || prefabDragPath != prefabs.front().generic_string())) {
        cancelPrefabDrag();
    }
    bool keepZ = false;
    if (!prefabDragPreview && prefabDragSceneId == NULL_PROJECT_SCENE) {
        // one attempt per drag: a prefab that cannot be made is not tried every frame
        prefabDragSceneId = sceneProject->id;
        prefabDragPath = prefabs.front().generic_string();
        ImportPrefabCmd* preview = new ImportPrefabCmd(project, sceneProject->id, prefabs.front(), NULL_ENTITY, true);
        preview->setQuiet(true);
        preview->setDropPosition(getPrefabDropPosition(sceneProject, x, y, keepZ), keepZ);
        if (preview->execute()) {
            prefabDragPreview = preview;
            const Entity root = preview->getRootEntity();
            prefabDragEntities.insert(root);
            // everything below the root, so the pointer never lands on the preview itself
            auto transforms = sceneProject->scene->getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++) {
                Entity entity = transforms->getEntity(i);
                Entity ancestor = transforms->getComponentFromIndex(i).parent;
                for (size_t depth = 0; ancestor != NULL_ENTITY && depth < 1024; depth++) {
                    if (ancestor == root) {
                        prefabDragEntities.insert(entity);
                        break;
                    }
                    Transform* parent = sceneProject->scene->findComponent<Transform>(ancestor);
                    ancestor = parent ? parent->parent : NULL_ENTITY;
                }
            }
            for (Entity entity : preview->getImportedEntities()) {
                prefabDragEntities.insert(entity);
            }
        } else {
            delete preview;
        }
    }

    const Vector3 position = getPrefabDropPosition(sceneProject, x, y, keepZ);
    if (prefabDragPreview) {
        if (Transform* transform = sceneProject->scene->findComponent<Transform>(prefabDragPreview->getRootEntity())) {
            Vector3 target = position;
            if (keepZ) {
                target.z = transform->position.z;
            }
            if (transform->position != target) {
                transform->position = target;
                transform->needUpdate = true;
                sceneProject->needUpdateRender = true;
            }
        }
    }

    if (!delivery) {
        return;
    }

    // The drop: the preview goes, the real instances come as one undo step
    cancelPrefabDrag();
    std::vector<ImportPrefabCmd*> commands;
    for (const fs::path& prefab : prefabs) {
        ImportPrefabCmd* cmd = new ImportPrefabCmd(project, sceneProject->id, prefab, NULL_ENTITY, true);
        cmd->setDropPosition(position, keepZ);
        commands.push_back(cmd);
    }
    if (commands.size() == 1) {
        CommandHandle::get(sceneProject->id)->addCommandNoMerge(commands.front());
    } else {
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
        for (ImportPrefabCmd* cmd : commands) {
            multiCmd->addCommand(std::unique_ptr<Command>(cmd));
        }
        CommandHandle::get(sceneProject->id)->addCommandNoMerge(multiCmd);
    }
    focusSceneWindow(*sceneProject);
}

void editor::SceneWindow::handleTileRectDragDrop(SceneProject* sceneProject) {
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* peekPayload = ImGui::GetDragDropPayload();
        if (peekPayload && peekPayload->IsDataType("tile_rect")) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("tile_rect")) {
                const TileRectPayload* tileRectPayload = static_cast<const TileRectPayload*>(payload->Data);

                if (tileRectPayload->sceneId == sceneProject->id) {
                    Entity entity = tileRectPayload->entity;
                    int rectIndex = tileRectPayload->rectIndex;

                    TilemapComponent* tilemap = sceneProject->scene->findComponent<TilemapComponent>(entity);
                    Transform* transform = sceneProject->scene->findComponent<Transform>(entity);

                    if (tilemap && transform && rectIndex >= 0 && rectIndex < (int)tilemap->numTilesRect) {
                        // Compute drop position in tilemap local space
                        ImVec2 windowPos = ImGui::GetWindowPos();
                        ImGuiIO& io = ImGui::GetIO();
                        float x = io.MousePos.x - windowPos.x;
                        float y = io.MousePos.y - windowPos.y;
                        toEngineCanvas(sceneProject->id, x, y);

                        Ray ray = sceneProject->sceneRender->screenToRay(x, y);
                        Plane tilePlane(Vector3(0, 0, 1), transform->worldPosition);
                        RayReturn rr = ray.intersects(tilePlane);

                        if (rr) {
                            Matrix4 invModel = transform->modelMatrix.inverse();
                            Vector3 localHit = invModel * rr.point;

                            const TileRectData& rectData = tilemap->tilesRect[rectIndex];
                            float tileW = rectData.rect.getWidth();
                            float tileH = rectData.rect.getHeight();

                            // Center tile on drop point and clamp to non-negative
                            float posX = std::max(0.0f, localHit.x - tileW * 0.5f);
                            float posY = std::max(0.0f, localHit.y - tileH * 0.5f);

                            // Snap to tile size so dropped tiles pack edge-to-edge
                            if (sceneProject->displaySettings.snapTile && tileW > 0.0f && tileH > 0.0f) {
                                posX = std::max(0.0f, std::round(posX / tileW) * tileW);
                                posY = std::max(0.0f, std::round(posY / tileH) * tileH);
                            }

                            int freeSlot = (int)tilemap->numTiles;
                            if (freeSlot < (int)tilemap->tiles.size()) {
                                std::string tilePrefix = "tiles[" + std::to_string(freeSlot) + "]";
                                MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                                multiCmd->addPropertyCmd<std::string>(project, sceneProject->id, entity, ComponentType::TilemapComponent,
                                    tilePrefix + ".name", rectData.name);
                                multiCmd->addPropertyCmd<int>(project, sceneProject->id, entity, ComponentType::TilemapComponent,
                                    tilePrefix + ".rectId", rectIndex);
                                multiCmd->addPropertyCmd<Vector2>(project, sceneProject->id, entity, ComponentType::TilemapComponent,
                                    tilePrefix + ".position", Vector2(posX, posY));
                                multiCmd->addPropertyCmd<float>(project, sceneProject->id, entity, ComponentType::TilemapComponent,
                                    tilePrefix + ".width", tileW);
                                multiCmd->addPropertyCmd<float>(project, sceneProject->id, entity, ComponentType::TilemapComponent,
                                    tilePrefix + ".height", tileH);
                                multiCmd->addPropertyCmd<unsigned int>(project, sceneProject->id, entity, ComponentType::TilemapComponent,
                                    "numTiles", (unsigned int)(freeSlot + 1));
                                multiCmd->setNoMerge();
                                CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);

                                focusSceneWindow(*sceneProject);
                            }
                        }
                    }
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
}

// Gives the cursor back and lets ImGui see the mouse again. NoMouse is global, so
// this has to run even when the viewport that armed it is no longer drawn.
void editor::SceneWindow::endLook(uint32_t sceneId){
    draggingMouse[sceneId] = false;
    lookActive[sceneId] = false;
    resyncLookDelta[sceneId] = false;
    Backend::enableMouseCursor();
    ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
}

void editor::SceneWindow::sceneEventHandler(SceneProject* sceneProject) {
    if (sceneProject->playState == ScenePlayState::SAVING || sceneProject->playState == ScenePlayState::LOADING || sceneProject->playState == ScenePlayState::CANCELLING) {
        return;
    }

    // Get the current window's position and size
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 windowSize = ImGui::GetWindowSize();
    ImGuiIO& io = ImGui::GetIO();
    {
        // relative to the main window (ImGui positions are screen positions with viewports on)
        const ImVec2 origin = ImGui::GetMainViewport()->Pos;
        viewRect[sceneProject->id] = ImVec4(windowPos.x - origin.x, windowPos.y - origin.y, windowSize.x, windowSize.y);
    }
    float mouseWheel = io.MouseWheel;
    ImVec2 mousePos = io.MousePos;
    ImVec2 mouseDelta = io.MouseDelta;

    // Check if the mouse is within the window bounds
    bool isMouseInWindow = ImGui::IsWindowHovered() && (mousePos.x >= windowPos.x && mousePos.x <= windowPos.x + windowSize.x &&
                            mousePos.y >= windowPos.y && mousePos.y <= windowPos.y + windowSize.y);

    // Keep the viewport redrawing while it is hovered or manipulated (hover highlight,
    // camera drag/fly, gizmo drag). draggingMouse tracks a drag so it survives the
    // cursor leaving the window; edits flag the scene through the command system.
    // A resting pointer changes nothing under it, so hovering alone does not redraw:
    // a headless editor's pointer rests in this view and kept the loop from idling.
    {
        bool viewportFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
        bool anyMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                            ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
                            ImGui::IsMouseDown(ImGuiMouseButton_Middle);
        bool pointerActive = anyMouseDown ||
                             io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f ||
                             io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f;
        for (int key = ImGuiKey_NamedKey_BEGIN; !pointerActive && key < ImGuiKey_GamepadStart; key++) {
            pointerActive = ImGui::IsKeyDown(static_cast<ImGuiKey>(key)) || ImGui::IsKeyReleased(static_cast<ImGuiKey>(key));
        }
        if ((isMouseInWindow && pointerActive) || draggingMouse[sceneProject->id] || (viewportFocused && anyMouseDown)) {
            sceneProject->needUpdateRender = true;
        }
    }

    // The game takes its input in the Game view. This view stays the editor's while
    // it plays: selection, transform tools and the editor camera.
    if (sceneProject->playState == ScenePlayState::STOPPED && sceneProject->sceneRender->isPreviewCameraActive()) {
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            stopViewingCamera(sceneProject->id);
        }
        return;
    }

    size_t sceneId = sceneProject->id;

    // The tools work on this scene's selection, or on a layer scene's (an inline child
    // scene, a running game's layer) when its GameObjects are the ones selected
    uint32_t editSceneId = project->syncSceneRenderEditLayer(sceneId);

    bool altHeld = ImGui::IsKeyDown(ImGuiMod_Alt);
    bool suppressLeftMouse = suppressLeftMouseUntilRelease[sceneId];
    bool handPanEnabled = sceneProject->sceneRender->getCursorSelected() == CursorSelected::HAND;
    GizmoSelected gizmoSelected = sceneProject->sceneRender->getToolsLayer()->getGizmoSelected();
    Gizmo2DSideSelected gizmo2DSide = sceneProject->sceneRender->getActiveToolsLayer()->getGizmo2DSideSelected();
    bool gizmoSideActive = sceneProject->sceneRender->isAnyGizmoSideSelected();
    // Alt + click on a gizmo handle duplicates the target and drags the copy.
    // For the 2D gizmo, only the center move area duplicates; resize handles
    // keep editing the current selection. Alt + click on empty space keeps its
    // camera-orbit behavior, so the guard is gated on a hovered/active gizmo side.
    bool altGizmoDrag = altHeld && gizmoSideActive;
    bool altGizmoDuplicate = altGizmoDrag &&
        (gizmoSelected != GizmoSelected::OBJECT2D || gizmo2DSide == Gizmo2DSideSelected::CENTER);

    bool disableSelection = 
        altHeld ||
        handPanEnabled || 
        sceneProject->sceneRender->isTerrainEditing() ||
        gizmoSideActive ||
        (sceneProject->sceneType != SceneType::SCENE_3D && ImGui::IsKeyDown(ImGuiKey_Space));

    if (isMouseInWindow){

        float logicalX = mousePos.x - windowPos.x;
        float logicalY = mousePos.y - windowPos.y;
        float x = logicalX;
        float y = logicalY;
        toEngineCanvas(sceneId, x, y);

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && (!altHeld || altGizmoDrag) && !suppressLeftMouse){
            subSelectionClickConsumesRelease[sceneId] = false;

            // Selecting and dragging an unselected object at same time (just for 2D object mode)
            if (!disableSelection && gizmoSelected == GizmoSelected::OBJECT2D) {
                uint32_t hitSceneId = sceneId;
                Entity hitEntity = findSelectableObjectByRay(sceneId, x, y, &hitSceneId);
                if (hitEntity != NULL_ENTITY) {
                    bool alreadySelected = project->isSelectedEntity(hitSceneId, hitEntity);
                    if (!alreadySelected || (io.KeyShift && !gizmoSideActive)) {
                        sceneProject->sceneRender->clearTileSelection();
                        sceneProject->sceneRender->clearInstanceSelection();
                        sceneProject->sceneRender->clearOccluderPointSelection();
                        sceneProject->sceneRender->clearLinePointSelection();
                        sceneProject->sceneRender->clearPolygonPointSelection();
                        sceneProject->sceneRender->clearTrackPointSelection();
                        bool changed = selectObjectByRay(sceneId, x, y, io.KeyShift);
                        if (changed) {
                            // the mouse through the camera of the scene now edited, so the gizmo
                            // this update places finds it on its body and the drag moves it
                            sceneProject->sceneRender->mouseHoverEvent(x, y);
                            project->updateSceneRender(sceneId);
                            editSceneId = sceneProject->sceneRender->hasEditLayer() ? sceneProject->sceneRender->getEditLayerSceneId() : (uint32_t)sceneId;
                            sceneProject->sceneRender->mouseHoverEvent(x, y);
                        }
                    }
                }
            }

            // Occluder polygon point sub-selection within a selected occluder entity
            // (mirrors the tile sub-selection flow below)
            if (gizmoSelected == GizmoSelected::OBJECT2D) {
                std::vector<Entity> selEntities = project->getSelectedEntities(sceneId);
                if (selEntities.size() == 1 && !io.KeyShift) {
                    Entity selEntity = selEntities[0];
                    Gizmo2DSideSelected gizmo2DSide = sceneProject->sceneRender->getActiveToolsLayer()->getGizmo2DSideSelected();
                    if (gizmo2DSide == Gizmo2DSideSelected::NONE || gizmo2DSide == Gizmo2DSideSelected::CENTER) {
                        int pointHit = sceneProject->sceneRender->hitTestOccluderPoint(selEntity, x, y);
                        if (pointHit >= 0) {
                            sceneProject->sceneRender->selectOccluderPoint(selEntity, pointHit);
                            subSelectionClickConsumesRelease[sceneId] = true;
                            // Re-update so the gizmo cross jumps to the point
                            project->updateSceneRender(sceneId);
                            sceneProject->sceneRender->mouseHoverEvent(x, y);
                        } else if (sceneProject->sceneRender->getSelectedOccluderPointIndex() >= 0 && gizmo2DSide == Gizmo2DSideSelected::NONE) {
                            // Only clear eagerly when clicking the entity body itself; clicking
                            // empty space clears point + entity together on click-up.
                            uint32_t bodyHitSceneId = sceneId;
                            Entity bodyHit = findSelectableObjectByRay(sceneId, x, y, &bodyHitSceneId);
                            if (bodyHitSceneId == sceneId && bodyHit == selEntity) {
                                sceneProject->sceneRender->clearOccluderPointSelection();
                                project->updateSceneRender(sceneId);
                                sceneProject->sceneRender->mouseHoverEvent(x, y);
                            }
                        }
                    }
                }
            }

            // Line endpoint sub-selection within a selected lines entity (2D gizmo
            // or 3D translate; the side guard mirrors the instance block below)
            if (gizmoSelected == GizmoSelected::OBJECT2D || gizmoSelected == GizmoSelected::TRANSLATE) {
                std::vector<Entity> selEntities = project->getSelectedEntities(sceneId);
                if (selEntities.size() == 1 && !io.KeyShift) {
                    Entity selEntity = selEntities[0];
                    Gizmo2DSideSelected gizmo2DSide = sceneProject->sceneRender->getActiveToolsLayer()->getGizmo2DSideSelected();
                    bool blockPointHit;
                    if (gizmoSelected == GizmoSelected::OBJECT2D) {
                        blockPointHit = !(gizmo2DSide == Gizmo2DSideSelected::NONE || gizmo2DSide == Gizmo2DSideSelected::CENTER);
                    } else {
                        blockPointHit = sceneProject->sceneRender->isAnyGizmoSideSelected();
                    }
                    if (!blockPointHit) {
                        int pointHit = sceneProject->sceneRender->hitTestLinePoint(selEntity, x, y);
                        if (pointHit >= 0) {
                            sceneProject->sceneRender->selectLinePoint(selEntity, pointHit);
                            subSelectionClickConsumesRelease[sceneId] = true;
                            // Re-update so the gizmo jumps to the endpoint
                            project->updateSceneRender(sceneId);
                            sceneProject->sceneRender->mouseHoverEvent(x, y);
                        } else if (sceneProject->sceneRender->getSelectedLinePointIndex() >= 0 && !sceneProject->sceneRender->isAnyGizmoSideSelected()) {
                            // Only clear eagerly when clicking the entity body itself; clicking
                            // empty space clears point + entity together on click-up.
                            uint32_t bodyHitSceneId = sceneId;
                            Entity bodyHit = findSelectableObjectByRay(sceneId, x, y, &bodyHitSceneId);
                            if (bodyHitSceneId == sceneId && bodyHit == selEntity) {
                                sceneProject->sceneRender->clearLinePointSelection();
                                project->updateSceneRender(sceneId);
                                sceneProject->sceneRender->mouseHoverEvent(x, y);
                            }
                        }
                    }
                }
            }

            // Polygon / MeshPolygon vertex sub-selection within a selected polygon entity
            // (2D gizmo or 3D translate; same side guards as the line block above)
            if (gizmoSelected == GizmoSelected::OBJECT2D || gizmoSelected == GizmoSelected::TRANSLATE) {
                std::vector<Entity> selEntities = project->getSelectedEntities(sceneId);
                if (selEntities.size() == 1 && !io.KeyShift) {
                    Entity selEntity = selEntities[0];
                    bool isMeshPolygon = sceneProject->scene->findComponent<MeshPolygonComponent>(selEntity) != nullptr;
                    bool isPolygon = sceneProject->scene->findComponent<PolygonComponent>(selEntity) != nullptr;
                    if (isMeshPolygon || isPolygon) {
                        Gizmo2DSideSelected gizmo2DSide = sceneProject->sceneRender->getActiveToolsLayer()->getGizmo2DSideSelected();
                        bool blockPointHit;
                        if (gizmoSelected == GizmoSelected::OBJECT2D) {
                            blockPointHit = !(gizmo2DSide == Gizmo2DSideSelected::NONE || gizmo2DSide == Gizmo2DSideSelected::CENTER);
                        } else {
                            blockPointHit = sceneProject->sceneRender->isAnyGizmoSideSelected();
                        }
                        if (!blockPointHit) {
                            int pointHit = sceneProject->sceneRender->hitTestPolygonPoint(selEntity, isMeshPolygon, x, y);
                            if (pointHit >= 0) {
                                sceneProject->sceneRender->selectPolygonPoint(selEntity, isMeshPolygon, pointHit);
                                subSelectionClickConsumesRelease[sceneId] = true;
                                // Re-update so the gizmo jumps to the vertex
                                project->updateSceneRender(sceneId);
                                sceneProject->sceneRender->mouseHoverEvent(x, y);
                            } else if (sceneProject->sceneRender->getSelectedPolygonPointIndex() >= 0 && !sceneProject->sceneRender->isAnyGizmoSideSelected()) {
                                uint32_t bodyHitSceneId = sceneId;
                                Entity bodyHit = findSelectableObjectByRay(sceneId, x, y, &bodyHitSceneId);
                                if (bodyHitSceneId == sceneId && bodyHit == selEntity) {
                                    sceneProject->sceneRender->clearPolygonPointSelection();
                                    project->updateSceneRender(sceneId);
                                    sceneProject->sceneRender->mouseHoverEvent(x, y);
                                }
                            }
                        }
                    }
                }
            }

            // TranslateTracks keyframe point sub-selection within a selected tracks entity
            // (2D gizmo or 3D translate; same side guards as the line block above)
            if (gizmoSelected == GizmoSelected::OBJECT2D || gizmoSelected == GizmoSelected::TRANSLATE) {
                std::vector<Entity> selEntities = project->getSelectedEntities(sceneId);
                if (selEntities.size() == 1 && !io.KeyShift) {
                    Entity selEntity = selEntities[0];
                    if (sceneProject->scene->findComponent<TranslateTracksComponent>(selEntity)) {
                        Gizmo2DSideSelected gizmo2DSide = sceneProject->sceneRender->getActiveToolsLayer()->getGizmo2DSideSelected();
                        bool blockPointHit;
                        if (gizmoSelected == GizmoSelected::OBJECT2D) {
                            blockPointHit = !(gizmo2DSide == Gizmo2DSideSelected::NONE || gizmo2DSide == Gizmo2DSideSelected::CENTER);
                        } else {
                            blockPointHit = sceneProject->sceneRender->isAnyGizmoSideSelected();
                        }
                        if (!blockPointHit) {
                            int pointHit = sceneProject->sceneRender->hitTestTrackPoint(selEntity, x, y);
                            if (pointHit >= 0) {
                                sceneProject->sceneRender->selectTrackPoint(selEntity, pointHit);
                                subSelectionClickConsumesRelease[sceneId] = true;
                                // Re-update so the gizmo jumps to the point
                                project->updateSceneRender(sceneId);
                                sceneProject->sceneRender->mouseHoverEvent(x, y);
                            }
                        }
                    }
                }
            }

            // Tile sub-selection within a selected tilemap (outside disableSelection guard
            // so clicking a tile works even when the tilemap gizmo body is hovered)
            if (gizmoSelected == GizmoSelected::OBJECT2D) {
                std::vector<Entity> selEntities = project->getSelectedEntities(sceneId);
                if (selEntities.size() == 1 && !io.KeyShift) {
                    Entity selEntity = selEntities[0];
                    Gizmo2DSideSelected gizmo2DSide = sceneProject->sceneRender->getActiveToolsLayer()->getGizmo2DSideSelected();
                    if (gizmo2DSide == Gizmo2DSideSelected::NONE || gizmo2DSide == Gizmo2DSideSelected::CENTER) {
                        int tileHit = sceneProject->sceneRender->hitTestTile(selEntity, x, y);
                        if (tileHit >= 0) {
                            sceneProject->sceneRender->selectTile(selEntity, tileHit);
                            subSelectionClickConsumesRelease[sceneId] = true;
                            // Re-update so gizmo wraps the tile
                            project->updateSceneRender(sceneId);
                            sceneProject->sceneRender->mouseHoverEvent(x, y);
                        } else if (sceneProject->sceneRender->getSelectedTileIndex() >= 0 && gizmo2DSide == Gizmo2DSideSelected::NONE) {
                            // Only clear eagerly when clicking the entity body itself. When
                            // clicking empty space the click-up handler clears both the tile
                            // and the entity together, avoiding a two-step gizmo jump.
                            uint32_t bodyHitSceneId = sceneId;
                            Entity bodyHit = findSelectableObjectByRay(sceneId, x, y, &bodyHitSceneId);
                            if (bodyHitSceneId == sceneId && bodyHit == selEntity) {
                                sceneProject->sceneRender->clearTileSelection();
                                project->updateSceneRender(sceneId);
                                sceneProject->sceneRender->mouseHoverEvent(x, y);
                            }
                        }
                    }
                }
            }

            // Instance sub-selection within a selected InstancedMeshComponent entity
            {
                std::vector<Entity> selEntities = project->getSelectedEntities(sceneId);
                if (selEntities.size() == 1 && !io.KeyShift) {
                    Entity selEntity = selEntities[0];
                    bool blockInstanceHit;
                    if (gizmoSelected == GizmoSelected::OBJECT2D) {
                        // With the 2D gizmo, CENTER is set when hovering over the entity body —
                        // the same region where instances live. Allow hit-testing in that case,
                        // mirroring the logic used for tile sub-selection.
                        Gizmo2DSideSelected gizmo2DSide = sceneProject->sceneRender->getActiveToolsLayer()->getGizmo2DSideSelected();
                        blockInstanceHit = !(gizmo2DSide == Gizmo2DSideSelected::NONE || gizmo2DSide == Gizmo2DSideSelected::CENTER);
                    } else {
                        blockInstanceHit = sceneProject->sceneRender->isAnyGizmoSideSelected();
                    }
                    if (!blockInstanceHit) {
                        int instHit = sceneProject->sceneRender->hitTestInstance(selEntity, x, y);
                        if (instHit >= 0) {
                            sceneProject->sceneRender->selectInstance(selEntity, instHit);
                            subSelectionClickConsumesRelease[sceneId] = true;
                            project->updateSceneRender(sceneId);
                            sceneProject->sceneRender->mouseHoverEvent(x, y);
                        } else if (sceneProject->sceneRender->getSelectedInstanceIndex() >= 0) {
                            // Only clear eagerly when clicking the entity body itself, so the
                            // gizmo correctly snaps to the entity level. When clicking empty
                            // space the click-up selectObjectByRay handler will clear both the
                            // instance and the entity at once, avoiding a two-step visual where
                            // the gizmo briefly jumps to entity position before disappearing.
                            uint32_t bodyHitSceneId = sceneId;
                            Entity bodyHit = findSelectableObjectByRay(sceneId, x, y, &bodyHitSceneId);
                            if (bodyHitSceneId == sceneId && bodyHit == selEntity) {
                                sceneProject->sceneRender->clearInstanceSelection();
                                project->updateSceneRender(sceneId);
                                sceneProject->sceneRender->mouseHoverEvent(x, y);
                            }
                        }
                    }
                }
            }
            // Alt+click on gizmo: duplicate before starting drag (Shift is reserved
            // for aspect-ratio lock on 2D corner handles, see mouseDragEvent).
            if (altGizmoDuplicate){
                int tileIdx = sceneProject->sceneRender->getSelectedTileIndex();
                Entity tileEntity = sceneProject->sceneRender->getSelectedTileEntity();
                if (tileIdx >= 0) {
                    // Duplicate tile
                    Command* dupCmd = ProjectUtils::buildDuplicateTileCmd(project, sceneId, tileEntity, (unsigned int)tileIdx);
                    if (dupCmd) {
                        CommandHandle::get(sceneId)->addCommand(dupCmd);
                        TilemapComponent* tilemap = sceneProject->scene->findComponent<TilemapComponent>(tileEntity);
                        if (tilemap) {
                            sceneProject->sceneRender->selectTile(tileEntity, (int)tilemap->numTiles - 1);
                        }
                        project->updateSceneRender(sceneId);
                        sceneProject->sceneRender->mouseHoverEvent(x, y);
                    }
                } else if (sceneProject->sceneRender->getSelectedInstanceIndex() >= 0) {
                    int instIdx = sceneProject->sceneRender->getSelectedInstanceIndex();
                    Entity instEntity = sceneProject->sceneRender->getSelectedInstanceEntity();
                    Command* dupCmd = ProjectUtils::buildDuplicateInstanceCmd(project, sceneId, instEntity, (unsigned int)instIdx);
                    if (dupCmd) {
                        CommandHandle::get(sceneId)->addCommand(dupCmd);
                        InstancedMeshComponent* instmesh = sceneProject->scene->findComponent<InstancedMeshComponent>(instEntity);
                        if (instmesh) {
                            sceneProject->sceneRender->clearTileSelection();
                            sceneProject->sceneRender->selectInstance(instEntity, (int)instmesh->instances.size() - 1);
                        }
                        project->updateSceneRender(sceneId);
                        sceneProject->sceneRender->mouseHoverEvent(x, y);
                    }
                } else {
                    // Duplicate entities
                    std::vector<Entity> selectedEntities = project->getSelectedEntities(sceneId);
                    if (!selectedEntities.empty()){
                        auto* dupCmd = new DuplicateEntityCmd(project, sceneId, selectedEntities);
                        CommandHandle::get(sceneId)->addCommandNoMerge(dupCmd);
                        project->updateSceneRender(sceneId);
                        sceneProject->sceneRender->mouseHoverEvent(x, y);
                    }
                }
            }
            sceneProject->sceneRender->mouseClickEvent(x, y, project->getSelectedEntities(editSceneId));
        }

        if (!(ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right))){
            sceneProject->sceneRender->mouseHoverEvent(x, y);
        }

        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && (!altHeld || altGizmoDrag) && !suppressLeftMouse){
            if (!mouseLeftDown){
                mouseLeftStartPos = Vector2(logicalX, logicalY);
                mouseLeftDown = true;
            }
            mouseLeftDragPos = Vector2(logicalX, logicalY);
            // Terrain brush strokes skip the drag threshold: holding the button
            // keeps applying flow and tiny precise drags still paint.
            if (mouseLeftStartPos.distance(mouseLeftDragPos) > 5 || sceneProject->sceneRender->isTerrainEditing()){
                mouseLeftDraggedInside = true;
            }
            if (mouseLeftDraggedInside){
                float origX = mouseLeftStartPos.x;
                float origY = mouseLeftStartPos.y;
                toEngineCanvas(sceneId, origX, origY);
                sceneProject->sceneRender->mouseDragEvent(x, y, origX, origY, project, sceneId, project->getSelectedEntities(editSceneId), disableSelection, io.KeyCtrl, io.KeyShift);
            }
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && (!altHeld || altGizmoDrag) && !suppressLeftMouse){
            bool consumeSelectionRelease = subSelectionClickConsumesRelease[sceneId];
            if (!mouseLeftDraggedInside && mouseLeftDown && !disableSelection && !consumeSelectionRelease){
                // Remember sub-selection anchors BEFORE re-selecting, so we can tell
                // whether the release click actually changed the host entity.
                int prevTileIndex = sceneProject->sceneRender->getSelectedTileIndex();
                Entity prevTileEntity = sceneProject->sceneRender->getSelectedTileEntity();
                int prevInstanceIndex = sceneProject->sceneRender->getSelectedInstanceIndex();
                Entity prevInstanceEntity = sceneProject->sceneRender->getSelectedInstanceEntity();
                int prevOccluderPointIndex = sceneProject->sceneRender->getSelectedOccluderPointIndex();
                Entity prevOccluderPointEntity = sceneProject->sceneRender->getSelectedOccluderPointEntity();
                int prevLinePointIndex = sceneProject->sceneRender->getSelectedLinePointIndex();
                Entity prevLinePointEntity = sceneProject->sceneRender->getSelectedLinePointEntity();
                int prevPolygonPointIndex = sceneProject->sceneRender->getSelectedPolygonPointIndex();
                Entity prevPolygonPointEntity = sceneProject->sceneRender->getSelectedPolygonPointEntity();
                int prevTrackPointIndex = sceneProject->sceneRender->getSelectedTrackPointIndex();
                Entity prevTrackPointEntity = sceneProject->sceneRender->getSelectedTrackPointEntity();

                selectObjectByRay(sceneId, x, y, io.KeyShift);

                // Sub-selection is only meaningful while its host entity is the sole
                // selected entity. Clear it whenever that invariant no longer holds
                // (different entity picked, nothing picked, or multi-select).
                std::vector<Entity> selAfter = project->getSelectedEntities(sceneId);
                bool soleHost = selAfter.size() == 1;
                if (prevTileIndex >= 0 && !(soleHost && selAfter[0] == prevTileEntity)){
                    sceneProject->sceneRender->clearTileSelection();
                }
                if (prevInstanceIndex >= 0 && !(soleHost && selAfter[0] == prevInstanceEntity)){
                    sceneProject->sceneRender->clearInstanceSelection();
                }
                if (prevOccluderPointIndex >= 0 && !(soleHost && selAfter[0] == prevOccluderPointEntity)){
                    sceneProject->sceneRender->clearOccluderPointSelection();
                }
                if (prevLinePointIndex >= 0 && !(soleHost && selAfter[0] == prevLinePointEntity)){
                    sceneProject->sceneRender->clearLinePointSelection();
                }
                if (prevPolygonPointIndex >= 0 && !(soleHost && selAfter[0] == prevPolygonPointEntity)){
                    sceneProject->sceneRender->clearPolygonPointSelection();
                }
                if (prevTrackPointIndex >= 0 && !(soleHost && selAfter[0] == prevTrackPointEntity)){
                    sceneProject->sceneRender->clearTrackPointSelection();
                }
            }
            subSelectionClickConsumesRelease[sceneId] = false;
            mouseLeftDown = false;
        }
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)){
        float x = mousePos.x - windowPos.x;
        float y = mousePos.y - windowPos.y;
        toEngineCanvas(sceneId, x, y);

        if (suppressLeftMouse){
            suppressLeftMouseUntilRelease[sceneId] = false;
            subSelectionClickConsumesRelease[sceneId] = false;
            mouseLeftDown = false;
            mouseLeftDraggedInside = false;
        }else{

            if (mouseLeftDraggedInside && !disableSelection){
                Vector2 clickStartPos = Vector2((2 * mouseLeftStartPos.x / width[sceneId]) - 1, -((2 * mouseLeftStartPos.y / height[sceneId]) - 1));
                Vector2 clickEndPos = Vector2((2 * mouseLeftDragPos.x / width[sceneId]) - 1, -((2 * mouseLeftDragPos.y / height[sceneId]) - 1));
                sceneProject->sceneRender->clearTileSelection();
                sceneProject->sceneRender->clearInstanceSelection();
                sceneProject->sceneRender->clearOccluderPointSelection();
                sceneProject->sceneRender->clearLinePointSelection();
                sceneProject->sceneRender->clearPolygonPointSelection();
                sceneProject->sceneRender->clearTrackPointSelection();
                project->selectObjectsByRect(sceneId, clickStartPos, clickEndPos);
                project->syncSceneRenderEditLayer(sceneId);
            }

            sceneProject->sceneRender->mouseReleaseEvent(x, y);
            subSelectionClickConsumesRelease[sceneId] = false;

            mouseLeftDraggedInside = false;
        }
    }

    if (isMouseInWindow && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
            (altHeld && !gizmoSideActive && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !suppressLeftMouse))) {
        draggingMouse[sceneId] = true;

        // X11 still delivers motion queued before the pointer was warped to the centre,
        // which converts to one huge delta, so the first motion of a look is discarded.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            resyncLookDelta[sceneId] = true;
            // Remember where the look began so we can restore it on release (see below)
            lookActive[sceneId] = true;
            lookReturnPos[sceneId] = mousePos;
        }

        ImGui::SetWindowFocus();
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle) || ImGui::IsMouseReleased(ImGuiMouseButton_Right) ||
            (altHeld && ImGui::IsMouseReleased(ImGuiMouseButton_Left))){
        draggingMouse[sceneId] = false;
        Backend::enableMouseCursor();
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

        // The captured cursor left io.MousePos far outside the window, which would fail the
        // isMouseInWindow test, so snap it back before the backend restore lands next frame.
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && lookActive[sceneId]) {
            io.AddMousePosEvent(lookReturnPos[sceneId].x, lookReturnPos[sceneId].y);
            lookActive[sceneId] = false;
        }
    }

    // Focus loss clears the buttons without reporting a release, and the Alt+LMB term
    // needs Alt still held, so end stale drags from the button state.
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle) &&
            !ImGui::IsMouseDown(ImGuiMouseButton_Right) &&
            (draggingMouse[sceneId] || lookActive[sceneId] || (io.ConfigFlags & ImGuiConfigFlags_NoMouse))) {
        endLook(sceneId);
    }

    Camera* camera = sceneProject->sceneRender->getCamera();

    bool walkingMode = false;

    if (sceneProject->sceneType == SceneType::SCENE_3D){

        SceneRender3D* sceneRender3D = static_cast<SceneRender3D*>(sceneProject->sceneRender);

        float distanceFromTarget = camera->getDistanceFromTarget();

        // Scale factor: movements should be proportional to distance
        // Base movement rate is maintained at around distanceFromTarget = 10
        float distanceScaleFactor = std::max(0.1f, distanceFromTarget / 10.0f);

        // Check for mouse clicks
        if (draggingMouse[sceneId] && (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right))) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                walkingMode = true;

                float lookDeltaX = mouseDelta.x;
                float lookDeltaY = mouseDelta.y;

                // Drop the first motion of the look, it may predate the warp (see above)
                if (resyncLookDelta[sceneId]) {
                    if (lookDeltaX != 0.0f || lookDeltaY != 0.0f) {
                        lookDeltaX = 0.0f;
                        lookDeltaY = 0.0f;
                        resyncLookDelta[sceneId] = false;
                    }
                }

                camera->rotateView(-0.1 * lookDeltaX);
                camera->elevateView(-0.1 * lookDeltaY);

                // Restore up vector after rotation (fixes Y-snap leaving Z as up)
                Vector3 dir = camera->getWorldDirection().normalize();
                if (std::abs(dir.y) < 0.99f) {
                    camera->setUp(0, 1, 0);
                }

                if (mouseWheel != 0.0f) {
                    sceneRender3D->setWalkSpeedOffset(sceneRender3D->getWalkSpeedOffset() + mouseWheel);
                }
                float walkSpeed = DEFAULT_EDITOR_WALK_SPEED + sceneRender3D->getWalkSpeedOffset();

                sceneRender3D->getUILayer()->showSpeedGauge(
                    walkSpeed / DEFAULT_EDITOR_WALK_SPEED,
                    MIN_EDITOR_WALK_SPEED / DEFAULT_EDITOR_WALK_SPEED,
                    MAX_EDITOR_WALK_SPEED / DEFAULT_EDITOR_WALK_SPEED);

                // Speed is per second, with the delta clamped so a stall cannot jump
                float finalSpeed = 1.2f * walkSpeed * distanceScaleFactor *
                                   std::min(io.DeltaTime, 0.1f);

                // Shift multiplier for faster movement
                if (ImGui::IsKeyDown(ImGuiMod_Shift)) {
                    finalSpeed *= 3.0f;
                }

                if (ImGui::IsKeyDown(ImGuiKey_W)) {
                    camera->slideForward(finalSpeed);
                }
                if (ImGui::IsKeyDown(ImGuiKey_S)) {
                    camera->slideForward(-finalSpeed);
                }
                if (ImGui::IsKeyDown(ImGuiKey_A)) {
                    camera->slide(-finalSpeed);
                }
                if (ImGui::IsKeyDown(ImGuiKey_D)) {
                    camera->slide(finalSpeed);
                }
                if (ImGui::IsKeyDown(ImGuiKey_E)) {
                    camera->slideUp(finalSpeed);
                }
                if (ImGui::IsKeyDown(ImGuiKey_Q)) {
                    camera->slideUp(-finalSpeed);
                }

                io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
                Backend::disableMouseCursor();
            }
            if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
                if (ImGui::IsKeyDown(ImGuiMod_Shift)) {
                    camera->slide(-0.01 * mouseDelta.x * distanceScaleFactor);
                    camera->slideUp(0.01 * mouseDelta.y * distanceScaleFactor);
                } else if (ImGui::IsKeyDown(ImGuiMod_Ctrl)) {
                    sceneRender3D->zoomCamera(-0.1f * mouseDelta.y * distanceScaleFactor);
                } else {
                    camera->rotatePosition(-0.1 * mouseDelta.x);
                    camera->elevatePosition(0.1 * mouseDelta.y);

                    // Restore up vector after orbit (fixes Y-snap leaving Z as up)
                    Vector3 dir = camera->getWorldDirection().normalize();
                    if (std::abs(dir.y) < 0.99f && camera->getUp() != Vector3(0, 1, 0)) {
                        camera->setUp(0, 1, 0);
                    }
                }
            }
        }

        // Alt+LMB orbit
        if (draggingMouse[sceneId] && altHeld && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            camera->rotatePosition(-0.1 * mouseDelta.x);
            camera->elevatePosition(0.1 * mouseDelta.y);

            // Restore up vector after orbit (fixes Y-snap leaving Z as up)
            Vector3 dir = camera->getWorldDirection().normalize();
            if (std::abs(dir.y) < 0.99f && camera->getUp() != Vector3(0, 1, 0)) {
                camera->setUp(0, 1, 0);
            }
        }

        if (isMouseInWindow && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !altHeld && handPanEnabled) {
            camera->slide(-0.01 * mouseDelta.x * distanceScaleFactor);
            camera->slideUp(0.01 * mouseDelta.y * distanceScaleFactor);
        }

        if (!walkingMode) {
            sceneRender3D->getUILayer()->hideSpeedGauge();
        }

        // The zoom speed itself can remain constant
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            if (isMouseInWindow && mouseWheel != 0.0f) {
                sceneRender3D->zoomCamera(2.0f * mouseWheel * distanceScaleFactor);
            }
        }

    }else{

        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || 
            (draggingMouse[sceneId] && altHeld && ImGui::IsMouseDown(ImGuiMouseButton_Left)) ||
            (ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsKeyDown(ImGuiKey_Space)) ||
            (isMouseInWindow && ImGui::IsMouseDown(ImGuiMouseButton_Left) && handPanEnabled)){

            SceneRender2D* sceneRender2D = static_cast<SceneRender2D*>(sceneProject->sceneRender);
            float currentZoom = sceneRender2D->getZoom();
            const ImVec2 scale = canvasEngineScale(sceneId);

            float slideX = -currentZoom * mouseDelta.x * scale.x;
            float slideY = -currentZoom * mouseDelta.y * scale.y;

            if (sceneProject->sceneType == SceneType::SCENE_2D){
                slideY = -slideY;
            }

            camera->slide(slideX);
            camera->slideUp(slideY);
        }

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)){
            if (isMouseInWindow && mouseWheel != 0.0f){
                float zoomFactor = 1.0f - (0.1f * mouseWheel);

                float mouseX = mousePos.x - windowPos.x;
                float mouseY = mousePos.y - windowPos.y;
                toEngineCanvas(sceneProject->id, mouseX, mouseY);

                if (sceneProject->sceneType == SceneType::SCENE_2D){
                    mouseY = getHeight(sceneProject->id) - mouseY;
                }

                SceneRender2D* sceneRender2D = static_cast<SceneRender2D*>(sceneProject->sceneRender);
                sceneRender2D->zoomAtPosition(getWidth(sceneProject->id), getHeight(sceneProject->id), Vector2(mouseX, mouseY), zoomFactor);
            }
        }

    }

    if (!ImGui::IsAnyItemActive() && !ImGui::IsAnyItemFocused()){
        if (project->getSelectedSceneId() == sceneId){
            if (!walkingMode){

                // Brush shortcuts while terrain editing: [ / ] resize the brush,
                // Shift+[ / Shift+] adjust its strength.
                if (TerrainEditWindow* terrainEditWindow = Backend::getApp().getTerrainEditWindow()){
                    if (terrainEditWindow->isEditingScene(sceneProject->scene)){
                        const bool shiftHeld = ImGui::IsKeyDown(ImGuiMod_Shift);
                        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)){
                            if (shiftHeld){
                                terrainEditWindow->adjustBrushStrength(1.0f / 1.15f);
                            }else{
                                terrainEditWindow->adjustBrushSize(1.0f / 1.15f);
                            }
                        }
                        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)){
                            if (shiftHeld){
                                terrainEditWindow->adjustBrushStrength(1.15f);
                            }else{
                                terrainEditWindow->adjustBrushSize(1.15f);
                            }
                        }
                    }
                }

                // Tool shortcuts (Q/W/E/R/T, X) live in the main toolbar.

                // F key: Focus camera on selected entities
                if (ImGui::IsKeyPressed(ImGuiKey_F)) {
                    frameSelection(sceneId);
                }

                // Home key: Frame all objects in the scene
                if (sceneProject->sceneType == SceneType::SCENE_3D && ImGui::IsKeyPressed(ImGuiKey_Home)) {
                    focusOnEntities(sceneProject, sceneProject->entities);
                }

                // Numpad preset views (3D only)
                if (sceneProject->sceneType == SceneType::SCENE_3D) {
                    Camera* cam = sceneProject->sceneRender->getCamera();
                    if (!ImGui::IsKeyDown(ImGuiMod_Ctrl)) {
                        if (ImGui::IsKeyPressed(ImGuiKey_Keypad1)) {
                            // Numpad 1: Front view (-Z looking toward +Z)
                            snapCameraToDirection(cam, Vector3(0, 0, 1));
                        }
                        if (ImGui::IsKeyPressed(ImGuiKey_Keypad3)) {
                            // Numpad 3: Right view (+X looking toward -X)
                            snapCameraToDirection(cam, Vector3(-1, 0, 0));
                        }
                        if (ImGui::IsKeyPressed(ImGuiKey_Keypad7)) {
                            // Numpad 7: Top view (+Y looking down)
                            snapCameraToDirection(cam, Vector3(0, -1, 0));
                        }
                        if (ImGui::IsKeyPressed(ImGuiKey_Keypad9)) {
                            // Numpad 9: Bottom view (-Y looking up)
                            snapCameraToDirection(cam, Vector3(0, 1, 0));
                        }
                    } else {
                        // Ctrl+Numpad for opposite directions
                        if (ImGui::IsKeyPressed(ImGuiKey_Keypad1)) {
                            // Ctrl+Numpad 1: Back view (+Z looking toward -Z)
                            snapCameraToDirection(cam, Vector3(0, 0, -1));
                        }
                        if (ImGui::IsKeyPressed(ImGuiKey_Keypad3)) {
                            // Ctrl+Numpad 3: Left view (-X looking toward +X)
                            snapCameraToDirection(cam, Vector3(1, 0, 0));
                        }
                        if (ImGui::IsKeyPressed(ImGuiKey_Keypad7)) {
                            // Ctrl+Numpad 7: Bottom view (-Y looking up)
                            snapCameraToDirection(cam, Vector3(0, 1, 0));
                        }
                        if (ImGui::IsKeyPressed(ImGuiKey_Keypad9)) {
                            // Ctrl+Numpad 9: Top view (+Y looking down)
                            snapCameraToDirection(cam, Vector3(0, -1, 0));
                        }
                    }
                }
            }
        }
    }

}

bool editor::SceneWindow::hasFrameableSelection(uint32_t sceneId) {
    const uint32_t propertiesSceneId = project->getSelectedSceneForProperties();
    if (propertiesSceneId != sceneId && project->isSceneViewLayer(sceneId, propertiesSceneId)) {
        return project->hasSelectedEntities(propertiesSceneId);
    }
    return project->getScene(sceneId) && project->hasSelectedEntities(sceneId);
}

void editor::SceneWindow::frameSelection(uint32_t sceneId) {
    const uint32_t propertiesSceneId = project->getSelectedSceneForProperties();
    if (propertiesSceneId != sceneId && project->isSceneViewLayer(sceneId, propertiesSceneId)
            && project->hasSelectedEntities(propertiesSceneId)) {
        frameEntities(sceneId, propertiesSceneId, project->getSelectedEntities(propertiesSceneId));
        return;
    }
    if (project->getScene(sceneId)) {
        frameEntities(sceneId, sceneId, project->getSelectedEntities(sceneId));
    }
}

void editor::SceneWindow::frameEntities(uint32_t viewSceneId, uint32_t entitySceneId, const std::vector<Entity>& entities) {
    SceneProject* sceneProject = project->getScene(viewSceneId);
    if (!sceneProject || entities.empty()) return;

    if (entitySceneId == viewSceneId) {
        focusOnEntities(sceneProject, entities);
        return;
    }

    // A layer scene is drawn through its own camera, pinned to the view (over the whole
    // view, or the running game's canvas): the editor camera does not move it, and every
    // GameObject it draws already shows where the game puts it, so the view stays.
    // Its GameObjects are never framed as the scene's own: an id means another object there.
    if (!project->isSceneViewLayer(viewSceneId, entitySceneId)) return;
    sceneProject->needUpdateRender = true;
}

void editor::SceneWindow::focusOnEntities(SceneProject* sceneProject, const std::vector<Entity>& entities) {
    if (entities.empty() || !sceneProject || !sceneProject->sceneRender) return;

    Camera* camera = sceneProject->sceneRender->getCamera();

    if (sceneProject->sceneType != SceneType::SCENE_3D) {
        // 2D: center the view on the bounds and zoom so they fit with a margin
        AABB bounds;
        for (Entity entity : entities) {
            AABB entityAABB = sceneProject->sceneRender->getEntitiesAABB({entity});
            if (!entityAABB.isNull() && !entityAABB.isInfinite()) {
                bounds.merge(entityAABB);
            } else if (Transform* transform = sceneProject->scene->findComponent<Transform>(entity)) {
                bounds.merge(transform->worldPosition);
            }
        }
        const int viewWidth = getWidth(sceneProject->id);
        const int viewHeight = getHeight(sceneProject->id);
        if (bounds.isNull() || bounds.isInfinite() || viewWidth <= 0 || viewHeight <= 0) return;

        SceneRender2D* sceneRender2D = static_cast<SceneRender2D*>(sceneProject->sceneRender);
        const Vector3 size = bounds.getSize();
        float zoom = std::max(size.x / (float)viewWidth, size.y / (float)viewHeight) * 1.25f;
        if (!std::isfinite(zoom) || zoom <= 0.0f) {
            zoom = sceneRender2D->getZoom();  // a point: keep the zoom, just center it
        }
        zoom = std::clamp(zoom, 0.01f, 1000.0f);
        sceneRender2D->setZoom(zoom);

        // the 2D editor camera sits at the view's corner, at x left and y at the first row
        const Vector3 center = bounds.getCenter();
        const Vector3 position = camera->getPosition();
        const Vector3 target = camera->getTarget();
        const float x = center.x - (float)viewWidth * zoom * 0.5f;
        const float y = center.y - (float)viewHeight * zoom * 0.5f;
        camera->setPosition(x, y, position.z);
        camera->setTarget(x, y, target.z);
        sceneProject->needUpdateRender = true;
        return;
    }

    AABB aabb;
    for (Entity entity : entities) {
        AABB entityAABB = sceneProject->sceneRender->getEntitiesAABB({entity});
        if (!entityAABB.isNull() && !entityAABB.isInfinite()) {
            aabb.merge(entityAABB);
        } else if (Transform* transform = sceneProject->scene->findComponent<Transform>(entity)) {
            // entities without bounds, like lights, still have a world origin to focus on
            aabb.merge(transform->worldPosition);
        }
    }
    if (aabb.isNull() || aabb.isInfinite()) return;

    Vector3 center = aabb.getCenter();
    Vector3 size = aabb.getSize();
    float radius = size.length() * 0.5f;
    radius = std::max(radius, 0.5f);

    float yfovRad = Angle::defaultToRad(camera->getYFov());
    float aspect = camera->getAspect();
    if (aspect <= 0.0f && height[sceneProject->id] > 0) {
        aspect = (float)width[sceneProject->id] / (float)height[sceneProject->id];
    }
    aspect = std::max(aspect, 0.0001f);

    float halfYFov = yfovRad * 0.5f;
    float halfXFov = std::atan(std::tan(halfYFov) * aspect);
    float limitingHalfFov = std::min(halfYFov, halfXFov);
    float paddedRadius = radius * 1.1f;
    float distance = paddedRadius / std::sin(limitingHalfFov);
    distance = std::max(distance, paddedRadius * 2.0f);
    distance = std::clamp(distance, MIN_EDITOR_CAMERA_DISTANCE, MAX_EDITOR_CAMERA_DISTANCE);

    Vector3 direction = camera->getWorldDirection();
    direction.normalize();
    Vector3 newPos = center - direction * distance;

    camera->setPosition(newPos.x, newPos.y, newPos.z);
    camera->setTarget(center.x, center.y, center.z);
    sceneProject->needUpdateRender = true;
}

void editor::SceneWindow::snapCameraToDirection(Camera* camera, const Vector3& direction) {
    Vector3 target = camera->getWorldTarget();
    float distance = camera->getDistanceFromTarget();

    Vector3 newPos = target - direction * distance;
    camera->setPosition(newPos.x, newPos.y, newPos.z);

    // When looking along Y axis, up vector (0,1,0) is parallel to direction — use Z as up instead
    if (std::abs(direction.y) > 0.9f) {
        camera->setUp(0, 0, direction.y > 0 ? -1.0f : 1.0f);
    } else {
        camera->setUp(0, 1, 0);
    }

    camera->setTarget(target.x, target.y, target.z);
}

bool editor::SceneWindow::handleViewportGizmoClick(SceneProject* sceneProject, float canvasX, float canvasY, int canvasWidth) {
    if (sceneProject->sceneType != SceneType::SCENE_3D) return false;

    SceneRender3D* sceneRender3D = static_cast<SceneRender3D*>(sceneProject->sceneRender);
    ViewportGizmo* viewGizmo = sceneRender3D->getViewportGizmo();

    const float gizmoSize = VIEWPORT_GIZMO_SIZE * getOverlayScale(sceneProject->id);

    // The gizmo Image is anchored at TOP_RIGHT of the canvas
    float gizmoLeft = canvasWidth - gizmoSize;
    float gizmoTop = 0.0f;

    // Check if click is within the gizmo area
    if (canvasX < gizmoLeft || canvasX > canvasWidth || canvasY < gizmoTop || canvasY > gizmoSize) {
        return false;
    }

    // Normalize to [-1, 1] within the gizmo area
    float normalizedX = ((canvasX - gizmoLeft) / gizmoSize) * 2.0f - 1.0f;
    float normalizedY = -(((canvasY - gizmoTop) / gizmoSize) * 2.0f - 1.0f); // flip Y

    ViewportGizmoAxis axis = viewGizmo->hitTest(normalizedX, normalizedY);
    if (axis == ViewportGizmoAxis::NONE) {
        return false;
    }

    Camera* camera = sceneProject->sceneRender->getCamera();

    switch (axis) {
        case ViewportGizmoAxis::POSITIVE_X:  snapCameraToDirection(camera, Vector3(-1, 0, 0)); break; // Look from +X toward origin
        case ViewportGizmoAxis::NEGATIVE_X:  snapCameraToDirection(camera, Vector3( 1, 0, 0)); break;
        case ViewportGizmoAxis::POSITIVE_Y:  snapCameraToDirection(camera, Vector3( 0,-1, 0)); break; // Look from +Y (top)
        case ViewportGizmoAxis::NEGATIVE_Y:  snapCameraToDirection(camera, Vector3( 0, 1, 0)); break;
        case ViewportGizmoAxis::POSITIVE_Z:  snapCameraToDirection(camera, Vector3( 0, 0,-1)); break;
        case ViewportGizmoAxis::NEGATIVE_Z:  snapCameraToDirection(camera, Vector3( 0, 0, 1)); break;
        default: break;
    }

    return true;
}

void editor::SceneWindow::drawGizmosMenu(SceneProject& sceneProject) {
    // same table id in every section keeps the columns aligned
    auto beginSection = [](const char* title) {
        ImGui::SeparatorText(title);
        if (!ImGui::BeginTable("scene_settings_table", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
            return false;
        }
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(175.0f));
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(80.0f));
        return true;
    };

    auto drawSettingRow = [&](const char* name, bool& value, bool disabled = false) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (disabled) ImGui::BeginDisabled();
        ImGui::Text("%s", name);
        ImGui::TableSetColumnIndex(1);
        if (ImGui::Checkbox((std::string("##") + name).c_str(), &value)) {
            sceneProject.needUpdateRender = true;
            project->saveWorkspaceFile();
        }
        if (disabled) ImGui::EndDisabled();
    };

    if (beginSection("Overlays")) {
        drawSettingRow(ICON_FA_LINK " Show all joints", sceneProject.displaySettings.showAllJoints);
        drawSettingRow(ICON_FA_BONE " Show all bones", sceneProject.displaySettings.showAllBones);
        drawSettingRow(ICON_FA_CUBES " Show all rigidbodies", sceneProject.displaySettings.showAllBodies);

        drawSettingRow(ICON_FA_CAMERA " Hide camera view", sceneProject.displaySettings.hideCameraView);
        drawSettingRow(ICON_FA_LIGHTBULB " Hide light icons", sceneProject.displaySettings.hideLightIcons, sceneProject.sceneType != SceneType::SCENE_3D);
        drawSettingRow(ICON_FA_VOLUME_HIGH " Hide Audio Source icons", sceneProject.displaySettings.hideSoundIcons);

        drawSettingRow(ICON_FA_SQUARE " Hide container guides", sceneProject.displaySettings.hideContainerGuides, sceneProject.sceneType == SceneType::SCENE_3D);

        if (sceneProject.sceneType != SceneType::SCENE_3D) {
            drawSettingRow(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT " Show origin axis", sceneProject.displaySettings.showOrigin);
        }

        drawSettingRow(ICON_FA_OBJECT_GROUP " Hide selection outline", sceneProject.displaySettings.hideSelectionOutline);

        ImGui::EndTable();
    }

    if (beginSection("Rendering")) {
        if (sceneProject.sceneType == SceneType::SCENE_3D) {
            drawSettingRow(ICON_FA_CLONE " Disable face culling", sceneProject.displaySettings.disableFaceCulling);
        }
        drawSettingRow(ICON_FA_SMOG " Disable fog", sceneProject.displaySettings.disableFog);
        drawSettingRow(ICON_FA_CLOUD " Disable clouds", sceneProject.displaySettings.disableClouds);
        if (sceneProject.sceneType != SceneType::SCENE_UI) {
            drawSettingRow(ICON_FA_WAND_MAGIC_SPARKLES " Show post-processing", sceneProject.displaySettings.showPostProcessing);
        }

        ImGui::EndTable();
    }

    if (beginSection("Grid & snapping")) {
        if (sceneProject.sceneType == SceneType::SCENE_3D) {
            drawSettingRow(ICON_FA_TABLE_CELLS " Show grid", sceneProject.displaySettings.showGrid3D);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", ICON_FA_TABLE_CELLS " Grid spacing");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##GridSpacing3D", &sceneProject.displaySettings.gridSpacing3D, 0.1f, 0.1f, 1000.0f, "%.1f")) {
                sceneProject.needUpdateRender = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                project->saveWorkspaceFile();
            }
        } else {
            drawSettingRow(ICON_FA_TABLE_CELLS " Show grid", sceneProject.displaySettings.showGrid2D);
            if (sceneProject.displaySettings.showGrid2D) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s", ICON_FA_TABLE_CELLS " Grid spacing");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::DragFloat("##GridSpacing2D", &sceneProject.displaySettings.gridSpacing2D, 1.0f, 1.0f, 10000.0f, "%.0f")) {
                    sceneProject.needUpdateRender = true;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    project->saveWorkspaceFile();
                }
            }
        }

        drawSettingRow(ICON_FA_MAGNET " Snap to grid", sceneProject.displaySettings.snapToGrid);
        if (sceneProject.sceneType != SceneType::SCENE_3D) {
            drawSettingRow(ICON_FA_BORDER_ALL " Snap tile", sceneProject.displaySettings.snapTile);
        }

        drawSettingRow(ICON_FA_ROTATE " Snap rotation", sceneProject.displaySettings.snapRotation);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%s", ICON_FA_ROTATE " Rotation step");
        ImGui::TableSetColumnIndex(1);
        if (!sceneProject.displaySettings.snapRotation) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##RotationSnapDegrees", &sceneProject.displaySettings.rotationSnapDegrees, 1.0f, 0.1f, 180.0f, "%.1f");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            project->saveWorkspaceFile();
        }
        if (!sceneProject.displaySettings.snapRotation) ImGui::EndDisabled();

        ImGui::EndTable();
    }

    if (sceneProject.sceneType == SceneType::SCENE_3D && sceneProject.sceneRender && beginSection("Editor camera")) {
        Camera* editorCamera = sceneProject.sceneRender->getCamera();

        auto drawFloatSettingRow = [&](const char* name, const char* id, float value, float defaultValue, float dragSpeed, float minValue, float maxValue, const char* format, auto validate, auto apply) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", name);

            bool defChanged = std::fabs(value - defaultValue) > 1e-4f;
            if (defChanged) {
                ImGui::SameLine();
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, ImGui::GetStyle().ItemSpacing.y));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, ImGui::GetStyle().FramePadding.y));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                if (ImGui::Button((std::string(ICON_FA_ROTATE_LEFT) + "##" + id).c_str())) {
                    apply(defaultValue);
                    sceneProject.needUpdateRender = true;
                    project->saveWorkspaceFile();
                }
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(3);
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            float editedValue = value;
            if (ImGui::DragFloat((std::string("##") + id).c_str(), &editedValue, dragSpeed, minValue, maxValue, format)) {
                apply(editedValue);
                validate();
                sceneProject.needUpdateRender = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                project->saveWorkspaceFile();
            }
        };

        auto validateNearClip = [&]() {
            if (editorCamera->getNearClip() >= editorCamera->getFarClip()) {
                editorCamera->setNearClip(editorCamera->getFarClip() - 0.01f);
            }
        };
        auto validateFarClip = [&]() {
            if (editorCamera->getFarClip() <= editorCamera->getNearClip()) {
                editorCamera->setFarClip(editorCamera->getNearClip() + 1.0f);
            }
        };

        drawFloatSettingRow(
            ICON_FA_CAMERA " Editor camera near", "EditorCameraNear",
            editorCamera->getNearClip(), DEFAULT_EDITOR_CAMERA_NEAR,
            0.01f, 0.01f, 100.0f, "%.2f", validateNearClip,
            [&](float v) { editorCamera->setNearClip(v); });

        drawFloatSettingRow(
            ICON_FA_CAMERA " Editor camera far", "EditorCameraFar",
            editorCamera->getFarClip(), DEFAULT_EDITOR_CAMERA_FAR,
            1.0f, 1.0f, 100000.0f, "%.0f", validateFarClip,
            [&](float v) { editorCamera->setFarClip(v); });

        ImGui::EndTable();
    }

}

void editor::SceneWindow::show() {
    for (uint32_t sceneId: closeSceneQueue){
        project->closeScene(sceneId);
    }
    closeSceneQueue.clear();

    // A prefab drag that ended where no Scene view saw it (a hidden tab, another window)
    if (prefabDragSceneId != NULL_PROJECT_SCENE && !ImGui::GetDragDropPayload()) {
        cancelPrefabDrag();
    }

    windowFocused = false;

    int openedScenesCount = 0;
    for (const auto& s : project->getScenes()) {
        if (s.opened) openedScenesCount++;
    }

    // The open scenes, then Prefab Mode's tab for the shown stage
    std::vector<SceneProject*> viewScenes;
    for (auto& openScene : project->getScenes()) {
        if (openScene.opened) viewScenes.push_back(&openScene);
    }
    PrefabStage* activeStage = project->getActivePrefabStage();
    const uint32_t activeStageId = activeStage ? activeStage->sceneProject.id : NULL_PROJECT_SCENE;
    bool focusStage = false;
    uint32_t focusScene = NULL_PROJECT_SCENE;
    if (activeStageId != shownPrefabStage) {
        // a stage opened (or the breadcrumbs went to another): its tab comes forward; Prefab
        // Mode left: the scene it was entered from does
        focusStage = activeStageId != NULL_PROJECT_SCENE;
        if (!focusStage) {
            focusScene = project->getSelectedSceneId();
        }
        shownPrefabStage = activeStageId;
    }
    if (activeStage) {
        viewScenes.push_back(&activeStage->sceneProject);
    }

    // Iterate through all scenes in the project
    for (SceneProject* viewScene : viewScenes) {
        SceneProject& sceneProject = *viewScene;
        const bool isStage = activeStage && &activeStage->sceneProject == viewScene;

        // Disable close button if this is the only open scene (Prefab Mode's tab exits it)
        bool canClose = isStage || openedScenesCount > 1;
        bool isOpen = true;

        const bool isRuntimeActive = sceneProject.playState == ScenePlayState::PLAYING
            || sceneProject.playState == ScenePlayState::PAUSED;
        GameWindow* gameWindow = Backend::getApp().getGameWindow();
        // the running game shows in the Game view; the dot marks a game nobody sees
        const bool gameShown = gameWindow && gameWindow->hostsScene(sceneProject.id) && gameWindow->isVisible();

        if (hasNotification[sceneProject.id]) {
            App::pushTabNotificationStyle();
        }
        ImGuiWindowFlags windowFlags = hasNotification[sceneProject.id] ? ImGuiWindowFlags_UnsavedDocument : 0;
        ImGui::SetNextWindowSizeConstraints(ImVec2(200, 200), ImVec2(FLT_MAX, FLT_MAX));
        if ((isStage && focusStage) || (!isStage && sceneProject.id == focusScene)) {
            ImGui::SetNextWindowFocus();
        }
        const std::string windowTitle = isStage ? getPrefabStageWindowTitle(sceneProject) : getWindowTitle(sceneProject);
        const bool sceneWindowVisible = ImGui::Begin(windowTitle.c_str(), canClose ? &isOpen : nullptr, windowFlags);
        if (hasNotification[sceneProject.id]) App::popTabNotificationStyle();

        if (!isRuntimeActive) {
            hasNotification[sceneProject.id] = false;
        } else if (sceneWindowVisible || gameShown) {
            hasNotification[sceneProject.id] = false;
        } else {
            hasNotification[sceneProject.id] = true;
        }

        if (sceneWindowVisible) {
            sceneProject.isVisible = true;

            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
                windowFocused = true;
                project->setSelectedSceneId(sceneProject.id);
            }

            bool isStopped = (sceneProject.playState == ScenePlayState::STOPPED);
            bool isCameraPreview = isStopped && sceneProject.sceneRender->isPreviewCameraActive();
            Entity previewCameraEntity = isCameraPreview ? sceneProject.sceneRender->getPreviewCameraEntity() : NULL_ENTITY;
            if (isCameraPreview) {
                sceneProject.needUpdateRender = true;
            }

            // Prefab Mode: its breadcrumbs and controls over the Scene view
            if (isStage) {
                drawPrefabStageBar(sceneProject);
            }

            // Scene view toolbar (Unity style): view-specific controls only. Play and the
            // transform tools live in the main toolbar. Full-height buttons vertically
            // centered on a text-height row, overlapping the spacing above/below.
            float toolbarRowY = ImGui::GetCursorPosY();
            ImGui::SetCursorPosY(toolbarRowY - (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f);

            const char* sceneKindLabel = sceneProject.sceneType == SceneType::SCENE_3D ? "3D"
                : (sceneProject.sceneType == SceneType::SCENE_UI ? "UI" : "2D");
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", sceneKindLabel);
            ImGui::SetItemTooltip("%s scene", sceneKindLabel);

            if (isCameraPreview) {
                ImGui::SameLine(0, Theme::dpi(10.0f));

                std::string cameraName = sceneProject.scene->getEntityName(previewCameraEntity);
                if (cameraName.empty()) {
                    cameraName = "Camera";
                }

                ImGui::Text("%s %s", ICON_FA_VIDEO, cameraName.c_str());
                ImGui::SetItemTooltip("Viewing through this camera");
                ImGui::SameLine();
                std::string stopPreviewButtonId = std::string(ICON_FA_XMARK) + "##StopCameraPreview" + std::to_string(sceneProject.id);
                if (ImGui::Button(stopPreviewButtonId.c_str())) {
                    stopViewingCamera(sceneProject.id);
                    isCameraPreview = false;
                }
                ImGui::SetItemTooltip("Exit camera preview (Esc)");
            }

            if (!isStage) {
                drawParentSceneButton(sceneProject);
            }

            {
                const std::string gizmosPopupId = "GizmosPopup" + std::to_string(sceneProject.id);
                const std::string gizmosLabel = std::string("Gizmos ") + ICON_FA_CARET_DOWN + "##Gizmos" + std::to_string(sceneProject.id);
                const float gizmosWidth = ImGui::CalcTextSize(gizmosLabel.c_str(), nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                // the post-processing toggle left of Gizmos (not in UI scenes: never post-processed)
                const bool postToggle = sceneProject.sceneType != SceneType::SCENE_UI;
                const std::string postLabel = std::string(ICON_FA_WAND_MAGIC_SPARKLES) + "##ScenePost" + std::to_string(sceneProject.id);
                const float postWidth = postToggle ? ImGui::CalcTextSize(postLabel.c_str(), nullptr, true).x +
                                                     ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x : 0.0f;
                const float rightX = ImGui::GetWindowContentRegionMax().x - gizmosWidth - postWidth;
                ImGui::SameLine();
                if (rightX > ImGui::GetCursorPosX()) {
                    ImGui::SetCursorPosX(rightX);
                }
                if (postToggle) {
                    bool& show = sceneProject.displaySettings.showPostProcessing;
                    ImGui::PushStyleColor(ImGuiCol_Button, show ? Theme::Colors::ButtonActivated : ImGui::GetStyle().Colors[ImGuiCol_Button]);
                    if (ImGui::Button(postLabel.c_str())) {
                        show = !show;
                        sceneProject.needUpdateRender = true;
                        project->saveWorkspaceFile();
                    }
                    ImGui::PopStyleColor();
                    ImGui::SetItemTooltip(show ? "Post-processing shown in this Scene view (click to hide; the Game view always shows it)"
                                               : "Show the scene's post-processing in this Scene view (the Game view always shows it)");
                    ImGui::SameLine();
                }
                if (ImGui::Button(gizmosLabel.c_str())) {
                    ImGui::OpenPopup(gizmosPopupId.c_str());
                }
                ImGui::SetItemTooltip("Overlays, grid and snapping for this Scene view");

                if (ImGui::BeginPopup(gizmosPopupId.c_str())) {
                    drawGizmosMenu(sceneProject);
                    ImGui::EndPopup();
                }
            }

            ImGui::SetCursorPosY(toolbarRowY + ImGui::GetTextLineHeightWithSpacing());

            ImGui::BeginChild(("Canvas" + std::to_string(sceneProject.id)).c_str());
            {
                if (focusCanvasOnNextFrame[sceneProject.id]) {
                    ImGui::SetWindowFocus();
                    focusCanvasOnNextFrame[sceneProject.id] = false;
                    windowFocused = true;
                    project->setSelectedSceneId(sceneProject.id);
                }

                int widthNew = ImGui::GetContentRegionAvail().x;
                int heightNew = ImGui::GetContentRegionAvail().y;

                ImGuiViewport* canvasViewport = ImGui::GetWindowViewport();
                ImVec2 scale = (canvasViewport && canvasViewport->FramebufferScale.x > 0.0f && canvasViewport->FramebufferScale.y > 0.0f)
                    ? canvasViewport->FramebufferScale
                    : ImGui::GetIO().DisplayFramebufferScale;
                if (scale.x <= 0.0f) scale.x = 1.0f;
                if (scale.y <= 0.0f) scale.y = 1.0f;

                float dpiScale = (canvasViewport && canvasViewport->DpiScale > 0.0f)
                    ? canvasViewport->DpiScale
                    : 1.0f;
                if (dpiScale < 0.5f || dpiScale > 8.0f) {
                    dpiScale = 1.0f;
                }

                const ImVec2 prevScale = framebufferScale[sceneProject.id];
                const float prevDpi = canvasDpiScale[sceneProject.id];
                if (widthNew != width[sceneProject.id] || heightNew != height[sceneProject.id]
                    || prevScale.x != scale.x || prevScale.y != scale.y
                    || prevDpi != dpiScale) {
                    width[sceneProject.id] = widthNew;
                    height[sceneProject.id] = heightNew;
                    framebufferScale[sceneProject.id] = scale;
                    canvasDpiScale[sceneProject.id] = dpiScale;

                    sceneProject.needUpdateRender = true;
                }

                // A stage opened in isolation shows its prefab whole once the view has a size
                if (isStage && activeStage->frameOnOpen && widthNew > 0 && heightNew > 0
                        && sceneProject.scene->isEntityCreated(activeStage->root)) {
                    // after a few drawn frames: the bounds and the camera's direction exist
                    sceneProject.needUpdateRender = true;
                    if (--activeStage->frameDelay <= 0) {
                        activeStage->frameOnOpen = false;
                        focusOnEntities(&sceneProject, {activeStage->root});
                    }
                }

                ImTextureID previewTex = Backend::getImGuiTexture(&sceneProject.sceneRender->getTexture());
                ImVec2 canvasAvail = ImGui::GetContentRegionAvail();

                if (isCameraPreview) {
                    // Fixed-aspect cameras (autoResize == false) render distorted into the
                    // full viewport; display them at their true aspect (letterbox/pillarbox)
                    // so the preview matches the framing they'll actually render. autoResize
                    // cameras already fill the viewport correctly, so they stay full-size.
                    ImVec2 imageSize = canvasAvail;
                    if (CameraComponent* previewCam = sceneProject.scene->findComponent<CameraComponent>(previewCameraEntity)) {
                        if (!previewCam->autoResize && canvasAvail.x > 0 && canvasAvail.y > 0) {
                            float camAspect;
                            if (previewCam->type == CameraType::CAMERA_PERSPECTIVE) {
                                camAspect = previewCam->aspect;
                            } else {
                                float clipW = previewCam->rightClip - previewCam->leftClip;
                                float clipH = previewCam->topClip - previewCam->bottomClip;
                                camAspect = (clipH != 0.0f) ? std::fabs(clipW / clipH) : 0.0f;
                            }
                            if (camAspect > 0.0f) {
                                float viewAspect = canvasAvail.x / canvasAvail.y;
                                if (camAspect > viewAspect) {
                                    imageSize = ImVec2(canvasAvail.x, canvasAvail.x / camAspect);
                                } else {
                                    imageSize = ImVec2(canvasAvail.y * camAspect, canvasAvail.y);
                                }
                            }
                        }
                    }

                    ImVec2 canvasMin = ImGui::GetCursorScreenPos();
                    ImVec2 imageMin(canvasMin.x + (canvasAvail.x - imageSize.x) * 0.5f,
                                    canvasMin.y + (canvasAvail.y - imageSize.y) * 0.5f);
                    ImVec2 imageMax(imageMin.x + imageSize.x, imageMin.y + imageSize.y);

                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    drawList->AddRectFilled(canvasMin, ImVec2(canvasMin.x + canvasAvail.x, canvasMin.y + canvasAvail.y), IM_COL32(0, 0, 0, 255));

                    ImGui::SetCursorScreenPos(imageMin);
                    Widgets::image(previewTex, imageSize);

                    // Yellow frame as a "viewing through camera" cue (inset 1px so it's not clipped at the edge).
                    // Matches the previewed camera's highlight color in the Hierarchy tree.
                    const float frameInset = Theme::dpi(1.0f);
                    drawList->AddRect(ImVec2(imageMin.x + frameInset, imageMin.y + frameInset), ImVec2(imageMax.x - frameInset, imageMax.y - frameInset), IM_COL32(255, 219, 51, 235), 0.0f, 0, Theme::dpi(2.0f));
                } else {
                    // Black until the first render creates the framebuffer.
                    Widgets::image(previewTex, canvasAvail, IM_COL32(0, 0, 0, 255));
                }

                const bool transitioning = sceneProject.playState == ScenePlayState::SAVING
                    || sceneProject.playState == ScenePlayState::LOADING
                    || sceneProject.playState == ScenePlayState::CANCELLING;

                if (!transitioning && !isCameraPreview){
                    if (ImGui::IsWindowHovered()) {
                        CursorSelected cursorSelected = sceneProject.sceneRender->getCursorSelected();
                        if (cursorSelected == CursorSelected::HAND) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                        } else {
                            Gizmo2DSideSelected side = sceneProject.sceneRender->getActiveToolsLayer()->getGizmo2DSideSelected();
                            // y runs down in a UI scene, and in a UI layer the tools edit
                            bool uiScene = sceneProject.sceneRender->hasEditLayer()
                                ? sceneProject.sceneRender->isEditLayerYDown()
                                : sceneProject.sceneType == SceneType::SCENE_UI;
                            if (side == Gizmo2DSideSelected::NX || side == Gizmo2DSideSelected::PX) {
                                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                            } else if (side == Gizmo2DSideSelected::NY || side == Gizmo2DSideSelected::PY) {
                                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                            } else if (side == Gizmo2DSideSelected::NX_NY || side == Gizmo2DSideSelected::PX_PY) {
                                ImGui::SetMouseCursor(uiScene ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
                            } else if (side == Gizmo2DSideSelected::NX_PY || side == Gizmo2DSideSelected::PX_NY) {
                                ImGui::SetMouseCursor(uiScene ? ImGuiMouseCursor_ResizeNESW : ImGuiMouseCursor_ResizeNWSE);
                            }
                        }
                    }

                    // dropped resources become authored entities, which a running scene cannot take
                    if (sceneProject.playState == ScenePlayState::STOPPED){
                        handleResourceFileDragDrop(&sceneProject);
                        handleTileRectDragDrop(&sceneProject);
                    }
                }

                // Viewport gizmo click-to-snap (3D scenes only)
                if (sceneProject.sceneType == SceneType::SCENE_3D && !transitioning && !isCameraPreview) {
                    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        ImVec2 windowPos = ImGui::GetWindowPos();
                        ImGuiIO& io = ImGui::GetIO();
                        float canvasX = io.MousePos.x - windowPos.x;
                        float canvasY = io.MousePos.y - windowPos.y;
                        toEngineCanvas(sceneProject.id, canvasX, canvasY);
                        suppressLeftMouseUntilRelease[sceneProject.id] = handleViewportGizmoClick(&sceneProject, canvasX, canvasY, getWidth(sceneProject.id));
                    }
                }

                sceneEventHandler(&sceneProject);
            }
            ImGui::EndChild();
        } else {
            sceneProject.isVisible = false;
        }
        ImGui::End();

        // Handle window closing (Prefab Mode's tab exits Prefab Mode)
        if (!isOpen) {
            if (isStage) {
                requestExitPrefabStages(0);
            } else {
                handleCloseScene(sceneProject.id);
            }
        }
    }

}

ImVec2 editor::SceneWindow::canvasFramebufferScale(uint32_t sceneId) const {
    auto it = framebufferScale.find(sceneId);
    return (it != framebufferScale.end()) ? it->second : ImVec2(1.0f, 1.0f);
}

float editor::SceneWindow::canvasDpi(uint32_t sceneId) const {
    auto it = canvasDpiScale.find(sceneId);
    return (it != canvasDpiScale.end() && it->second > 0.0f) ? it->second : 1.0f;
}

ImVec2 editor::SceneWindow::canvasEngineScale(uint32_t sceneId) const {
    return Backend::sceneRenderScale(canvasFramebufferScale(sceneId), canvasDpi(sceneId));
}

void editor::SceneWindow::toEngineCanvas(uint32_t sceneId, float& x, float& y) const {
    ImVec2 scale = canvasEngineScale(sceneId);
    x *= scale.x;
    y *= scale.y;
}

bool editor::SceneWindow::getViewRect(uint32_t sceneId, float& x, float& y, float& w, float& h) const {
    auto it = viewRect.find(sceneId);
    if (it == viewRect.end()) {
        return false;
    }
    x = it->second.x;
    y = it->second.y;
    w = it->second.z;
    h = it->second.w;
    return true;
}

int editor::SceneWindow::getWidth(uint32_t sceneId) const{
    const auto it = width.find(sceneId);
    if (it == width.end() || it->second <= 0){
        return 0;
    }
    return std::max(1, (int)std::lround((float)it->second * canvasEngineScale(sceneId).x));
}

int editor::SceneWindow::getHeight(uint32_t sceneId) const{
    const auto it = height.find(sceneId);
    if (it == height.end() || it->second <= 0){
        return 0;
    }
    return std::max(1, (int)std::lround((float)it->second * canvasEngineScale(sceneId).y));
}

float editor::SceneWindow::getOverlayScale(uint32_t sceneId) const {
    return canvasDpi(sceneId) * canvasEngineScale(sceneId).x;
}
