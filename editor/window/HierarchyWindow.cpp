// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HierarchyWindow.h"
#include "titleart/runtime/TitleArtCreate.h"
#include "pixelart/PaletteUse.h"
#include "pixelart/PixelSnap.h"
#include "pixelart/particles/PixelParticlePresets.h"
#include "pixelart/particles/PixelPresetCreate.h"
#include "util/VehicleRig.h"
#include "util/ProjectFileWrites.h"
#include "util/ScriptAttach.h"
#include "audio/SoundDrop.h"
#include "util/UIProbe.h"
#include "component/ActionComponent.h"
#include "component/AlphaTweenComponent.h"
#include "component/BoneComponent.h"
#include "component/ButtonComponent.h"
#include "component/CanvasGroupComponent.h"
#include "component/ColorTweenComponent.h"
#include "component/ForceFieldComponent.h"
#include "component/ImageComponent.h"
#include "component/InputFieldComponent.h"
#include "component/LatticeTracksComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/Light2DComponent.h"
#include "component/LightComponent.h"
#include "component/MirrorComponent.h"
#include "component/MorphTracksComponent.h"
#include "component/OnScreenControlComponents.h"
#include "component/PanelComponent.h"
#include "component/PointsComponent.h"
#include "component/PositionTweenComponent.h"
#include "component/ReflectionProbeComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/RotationTweenComponent.h"
#include "component/SafeAreaComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/ScaleTweenComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/ShadowCaster2DComponent.h"
#include "component/SkyboxComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/TextComponent.h"
#include "component/TranslateTracksComponent.h"
#include "component/UIComponent.h"
#include "component/UIDocumentComponent.h"
#include "window/ProjectWindow.h"
#include "component/CharacterController2DComponent.h"
#include "component/CharacterControllerComponent.h"
#include "component/FogComponent.h"
#include "component/Joint2DComponent.h"
#include "EditorHost.h"
#include "component/ParticleSystemComponent.h"
#include "component/ScriptComponent.h"
#include "component/FluidSimulationComponent.h"
#include "component/TrailRendererComponent.h"
#include "render/SceneRender.h"
#include "component/CameraComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/ModelComponent.h"
#include "component/SpriteComponent.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"
#include "pixelart/aseprite/AsepriteSpriteSetup.h"
#include "sprite/SpriteSheetPool.h"
#include "spline/SplineEditor.h"
#include "util/EditorScripts.h"
#include "shortcuts/CommandCatalog.h"
#include "search/HierarchyFilter.h"
#include "util/EntityLabels.h"
#include "organisation/Bookmarks.h"
#include "window/BookmarksWindow.h"
#include "window/dialog/DataSafetyDialogs.h"
#include "StreamPreserved.h"
#include "util/PhysicsMaterialAssets.h"
#include "window/dialog/CreateTerrainDialog.h"
#include "terrainworld/TerrainWorldPanel.h"
#include "window/dialog/RagdollWizardDialog.h"
#include "window/VoxelCreateDialog.h"
#include "pose/PoseEditSession.h"

#include "App.h"
#include "Backend.h"
#include "animation/AnimationClipAppHooks.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "util/BidiText.h"
#include "command/PlayChangeReview.h"
#include "command/CommandHandle.h"
#include "command/type/ReflectedValueCmd.h"
#include "component/GridComponent.h"
#include "reflect/Reflect.h"
#include "tilemap/import/TilemapImportApply.h"
#include "window/EditorModal.h"
#include "command/type/MoveEntityOrderCmd.h"
#include "component/Rigidbody2DComponent.h"
#include "command/type/AddChildSceneCmd.h"
#include "command/type/RemoveChildSceneCmd.h"
#include "command/type/SetChildSceneStartActiveCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "lattice/LatticeEditorOps.h"
#include "command/type/EntityNameCmd.h"
#include "command/type/SceneNameCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/DuplicateEntityCmd.h"
#include "util/EntityClipboard.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/ObjectTransformCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "vat/VatSceneActions.h"
#include "component/InstancedMeshComponent.h"
#include "component/CapsuleShadowCasterComponent.h"
#include "component/TerrainComponent.h"
#include "subsystem/MeshSystem.h"
#include "command/type/ImportPrefabCmd.h"
#include "command/type/RemoveEntityFromPrefabCmd.h"
#include "command/type/AddEntityToPrefabCmd.h"
#include "command/type/UnpackPrefabCmd.h"
#include "command/type/ReplacePrefabCmd.h"
#include "command/type/SetMainCameraCmd.h"
#include "util/CameraTextureLink.h"
#include "util/EntityPayload.h"
#include "util/UIUtils.h"
#include "util/Util.h"
#include "util/ProjectUtils.h"
#include "Out.h"
#include "Stream.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;

namespace {
    // a Particle System at or below the GameObject (Open in Particle Effect)
    bool subtreeHasParticleSystems(editor::SceneProject* sceneProject, Entity root) {
        if (!sceneProject || !sceneProject->scene || !sceneProject->scene->isComponentRegistered<ParticleSystemComponent>()) return false;
        Scene* scene = sceneProject->scene;
        auto systems = scene->getComponentArray<ParticleSystemComponent>();
        for (size_t i = 0; i < systems->size(); i++) {
            for (Entity e = systems->getEntity(i); e != NULL_ENTITY;) {
                if (e == root) return true;
                const Transform* transform = scene->findComponent<Transform>(e);
                e = transform ? transform->parent : NULL_ENTITY;
            }
        }
        return false;
    }

    // A context menu item's shortcut text, from the command catalog (nullptr when unbound)
    const char* shortcutText(const char* commandId) {
        static std::string text;
        text = editor::CommandCatalog::editor().menuShortcutText(commandId);
        return text.empty() ? nullptr : text.c_str();
    }
}

// Unity's prefab-instance blue, for rows that belong to a prefab
static const ImVec4 PREFAB_TEXT_COLOR = ImVec4(0.48f, 0.68f, 0.98f, 1.0f); // Theme::Colors::PrefabText, #7BAEFA
static const ImVec4 PREFAB_MEMBER_TEXT_COLOR = ImVec4(0.49f, 0.67f, 0.97f, 0.82f);
static const ImVec4 MISSING_PREFAB_TEXT_COLOR = ImVec4(0.96f, 0.36f, 0.36f, 1.0f); // Unity's red Missing Prefab row

static bool hasScripts(Scene* scene, Entity entity) {
    ScriptComponent* scriptComponent = scene->findComponent<ScriptComponent>(entity);
    return scriptComponent && !scriptComponent->scripts.empty();
}

// Collapsed prefab roots also show the script icon when any child has a script
static bool markPrefabScripts(editor::TreeNode& node) {
    bool hasChildScript = false;
    for (editor::TreeNode& child : node.children) {
        if (markPrefabScripts(child)) {
            hasChildScript = true;
        }
    }
    if (node.isPrefabRoot) {
        node.hasChildScript = hasChildScript;
    }
    return node.hasScript || hasChildScript;
}

std::vector<Entity> editor::HierarchyWindow::getTopLevelSelectedEntities(Entity draggedEntity) {
    SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->scene) {
        return {draggedEntity};
    }

    std::vector<Entity> selection = project->getSelectedEntities(sceneProject->id);
    if (selection.empty() || std::find(selection.begin(), selection.end(), draggedEntity) == selection.end()) {
        return {draggedEntity};
    }

    std::vector<Entity> result = Project::getTopLevelEntities(sceneProject->scene, selection);

    if (result.empty()) {
        result.push_back(draggedEntity);
    }

    std::unordered_set<Entity> sceneEntitiesSet(sceneProject->entities.begin(), sceneProject->entities.end());
    std::unordered_map<Entity, size_t> entityOrder;
    size_t order = 0;

    for (Entity entity : sceneProject->entities) {
        Signature signature = sceneProject->scene->getSignature(entity);
        if (!signature.test(sceneProject->scene->getComponentId<Transform>())) {
            entityOrder[entity] = order++;
        }
    }

    auto transforms = sceneProject->scene->getComponentArray<Transform>();
    for (size_t i = 0; i < transforms->size(); i++) {
        Entity entity = transforms->getEntity(i);
        if (sceneEntitiesSet.find(entity) != sceneEntitiesSet.end()) {
            entityOrder[entity] = order++;
        }
    }

    std::stable_sort(result.begin(), result.end(), [&entityOrder](Entity a, Entity b) {
        auto aIt = entityOrder.find(a);
        auto bIt = entityOrder.find(b);
        if (aIt == entityOrder.end() || bIt == entityOrder.end()) {
            return aIt != entityOrder.end();
        }
        return aIt->second < bIt->second;
    });

    return result;
}

std::vector<Entity> editor::HierarchyWindow::getMovableDraggedEntities(Entity draggedEntity, const TreeNode& targetNode, InsertionType type) {
    SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->scene) {
        return {};
    }

    Scene* scene = sceneProject->scene;
    std::vector<Entity> draggedEntities = getTopLevelSelectedEntities(draggedEntity);
    std::vector<Entity> movableEntities;
    movableEntities.reserve(draggedEntities.size());

    if (targetNode.isPlayCreated) {
        return {};
    }

    for (Entity sourceEntity : draggedEntities) {
        if (sourceEntity == NULL_ENTITY || !scene->isEntityCreated(sourceEntity)) {
            return {};
        }

        Transform* sourceTransform = scene->findComponent<Transform>(sourceEntity);
        bool sourceHasTransform = sourceTransform != nullptr;

        if (targetNode.isScene) {
            if (!sourceTransform) {
                return {};
            }
            if (sourceTransform->parent == NULL_ENTITY) {
                continue;
            }
            if (ProjectUtils::isEntityLocked(scene, sourceEntity)
                || ProjectUtils::getModelBranchOwner(scene, sourceEntity) != NULL_ENTITY) {
                return {};
            }

            movableEntities.push_back(sourceEntity);
            continue;
        }

        if (sourceEntity == targetNode.id || !scene->isEntityCreated(targetNode.id)) {
            return {};
        }

        if (sourceHasTransform != targetNode.hasTransform) {
            return {};
        }

        if (sourceHasTransform && scene->isParentOf(sourceEntity, targetNode.id)) {
            return {};
        }

        if (!sourceHasTransform && !targetNode.hasTransform
            && (type == InsertionType::BEFORE || type == InsertionType::AFTER)) {
            Entity sourceParent = ProjectUtils::getEffectiveParent(scene, sourceEntity);
            if (sourceParent != targetNode.parent
                && (sourceParent != NULL_ENTITY || targetNode.parent != NULL_ENTITY)) {
                return {};
            }
        }

        if (!ProjectUtils::canMoveLockedEntityOrder(scene, sourceEntity, targetNode.id, type)) {
            return {};
        }

        movableEntities.push_back(sourceEntity);
    }

    return movableEntities;
}

uint32_t editor::HierarchyWindow::getNodeSceneId(const TreeNode& node) const {
    if (node.entitySceneId != 0) {
        return node.entitySceneId;
    }

    return project->getSelectedSceneId();
}

void editor::HierarchyWindow::collectVisibleEntitySelectionOrder(const TreeNode& node, bool hasSearch) {
    if (hasSearch && !node.matchesSearch && !node.hasMatchingDescendant) {
        return;
    }

    if (!node.isScene && !node.isChildScene) {
        visibleEntitySelectionOrder.push_back({getNodeSceneId(node), static_cast<Entity>(node.id)});
    }

    for (const auto& child : node.children) {
        collectVisibleEntitySelectionOrder(child, hasSearch);
    }
}

std::vector<Entity> editor::HierarchyWindow::getEntitySelectionRange(uint32_t sceneId, Entity startEntity, Entity endEntity) const {
    int startIndex = -1;
    int endIndex = -1;

    for (size_t i = 0; i < visibleEntitySelectionOrder.size(); i++) {
        const EntitySelectionEntry& entry = visibleEntitySelectionOrder[i];
        if (entry.sceneId != sceneId) {
            continue;
        }
        if (entry.entity == startEntity && startIndex == -1) {
            startIndex = static_cast<int>(i);
        }
        if (entry.entity == endEntity) {
            endIndex = static_cast<int>(i);
        }
    }

    if (startIndex == -1 || endIndex == -1) {
        return {};
    }

    if (startIndex > endIndex) {
        std::swap(startIndex, endIndex);
    }

    std::vector<Entity> range;
    for (int i = startIndex; i <= endIndex; i++) {
        const EntitySelectionEntry& entry = visibleEntitySelectionOrder[i];
        if (entry.sceneId == sceneId) {
            range.push_back(entry.entity);
        }
    }

    return range;
}

void editor::HierarchyWindow::removeSelectedEntity(uint32_t sceneId, Entity entity) {
    std::vector<Entity> selectedEntities = project->getSelectedEntities(sceneId);
    selectedEntities.erase(
        std::remove(selectedEntities.begin(), selectedEntities.end(), entity),
        selectedEntities.end()
    );
    project->replaceSelectedEntities(sceneId, selectedEntities);
}

void editor::HierarchyWindow::applyEntitySelection(uint32_t sceneId, Entity entity, bool shiftPressed, bool ctrlPressed) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || entity == NULL_ENTITY || !sceneProject->scene->isEntityCreated(entity)) {
        return;
    }

    bool canSelectRange = shiftPressed
        && selectionAnchorSceneId == sceneId
        && selectionAnchorEntity != NULL_ENTITY
        && sceneProject->scene->isEntityCreated(selectionAnchorEntity);

    std::vector<Entity> entitiesToSelect;
    if (canSelectRange) {
        entitiesToSelect = getEntitySelectionRange(sceneId, selectionAnchorEntity, entity);
    }
    if (entitiesToSelect.empty()) {
        entitiesToSelect.push_back(entity);
    }

    if (!ctrlPressed) {
        project->clearAllSelections(project->getSelectedSceneId());
    }

    if (ctrlPressed && !shiftPressed && project->isSelectedEntity(sceneId, entity)) {
        removeSelectedEntity(sceneId, entity);
    } else {
        for (Entity selectedEntity : entitiesToSelect) {
            project->addSelectedEntity(sceneId, selectedEntity);
        }
    }

    selectedScenes.clear();
    project->setSelectedSceneForProperties(sceneId);
    clearSubSelectionForEntity(sceneId, entity);
    // a user selection makes the GameObject the inspected object (animation clips 9.R30)
    project->clearInspectedAsset();

    selectionAnchorSceneId = sceneId;
    selectionAnchorEntity = entity;
}

void editor::HierarchyWindow::clearSubSelectionForEntity(uint32_t sceneId, Entity entity) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->sceneRender) {
        return;
    }

    if (sceneProject->sceneRender->getSelectedInstanceEntity() == entity) {
        sceneProject->sceneRender->clearInstanceSelection();
    }
    if (sceneProject->sceneRender->getSelectedTileEntity() == entity) {
        sceneProject->sceneRender->clearTileSelection();
    }
    if (sceneProject->sceneRender->getSelectedOccluderPointEntity() == entity) {
        sceneProject->sceneRender->clearOccluderPointSelection();
    }
    if (sceneProject->sceneRender->getSelectedLinePointEntity() == entity) {
        sceneProject->sceneRender->clearLinePointSelection();
    }
    if (sceneProject->sceneRender->getSelectedPolygonPointEntity() == entity) {
        sceneProject->sceneRender->clearPolygonPointSelection();
    }
}

void editor::HierarchyWindow::resetEntitySelectionAnchor() {
    selectionAnchorSceneId = NULL_PROJECT_SCENE;
    selectionAnchorEntity = NULL_ENTITY;
}

void editor::HierarchyWindow::moveCameraToEditorView(Entity cameraEntity) {
    SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->scene || !sceneProject->sceneRender) {
        return;
    }

    Scene* scene = sceneProject->scene;
    Camera* editorCamera = sceneProject->sceneRender->getCamera();
    if (!editorCamera) {
        return;
    }

    Transform* targetTransform = scene->findComponent<Transform>(cameraEntity);
    CameraComponent* targetCamera = scene->findComponent<CameraComponent>(cameraEntity);
    if (!targetTransform || !targetCamera) {
        return;
    }

    editorCamera->updateTransform();
    editorCamera->updateCamera();

    Vector3 editorWorldPosition = editorCamera->getWorldPosition();
    Vector3 editorWorldTarget = editorCamera->getWorldTarget();

    Vector3 backDirection = editorWorldPosition - editorWorldTarget;
    if (!backDirection.isValid() || backDirection.squaredLength() <= 1e-8f) {
        backDirection = editorCamera->getWorldDirection();
    }
    if (!backDirection.isValid() || backDirection.squaredLength() <= 1e-8f) {
        return;
    }
    backDirection.normalize();

    Vector3 desiredWorldUp = editorCamera->getWorldUp();
    if (!desiredWorldUp.isValid() || desiredWorldUp.squaredLength() <= 1e-8f) {
        desiredWorldUp = Vector3::UNIT_Y;
    }
    desiredWorldUp.normalize();

    Vector3 rightDirection = desiredWorldUp.crossProduct(backDirection);
    if (!rightDirection.isValid() || rightDirection.squaredLength() <= 1e-8f) {
        rightDirection = editorCamera->getWorldRight();
    }
    if (!rightDirection.isValid() || rightDirection.squaredLength() <= 1e-8f) {
        rightDirection = backDirection.perpendicular();
    }
    if (!rightDirection.isValid() || rightDirection.squaredLength() <= 1e-8f) {
        return;
    }
    rightDirection.normalize();

    desiredWorldUp = backDirection.crossProduct(rightDirection);
    if (!desiredWorldUp.isValid() || desiredWorldUp.squaredLength() <= 1e-8f) {
        return;
    }
    desiredWorldUp.normalize();

    Quaternion desiredWorldRotation(rightDirection, desiredWorldUp, backDirection);
    desiredWorldRotation.normalize();

    Vector3 newPosition = editorWorldPosition;
    Quaternion newRotation = desiredWorldRotation;
    Transform* parentTransform = scene->findComponent<Transform>(targetTransform->parent);
    if (parentTransform) {
        newPosition = parentTransform->modelMatrix.inverse() * editorWorldPosition;
        newRotation = parentTransform->worldRotation.inverse() * desiredWorldRotation;
        newRotation.normalize();
    }

    auto command = new MultiPropertyCmd();
    command->addCommand(std::make_unique<ObjectTransformCmd>(
        project,
        sceneProject->id,
        cameraEntity,
        newPosition,
        newRotation,
        targetTransform->scale
    ));

    if (targetCamera->useTarget) {
        Vector3 newTarget = editorWorldTarget;
        Vector3 newUp = desiredWorldUp;

        if (parentTransform) {
            Matrix4 newLocalMatrix = Matrix4::translateMatrix(newPosition)
                * newRotation.getRotationMatrix()
                * Matrix4::scaleMatrix(targetTransform->scale);
            Matrix4 newWorldMatrix = parentTransform->modelMatrix * newLocalMatrix;
            Matrix4 inverseNewWorldMatrix = newWorldMatrix.inverse();

            newTarget = (inverseNewWorldMatrix * editorWorldTarget) + newPosition;

            Vector3 worldOrigin = newWorldMatrix * Vector3::ZERO;
            Vector3 localOrigin = inverseNewWorldMatrix * worldOrigin;
            Vector3 localUpPoint = inverseNewWorldMatrix * (worldOrigin + desiredWorldUp);
            newUp = localUpPoint - localOrigin;
            if (!newUp.isValid() || newUp.squaredLength() <= 1e-8f) {
                newUp = targetCamera->up;
            } else {
                newUp.normalize();
            }
        }

        command->addPropertyCmd<Vector3>(project, sceneProject->id, cameraEntity, ComponentType::CameraComponent, "target", newTarget);
        command->addPropertyCmd<Vector3>(project, sceneProject->id, cameraEntity, ComponentType::CameraComponent, "up", newUp);
    }

    CommandHandle::get(sceneProject->id)->addCommandNoMerge(command);
}

editor::HierarchyWindow::HierarchyWindow(Project* project, SceneWindow* sceneWindow){
    this->project = project;
    this->sceneWindow = sceneWindow;
    this->windowOpen = true;
    this->focusRequested = false;
    this->openParent = NULL_ENTITY;
}

void editor::HierarchyWindow::setOpen(bool open){
    if (open){
        if (!windowOpen){
            focusRequested = true;
        }
        windowOpen = true;
        return;
    }

    windowOpen = false;
    focusRequested = false;
}

bool editor::HierarchyWindow::isOpen() const{
    return windowOpen;
}

const std::vector<editor::CreateMenuEntry>& editor::HierarchyWindow::getCreateMenuEntries(){
    using T = EntityCreationType;
    // submenu, label, icon, type, new GameObject name, scenes, separatorBefore, rootOnly, command id
    static const std::vector<CreateMenuEntry> entries = {
        {nullptr, "Create Empty", ICON_FA_CUBE, T::GAMEOBJECT, "GameObject", CREATE_IN_ALL, false, false, "gameobject.createEmpty"},
        {nullptr, "Create Empty (No Transform)", ICON_FA_CIRCLE_DOT, T::EMPTY, "GameObject", CREATE_IN_ALL, false, true},

        {"3D Object", "Cube", ICON_FA_CUBE, T::BOX, "Cube", CREATE_IN_3D},
        {"3D Object", "Sphere", ICON_FA_CIRCLE, T::SPHERE, "Sphere", CREATE_IN_3D},
        {"3D Object", "Capsule", ICON_FA_CAPSULES, T::CAPSULE, "Capsule", CREATE_IN_3D},
        {"3D Object", "Cylinder", ICON_FA_DATABASE, T::CYLINDER, "Cylinder", CREATE_IN_3D},
        {"3D Object", "Plane", ICON_FA_SQUARE, T::PLANE, "Plane", CREATE_IN_3D},
        {"3D Object", "Quad", ICON_FA_SQUARE_FULL, T::WALL, "Quad", CREATE_IN_3D},
        {"3D Object", "Torus", ICON_FA_RING, T::TORUS, "Torus", CREATE_IN_3D},
        {"3D Object", "Model", ICON_FA_PERSON_RUNNING, T::MODEL, "Model", CREATE_IN_3D, true},
        {"3D Object", "Terrain", ICON_FA_MOUNTAIN, T::TERRAIN, "Terrain", CREATE_IN_3D},
        // the dialog of T6 R59: size, base and a memory estimate first
        {"3D Object", "Voxel Volume", ICON_FA_CUBES, T::VOXEL_VOLUME, "Voxel Volume", CREATE_IN_3D},
        {"3D Object", "Terrain World", ICON_FA_MAP, T::TERRAIN_WORLD, "Terrain World", CREATE_IN_3D},
        {"3D Object", "Mirror", ICON_FA_CLONE, T::MIRROR, "Mirror", CREATE_IN_3D},
        {"3D Object", "Water", ICON_FA_WATER, T::WATER, "Water", CREATE_IN_3D},
        // Unity's Ragdoll Wizard on the selected character (or the row's GameObject)
        {"3D Object", "Ragdoll...", ICON_FA_PERSON_FALLING, T::RAGDOLL_WIZARD, "Ragdoll", CREATE_IN_3D, true},
        // a car on the selected mesh (or a new one): a dynamic body and four Wheel Colliders (S96)
        {"3D Object", "Vehicle...", ICON_FA_CAR, T::VEHICLE_RIG, "Vehicle", CREATE_IN_3D},
        // around the selection (or its row's GameObject) when there is one, see createGameObject
        {"3D Object", "Lattice", ICON_FA_BORDER_NONE, T::LATTICE, "Lattice", CREATE_IN_3D, true},

        {"2D Object", "Sprite", ICON_FA_IMAGE, T::SPRITE, "Sprite", CREATE_IN_WORLD},
        {"2D Object", "Sprite Batch", ICON_FA_TABLE_CELLS_LARGE, T::SPRITE_BATCH, "Sprite Batch", CREATE_IN_WORLD},
        // S50 R63: a Grid with a child Tilemap Layer, per layout (createTilemapGrid, by its label)
        {"2D Object/Tilemap", "Rectangular", ICON_FA_BORDER_ALL, T::TILEMAP_GRID, "Grid", CREATE_IN_WORLD},
        {"2D Object/Tilemap", "Isometric", ICON_FA_BORDER_ALL, T::TILEMAP_GRID, "Grid", CREATE_IN_WORLD},
        {"2D Object/Tilemap", "Isometric Staggered", ICON_FA_BORDER_ALL, T::TILEMAP_GRID, "Grid", CREATE_IN_WORLD},
        {"2D Object/Tilemap", "Hexagonal (Pointy Top)", ICON_FA_BORDER_ALL, T::TILEMAP_GRID, "Grid", CREATE_IN_WORLD},
        {"2D Object/Tilemap", "Hexagonal (Flat Top)", ICON_FA_BORDER_ALL, T::TILEMAP_GRID, "Grid", CREATE_IN_WORLD},
        {"2D Object", "Mesh Polygon", ICON_FA_VECTOR_SQUARE, T::MESH_POLYGON, "Mesh Polygon", CREATE_IN_WORLD},
        {"2D Object", "Shadow Caster 2D", ICON_FA_CIRCLE_HALF_STROKE, T::SHADOW_CASTER_2D, "Shadow Caster 2D", CREATE_IN_2D, true},
        {"2D Object", "Lattice", ICON_FA_BORDER_NONE, T::LATTICE, "Lattice", CREATE_IN_2D, true},
        // a Sprite showing a .titleart (S81 R6)
        {"2D Object", "Title Art", ICON_FA_WAND_MAGIC_SPARKLES, T::TITLE_ART_SPRITE, "Title Art", CREATE_IN_WORLD, true},

        {"Effects", "Particle System", ICON_FA_FIRE, T::PARTICLE_SYSTEM, "Particle System", CREATE_IN_WORLD},
        {"Effects", "Force Field", ICON_FA_WIND, T::FORCE_FIELD, "Force Field", CREATE_IN_3D},
        {"Effects", "Line Renderer", ICON_FA_GRIP_LINES, T::LINE_RENDERER, "Line", CREATE_IN_WORLD},
        // on the selected GameObject when it has none, else as a child (createGameObject)
        {"Effects", "Trail Renderer", ICON_FA_WIND, T::TRAIL_RENDERER, "Trail", CREATE_IN_WORLD},
        {"Effects", "Point Renderer", ICON_FA_CIRCLE, T::POINTS, "Points", CREATE_IN_WORLD},
        {"Effects", "Fluid Simulation", ICON_FA_DROPLET, T::FLUID_SIMULATION, "Fluid Simulation", CREATE_IN_WORLD},
        {"Effects", "Fog", ICON_FA_SMOG, T::FOG, "Fog", CREATE_IN_3D, true},
        {"Effects", "Local Volumetric Fog", ICON_FA_SMOG, T::LOCAL_VOLUMETRIC_FOG, "Local Volumetric Fog", CREATE_IN_3D},
        {"Effects", "Volumetric Clouds", ICON_FA_CLOUD, T::VOLUMETRIC_CLOUDS, "Volumetric Clouds", CREATE_IN_3D},
        // S49's pixel presets (editor/pixelart/particles): a nested submenu, the entity name the preset's label
        {"Effects/Pixel", "Spark", ICON_FA_FIRE, T::PIXEL_PARTICLE_SYSTEM, "Pixel Spark", CREATE_IN_WORLD},
        {"Effects/Pixel", "Smoke", ICON_FA_FIRE, T::PIXEL_PARTICLE_SYSTEM, "Pixel Smoke", CREATE_IN_WORLD},
        {"Effects/Pixel", "Fire", ICON_FA_FIRE, T::PIXEL_PARTICLE_SYSTEM, "Pixel Fire", CREATE_IN_WORLD},
        {"Effects/Pixel", "Ember", ICON_FA_FIRE, T::PIXEL_PARTICLE_SYSTEM, "Pixel Ember", CREATE_IN_WORLD},
        {"Effects/Pixel", "Grit", ICON_FA_FIRE, T::PIXEL_PARTICLE_SYSTEM, "Pixel Grit", CREATE_IN_WORLD},
        {"Effects/Pixel", "Debris", ICON_FA_FIRE, T::PIXEL_PARTICLE_SYSTEM, "Pixel Debris", CREATE_IN_WORLD},
        {"Effects/Pixel", "Light Flash", ICON_FA_FIRE, T::PIXEL_PARTICLE_SYSTEM, "Pixel Light Flash", CREATE_IN_WORLD},
        {"Effects/Pixel", "Smoke Loop", ICON_FA_FIRE, T::PIXEL_PARTICLE_SYSTEM, "Pixel Smoke Loop", CREATE_IN_WORLD, true},
        {"Effects/Pixel", "Ember Loop", ICON_FA_FIRE, T::PIXEL_PARTICLE_SYSTEM, "Pixel Ember Loop", CREATE_IN_WORLD},

        {"Light", "Directional Light", ICON_FA_SUN, T::DIRECTIONAL_LIGHT, "Directional Light", CREATE_IN_3D},
        {"Light", "Point Light", ICON_FA_LIGHTBULB, T::POINT_LIGHT, "Point Light", CREATE_IN_3D},
        {"Light", "Spot Light", ICON_FA_LIGHTBULB, T::SPOT_LIGHT, "Spot Light", CREATE_IN_3D},
        {"Light", "Light 2D", ICON_FA_LIGHTBULB, T::POINT_LIGHT_2D, "Light 2D", CREATE_IN_2D},
        {"Light", "Reflection Probe", ICON_FA_BULLSEYE, T::REFLECTION_PROBE, "Reflection Probe", CREATE_IN_3D, true},

        {"Audio", "Audio Source", ICON_FA_FILE_AUDIO, T::AUDIO_SOURCE, "Audio Source", CREATE_IN_ALL},
        {"Audio", "Audio Source 3D", ICON_FA_VOLUME_HIGH, T::AUDIO_SOURCE_3D, "Audio Source 3D", CREATE_IN_WORLD},

        {"Video", "Video Player", ICON_FA_FILM, T::VIDEO_PLAYER, "Video Player", CREATE_IN_ALL},
        {"Video", "Video Player (Camera Background)", ICON_FA_VIDEO, T::VIDEO_BACKGROUND, "Video Background", CREATE_IN_ALL},

        {"UI", "Image", ICON_FA_IMAGE, T::IMAGE, "Image", CREATE_IN_ALL},
        {"UI", "Text", ICON_FA_FONT, T::TEXT, "Text", CREATE_IN_ALL},
        {"UI", "Text (Pixel)", ICON_FA_FONT, T::TEXT_PIXEL, "Text", CREATE_IN_ALL},
        {"UI", "Button", ICON_FA_SQUARE_CARET_RIGHT, T::BUTTON, "Button", CREATE_IN_ALL},
        {"UI", "Scrollbar", ICON_FA_ARROWS_UP_DOWN, T::SCROLLBAR, "Scrollbar", CREATE_IN_ALL},
        {"UI", "Progress Bar", ICON_FA_BARS_PROGRESS, T::PROGRESSBAR, "Progress Bar", CREATE_IN_ALL},
        {"UI", "Input Field", ICON_FA_I_CURSOR, T::INPUT_FIELD, "Input Field", CREATE_IN_ALL},
        {"UI", "Panel", ICON_FA_WINDOW_MAXIMIZE, T::PANEL, "Panel", CREATE_IN_ALL},
        {"UI", "Polygon", ICON_FA_DRAW_POLYGON, T::POLYGON, "Polygon", CREATE_IN_ALL},
        // an Image showing a .titleart (S81 R6)
        {"UI", "Title Art", ICON_FA_WAND_MAGIC_SPARKLES, T::TITLE_ART_UI, "Title Art", CREATE_IN_ALL},
        {"UI", "On-Screen Stick (Fixed)", ICON_FA_GAMEPAD, T::ONSCREEN_STICK_FIXED, "On-Screen Stick", CREATE_IN_ALL, true},
        {"UI", "On-Screen Stick (Floating)", ICON_FA_GAMEPAD, T::ONSCREEN_STICK_FLOATING, "On-Screen Stick", CREATE_IN_ALL},
        {"UI", "On-Screen Stick (Dynamic)", ICON_FA_GAMEPAD, T::ONSCREEN_STICK_DYNAMIC, "On-Screen Stick", CREATE_IN_ALL},
        {"UI", "On-Screen Button", ICON_FA_CIRCLE_DOT, T::ONSCREEN_BUTTON, "On-Screen Button", CREATE_IN_ALL},
        {"UI", "Touch Area", ICON_FA_HAND_POINTER, T::TOUCH_AREA, "Touch Area", CREATE_IN_ALL},
        {"UI", "Layout Group", ICON_FA_OBJECT_GROUP, T::LAYOUT_GROUP, "Layout Group", CREATE_IN_ALL, true},
        {"UI", "Canvas Group", ICON_FA_LAYER_GROUP, T::CANVAS_GROUP, "Canvas Group", CREATE_IN_ALL},
        {"UI", "Particle System", ICON_FA_FIRE, T::UI_PARTICLE_SYSTEM, "UI Particles", CREATE_IN_ALL},
        {"UI", "Safe Area", ICON_FA_MOBILE_SCREEN_BUTTON, T::SAFE_AREA, "Safe Area", CREATE_IN_ALL},
        {"UI", "UI Document", ICON_FA_FILE_CODE, T::UI_DOCUMENT, "UI Document", CREATE_IN_ALL, true},

        {"Physics", "Rigidbody", ICON_FA_WEIGHT_HANGING, T::RIGIDBODY, "Rigidbody", CREATE_IN_3D},
        {"Physics", "Joint", ICON_FA_LINK, T::JOINT3D, "Joint", CREATE_IN_3D},
        {"Physics", "Rigidbody 2D", ICON_FA_WEIGHT_HANGING, T::RIGIDBODY2D, "Rigidbody 2D", CREATE_IN_2D, true},
        {"Physics", "Joint 2D", ICON_FA_LINK, T::JOINT2D, "Joint 2D", CREATE_IN_2D},

        {"Rendering", "Skybox", ICON_FA_CLOUD, T::SKYBOX, "Skybox", CREATE_IN_3D},

        {"Tween", "Position Tween", ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, T::POSITION_TWEEN, "Position Tween", CREATE_IN_ALL},
        {"Tween", "Rotation Tween", ICON_FA_ROTATE, T::ROTATION_TWEEN, "Rotation Tween", CREATE_IN_ALL},
        {"Tween", "Scale Tween", ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, T::SCALE_TWEEN, "Scale Tween", CREATE_IN_ALL},
        {"Tween", "Color Tween", ICON_FA_PALETTE, T::COLOR_TWEEN, "Color Tween", CREATE_IN_ALL},
        {"Tween", "Alpha Tween", ICON_FA_EYE, T::ALPHA_TWEEN, "Alpha Tween", CREATE_IN_ALL},

        {"Animation", "Animation", ICON_FA_FILM, T::ANIMATION, "Animation", CREATE_IN_ALL},
        {"Animation", "Sprite Animation", ICON_FA_PLAY, T::SPRITE_ANIMATION, "Sprite Animation", CREATE_IN_ALL},
        {"Animation", "Position Track", ICON_FA_ROUTE, T::TRANSLATE_TRACKS, "Position Track", CREATE_IN_ALL, true},
        {"Animation", "Rotation Track", ICON_FA_ARROWS_SPIN, T::ROTATE_TRACKS, "Rotation Track", CREATE_IN_ALL},
        {"Animation", "Scale Track", ICON_FA_MAXIMIZE, T::SCALE_TRACKS, "Scale Track", CREATE_IN_ALL},
        {"Animation", "Morph Track", ICON_FA_SHAPES, T::MORPH_TRACKS, "Morph Track", CREATE_IN_ALL},
        {"Animation", "Lattice Track", ICON_FA_CUBES_STACKED, T::LATTICE_TRACKS, "Lattice Track", CREATE_IN_ALL},

        {"Spline", "Empty", ICON_FA_BEZIER_CURVE, T::SPLINE_EMPTY, "Spline", CREATE_IN_WORLD},
        {"Spline", "Draw Spline Tool", ICON_FA_PEN, T::SPLINE_DRAW, "Spline", CREATE_IN_WORLD},
        {"Spline", "Road", ICON_FA_ROAD, T::SPLINE_ROAD, "Road", CREATE_IN_3D, true},
        {"Spline", "Path", ICON_FA_SHOE_PRINTS, T::SPLINE_PATH, "Path", CREATE_IN_3D},
        {"Spline", "Wall", ICON_FA_DUNGEON, T::SPLINE_WALL, "Wall", CREATE_IN_3D},
        {"Spline", "Fence", ICON_FA_GRIP_LINES_VERTICAL, T::SPLINE_FENCE, "Fence", CREATE_IN_3D},
        {"Spline", "River", ICON_FA_WATER, T::SPLINE_RIVER, "River", CREATE_IN_3D},
        {"Spline", "Lake", ICON_FA_DROPLET, T::SPLINE_LAKE, "Lake", CREATE_IN_3D},
        {"Spline", "Circle", ICON_FA_CIRCLE_NOTCH, T::SPLINE_CIRCLE, "Circle", CREATE_IN_3D, true},
        {"Spline", "Square", ICON_FA_SQUARE, T::SPLINE_SQUARE, "Square", CREATE_IN_3D},
        {"Spline", "Edge", ICON_FA_WAVE_SQUARE, T::SPLINE_EDGE_2D, "Edge", CREATE_IN_2D, true},
        {"Spline", "Filled Shape", ICON_FA_DRAW_POLYGON, T::SPLINE_FILL_2D, "Filled Shape", CREATE_IN_2D},
        {"Spline", "Circle", ICON_FA_CIRCLE_NOTCH, T::SPLINE_CIRCLE_2D, "Circle", CREATE_IN_2D},

        {nullptr, "Camera", ICON_FA_VIDEO, T::CAMERA, "Camera", CREATE_IN_ALL},
    };
    return entries;
}

bool editor::HierarchyWindow::isCreateEntryAvailable(const CreateMenuEntry& entry, SceneType sceneType){
    switch (sceneType) {
        case SceneType::SCENE_3D: return (entry.scenes & CREATE_IN_3D) != 0;
        case SceneType::SCENE_2D: return (entry.scenes & CREATE_IN_2D) != 0;
        case SceneType::SCENE_UI: return (entry.scenes & CREATE_IN_UI) != 0;
    }
    return true;
}

void editor::HierarchyWindow::createGameObject(const CreateMenuEntry& entry, Entity parent, bool addToPrefab){
    SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->scene) {
        return;
    }
    if (entry.type == EntityCreationType::VOXEL_VOLUME) {
        VoxelCreateDialog::instance().open(project, sceneProject->id, parent);
        return;
    }
    if (entry.type == EntityCreationType::RAGDOLL_WIZARD) {
        std::vector<Entity> selected = project->getSelectedEntities(sceneProject->id);
        const Entity character = parent != NULL_ENTITY ? parent : (selected.size() == 1 ? selected[0] : NULL_ENTITY);
        if (character == NULL_ENTITY) {
            Out::warning("Ragdoll: select the character's root GameObject first (the one holding its model)");
            return;
        }
        RagdollWizardDialog::instance().open(project, sceneProject->id, character);
        return;
    }
    if (entry.type == EntityCreationType::VEHICLE_RIG) {
        // the selected mesh (or the row's GameObject) becomes the vehicle, else a new one is made
        std::vector<Entity> selected = project->getSelectedEntities(sceneProject->id);
        const Entity target = parent != NULL_ENTITY ? parent : (selected.size() == 1 ? selected[0] : NULL_ENTITY);
        std::string error;
        if (createVehicleRig(project, sceneProject->id, target, NULL_ENTITY, error) == NULL_ENTITY) {
            Out::warning("Vehicle: %s", error.c_str());
        }
        return;
    }
    if (entry.type == EntityCreationType::TERRAIN_WORLD) {
        // T4 R40: its folder asset, the GameObject and its Rigidbody, one undo step
        CreateTerrainWorldDialog::instance().open(project, sceneProject->id, entry.rootOnly ? NULL_ENTITY : parent);
        return;
    }
    if (entry.type == EntityCreationType::TERRAIN) {
        // T1 R58: sizes, a height map and a collider first, one undo step
        CreateTerrainDialog::instance().open(project, sceneProject->id, entry.rootOnly ? NULL_ENTITY : parent);
        return;
    }
    if (entry.type == EntityCreationType::LATTICE) {
        // a lattice wraps GameObjects instead of being made under one: the row it was asked
        // from (unless that row is in the selection), else the selection
        std::vector<Entity> targets = project->getSelectedEntities(sceneProject->id);
        if (parent != NULL_ENTITY && std::find(targets.begin(), targets.end(), parent) == targets.end()) {
            targets = {parent};
        }
        createLatticeAround(targets);
        return;
    }
    if (entry.rootOnly) {
        parent = NULL_ENTITY;
    }
    // a Joint 2D goes under the selected Rigidbody 2D, which it is then on (Unity adds the
    // joint to the body)
    if (entry.type == EntityCreationType::JOINT2D && parent == NULL_ENTITY) {
        std::vector<Entity> selected = project->getSelectedEntities(sceneProject->id);
        if (selected.size() == 1 && sceneProject->scene->findComponent<Rigidbody2DComponent>(selected[0])) {
            parent = selected[0];
        }
    }
    // Unity puts a trail on the moving object itself: the selected GameObject gets one when it
    // has none and is not locked (one undo step, its own Trail Renderer); else a child trail
    if (entry.type == EntityCreationType::TRAIL_RENDERER && parent != NULL_ENTITY &&
            sceneProject->scene->isEntityCreated(parent) && sceneProject->scene->findComponent<Transform>(parent) &&
            !ProjectUtils::isEntityLocked(sceneProject->scene, parent) &&
            !sceneProject->scene->findComponent<TrailRendererComponent>(parent)) {
        CommandHandle::get(sceneProject->id)->addCommandNoMerge(
            new AddComponentCmd(project, sceneProject->id, parent, ComponentType::TrailRendererComponent));
        project->setSelectedEntity(sceneProject->id, parent);
        return;
    }
    if (entry.type == EntityCreationType::PIXEL_PARTICLE_SYSTEM) {
        createPixelParticles(entry, parent, addToPrefab);
        return;
    }
    if (entry.type == EntityCreationType::TILEMAP_GRID) {
        createTilemapGrid(entry, parent, addToPrefab);
        return;
    }
    CreateEntityCmd* createCmd = new CreateEntityCmd(project, sceneProject->id, entry.entityName, entry.type, parent, addToPrefab);
    CommandHandle::get(sceneProject->id)->addCommandNoMerge(createCmd);
    openParent = parent;
    if (entry.type == EntityCreationType::SPLINE_DRAW) {
        // the new spline, drawn in the Scene view (Enter finishes, Esc cancels)
        SplineEditor::get().beginDraw(sceneProject->id, createCmd->getEntity());
    }
    // The new GameObject is selected: scroll it into view, as Unity does
    std::vector<Entity> selected = project->getSelectedEntities(sceneProject->id);
    if (selected.size() == 1) {
        revealEntity(sceneProject->id, selected[0]);
    }
}

// GameObject › Effects › Pixel › <preset> (S49 R20): a Particle System playing the preset inline at
// the Scene view's pivot, snapped to the pixel grid; the palette copy and the child effects it
// needs are written as one project step linked to the creation (one undo)
void editor::HierarchyWindow::createPixelParticles(const CreateMenuEntry& entry, Entity parent, bool addToPrefab){
    SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->scene) {
        return;
    }
    std::string preset;
    for (const std::string& name : pixelart::pixelParticlePresetNames()) {
        if (pixelart::pixelParticlePresetLabel(name) == entry.label) preset = name;
    }
    pixelart::ProjectPixelPreset made;
    std::string error;
    if (preset.empty() || !pixelart::prepareProjectPixelPreset(*project, preset, "Particles/Pixel", made, error)) {
        Out::error("Could not make the pixel preset %s: %s", entry.label, error.c_str());
        return;
    }
    const bool wrote = !made.writes.empty();
    if (wrote && !ProjectFileWrites::writeUndoable(project, made.writes, error, nullptr, nullptr, {}, true)) {
        Out::error("Could not write the pixel preset's files: %s", error.c_str());
        return;
    }
    Vector3 pivot = Vector3::ZERO;
    if (parent == NULL_ENTITY && sceneProject->sceneRender) {
        if (Camera* camera = sceneProject->sceneRender->getCamera()) {
            camera->updateTransform();
            pivot = camera->getWorldTarget();
            if (!pivot.isValid()) pivot = Vector3::ZERO;
        }
        pivot = jasperengine::pixelart::snapWorld(pivot, std::max(project->getPixelsPerUnit(), 1));
    }
    CreateEntityCmd* createCmd = new CreateEntityCmd(project, sceneProject->id, entry.entityName, EntityCreationType::PARTICLE_SYSTEM, parent, addToPrefab);
    createCmd->addProperty<particles::ParticleEffectDocument>(ComponentType::ParticleSystemComponent, "inlineEffect", made.document);
    createCmd->addProperty<Vector3>(ComponentType::Transform, "position", pivot);
    CommandHandle::get(sceneProject->id)->addCommandNoMerge(createCmd);
    if (wrote) pixelart::linkPaletteCopy(*project, sceneProject->id);
    openParent = parent;
    std::vector<Entity> selected = project->getSelectedEntities(sceneProject->id);
    if (selected.size() == 1) {
        revealEntity(sceneProject->id, selected[0]);
    }
}

void editor::HierarchyWindow::createLatticeAround(const std::vector<Entity>& targets){
    SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->scene) {
        return;
    }
    // only authored GameObjects with a Transform can be bound and measured
    std::vector<Entity> bound;
    for (Entity entity : targets) {
        if (cachedPlayCreatedEntities.count(entity) == 0 && sceneProject->scene->isEntityCreated(entity) &&
            sceneProject->scene->findComponent<Transform>(entity)) {
            bound.push_back(entity);
        }
    }
    Vector3 focus = Vector3::ZERO;
    if (sceneProject->sceneRender) {
        if (Camera* camera = sceneProject->sceneRender->getCamera()) {
            camera->updateTransform();
            focus = camera->getWorldTarget();
            if (!focus.isValid()) {
                focus = Vector3::ZERO;
            }
        }
    }
    std::string error;
    Entity lattice = LatticeEditorOps::createLattice(project, sceneProject->id, bound, focus, "Lattice", &error);
    if (lattice == NULL_ENTITY) {
        Out::error("Could not create the lattice: %s", error.c_str());
        return;
    }
    revealEntity(sceneProject->id, lattice);
}

Entity editor::HierarchyWindow::getSelectedParentForCreate() const{
    SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->scene) {
        return NULL_ENTITY;
    }
    std::vector<Entity> selected = project->getSelectedEntities(sceneProject->id);
    if (selected.size() != 1 || cachedPlayCreatedEntities.count(selected[0]) > 0) {
        return NULL_ENTITY;
    }
    if (!sceneProject->scene->findComponent<Transform>(selected[0])) {
        return NULL_ENTITY;
    }
    return selected[0];
}

static const char* createSubmenuIcon(const char* submenu){
    const std::string name = submenu;
    if (name == "3D Object") return ICON_FA_CUBE;
    if (name == "2D Object") return ICON_FA_IMAGE;
    if (name == "Effects") return ICON_FA_FIRE;
    if (name == "Light") return ICON_FA_LIGHTBULB;
    if (name == "Audio") return ICON_FA_VOLUME_HIGH;
    if (name == "Video") return ICON_FA_FILM;
    if (name == "UI") return ICON_FA_WINDOW_RESTORE;
    if (name == "Physics") return ICON_FA_WEIGHT_HANGING;
    if (name == "Rendering") return ICON_FA_CLOUD;
    if (name == "Tween") return ICON_FA_BOLT;
    if (name == "Animation") return ICON_FA_FILM;
    return ICON_FA_FOLDER;
}

// Unity-order create items: top-level items, submenus, then the trailing items
// (Camera), with a separator wherever the list switches between the two.
void editor::HierarchyWindow::showCreateMenuItems(bool atRoot, Entity parent, bool addToPrefab){
    SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject) {
        return;
    }
    const SceneType sceneType = sceneProject->sceneType;
    const auto& entries = getCreateMenuEntries();

    auto offered = [&](const CreateMenuEntry& entry) {
        return isCreateEntryAvailable(entry, sceneType) && (atRoot || !entry.rootOnly);
    };
    auto itemLabel = [](const CreateMenuEntry& entry) {
        return std::string(entry.icon) + "  " + entry.label;
    };

    std::vector<std::string> shownSubmenus;
    int lastKind = -1; // 0 = item, 1 = submenu
    for (const CreateMenuEntry& entry : entries) {
        if (!offered(entry)) {
            continue;
        }
        if (!entry.submenu) {
            if (lastKind == 1) ImGui::Separator();
            lastKind = 0;
            if (ImGui::MenuItem(itemLabel(entry).c_str())) {
                createGameObject(entry, parent, addToPrefab);
            }
            continue;
        }

        // "Effects/Pixel": a submenu inside "Effects"
        const std::string full = entry.submenu;
        const std::string submenu = full.substr(0, full.find('/'));
        if (std::find(shownSubmenus.begin(), shownSubmenus.end(), submenu) != shownSubmenus.end()) {
            continue;
        }
        shownSubmenus.push_back(submenu);
        if (lastKind == 0) ImGui::Separator();
        lastKind = 1;

        const std::string submenuLabel = std::string(createSubmenuIcon(submenu.c_str())) + "  " + submenu;
        if (ImGui::BeginMenu(submenuLabel.c_str())) {
            bool first = true;
            std::vector<std::string> nested;
            for (const CreateMenuEntry& child : entries) {
                if (!child.submenu || !offered(child)) {
                    continue;
                }
                const std::string childMenu = child.submenu;
                if (childMenu.rfind(submenu + "/", 0) == 0) {
                    const std::string inner = childMenu.substr(submenu.size() + 1);
                    if (std::find(nested.begin(), nested.end(), inner) != nested.end()) continue;
                    nested.push_back(inner);
                    if (ImGui::BeginMenu(inner.c_str())) {
                        bool firstInner = true;
                        for (const CreateMenuEntry& item : entries) {
                            if (!item.submenu || childMenu != item.submenu || !offered(item)) continue;
                            if (item.separatorBefore && !firstInner) ImGui::Separator();
                            firstInner = false;
                            if (ImGui::MenuItem(itemLabel(item).c_str())) createGameObject(item, parent, addToPrefab);
                        }
                        ImGui::EndMenu();
                    }
                    first = false;
                    continue;
                }
                if (submenu != childMenu) {
                    continue;
                }
                if (child.separatorBefore && !first) ImGui::Separator();
                first = false;
                if (ImGui::MenuItem(itemLabel(child).c_str())) {
                    createGameObject(child, parent, addToPrefab);
                }
            }
            ImGui::EndMenu();
        }
    }
}

void editor::HierarchyWindow::showToolbar(){
    // Unity's "+ v" create dropdown, then the search field filling the rest
    if (ImGui::Button(ICON_FA_PLUS " " ICON_FA_CARET_DOWN)) {
        ImGui::OpenPopup("HierarchyCreateMenu");
    }
    ImGui::SetItemTooltip("Create a GameObject");

    ImGui::SameLine();
    showVcsToolbarToggle();

    UIUtils::searchInput("##structure_search", "Search  (t:Camera)", searchBuffer, sizeof(searchBuffer), false, &matchCase);

    if (ImGui::BeginPopup("HierarchyCreateMenu")) {
        showCreateMenuItems(true, NULL_ENTITY, false);
        ImGui::EndPopup();
    }
}

void editor::HierarchyWindow::showSceneMenu(SceneProject* sceneProject){
    if (!sceneProject) {
        return;
    }
    if (project->isPrefabStage(sceneProject->id)) {
        showPrefabStageMenu(sceneProject);
        return;
    }
    const uint32_t sceneId = sceneProject->id;
    const bool busy = project->isAnyScenePlaying();

    if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save Scene", nullptr, false, !busy)) {
        project->saveScene(sceneId);
    }
    if (ImGui::MenuItem(ICON_FA_I_CURSOR "  Rename")) {
        beginRename(RenameTarget::Scene, sceneId, NULL_ENTITY, sceneProject->name);
    }

    ImGui::Separator();

    if (ImGui::BeginMenu(ICON_FA_CUBE "  GameObject")) {
        showCreateMenuItems(true, NULL_ENTITY, false);
        ImGui::EndMenu();
    }
    showAddChildSceneMenu();

    ImGui::Separator();

    if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS "  Select Scene Asset", nullptr, false, !sceneProject->filepath.empty())) {
        revealInProject(sceneProject->filepath);
    }
    showVcsMenu(sceneProject);
    const bool canRemove = !busy && project->getScenes().size() > 1;
    if (ImGui::MenuItem(ICON_FA_TRASH "  Remove Scene from Project", nullptr, false, canRemove)) {
        // Deferred: the scene is still being drawn this frame
        Project* proj = project;
        Backend::getApp().enqueueMainThreadTask([proj, sceneId]() {
            if (!proj->getScene(sceneId) || proj->isAnyScenePlaying() || proj->getScenes().size() <= 1) return;
            proj->checkUnsavedAndExecute(sceneId, [proj, sceneId]() {
                proj->removeScene(sceneId);
            });
        });
    }
}

void editor::HierarchyWindow::showPrefabMenu(const TreeNode& node){
    const uint32_t sceneId = project->getSelectedSceneId();

    // Missing Prefab roots have a menu of their own (HierarchyPrefabRows.cpp)
    if (node.isMissingPrefab) {
        showMissingPrefabMenu(node);
        return;
    }

    if (!node.isPrefab && !node.isParentPrefab) {
        if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Create Prefab", nullptr, false, !node.isLocked && node.hasTransform)) {
            saveNodeAsPrefab(node);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            std::filesystem::path directory = std::filesystem::relative(getPrefabSaveDirectory(), project->getProjectPath());
            ImGui::SetItemTooltip("Saves to %s", directory == "." ? "the project root" : directory.string().c_str());
        }

        const auto& scenePrefabs = project->getPrefabs(sceneId);
        if (ImGui::BeginMenu(ICON_FA_BOXES_STACKED "  Add to Prefab", !scenePrefabs.empty())) {
            for (const auto& [path, prefabPtr] : scenePrefabs) {
                auto sceneIt = prefabPtr->instances.find(sceneId);
                if (sceneIt == prefabPtr->instances.end() || sceneIt->second.empty()) continue;
                std::string label = path.stem().string();
                if (ImGui::MenuItem(label.c_str())) {
                    Entity rootEntity = sceneIt->second.front().rootEntity;
                    CommandHandle::get(sceneId)->addCommandNoMerge(new AddEntityToPrefabCmd(project, sceneId, node.id, rootEntity));
                }
            }
            ImGui::EndMenu();
        }
    }

    if (node.isParentPrefab && !node.isPrefabRoot) {
        if (ImGui::MenuItem(ICON_FA_LOCK_OPEN "  Remove from Prefab", nullptr, false, node.isPrefab)) {
            CommandHandle::get(sceneId)->addCommandNoMerge(new RemoveEntityFromPrefabCmd(project, sceneId, node.id, node.parent));
        }
        if (ImGui::MenuItem(ICON_FA_BOXES_STACKED "  Add to Prefab", nullptr, false, !node.isPrefab)) {
            CommandHandle::get(sceneId)->addCommandNoMerge(new AddEntityToPrefabCmd(project, sceneId, node.id, node.parent));
        }
    }

    if (node.isPrefabRoot && node.hasPrefabParent) {
        bool isNestedMember = node.isPrefab && (node.prefabFilepath != node.nestedPrefabFilepath);
        if (ImGui::MenuItem(ICON_FA_LOCK_OPEN "  Remove from Parent Prefab", nullptr, false, isNestedMember)) {
            CommandHandle::get(sceneId)->addCommandNoMerge(new RemoveEntityFromPrefabCmd(project, sceneId, node.id, node.parent));
        }
        if (ImGui::MenuItem(ICON_FA_BOXES_STACKED "  Add to Parent Prefab", nullptr, false, !isNestedMember)) {
            CommandHandle::get(sceneId)->addCommandNoMerge(new AddEntityToPrefabCmd(project, sceneId, node.id, node.parent));
        }
    }

    // Unity acts on the whole selection when the clicked row is part of it; a GameObject
    // inside an instance stands for the outermost instance holding it
    std::vector<Entity> targets = project->getSelectedEntities(sceneId);
    if (targets.empty() || !project->isSelectedEntity(sceneId, node.id)) {
        targets = {node.id};
    }
    const bool canUnpack = UnpackPrefabCmd::canUnpack(project, sceneId, targets);

    if (node.isPrefab || node.isPrefabRoot || canUnpack) {
        const bool itemsAbove = (!node.isPrefab && !node.isParentPrefab) ||
            (node.isParentPrefab && !node.isPrefabRoot) || (node.isPrefabRoot && node.hasPrefabParent);
        if (itemsAbove) {
            ImGui::Separator();
        }
        showOpenPrefabItems(node);
        std::filesystem::path assetPath = node.isPrefabRoot ? node.nestedPrefabFilepath : node.prefabFilepath;
        if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS "  Select Asset", nullptr, false, !assetPath.empty())) {
            revealInProject(assetPath);
        }
        // ---- Prefab Variants (PREFABS.md 19) ----
        {
            const std::filesystem::path basePath = assetPath.empty() ? std::filesystem::path() : project->getPrefabFileBase(assetPath);
            if (!basePath.empty()) {
                if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS "  Select Base", nullptr, false, true)) {
                    revealInProject(basePath);
                }
                ImGui::SetItemTooltip("This Prefab is a Variant of '%s': selects it in the Project window", basePath.stem().string().c_str());
            }
            std::string variantReason;
            const bool canVariant = node.isPrefabRoot && project->canCreatePrefabVariant(sceneId, node.id, &variantReason);
            if (ImGui::MenuItem(ICON_FA_CLONE "  Create Variant", nullptr, false, canVariant)) {
                requestCreateVariant(sceneId, node.id);
            }
            ImGui::SetItemTooltip("%s", canVariant ? "Makes a Prefab Variant of this instance's Prefab from it: its overrides become the variant's, and it is an instance of the variant"
                : (variantReason.empty() ? "Only a Prefab instance root" : variantReason.c_str()));
        }
        // ---- Replace Prefab (HierarchyPrefabRows.cpp) ----
        {
            std::vector<Entity> replaceRoots = ReplacePrefabCmd::replaceableRoots(project, sceneId, targets);
            const bool canReplace = project->canReplacePrefab(sceneId, node.id);
            if (!canReplace) {
                replaceRoots.clear();
            }
            if (ImGui::MenuItem(ICON_FA_RIGHT_LEFT "  Replace...", nullptr, false, !replaceRoots.empty())) {
                requestReplacePrefab(sceneId, replaceRoots);
            }
            ImGui::SetItemTooltip("%s", project->isAnyScenePlaying() ? "Stop the scene to replace" :
                canReplace ? "Makes the instance an instance of another prefab, keeping the overrides that match (Alt-drop a prefab on it too)"
                : "Only a Prefab instance's own root can be replaced: a nested one belongs to its prefab");
        }
        if (ImGui::MenuItem(ICON_FA_BOX_OPEN "  Unpack", nullptr, false, canUnpack)) {
            CommandHandle::get(sceneId)->addCommandNoMerge(new UnpackPrefabCmd(project, sceneId, targets, false));
        }
        {
            // Unity: an instance of a Prefab Variant unpacks to an instance of its base
            const std::filesystem::path unpackBase = assetPath.empty() || !node.isPrefabRoot ? std::filesystem::path() : project->getPrefabFileBase(assetPath);
            const std::string unpackTip = !unpackBase.empty() ? "Makes it an instance of the base Prefab '" + unpackBase.stem().string() + "', the variant's overrides kept as its own"
                : std::string("Turns the Prefab instance into regular GameObjects; nested Prefab instances stay");
            ImGui::SetItemTooltip("%s", project->isAnyScenePlaying() ? "Stop the scene to unpack" : unpackTip.c_str());
        }
        if (ImGui::MenuItem(ICON_FA_BOXES_PACKING "  Unpack Completely", nullptr, false, canUnpack)) {
            CommandHandle::get(sceneId)->addCommandNoMerge(new UnpackPrefabCmd(project, sceneId, targets, true));
        }
        ImGui::SetItemTooltip("%s", project->isAnyScenePlaying() ? "Stop the scene to unpack"
            : "Turns the Prefab instance and every Prefab instance inside it into regular GameObjects");
    }
}

void editor::HierarchyWindow::revealInProject(const std::filesystem::path& path){
    if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) {
        std::filesystem::path absolute = path;
        if (absolute.is_relative()) {
            absolute = project->getProjectPath() / absolute;
        }
        projectWindow->revealAsset(absolute);
    }
}

void editor::HierarchyWindow::beginRename(RenameTarget target, uint32_t sceneId, Entity entity, const std::string& currentName){
    renameTarget = target;
    renameSceneId = sceneId;
    renameEntity = entity;
    strncpy(renameBuffer, currentName.c_str(), sizeof(renameBuffer) - 1);
    renameBuffer[sizeof(renameBuffer) - 1] = '\0';
    renameFocusFrames = 3; // the field activates a frame after SetKeyboardFocusHere
    renameMissedFrames = 0;
    if (target == RenameTarget::Entity) {
        revealEntity(sceneId, entity);
    }
}

bool editor::HierarchyWindow::isRenaming(const TreeNode& node) const{
    if (renameTarget == RenameTarget::Scene) {
        return node.isScene && node.id == renameSceneId;
    }
    if (renameTarget == RenameTarget::Entity) {
        return !node.isScene && !node.isChildScene && node.id == renameEntity && getNodeSceneId(node) == renameSceneId;
    }
    return false;
}

void editor::HierarchyWindow::drawRenameField(){
    renameMissedFrames = 0;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (renameFocusFrames > 0) {
        ImGui::SetKeyboardFocusHere();
        renameFocusFrames--;
    }
    // No vertical padding, so the field keeps the row height
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
    const bool entered = ImGui::InputText("##HierarchyRename", renameBuffer, sizeof(renameBuffer),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopStyleVar();
    if (ImGui::IsItemActive()) {
        renameFocusFrames = 0;
    }
    if (entered) {
        commitRename();
    } else if (ImGui::IsItemDeactivated()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            renameTarget = RenameTarget::None;
        } else {
            commitRename();
        }
    } else if (!ImGui::IsItemActive() && renameFocusFrames == 0) {
        // Never got focus (the row scrolled away or another window took it)
        renameTarget = RenameTarget::None;
    }
}

void editor::HierarchyWindow::commitRename(){
    const RenameTarget target = renameTarget;
    renameTarget = RenameTarget::None;
    if (renameBuffer[0] == '\0') {
        return;
    }

    if (target == RenameTarget::Scene) {
        SceneProject* sceneProject = project->getScene(renameSceneId);
        if (sceneProject && sceneProject->name != renameBuffer) {
            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(new SceneNameCmd(project, renameSceneId, renameBuffer));
        }
    } else if (target == RenameTarget::Entity) {
        SceneProject* sceneProject = project->getScene(renameSceneId);
        if (sceneProject && sceneProject->scene && sceneProject->scene->isEntityCreated(renameEntity)
            && sceneProject->scene->getEntityName(renameEntity) != renameBuffer) {
            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(new EntityNameCmd(project, renameSceneId, renameEntity, renameBuffer));
        }
    }
}

bool editor::HierarchyWindow::renameSelected(){
    SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->scene) {
        return false;
    }
    std::vector<Entity> selected = project->getSelectedEntities(sceneProject->id);
    if (selected.size() != 1 || cachedPlayCreatedEntities.count(selected[0]) > 0) {
        return false;
    }
    setOpen(true);
    ImGui::SetWindowFocus(HierarchyWindow::WINDOW_NAME);
    beginRename(RenameTarget::Entity, sceneProject->id, selected[0], sceneProject->scene->getEntityName(selected[0]));
    return true;
}

bool editor::HierarchyWindow::isFocused() const{
    return windowOpen && windowFocused;
}

void editor::HierarchyWindow::drawInsertionMarker(const ImVec2& p1, const ImVec2& p2) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImU32 col = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
    float thickness = Theme::dpi(2.0f);
    draw_list->AddLine(p1, p2, col, thickness);
}

std::string editor::HierarchyWindow::getObjectIcon(Signature signature, Scene* scene){
    if (signature.test(scene->getComponentId<PrefabComponent>())){
        return ICON_FA_CUBE;
    }else if (signature.test(scene->getComponentId<ModelComponent>())){
        return ICON_FA_PERSON_RUNNING;
    }else if (signature.test(scene->getComponentId<BoneComponent>())){
        return ICON_FA_BONE;
    }else if (signature.test(scene->getComponentId<FluidSimulationComponent>())){
        // before the Sprite or Mesh it is drawn on: the fluid is what the object is
        return ICON_FA_DROPLET;
    }else if (signature.test(scene->getComponentId<ParticleSystemComponent>())){
        // before the Mesh it draws with (a version 2 system's own quad)
        return ICON_FA_FIRE;
    }else if (signature.test(scene->getComponentId<TilemapComponent>())){
        return ICON_FA_BORDER_ALL;
    }else if (signature.test(scene->getComponentId<SpriteComponent>())){
        return ICON_FA_IMAGE;
    }else if (signature.test(scene->getComponentId<TerrainComponent>())){
        return ICON_FA_MOUNTAIN;
    }else if (signature.test(scene->getComponentId<MeshPolygonComponent>())){
        return ICON_FA_VECTOR_SQUARE;
    }else if (signature.test(scene->getComponentId<MirrorComponent>())){
        return ICON_FA_CLONE;
    }else if (signature.test(scene->getComponentId<MeshComponent>())){
        return ICON_FA_DICE_D20;
    }else if (signature.test(scene->getComponentId<LatticeComponent>())){
        return ICON_FA_BORDER_NONE;
    }else if (signature.test(scene->getComponentId<SkyboxComponent>())){
        return ICON_FA_CLOUD;
    }else if (signature.test(scene->getComponentId<FogComponent>())){
        return ICON_FA_SMOG;
    }else if (signature.test(scene->getComponentId<ReflectionProbeComponent>())){
        return ICON_FA_BULLSEYE;
    }else if (signature.test(scene->getComponentId<UIDocumentComponent>())){
        return ICON_FA_FILE_CODE;
    }else if (signature.test(scene->getComponentId<LayoutGroupComponent>())){
        return ICON_FA_OBJECT_GROUP;
    }else if (signature.test(scene->getComponentId<OnScreenStickComponent>())){
        return ICON_FA_GAMEPAD;
    }else if (signature.test(scene->getComponentId<OnScreenButtonComponent>())){
        return ICON_FA_CIRCLE_DOT;
    }else if (signature.test(scene->getComponentId<TouchAreaComponent>())){
        return ICON_FA_HAND_POINTER;
    }else if (signature.test(scene->getComponentId<CanvasGroupComponent>()) && !signature.test(scene->getComponentId<UIComponent>())){
        // a group container (an element with its own group keeps its element's icon)
        return ICON_FA_LAYER_GROUP;
    }else if (signature.test(scene->getComponentId<SafeAreaComponent>())){
        return ICON_FA_MOBILE_SCREEN_BUTTON;
    }else if (signature.test(scene->getComponentId<ButtonComponent>())){
        return ICON_FA_SQUARE_CARET_RIGHT;
    }else if (signature.test(scene->getComponentId<ScrollbarComponent>())){
        return ICON_FA_ARROWS_UP_DOWN;
    }else if (signature.test(scene->getComponentId<ProgressbarComponent>())){
        return ICON_FA_BARS_PROGRESS;
    }else if (signature.test(scene->getComponentId<TextComponent>())){
        return ICON_FA_FONT;
    }else if (signature.test(scene->getComponentId<InputFieldComponent>())){
        return ICON_FA_I_CURSOR;
    }else if (signature.test(scene->getComponentId<PolygonComponent>())){
        return ICON_FA_DRAW_POLYGON;
    }else if (signature.test(scene->getComponentId<PanelComponent>())){
        return ICON_FA_WINDOW_MAXIMIZE;
    }else if (signature.test(scene->getComponentId<ImageComponent>())){
        return ICON_FA_IMAGE;
    }else if (signature.test(scene->getComponentId<UIComponent>())){
        return ICON_FA_IMAGE;
    }else if (signature.test(scene->getComponentId<LightComponent>())){
        return ICON_FA_LIGHTBULB;
    }else if (signature.test(scene->getComponentId<Light2DComponent>())){
        return ICON_FA_LIGHTBULB;
    }else if (signature.test(scene->getComponentId<ShadowCaster2DComponent>())){
        return ICON_FA_CIRCLE_HALF_STROKE;
    }else if (signature.test(scene->getComponentId<CapsuleShadowCasterComponent>())){
        return ICON_FA_CAPSULES;
    }else if (signature.test(scene->getComponentId<CameraComponent>())){
        return ICON_FA_VIDEO;
    }else if (signature.test(scene->getComponentId<Joint2DComponent>()) || signature.test(scene->getComponentId<Joint3DComponent>())){
        return ICON_FA_LINK;
    }else if (signature.test(scene->getComponentId<AnimationComponent>())){
        return ICON_FA_FILM;
    }else if (signature.test(scene->getComponentId<SpriteAnimationComponent>())){
        return ICON_FA_PLAY;
    }else if (signature.test(scene->getComponentId<PositionTweenComponent>())){
        return ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT;
    }else if (signature.test(scene->getComponentId<RotationTweenComponent>())){
        return ICON_FA_ROTATE;
    }else if (signature.test(scene->getComponentId<ScaleTweenComponent>())){
        return ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER;
    }else if (signature.test(scene->getComponentId<ColorTweenComponent>())){
        return ICON_FA_PALETTE;
    }else if (signature.test(scene->getComponentId<AlphaTweenComponent>())){
        return ICON_FA_EYE;
    }else if (signature.test(scene->getComponentId<TranslateTracksComponent>())){
        return ICON_FA_ROUTE;
    }else if (signature.test(scene->getComponentId<RotateTracksComponent>())){
        return ICON_FA_ARROWS_SPIN;
    }else if (signature.test(scene->getComponentId<ScaleTracksComponent>())){
        return ICON_FA_MAXIMIZE;
    }else if (signature.test(scene->getComponentId<MorphTracksComponent>())){
        return ICON_FA_SHAPES;
    }else if (signature.test(scene->getComponentId<LatticeTracksComponent>())){
        return ICON_FA_CUBES_STACKED;
    }else if (signature.test(scene->getComponentId<ParticleSystemComponent>())){
        return ICON_FA_FIRE;
    }else if (signature.test(scene->getComponentId<ForceFieldComponent>())){
        return ICON_FA_WIND;
    }else if (signature.test(scene->getComponentId<LineRendererComponent>())){
        return ICON_FA_GRIP_LINES;
    }else if (signature.test(scene->getComponentId<PointsComponent>())){
        return ICON_FA_CIRCLE;
    }else if (signature.test(scene->getComponentId<ActionComponent>())){
        return ICON_FA_PLAY;
    }else if (signature.test(scene->getComponentId<AudioSourceComponent>())){
        if (signature.test(scene->getComponentId<Transform>())) {
            return ICON_FA_VOLUME_HIGH;
        }
        return ICON_FA_FILE_AUDIO;
    }else if (signature.test(scene->getComponentId<CharacterController2DComponent>()) || signature.test(scene->getComponentId<CharacterControllerComponent>())){
        return ICON_FA_PERSON_WALKING;
    }else if (signature.test(scene->getComponentId<Rigidbody2DComponent>()) || signature.test(scene->getComponentId<RigidbodyComponent>())){
        return ICON_FA_WEIGHT_HANGING;
    }else if (signature.test(scene->getComponentId<TrailRendererComponent>())){
        return ICON_FA_WIND;
    }else if (signature.test(scene->getComponentId<Transform>())){
        return ICON_FA_CUBE; // plain GameObject, Unity's generic cube
    }

    return ICON_FA_CIRCLE_DOT;
}

void editor::HierarchyWindow::moveDraggedEntitiesToTarget(const std::vector<Entity>& draggedEntities, Entity target, InsertionType type) {
    if (draggedEntities.empty()) {
        return;
    }

    uint32_t sceneId = project->getSelectedSceneId();

    // Unity refuses the whole drop when a member of a Prefab instance is in it, and offers
    // to open the Prefab (HierarchyPrefabRows.cpp)
    if (refusePrefabRestructure(sceneId, draggedEntities)) {
        return;
    }

    if (draggedEntities.size() == 1) {
        CommandHandle::get(sceneId)->addCommand(new MoveEntityOrderCmd(project, sceneId, draggedEntities[0], target, type));
        return;
    }

    MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
    Entity currentTarget = target;
    InsertionType currentType = type;
    size_t commandCount = 0;

    for (Entity sourceEntity : draggedEntities) {
        if (sourceEntity == currentTarget) {
            continue;
        }

        multiCmd->addCommand(std::make_unique<MoveEntityOrderCmd>(project, sceneId, sourceEntity, currentTarget, currentType));
        currentTarget = sourceEntity;
        currentType = InsertionType::AFTER;
        commandCount++;
    }

    if (commandCount > 0) {
        CommandHandle::get(sceneId)->addCommandNoMerge(multiCmd);
    } else {
        delete multiCmd;
    }
}

void editor::HierarchyWindow::moveDraggedEntitiesToRootLevel(const std::vector<Entity>& draggedEntities, const std::unordered_set<Entity>& entitiesSet) {
    if (draggedEntities.empty()) {
        return;
    }

    SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->scene) {
        return;
    }

    auto transforms = sceneProject->scene->getComponentArray<Transform>();
    Entity lastRootEntity = NULL_ENTITY;

    for (int i = transforms->size() - 1; i >= 0; i--) {
        Transform& transform = transforms->getComponentFromIndex(i);
        Entity entity = transforms->getEntity(i);

        if (transform.parent == NULL_ENTITY &&
            entitiesSet.find(entity) != entitiesSet.end()) {
            lastRootEntity = entity;
            break;
        }
    }

    if (lastRootEntity == NULL_ENTITY) {
        return;
    }

    moveDraggedEntitiesToTarget(draggedEntities, lastRootEntity, InsertionType::AFTER);
}

bool editor::HierarchyWindow::acceptsModelDrop(const std::string& filePath) const {
    // As in the Scene window: a model file of the assets makes a Model GameObject of a 3D scene
    const SceneProject* sceneProject = project->getSelectedScene();
    if (SpriteSheetPool::isAsepritePath(filePath)) {
        // an .aseprite file (S44 R33): an animated Sprite in any scene
        return sceneProject && sceneProject->playState == ScenePlayState::STOPPED;
    }
    return (Util::isModelFile(filePath) || Util::isVertexAnimationFile(filePath)) && sceneProject &&
        sceneProject->sceneType == SceneType::SCENE_3D && sceneProject->playState == ScenePlayState::STOPPED;
}

// GameObject > 2D Object > Tilemap > <layout> (S50 R63): "Grid" with a child "Tilemap", one undo step
void editor::HierarchyWindow::createTilemapGrid(const CreateMenuEntry& entry, Entity parent, bool addToPrefab){
    SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->scene) {
        return;
    }
    TileGridLayout layout = TileGridLayout::Rectangle;
    StaggerAxis axis = StaggerAxis::Y;
    Vector2 cellSize(16.0f, 16.0f);
    const std::string label = entry.label;
    if (label == "Isometric" || label == "Isometric Staggered") {
        layout = label == "Isometric" ? TileGridLayout::Isometric : TileGridLayout::IsometricStaggered;
        cellSize = Vector2(32.0f, 16.0f);
    } else if (label == "Hexagonal (Pointy Top)" || label == "Hexagonal (Flat Top)") {
        layout = TileGridLayout::Hexagon;
        axis = label == "Hexagonal (Pointy Top)" ? StaggerAxis::Y : StaggerAxis::X;
        cellSize = axis == StaggerAxis::Y ? Vector2(16.0f, 14.0f) : Vector2(14.0f, 16.0f);
    }
    CommandHistory* history = CommandHandle::get(sceneProject->id);
    const size_t mark = history->mark();
    auto* createGrid = new CreateEntityCmd(project, sceneProject->id, "Grid", EntityCreationType::TILEMAP_GRID, parent, addToPrefab);
    history->addCommandNoMerge(createGrid);
    const Entity grid = createGrid->getEntity();
    if (layout != TileGridLayout::Rectangle) {
        // through the reflection table, as the Grid Inspector's rows write these fields (one undoable value each)
        const reflect::ComponentDesc* desc = reflect::find(ComponentType::GridComponent);
        void* component = desc->find(sceneProject->scene, grid);
        const auto setField = [&](const char* path, const void* value) {
            const reflect::FieldRef field = reflect::resolve(*desc, component, path);
            history->addCommandNoMerge(new ReflectedValueCmd(project, sceneProject->id, {grid}, ComponentType::GridComponent, path, reflect::Value(field.ops, value)));
        };
        setField("layout", &layout);
        setField("cellSize", &cellSize);
        if (axis != StaggerAxis::Y) setField("staggerAxis", &axis);
    }
    auto* createLayer = new CreateEntityCmd(project, sceneProject->id, "Tilemap", EntityCreationType::TILEMAP_LAYER, grid, addToPrefab);
    history->addCommandNoMerge(createLayer);
    history->groupSince(mark, "Create Tilemap");
    project->setSelectedEntity(sceneProject->id, createLayer->getEntity());
    openParent = parent;
    revealEntity(sceneProject->id, createLayer->getEntity());
}

// a Tiled or LDtk map dropped here (R46, R63): the plan's report, Import makes it one undo step
void editor::HierarchyWindow::openTilemapImportPreview(const std::string& mapFile, Entity parent) {
    tilemapImport = TilemapImportPreview();
    tilemapImport.file = mapFile;
    tilemapImport.parent = parent;
    tilemapImport.opening = true;
    std::vector<tilemapimport::ImportProblem> problems;
    tilemapImport.loaded = tilemapimport::loadPlan(project, std::filesystem::path(mapFile), tilemapImport.plan, problems);
    if (!tilemapImport.loaded) {
        tilemapImport.error = problems.empty() ? std::string("The map could not be read") : problems.front().message;
    } else {
        tilemapimport::ImportTarget target;
        target.project = project;
        target.sceneId = project->getSelectedSceneId();
        target.parent = parent;
        target.mapFile = std::filesystem::path(mapFile);
        tilemapImport.report = tilemapimport::previewReport(tilemapImport.plan, target);
    }
}

void editor::HierarchyWindow::showTilemapImportPreview() {
    if (tilemapImport.opening) {
        ImGui::OpenPopup("Import Tile Map###HierarchyTilemapImport");
        tilemapImport.opening = false;
    }
    if (!EditorModal::begin("Import Tile Map###HierarchyTilemapImport", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const std::string title = "Import " + std::filesystem::path(tilemapImport.file).filename().string();
    ImGui::TextUnformatted(title.c_str());
    ImGui::Separator();
    bool canImport = false;
    if (!tilemapImport.loaded) {
        EditorModal::error(tilemapImport.error);
        ImGui::TextColored(Theme::Colors::WarningText, "%s", tilemapImport.error.c_str());
    } else {
        const tilemapimport::ImportReport& report = tilemapImport.report;
        const tilemapimport::ImportCounts& counts = report.counts;
        const std::string summary = std::to_string(counts.grids) + " Grid(s), " + std::to_string(counts.layers) + " layer(s), " +
            std::to_string(counts.tilesets) + " tileset(s), " + std::to_string(counts.tiles) + " tile(s)";
        EditorModal::message(summary);
        ImGui::TextUnformatted(summary.c_str());
        for (const tilemapimport::ImportReport::Grid& grid : report.grids) {
            ImGui::BulletText("Grid %s: %d layer(s)", grid.name.c_str(), (int)grid.layers.size());
        }
        for (const tilemapimport::ImportReport::Tileset& tileset : report.tilesets) {
            ImGui::BulletText("Tileset %s (%s)", tileset.name.c_str(), tileset.file.c_str());
        }
        for (const tilemapimport::ImportSkipped& skipped : report.skipped) {
            ImGui::TextDisabled("Skipped: %s (%s) x%d", skipped.kind.c_str(), skipped.name.c_str(), skipped.count);
        }
        for (const std::string& warning : report.warnings) ImGui::TextColored(Theme::Colors::WarningText, "%s", warning.c_str());
        canImport = true;
    }
    ImGui::Separator();
    ImGui::BeginDisabled(!canImport);
    if (EditorModal::button("Import", "ok")) {
        tilemapimport::ImportTarget target;
        target.project = project;
        target.sceneId = project->getSelectedSceneId();
        target.parent = tilemapImport.parent;
        target.mapFile = std::filesystem::path(tilemapImport.file);
        const tilemapimport::ImportResult result = tilemapimport::apply(tilemapImport.plan, target);
        if (!result.ok) {
            Out::error("Import %s: %s", tilemapImport.file.c_str(), result.problem.message.c_str());
        } else if (!result.grids.empty()) {
            project->setSelectedEntity(target.sceneId, result.grids.front());
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void editor::HierarchyWindow::handleEntityFilesDrop(const std::vector<std::string>& filePaths, Entity parent) {
    for (const std::string& filePath : filePaths) {
        std::filesystem::path path(filePath);

        // a Tiled or LDtk map opens its import preview (S50 R46, R63)
        if (Util::isTilemapImportFile(filePath)) {
            if (!project->isInsideAssetsPath(path)) {
                Backend::getApp().registerOutsideAssetsAlert(filePath);
            } else {
                openTilemapImportPreview(filePath, parent);
            }
            continue;
        }

        // a Vertex Animation: Model = its .vat.glb + Vertex Animator, one step (vertex animation spec 3.3)
        if (Util::isVertexAnimationFile(filePath) && acceptsModelDrop(filePath)) {
            if (!project->isInsideAssetsPath(path)) {
                Backend::getApp().registerOutsideAssetsAlert(filePath);
                continue;
            }
            std::string error;
            if (createVatObject(project, project->getSelectedSceneId(), filePath, Vector3(0, 0, 0), parent, &error) == NULL_ENTITY) {
                Out::error("%s", error.c_str());
            }
            continue;
        }

        if (SpriteSheetPool::isAsepritePath(filePath) && acceptsModelDrop(filePath)) {
            if (!project->isInsideAssetsPath(path)) {
                Backend::getApp().registerOutsideAssetsAlert(filePath);
                continue;
            }
            // onto a Sprite: its sheet; onto an Animation Player: its clips; else a child Sprite
            pixelart::aseprite::dropAseprite(project, project->getSelectedSceneId(), project->normalizeToAssetsRelative(path).generic_string(),
                                             parent, parent, Vector3(0, 0, 0));
            continue;
        }

        if (acceptsModelDrop(filePath)) {
            if (!project->isInsideAssetsPath(path)) {
                Backend::getApp().registerOutsideAssetsAlert(filePath);
                continue;
            }
            const std::string modelPath = project->normalizeToAssetsRelative(path).generic_string();
            std::string entityName = path.stem().string();
            if (entityName.empty()) {
                entityName = "Model";
            }
            const uint32_t sceneId = project->getSelectedSceneId();
            CommandHandle::get(sceneId)->addCommandNoMerge(
                new ModelLoadCmd(project, sceneId, entityName, parent, Vector3(0, 0, 0), Quaternion(), Vector3(1, 1, 1), modelPath));
            continue;
        }

        if (path.extension() == ".prefab") {
            // Import the prefab into the current scene using command
            std::filesystem::path relativePath = std::filesystem::relative(path, project->getProjectPath());

            ImportPrefabCmd* importCmd = new ImportPrefabCmd(project, project->getSelectedSceneId(), relativePath, parent, true);
            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(importCmd);

            std::vector<Entity> newEntities = importCmd->getImportedEntities();

            if (newEntities.size() > 0) {
                Out::info("Instantiated prefab from: %s", path.string().c_str());
            } else {
                Out::warning("Failed to instantiate prefab from: %s", path.string().c_str());
            }
        }
    }
}

void editor::HierarchyWindow::handleSceneFilesDropAsChildScenes(const std::vector<std::string>& filePaths, uint32_t ownerSceneId) {
    if (ownerSceneId == NULL_PROJECT_SCENE) {
        return;
    }

    auto normalizeProjectScenePath = [this](const std::filesystem::path& p) -> std::filesystem::path {
        if (p.empty()) {
            return p;
        }
        if (p.is_relative()) {
            return (project->getProjectPath() / p).lexically_normal();
        }
        return p.lexically_normal();
    };

    auto pathsReferToSameFile = [&](const std::filesystem::path& a, const std::filesystem::path& b) -> bool {
        std::filesystem::path normalizedA = normalizeProjectScenePath(a);
        std::filesystem::path normalizedB = normalizeProjectScenePath(b);

        std::error_code ecA;
        std::error_code ecB;
        if (std::filesystem::exists(normalizedA, ecA) && !ecA &&
            std::filesystem::exists(normalizedB, ecB) && !ecB) {
            std::error_code equivalentEc;
            if (std::filesystem::equivalent(normalizedA, normalizedB, equivalentEc) && !equivalentEc) {
                return true;
            }
        }
        return normalizedA == normalizedB;
    };

    for (const std::string& filePath : filePaths) {
        std::filesystem::path droppedPath = normalizeProjectScenePath(std::filesystem::path(filePath));
        if (droppedPath.extension() != ".scene") {
            continue;
        }

        uint32_t droppedSceneId = NULL_PROJECT_SCENE;
        for (const auto& scene : project->getScenes()) {
            if (!scene.filepath.empty() && pathsReferToSameFile(scene.filepath, droppedPath)) {
                droppedSceneId = scene.id;
                break;
            }
        }

        if (droppedSceneId == NULL_PROJECT_SCENE) {
            Out::warning("Dropped scene is not part of the current project: %s", droppedPath.string().c_str());
            continue;
        }

        CommandHandle::get(ownerSceneId)->addCommand(new AddChildSceneCmd(project, ownerSceneId, droppedSceneId));
    }
}

void editor::HierarchyWindow::showAddChildSceneMenu() {
    if (ImGui::BeginMenu(ICON_FA_FOLDER_TREE "  Add Child Scene")) {
        uint32_t currentSceneId = project->getSelectedSceneId();
        const auto& scenes = project->getScenes();

        bool hasAvailableScenes = false;
        for (const auto& scene : scenes) {
            // Skip current scene, already added child scenes and scenes that contain it
            if (scene.id == currentSceneId) {
                continue;
            }
            if (project->hasChildScene(currentSceneId, scene.id)) {
                continue;
            }
            if (project->wouldCreateChildSceneCycle(currentSceneId, scene.id)) {
                continue;
            }

            hasAvailableScenes = true;
            std::string menuLabel = scene.name + " (ID: " + std::to_string(scene.id) + ")";
            if (ImGui::MenuItem(menuLabel.c_str())) {
                CommandHandle::get(currentSceneId)->addCommand(new AddChildSceneCmd(project, currentSceneId, scene.id));
            }
        }

        if (!hasAvailableScenes) {
            ImGui::TextDisabled("No scenes available");
        }

        ImGui::EndMenu();
    }
}

void editor::HierarchyWindow::updateSearchAndSelectionOrder(TreeNode& root, bool treeRebuilt) {
    // Names and ordering belong to the cached tree. Reuse their derived search flags
    // and range-selection order until that tree or the toolbar's search settings change.
    if (!treeRebuilt && cachedSearch == searchBuffer && cachedMatchCase == matchCase) {
        return;
    }
    cachedSearch = searchBuffer;
    cachedMatchCase = matchCase;

    const bool hasSearch = !cachedSearch.empty();
    if (hasSearch) {
        // the editor's one scorer in Filter mode, t:<Component> by display name (S25 R14)
        const HierarchyFilter filter(cachedSearch, matchCase);
        filter.markTree(root, [this](const TreeNode& node) {
            std::vector<std::string> names;
            if (node.isScene || node.isChildScene) return names;
            SceneProject* scene = project->getScene(getNodeSceneId(node));
            if (!scene || !scene->scene || !scene->scene->isEntityCreated(node.id)) return names;
            for (ComponentType type : Catalog::findComponents(scene->scene, node.id)) {
                names.push_back(Catalog::getComponentDisplayName(type));
            }
            return names;
        }, [](const TreeNode& node) { return nodeText(node); });
    }

    visibleEntitySelectionOrder.clear();
    collectVisibleEntitySelectionOrder(root, hasSearch);
}

std::string editor::HierarchyWindow::nodeText(const TreeNode& node) {
    if (node.isScene || node.isChildScene) return node.name;
    return entityDisplayName(node.name, node.id);
}

EntityPayload editor::HierarchyWindow::buildEntityPayload(const TreeNode& node, bool& allowed) {
    EntityPayload payload{};
    allowed = false;
    SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->scene) {
        return payload;
    }
    // Stop destroys runtime entities, so none can be moved, referenced or saved as a prefab
    for (Entity entity : getTopLevelSelectedEntities(node.id)) {
        if (cachedPlayCreatedEntities.count(entity) > 0) {
            return payload;
        }
    }
    payload.entity = node.id;
    payload.parent = node.parent;
    payload.order = node.order;
    payload.hasTransform = node.hasTransform;
    payload.entitySceneId = sceneProject->id;
    allowed = true;
    return payload;
}

std::string editor::HierarchyWindow::encodeDraggedSelection(uint32_t sceneId, Entity dragged) {
    SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->scene || sceneProject->id != sceneId || !sceneProject->scene->isEntityCreated(dragged)) {
        return {};
    }

    std::vector<Entity> draggedEntities = getTopLevelSelectedEntities(dragged);
    for (Entity entity : draggedEntities) {
        if (cachedPlayCreatedEntities.count(entity) > 0) {
            return {};
        }
    }
    std::vector<Entity> exportEntities = draggedEntities;

    // Add virtual children (and chained children) of dragged entities
    std::vector<Entity> virtualChildren = ProjectUtils::getVirtualChildren(sceneProject->scene, draggedEntities);
    exportEntities.insert(exportEntities.end(), virtualChildren.begin(), virtualChildren.end());

    // a posed character (Pose Animation window) in the copy is encoded with its rest bones
    PoseEditSession::suspendActiveFor(sceneProject->id, exportEntities);
    YAML::Node entityData;
    if (exportEntities.size() == 1) {
        entityData = Stream::encodeEntity(draggedEntities[0], sceneProject->scene, project, sceneProject);
    } else {
        // With the project: prefab instances in it stay instances (their members are not
        // copied as plain GameObjects)
        entityData = Stream::encodeEntitySelection(exportEntities, sceneProject->scene, project, sceneProject);
    }
    return YAML::Dump(entityData);
}

std::filesystem::path editor::HierarchyWindow::getPrefabSaveDirectory() const {
    if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) {
        std::filesystem::path currentPath = projectWindow->getCurrentPath();
        if (!currentPath.empty()) {
            return currentPath;
        }
    }
    return project->getProjectPath();
}

void editor::HierarchyWindow::saveNodeAsPrefab(const TreeNode& node) {
    ProjectWindow* projectWindow = Backend::getApp().getProjectWindow();
    if (!projectWindow) {
        return;
    }

    const std::string yaml = encodeDraggedSelection(project->getSelectedSceneId(), node.id);
    if (!yaml.empty()) {
        projectWindow->saveEntityFile(getPrefabSaveDirectory(), project->getSelectedSceneId(), yaml);
    }
}

void editor::HierarchyWindow::showTreeNode(editor::TreeNode& node) {
    // Skip nodes that don't match search and don't have matching descendants
    bool hasSearch = strlen(searchBuffer) > 0;
    if (hasSearch && !node.matchesSearch && !node.hasMatchingDescendant) {
        return;
    }

    // A runtime row waits for the 0.5s rebuild, so it can outlive its entity until then
    if (node.isPlayCreated) {
        const SceneProject* rowScene = project->getScene(getNodeSceneId(node));
        if (!rowScene || !rowScene->scene || !rowScene->scene->isEntityCreated(node.id)) {
            return;
        }
    }
    // the scene a child scene row selects into the Inspector: its running copy while listed
    const uint32_t childPropertiesSceneId = node.runtimeSceneId != 0 ? node.runtimeSceneId : node.childSceneId;

    pushNodeImGuiId(node);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap;

    if (node.children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }

    if ((node.isScene && std::find(selectedScenes.begin(), selectedScenes.end(), node.id) != selectedScenes.end()) ||
        (node.isChildScene && std::find(selectedScenes.begin(), selectedScenes.end(), childPropertiesSceneId) != selectedScenes.end())) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    bool isChildSceneEntity = (!node.isScene && !node.isChildScene && node.entitySceneId != 0 && node.entitySceneId != project->getSelectedSceneId());

    if (!node.isScene && !node.isChildScene && !isChildSceneEntity && project->isSelectedEntity(project->getSelectedSceneId(), node.id)) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    if (isChildSceneEntity && project->isSelectedEntity(node.entitySceneId, node.id)) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    SceneProject* nodeSceneProject = (!node.isScene && !node.isChildScene)
        ? project->getScene(getNodeSceneId(node))
        : nullptr;
    bool isModelWithChildren = !node.children.empty() && nodeSceneProject && nodeSceneProject->scene
        && nodeSceneProject->scene->findComponent<ModelComponent>(node.id);
    InstancedMeshComponent* instancedMesh = nodeSceneProject && nodeSceneProject->scene
        ? nodeSceneProject->scene->findComponent<InstancedMeshComponent>(node.id)
        : nullptr;
    size_t instanceCount = instancedMesh ? instancedMesh->instances.size() : 0;

    // Auto-expand nodes when searching to show matches
    if (hasSearch && node.hasMatchingDescendant) {
        ImGui::SetNextItemOpen(true);
    } else if (!hasSearch && !node.children.empty()) {
        // Model entities can exist for a few frames as leaves while their children load
        // asynchronously. Initialize tree state only once children exist so imported
        // model hierarchies start collapsed instead of inheriting an earlier leaf state.
        ImGui::SetNextItemOpen(!node.isBone && !node.isPrefabRoot && !isModelWithChildren, ImGuiCond_Once);
    }

    if(!node.isScene && openParent == node.id) {
        ImGui::SetNextItemOpen(true);
        openParent = NULL_ENTITY;
    }

    if (revealOpenNodes.find(&node) != revealOpenNodes.end()) {
        ImGui::SetNextItemOpen(true);
    }

    // Push colors for visual feedback
    bool pushedHighlightColor = false;

    // Is this camera entity currently being viewed through (preview mode)? Resolved via
    // the node's owning scene render. getPreviewCameraEntity() is a cheap member read, so
    // it short-circuits for every other node before the heavier isPreviewCameraActive().
    bool previewingThisCamera = nodeSceneProject && nodeSceneProject->sceneRender
        && nodeSceneProject->sceneRender->getPreviewCameraEntity() == node.id
        && nodeSceneProject->sceneRender->isPreviewCameraActive();

    // Highlight matching nodes when searching
    if (previewingThisCamera) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.86f, 0.2f, 1.0f)); // Yellow: viewing through this camera
        pushedHighlightColor = true;
    } else if (hasSearch && node.matchesSearch) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.5f, 1.0f)); // Yellow for search matches
        pushedHighlightColor = true;
    } else if (node.isPlayCreated) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 0.65f, 1.0f)); // Green: created by the running scene
        pushedHighlightColor = true;
    } else if (!node.isScene && !node.isChildScene && node.isLocked && node.isPrefab) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.67f, 0.74f, 0.85f, 0.95f)); // Muted blue-gray for locked prefab entities
        pushedHighlightColor = true;
    } else if (!node.isScene && !node.isChildScene && node.isLocked && !node.canEditModelHierarchy) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]); // Use theme's disabled color
        pushedHighlightColor = true;
    } else if (node.isChildScene) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ChildSceneText);
        pushedHighlightColor = true;
    } else if (node.isMissingPrefab) {
        ImGui::PushStyleColor(ImGuiCol_Text, MISSING_PREFAB_TEXT_COLOR); // Missing Prefab instance root
        pushedHighlightColor = true;
    } else if ((node.isPrefab && !node.isParentPrefab) || (node.isPrefabRoot && node.isParentPrefab)) {
        ImGui::PushStyleColor(ImGuiCol_Text, PREFAB_TEXT_COLOR); // Prefab instance root
        pushedHighlightColor = true;
    } else if (node.isPrefab && node.isParentPrefab) {
        ImGui::PushStyleColor(ImGuiCol_Text, PREFAB_MEMBER_TEXT_COLOR); // GameObject inside a prefab instance
        pushedHighlightColor = true;
    }

    const bool renamingNode = isRenaming(node);
    // an object a script made without a name reads "GameObject <id>", dimmed (unless a colour
    // rule above already took the row)
    const bool unnamed = !node.isScene && !node.isChildScene && isUnnamedEntity(node.name);
    if (unnamed && !pushedHighlightColor) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        pushedHighlightColor = true;
    }
    std::string nodeLabel = BidiText::toVisual(nodeText(node));
    SceneProject* headerScene = node.isScene ? project->getScene(node.id) : nullptr;
    if (node.isScene) {
        // Unity's scene header row: framed, always spans the panel, "*" while unsaved
        flags |= ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_FramePadding;
        if (headerScene && headerScene->isModified) {
            nodeLabel += "*";
        }
    }
    if (node.isMissingPrefab) {
        nodeLabel += " (Missing Prefab)";   // Unity's red Missing Prefab row
    }
    // Unity dims GameObjects that are inactive themselves or through a parent
    const bool dimInactive = (!node.isScene && !node.isChildScene && nodeSceneProject && nodeSceneProject->scene
        && nodeSceneProject->scene->hasInactiveEntities() && !nodeSceneProject->scene->isEntityActiveInHierarchy(node.id))
        || node.isNotRunning;
    if (dimInactive) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    }
    bool nodeOpen = renamingNode
        ? ImGui::TreeNodeEx("##node", flags, "%s", node.icon.c_str())
        : ImGui::TreeNodeEx("##node", flags, "%s  %s", node.icon.c_str(), nodeLabel.c_str());
    if (!node.isScene) UIProbe::item("hierarchy.node." + nodeLabel);
    if (dimInactive) {
        ImGui::PopStyleVar();
    }
    if (!node.isScene && !node.isChildScene) {
        auto& sceneCollapsedEntities = collapsedEntities[getNodeSceneId(node)];
        if (!node.children.empty() && !nodeOpen) {
            sceneCollapsedEntities.insert(node.id);
        } else {
            sceneCollapsedEntities.erase(node.id);
        }
    }

    // Only scroll an off-screen row, so selecting in the viewport does not jerk the tree
    if (revealTargetNode == &node && !ImGui::IsItemVisible()) {
        ImGui::SetScrollHereY(0.5f);
    }

    const float rowMinY = ImGui::GetItemRectMin().y;
    const float rowMaxY = ImGui::GetItemRectMax().y;
    bool nodeHovered = ImGui::IsItemHovered();
    bool nodeRightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    const ImGuiID nodeItemId = ImGui::GetItemID();
    if (!renamingNode) {
        drawPrefabRowOverlay(node, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        drawVcsRowOverlay(node, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }

    // Pop color if we pushed it
    if (pushedHighlightColor) {
        ImGui::PopStyleColor();
    }

    if (renamingNode){
        // no tooltip over the rename field
    }else if (node.isScene){
        if (const PrefabStage* stage = project->getPrefabStage(node.id)) {
            ImGui::SetItemTooltip("Prefab Mode: %s%s", stage->assetPath.generic_string().c_str(), stage->inContext() ? "\nIn Context" : "");
        } else if (const SceneProject* loaded = project->getReplacingMainScene(node.id)) {
            // the Scene view and the Game view show the scene the game loaded
            ImGui::SetItemTooltip("Scene: %s\nId: %u\nNot running: the game loaded \"%s\"", node.name.c_str(), node.id, loaded->name.c_str());
        } else {
            ImGui::SetItemTooltip("Scene: %s\nId: %u", node.name.c_str(), node.id);
        }
    }else if (node.isChildScene){
        const SceneProject* childScene = project->getScene(node.childSceneId);
        std::filesystem::path childScenePath = childScene ? childScene->filepath : std::filesystem::path();
        if (!childScenePath.empty() && childScenePath.is_absolute()) {
            std::error_code ec;
            std::filesystem::path relativePath = std::filesystem::relative(childScenePath, project->getProjectPath(), ec);
            if (!ec && !relativePath.empty()) {
                childScenePath = relativePath;
            }
        }
        const std::string pathText = childScenePath.empty() ? "Unsaved scene" : childScenePath.generic_string();
        if (node.isPersistent) {
            ImGui::SetItemTooltip("DontDestroyOnLoad\nThe GameObjects that survive scene loads (DontDestroyOnLoad)\n"
                "Running: they go on Stop");
        } else if (node.runtimeSceneId != 0) {
            ImGui::SetItemTooltip("%s\nPath: %s\nRunning: changes made while playing revert on Stop",
                node.isRuntimeLayer ? "Scene run by the game" : "Child Scene", pathText.c_str());
        } else if (node.isNotRunning) {
            ImGui::SetItemTooltip("Child Scene\nPath: %s\nNot running in the game", pathText.c_str());
        } else {
            ImGui::SetItemTooltip("Child Scene\nPath: %s", pathText.c_str());
        }
    }else{
        if (node.isMissingPrefab) {
            const MissingPrefabInstance* missing = project->getMissingPrefab(getNodeSceneId(node), node.id);
            ImGui::SetItemTooltip("GameObject: %u\nMissing Prefab: %s\nGUID: %s", node.id,
                missing ? missing->path.c_str() : node.nestedPrefabFilepath.string().c_str(),
                missing && !missing->guid.empty() ? missing->guid.c_str() : "none");
        } else if (node.isPrefab) {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                std::filesystem::path tooltipPath = node.isPrefabRoot ? node.nestedPrefabFilepath : node.prefabFilepath;
                Prefab* prefab = project->getPrefab(tooltipPath);
                uint32_t instanceId = prefab ? prefab->getInstanceId(project->getSelectedSceneId(), node.id) : 0;
                Entity registryEntity = prefab ? prefab->getRegistryEntity(project->getSelectedSceneId(), node.id) : NULL_ENTITY;

                ImGui::BeginTooltip();
                ImGui::Text("GameObject: %u", node.id);
                ImGui::Separator();
                ImGui::Text("Prefab: %s", tooltipPath.string().c_str());
                if (instanceId != 0) {
                    ImGui::Text("Instance: %u", instanceId);
                }
                if (registryEntity != NULL_ENTITY) {
                    ImGui::Text("Prefab GameObject: %u", registryEntity);
                }
                ImGui::EndTooltip();
            }
        } else {
            if (node.canEditModelHierarchy) {
                ImGui::SetItemTooltip("GameObject: %u\nDrag to organize the parts of this model", node.id);
            } else if (!node.hierarchyReason.empty()) {
                ImGui::SetItemTooltip("GameObject: %u\n%s", node.id, node.hierarchyReason.c_str());
            } else {
                ImGui::SetItemTooltip("GameObject: %u", node.id);
            }
        }
    }

    if (!node.isChildScene && !isChildSceneEntity && !node.isPlayCreated) {
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            // the header only (ImGui keeps it): the Project window encodes the selection when
            // it takes the drop, so a long drag over a big hierarchy encodes nothing
            if (!node.isScene) {
                bool allowed = false;
                const EntityPayload payload = buildEntityPayload(node, allowed);
                if (allowed) {
                    ImGui::SetDragDropPayload("entity", &payload, sizeof(payload), ImGuiCond_Once);
                }
            }

            ImGui::Text("Moving %s", nodeText(node).c_str());
            ImGui::EndDragDropSource();
        }
    }

    // Child scene entities: allow drag for cross-scene entity reference (ExternalEntity properties).
    // Not of a running copy: it goes with Stop, and a reference to it with it.
    if (isChildSceneEntity && !node.isScene && !project->isPlayRuntimeScene(node.entitySceneId)) {
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            SceneProject* childSceneProject = project->getScene(node.entitySceneId);
            if (childSceneProject && childSceneProject->scene) {
                // a reference only: its id means a GameObject of the child scene, never of this one
                EntityPayload payload{};
                payload.entity = node.id;
                payload.parent = node.parent;
                payload.order = node.order;
                payload.hasTransform = node.hasTransform;
                payload.entitySceneId = node.entitySceneId;
                payload.referenceOnly = true;
                ImGui::SetDragDropPayload("entity", &payload, sizeof(payload), ImGuiCond_Once);
            }

            ImGui::Text("Reference %s", nodeText(node).c_str());
            ImGui::EndDragDropSource();
        }
    }

    bool insertBefore = false;
    bool insertAfter = false;

    if (!node.isChildScene && !isChildSceneEntity && ImGui::BeginDragDropTarget()) {
        bool allowEntityDragDrop = false;
        const ImGuiPayload* payload = ImGui::GetDragDropPayload();
        Entity sourceEntity = 0;
        Entity sourceParent = 0;
        size_t sourceOrder = 0;
        bool sourceHasTransform = false;
        std::vector<Entity> movableEntities;
        if (payload && payload->IsDataType("entity")) {
            if (payload->DataSize >= sizeof(EntityPayload)) {
                allowEntityDragDrop = true;
                const EntityPayload* p = reinterpret_cast<const EntityPayload*>(payload->Data);
                sourceEntity = p->entity;
                sourceParent = p->parent;
                sourceOrder = p->order;
                sourceHasTransform = p->hasTransform;
                // a child scene's GameObject: its id names another GameObject here
                const std::string problem = sameSceneDropProblem(*project, *p, project->getSelectedSceneId(), true);
                if (!problem.empty()) {
                    allowEntityDragDrop = false;
                    ImGui::SetTooltip("%s", problem.c_str());
                }
            }

            if (sourceHasTransform != node.hasTransform && !node.isScene) {
                allowEntityDragDrop = false;
            }
            if (node.isScene && !sourceHasTransform) {
                allowEntityDragDrop = false;
            }

        }

        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 itemMin = ImGui::GetItemRectMin();
        ImVec2 itemMax = ImGui::GetItemRectMax();
        if (!node.isScene && node.hasTransform){
            insertBefore = (mousePos.y - itemMin.y) < (itemMax.y - itemMin.y) * 0.2f;
            insertAfter = (mousePos.y - itemMin.y) > (itemMax.y - itemMin.y) * 0.8f;
        }else{
            insertBefore = (mousePos.y - itemMin.y) < (itemMax.y - itemMin.y) * 0.5f;
            insertAfter = (mousePos.y - itemMin.y) >= (itemMax.y - itemMin.y) * 0.5f;
        }

        if (node.parent == sourceParent){
            if (node.order == (sourceOrder+1) && insertBefore){
                allowEntityDragDrop = false;
            }
            if (node.order == (sourceOrder-1) && insertAfter){
                allowEntityDragDrop = false;
            }
        }

        // Block reordering between entities with different virtual parents
        if (!sourceHasTransform && !node.hasTransform && !node.isScene
            && (insertBefore || insertAfter)
            && sourceParent != node.parent
            && (sourceParent != NULL_ENTITY || node.parent != NULL_ENTITY)) {
            allowEntityDragDrop = false;
        }

        InsertionType insertionType;
        if (insertBefore) {
            insertionType = InsertionType::BEFORE;
        } else if (insertAfter) {
            insertionType = InsertionType::AFTER;
        } else {
            insertionType = InsertionType::INTO;
        }

        if (allowEntityDragDrop) {
            movableEntities = getMovableDraggedEntities(sourceEntity, node, insertionType);
            allowEntityDragDrop = !movableEntities.empty();
        }

        if (allowEntityDragDrop){
            ImGuiDragDropFlags flags = 0;

            if (!node.isScene && (insertBefore || insertAfter)){
                flags |= ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
            }

            if (ImGui::AcceptDragDropPayload("entity", flags)) {

                if (payload->IsDelivery()){
                    if (node.isScene) {
                        SceneProject* sceneProject = project->getSelectedScene();
                        std::unordered_set<Entity> entitiesSet(sceneProject->entities.begin(), sceneProject->entities.end());
                        moveDraggedEntitiesToRootLevel(movableEntities, entitiesSet);
                    } else {
                        moveDraggedEntitiesToTarget(movableEntities, node.id, insertionType);
                    }
                }

            }

            if (!node.isScene && insertBefore) {
                const ImVec2& padding = ImGui::GetStyle().FramePadding;

                ImVec2 lineStart = ImGui::GetCursorScreenPos();
                ImVec2 lineEnd = lineStart;
                lineEnd.x += ImGui::GetContentRegionAvail().x;

                lineStart.y -= padding.y * 2.0;
                lineEnd.y -= padding.y * 2.0;

                ImVec2 node_size = ImGui::GetItemRectSize();

                lineStart.y -= node_size.y;
                lineEnd.y -= node_size.y;

                drawInsertionMarker(lineStart, lineEnd);
            }

            if (!node.isScene && insertAfter) {
                const ImVec2& padding = ImGui::GetStyle().FramePadding;

                ImVec2 lineStart = ImGui::GetCursorScreenPos();
                ImVec2 lineEnd = lineStart;
                lineEnd.x += ImGui::GetContentRegionAvail().x;

                lineStart.y -= padding.y;
                lineEnd.y -= padding.y;

                drawInsertionMarker(lineStart, lineEnd);
            }

        }

        bool allowResourceDragDrop = false;
        if (const ImGuiPayload* payload = ImGui::GetDragDropPayload()) {
            if (!node.isPlayCreated && payload->IsDataType("resource_files")) {
                const char* data = (const char*)payload->Data;
                size_t dataSize = payload->DataSize;

                size_t offset = 0;
                while (offset < dataSize) {
                    std::string path(data + offset);
                    if (!path.empty()) {
                        if (node.isScene) {
                            if (Util::isSceneFile(path) || Util::isPrefabFile(path) || acceptsModelDrop(path)) {
                                allowResourceDragDrop = true;
                                break;
                            }
                        } else {
                            // a model goes under a GameObject of the selected scene, not of a child scene
                            if (Util::isPrefabFile(path) || (acceptsModelDrop(path) && getNodeSceneId(node) == project->getSelectedSceneId())) {
                                allowResourceDragDrop = true;
                                break;
                            }
                        }
                    }
                    offset += path.size() + 1;
                }
            }
        }

        // a sound from the Sound Browser or the Project window: onto a Button it plays on clicks
        if (!node.isPlayCreated && !node.isScene && SoundDrop::accepts(ImGui::GetDragDropPayload())) {
            const ImGuiPayload* payload = ImGui::GetDragDropPayload();
            SceneProject* dropScene = project->getScene(getNodeSceneId(node));
            if (dropScene && dropScene->scene && dropScene->scene->findComponent<ButtonComponent>(static_cast<Entity>(node.id))) {
                if (const ImGuiPayload* dropped = ImGui::AcceptDragDropPayload(payload->DataType)) {
                    std::vector<std::string> sounds;
                    std::string error;
                    if (SoundDrop::resolve(project, dropped, sounds, error) && !sounds.empty()) {
                        SoundDrop::assignToButton(project, dropScene->id, static_cast<Entity>(node.id), sounds.front(), error);
                    }
                    if (!error.empty()) Out::warning("Sound drop: %s", error.c_str());
                }
            }
        }

        // Alt-drop of a prefab on an instance root: Replace Prefab (HierarchyPrefabRows.cpp)
        if (allowResourceDragDrop && !node.isScene && handlePrefabReplaceDrop(node)) {
            allowResourceDragDrop = false;
        }

        if (allowResourceDragDrop) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
                // Parse the dropped file paths
                std::vector<std::string> droppedPaths;
                const char* data = (const char*)payload->Data;
                size_t dataSize = payload->DataSize;

                // Parse null-terminated strings from the payload
                size_t offset = 0;
                while (offset < dataSize) {
                    std::string path(data + offset);
                    if (!path.empty()) {
                        droppedPaths.push_back(path);
                    }
                    offset += path.size() + 1; // +1 for null terminator
                }

                // Dropping on the scene root: allow adding child scenes via .scene files,
                // and keep supporting .prefab imports.
                if (node.isScene) {
                    handleSceneFilesDropAsChildScenes(droppedPaths, node.id);
                    handleEntityFilesDrop(droppedPaths, NULL_ENTITY);
                } else {
                    // Determine the parent entity for the drop
                    Entity parentEntity = NULL_ENTITY;
                    // If dropping on an entity, use it as parent if it has transform
                    if (node.hasTransform) {
                        parentEntity = node.id;
                    }
                    handleEntityFilesDrop(droppedPaths, parentEntity);
                }

                if (payload->IsDelivery()) {
                    ImGui::SetWindowFocus(HierarchyWindow::WINDOW_NAME);
                }
            }
        }

        // clips onto a GameObject: an Animation Player (added or reused) plays them, one scene
        // step (animation clips spec 4.1; the hook is the converters')
        // (not a payload the drop above took: a prefab with a clip makes the instance only)
        if (!allowResourceDragDrop && !node.isScene && !node.isPlayCreated && animationClipAppHooks().dropClips && !project->isAnyScenePlaying()) {
            const ImGuiPayload* peek = ImGui::GetDragDropPayload();
            std::vector<std::string> clips;
            if (peek && peek->IsDataType("resource_files")) {
                for (const std::string& path : Util::getStringsFromPayload(peek)) {
                    if (Util::isAnimationClipFile(path)) clips.push_back(path);
                }
            }
            if (!clips.empty() && ImGui::AcceptDragDropPayload("resource_files")) {
                Backend::getApp().dispatchClipDrop(getNodeSceneId(node), node.id, clips);
                ImGui::SetWindowFocus(HierarchyWindow::WINDOW_NAME);
            }
        }

        // a script onto a GameObject (Unity): attached to it, one step
        if (!allowResourceDragDrop && !node.isScene && !node.isPlayCreated && !project->isAnyScenePlaying()) {
            const ImGuiPayload* peek = ImGui::GetDragDropPayload();
            std::vector<std::string> received;
            if (peek && peek->IsDataType("resource_files")) received = Util::getStringsFromPayload(peek);
            if (!received.empty() && ScriptAttach::isScriptFile(received[0]) && ImGui::AcceptDragDropPayload("resource_files")) {
                if (SceneProject* target = project->getScene(getNodeSceneId(node))) {
                    ScriptAttach::attachFile(project, target, {node.id}, received[0]);
                }
                ImGui::SetWindowFocus(HierarchyWindow::WINDOW_NAME);
            }
        }

        // a physics material onto a GameObject: the material of all its colliders, one step
        if (!allowResourceDragDrop && !node.isScene && !node.isPlayCreated && !project->isAnyScenePlaying()) {
            const ImGuiPayload* peek = ImGui::GetDragDropPayload();
            std::vector<std::string> received;
            if (peek && peek->IsDataType("resource_files")) received = Util::getStringsFromPayload(peek);
            if (!received.empty() && PhysicsMaterialAssets::isPhysicsMaterialFile(received[0]) && project->isInsideAssetsPath(received[0]) &&
                ImGui::AcceptDragDropPayload("resource_files")) {
                std::string error;
                const std::string assetPath = project->normalizeToAssetsRelative(received[0]).generic_string();
                if (!PhysicsMaterialAssets::applyToObject(project, getNodeSceneId(node), node.id, assetPath, error)) {
                    Out::warning("%s", error.c_str());
                }
                ImGui::SetWindowFocus(HierarchyWindow::WINDOW_NAME);
            }
        }

        ImGui::EndDragDropTarget();
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered() && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) {
        project->clearAllSelections(project->getSelectedSceneId());
        project->clearInspectedAsset();
        selectedScenes.clear();
        project->setSelectedSceneForProperties(project->getSelectedSceneId());
        resetEntitySelectionAnchor();
    }

    // Check for selection on mouse release (not click) to allow drag without selection
    if (nodeHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (node.isChildScene) {
            project->clearAllSelections(project->getSelectedSceneId());
            selectedScenes.clear();
            selectedScenes.push_back(childPropertiesSceneId);
            project->setSelectedSceneForProperties(childPropertiesSceneId);
            resetEntitySelectionAnchor();
        } else if (isChildSceneEntity) {
            ImGuiIO& io = ImGui::GetIO();
            applyEntitySelection(node.entitySceneId, node.id, io.KeyShift, io.KeyCtrl);
        } else if (!node.isScene){
            ImGuiIO& io = ImGui::GetIO();
            applyEntitySelection(project->getSelectedSceneId(), node.id, io.KeyShift, io.KeyCtrl);
        }else{
            project->clearAllSelections(project->getSelectedSceneId());
            selectedScenes.clear();
            selectedScenes.push_back(node.id);
            project->setSelectedSceneForProperties(node.id);
            resetEntitySelectionAnchor();
        }
    }

    if (project->hasSelectedEntities(project->getSelectedSceneId())){
        selectedScenes.clear();
    }

    // Handle double-click on child scene to select it (kept for UX consistency)
    if (node.isChildScene && nodeHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        SceneProject* childScene = project->getScene(node.childSceneId);
        if (childScene) {
            project->openScene(childScene->filepath);
        }
    }

    // Handle double-click on entity to focus camera on it: in the Scene view of the scene
    // shown, a GameObject of it or of a scene layered over it
    if (!node.isScene && !node.isChildScene && nodeHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (project->getSelectedScene()) {
            std::vector<Entity> entities = { static_cast<Entity>(node.id) };
            sceneWindow->frameEntities(project->getSelectedSceneId(), getNodeSceneId(node), entities);
        }
    }

    if (nodeRightClicked) {
        // Unity selects the row it opens a context menu for
        if (!node.isScene && !node.isChildScene && !isChildSceneEntity
            && !project->isSelectedEntity(project->getSelectedSceneId(), node.id)) {
            applyEntitySelection(project->getSelectedSceneId(), node.id, false, false);
        }
        // Taking focus in the same frame closes a popup opened in it, so a click
        // that just focused the panel opens the menu on the next frame
        if (focusedThisFrame) {
            pendingContextMenuItem = nodeItemId;
        } else {
            ImGui::OpenPopup(node.isScene ? "SceneMenu" : "ContextMenu");
        }
    } else if (pendingContextMenuItem != 0 && pendingContextMenuItem == nodeItemId) {
        pendingContextMenuItem = 0;
        ImGui::OpenPopup(node.isScene ? "SceneMenu" : "ContextMenu");
    }

    if (!node.isScene && ImGui::BeginPopup("ContextMenu")) {
        if (node.isPersistent) {
            // made by the game, no scene file: nothing to open, show or remove
            ImGui::TextDisabled("%s", node.name.c_str());
            ImGui::Separator();
            ImGui::TextDisabled("Persistent GameObjects (DontDestroyOnLoad)");
            ImGui::TextDisabled("Running: they go on Stop");
            ImGui::EndPopup();
        } else if (node.isChildScene) {
            ImGui::TextDisabled("%s", node.name.c_str());
            ImGui::Separator();
            SceneProject* childScene = project->getScene(node.childSceneId);
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open Scene", nullptr, false, childScene && !childScene->filepath.empty())) {
                project->openScene(childScene->filepath);
            }
            // while the game runs, the row lists what it runs: no inline contents to switch
            const bool gameRow = node.runtimeSceneId != 0 || node.isNotRunning;
            bool isLoaded = childScene && childScene->expandedInline;
            if (ImGui::MenuItem(isLoaded ? ICON_FA_EYE_SLASH "  Hide Contents" : ICON_FA_EYE "  Show Contents", nullptr, false, !gameRow)) {
                if (isLoaded) {
                    project->unloadChildSceneInline(node.childSceneId);
                } else {
                    project->loadChildSceneInline(node.childSceneId);
                }
            }
            if (!node.isRuntimeLayer) {
                bool startActive = project->isChildSceneStartActive(node.ownerSceneId, node.childSceneId);
                if (ImGui::MenuItem(ICON_FA_PLAY "  Start Active", nullptr, startActive)) {
                    CommandHandle::get(node.ownerSceneId)->addCommand(new SetChildSceneStartActiveCmd(project, node.ownerSceneId, node.childSceneId, !startActive));
                }
            }
            if (childScene && ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS "  Select Scene Asset", nullptr, false, !childScene->filepath.empty())) {
                revealInProject(childScene->filepath);
            }
            if (!node.isRuntimeLayer) {
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_TRASH "  Remove Child Scene")) {
                    CommandHandle::get(node.ownerSceneId)->addCommand(new RemoveChildSceneCmd(project, node.ownerSceneId, node.childSceneId));
                }
            }
            ImGui::EndPopup();
        } else if (isChildSceneEntity) {
            // GameObject of a child scene: view only, edit it in its own scene
            ImGui::TextDisabled("%s", node.name.c_str());
            ImGui::Separator();
            ImGui::Text("GameObject: %u", node.id);
            const SceneProject* childScene = project->getScene(node.entitySceneId);
            if (childScene) {
                ImGui::Text("Scene: %s", childScene->name.c_str());
            }
            if (project->isPlayRuntimeScene(node.entitySceneId)) {
                ImGui::TextDisabled("Running: changes revert on Stop");
            } else {
                ImGui::TextDisabled("Open the child scene to edit it");
            }
            ImGui::EndPopup();
        } else {
            const uint32_t sceneId = project->getSelectedSceneId();
            // Unity acts on the whole selection when the clicked row is part of it
            std::vector<Entity> targets = project->getSelectedEntities(sceneId);
            if (targets.empty() || !project->isSelectedEntity(sceneId, node.id)) {
                targets = {node.id};
            }

            bool entityDeleted = false;

            // Unity's clipboard group, on the whole selection like the items below
            if (ImGui::IsWindowAppearing()) {
                EntityClipboard::refresh();
            }
            const bool canPaste = EntityClipboard::hasGameObjects(project) && !node.isPlayCreated;
            auto copyTargets = [&]() {
                return project->isSelectedEntity(sceneId, node.id) ?
                    EntityClipboard::getCopyableSelection(project, sceneId) : std::vector<Entity>{node.id};
            };
            if (ImGui::MenuItem(ICON_FA_SCISSORS "  Cut", shortcutText("edit.cut"), false, !node.isLocked)) {
                EntityClipboard::cutGameObjects(project, sceneId, copyTargets());
                entityDeleted = true;
            }
            if (ImGui::MenuItem(ICON_FA_CLONE "  Copy", shortcutText("edit.copy"), false, !node.isLocked)) {
                EntityClipboard::copyGameObjects(project, sceneId, copyTargets());
            }
            if (ImGui::MenuItem(ICON_FA_PASTE "  Paste", shortcutText("edit.paste"), false, canPaste && !entityDeleted)) {
                EntityClipboard::pasteGameObjects(project, sceneId, EntityClipboard::Placement::After, node.id);
            }
            if (ImGui::MenuItem(ICON_FA_PASTE "  Paste As Child", shortcutText("edit.pasteAsChild"), false, canPaste && node.hasTransform && !entityDeleted)) {
                EntityClipboard::pasteGameObjects(project, sceneId, EntityClipboard::Placement::Into, node.id);
            }
            ImGui::Separator();

            if (!entityDeleted && ImGui::MenuItem(ICON_FA_I_CURSOR "  Rename", shortcutText("edit.rename"), false, !node.isPlayCreated)) {
                beginRename(RenameTarget::Entity, sceneId, node.id, node.name);
            }
            if (!entityDeleted && ImGui::MenuItem(ICON_FA_COPY "  Duplicate", shortcutText("edit.duplicate"), false, !node.isLocked)) {
                CommandHandle::get(sceneId)->addCommandNoMerge(new DuplicateEntityCmd(project, sceneId, targets));
            }
            if (!entityDeleted && ImGui::MenuItem(ICON_FA_TRASH "  Delete", shortcutText("edit.delete"), false, !node.isLocked)) {
                // One command for all of them: one snapshot pass and one undo step
                if (!targets.empty()) {
                    CommandHandle::get(sceneId)->addCommandNoMerge(new DeleteEntityCmd(project, sceneId, targets));
                }
                entityDeleted = true;
            }
            // Window > General > Bookmarks (S25 R28): the GameObjects, by id and path
            if (!entityDeleted && !node.isPlayCreated && ImGui::MenuItem(ICON_FA_BOOKMARK "  Add to Bookmarks")) {
                for (Entity target : targets) {
                    std::string error;
                    if (!Backend::getApp().getBookmarksWindow()->add(Bookmarks::forGameObject(project, sceneId, target), &error)) {
                        Out::warning("Bookmarks: %s", error.c_str());
                    }
                }
            }

            // the Particle Effect window on the GameObject's subtree (S91): its systems as one effect
            if (!entityDeleted && !node.isPlayCreated && subtreeHasParticleSystems(project->getScene(sceneId), node.id) &&
                ImGui::MenuItem(ICON_FA_FIRE "  Open in Particle Effect")) {
                editor::getEditorHost().openParticleEffectSubtree(sceneId, node.id);
            }

            // Play Mode Changes: what scripts change here is offered at Stop too
            SceneProject* playingScene = project->getScene(sceneId);
            if (!entityDeleted && playingScene && !node.isPlayCreated && !project->isPlayRuntimeScene(sceneId) &&
                (playingScene->playState == ScenePlayState::PLAYING || playingScene->playState == ScenePlayState::PAUSED)) {
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_THUMBTACK "  Keep Values After Play (all components)")) {
                    for (Entity target : targets) {
                        PlayChangeReview::get().markGameObject(sceneId, target);
                    }
                }
                ImGui::SetItemTooltip("At Stop, Play Mode Changes offers to keep every component's values as they are then");
            }

            // Create children of this GameObject
            if (!entityDeleted && !node.isPlayCreated && node.hasTransform) {
                ImGui::Separator();
                static bool createInPrefab = false;
                if (node.isPrefab) {
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y * 0.5f));
                    ImGui::Checkbox("Add to Prefab", &createInPrefab);
                    ImGui::SetItemTooltip("New GameObjects become part of this prefab");
                    ImGui::PopStyleVar(1);
                }
                showCreateMenuItems(false, node.id, node.isPrefab && createInPrefab);
            }

            if (!entityDeleted && !node.isPlayCreated) {
                ImGui::Separator();
                if (ImGui::BeginMenu(ICON_FA_CUBE "  Prefab")) {
                    showPrefabMenu(node);
                    ImGui::EndMenu();
                }
            }

            SceneProject* selectedScene = project->getSelectedScene();
            if (!entityDeleted && node.hasTransform && selectedScene && selectedScene->scene) {
                Scene* scene = selectedScene->scene;
                Signature signature = scene->getSignature(node.id);
                bool separatorShown = false;
                auto separatorOnce = [&separatorShown]() {
                    if (!separatorShown) ImGui::Separator();
                    separatorShown = true;
                };

                if (selectedScene->sceneType == SceneType::SCENE_2D || selectedScene->sceneType == SceneType::SCENE_3D) {
                    const bool is3D = selectedScene->sceneType == SceneType::SCENE_3D;
                    ComponentType bodyType = is3D ? ComponentType::RigidbodyComponent : ComponentType::Rigidbody2DComponent;
                    bool hasBody = is3D ? signature.test(scene->getComponentId<RigidbodyComponent>())
                                        : signature.test(scene->getComponentId<Rigidbody2DComponent>());
                    separatorOnce();
                    if (ImGui::MenuItem(is3D ? ICON_FA_WEIGHT_HANGING "  Add Rigidbody" : ICON_FA_WEIGHT_HANGING "  Add Rigidbody 2D",
                                        nullptr, false, !node.isLocked && !hasBody)) {
                        CommandHandle::get(sceneId)->addCommandNoMerge(new AddComponentCmd(project, sceneId, node.id, bodyType));
                    }

                    bool canAddShadowCaster = !is3D
                        && !signature.test(scene->getComponentId<ShadowCaster2DComponent>())
                        && (signature.test(scene->getComponentId<SpriteComponent>())
                            || signature.test(scene->getComponentId<TilemapComponent>()));
                    if (!is3D && ImGui::MenuItem(ICON_FA_CIRCLE_HALF_STROKE "  Add Shadow Caster 2D", nullptr, false, !node.isLocked && canAddShadowCaster)) {
                        CommandHandle::get(sceneId)->addCommandNoMerge(new AddComponentCmd(project, sceneId, node.id, ComponentType::ShadowCaster2DComponent));
                    }

                    // a mesh on a target or below it: what the lattice is fitted to and deforms
                    bool targetsHaveMesh = false;
                    if (!node.isLocked) {
                        const std::unordered_set<Entity> targetSet(targets.begin(), targets.end());
                        auto meshes = scene->getComponentArray<MeshComponent>();
                        for (size_t m = 0; m < meshes->size() && !targetsHaveMesh; m++) {
                            // the mesh or one of its Transform parents is a target
                            Entity current = meshes->getEntity(m);
                            while (current != NULL_ENTITY && !targetsHaveMesh) {
                                targetsHaveMesh = targetSet.count(current) > 0;
                                const Transform* transform = scene->findComponent<Transform>(current);
                                current = transform ? transform->parent : NULL_ENTITY;
                            }
                        }
                    }
                    if (ImGui::MenuItem(ICON_FA_BORDER_NONE "  Add Lattice to Selection", nullptr, false, targetsHaveMesh)) {
                        createLatticeAround(targets);
                    }
                    if (!targetsHaveMesh && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::SetTooltip(node.isLocked ? "GameObject is locked" : "No mesh, sprite or model on the selection or below it");
                    }

                    bool hasMesh = signature.test(scene->getComponentId<MeshComponent>());
                    bool hasInstanced = signature.test(scene->getComponentId<InstancedMeshComponent>());
                    if (hasMesh && ImGui::MenuItem(ICON_FA_LAYER_GROUP "  Add Instanced Mesh", nullptr, false, !node.isLocked && !hasInstanced)) {
                        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                        multiCmd->addCommand(std::make_unique<AddComponentCmd>(project, sceneId, node.id, ComponentType::InstancedMeshComponent));
                        std::vector<InstanceData> initialInstances = { InstanceData() };
                        multiCmd->addPropertyCmd<std::vector<InstanceData>>(project, sceneId, node.id, ComponentType::InstancedMeshComponent, "instances", initialInstances);
                        CommandHandle::get(sceneId)->addCommandNoMerge(multiCmd);
                    }
                }

                ModelComponent* model = scene->findComponent<ModelComponent>(node.id);
                MeshComponent* mesh = scene->findComponent<MeshComponent>(node.id);
                if (model && mesh && !model->filename.empty()) {
                    ImGui::Separator();

                    const bool restoreHierarchy = model->mergeStaticMeshes;
                    std::string mergeReason;
                    bool canChangeMerge = restoreHierarchy ||
                        scene->getSystem<MeshSystem>()->canMergeStaticModel(*model, &mergeReason);
                    bool canEditHierarchy = ProjectUtils::hasCustomMeshParenting(scene, node.id) &&
                        scene->getSystem<MeshSystem>()->canEditModelHierarchy(*model);
                    if (!restoreHierarchy && canEditHierarchy) {
                        canChangeMerge = false;
                        mergeReason = "Reset the mesh parenting before merging this model";
                    }

                    if (ImGui::MenuItem(ICON_FA_SITEMAP "  Reset Mesh Parenting", nullptr, false, !node.isLocked && canEditHierarchy)) {
                        MultiPropertyCmd* resetCmd = new MultiPropertyCmd();
                        std::map<int, Entity> defaults = scene->getSystem<MeshSystem>()->getModelNodeDefaultParents(node.id, *model);
                        // Move descendants first so a reversed parent/child arrangement can be reset.
                        std::vector<std::pair<int, Entity>> parts(model->meshNodesMapping.begin(), model->meshNodesMapping.end());
                        std::sort(parts.begin(), parts.end(), [&](const auto& a, const auto& b) {
                            return ProjectUtils::getTransformIndex(scene, a.second) >
                                ProjectUtils::getTransformIndex(scene, b.second);
                        });
                        for (const auto& meshNode : parts) {
                            auto defaultIt = defaults.find(meshNode.first);
                            Entity defaultParent = defaultIt != defaults.end() ? defaultIt->second : node.id;
                            if (scene->getComponent<Transform>(meshNode.second).parent != defaultParent) {
                                resetCmd->addCommand(std::make_unique<MoveEntityOrderCmd>(
                                    project, selectedScene->id, meshNode.second, defaultParent, InsertionType::INTO));
                            }
                        }
                        CommandHandle::get(selectedScene->id)->addCommandNoMerge(resetCmd);
                    }

                    bool mergeEnabled = !node.isLocked && canChangeMerge;
                    const char* mergeLabel = restoreHierarchy
                        ? ICON_FA_OBJECT_GROUP "  Restore Model Mesh Children"
                        : ICON_FA_COMPRESS "  Merge Static Model";

                    if (ImGui::MenuItem(mergeLabel, nullptr, false, mergeEnabled)) {
                        CommandHandle::get(sceneId)->addCommandNoMerge(
                            new ModelLoadCmd(project, selectedScene->id, node.id, model->filename, !restoreHierarchy));
                    }
                    if (!mergeEnabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::SetItemTooltip("%s", node.isLocked ? "GameObject is locked" : mergeReason.c_str());
                    }
                }
            }

            if (!entityDeleted && selectedScene && selectedScene->scene
                && selectedScene->scene->getSignature(node.id).test(selectedScene->scene->getComponentId<CameraComponent>())) {
                ImGui::Separator();
                bool canAlignWithView = !node.isLocked && node.hasTransform
                    && selectedScene->sceneRender
                    && selectedScene->sceneRender->getCamera();
                if (ImGui::MenuItem(ICON_FA_CAMERA "  Align With View", nullptr, false, canAlignWithView)) {
                    moveCameraToEditorView(node.id);
                }
                if (!canAlignWithView && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip(node.isLocked ? "Camera is locked" : "Scene view camera unavailable");
                }
                bool previewingCamera = selectedScene->sceneRender
                    && selectedScene->sceneRender->isPreviewCameraActive()
                    && selectedScene->sceneRender->getPreviewCameraEntity() == node.id;
                bool canPreviewCamera = node.hasTransform
                    && selectedScene->playState == ScenePlayState::STOPPED
                    && selectedScene->sceneRender;
                if (previewingCamera) {
                    if (ImGui::MenuItem(ICON_FA_XMARK "  Exit Camera Preview", nullptr, false, canPreviewCamera)) {
                        sceneWindow->stopViewingCamera(selectedScene->id);
                    }
                } else {
                    if (ImGui::MenuItem(ICON_FA_VIDEO "  View Through Camera", nullptr, false, canPreviewCamera)) {
                        sceneWindow->viewThroughCamera(selectedScene->id, node.id);
                    }
                }
                if (!canPreviewCamera && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip(selectedScene->playState == ScenePlayState::STOPPED ? "Camera transform unavailable" : "Stop the scene to preview cameras");
                }
                if (node.isMainCamera) {
                    if (ImGui::MenuItem(ICON_FA_EYE_SLASH "  Unset as Main Camera")) {
                        CommandHandle::get(sceneId)->addCommand(new SetMainCameraCmd(project, sceneId, NULL_ENTITY));
                    }
                } else {
                    bool usedAsTexture = CameraTextureLink::isCameraUsed(selectedScene->scene, node.id);
                    if (ImGui::MenuItem(ICON_FA_EYE "  Set as Main Camera", nullptr, false, !usedAsTexture && !node.isPlayCreated)) {
                        CommandHandle::get(sceneId)->addCommand(new SetMainCameraCmd(project, sceneId, node.id));
                    }
                    if (usedAsTexture && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::SetTooltip("Camera is used as a render texture source");
                    }
                }
            }

            // the project's editor scripts: GameObject/... items (S23)
            EditorMenuRunContext scriptContext;
            scriptContext.sceneId = sceneId;
            for (Entity selected : project->getSelectedEntities(sceneId)) scriptContext.selection.push_back(selected);
            EditorScripts::get().drawContextItems("gameobject", {}, scriptContext);

            ImGui::EndPopup();
        }
    }

    if (renamingNode) {
        drawRenameField();
    }

    // Scene header ⋮ button, right-aligned like Unity's
    if (node.isScene && !renamingNode) {
        const char* icon = ICON_FA_ELLIPSIS_VERTICAL;
        float buttonWidth = ImGui::CalcTextSize(icon).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float buttonPosX = ImGui::GetWindowContentRegionMax().x - buttonWidth;
        ImGui::SameLine(buttonPosX > ImGui::GetCursorPosX() ? buttonPosX : 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
        if (ImGui::Button(icon)) {
            ImGui::OpenPopup("SceneMenu");
        }
        ImGui::SetItemTooltip("Scene options");
        ImGui::PopStyleColor(3);
    }

    // Scene header: the same menu from right-click and from its ⋮ button
    if (node.isScene && ImGui::BeginPopup("SceneMenu")) {
        showSceneMenu(headerScene);
        ImGui::EndPopup();
    }

    if (node.isMainCamera && !renamingNode) {
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.70f, 0.70f, 1.0f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetStyle().FramePadding.y * 0.5f);
        ImGui::SetWindowFontScale(0.7f);
        ImGui::TextUnformatted(ICON_FA_EYE);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Main Camera");
    }

    if ((node.hasScript || (node.hasChildScript && !nodeOpen)) && !renamingNode) {
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.70f, 0.70f, 1.0f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetStyle().FramePadding.y * 0.5f);
        ImGui::SetWindowFontScale(0.7f);
        ImGui::TextUnformatted(ICON_FA_FILE_CODE);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Has scripts");
    }

    if (instanceCount > 0 && !renamingNode) {
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.70f, 0.70f, 1.0f));
        if (!node.isMainCamera) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetStyle().FramePadding.y * 0.5f);
        }
        ImGui::SetWindowFontScale(0.7f);
        ImGui::TextUnformatted(ICON_FA_CLONE);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%zu instance%s", instanceCount, instanceCount == 1 ? "" : "s");
    }

    if (node.isChildScene && !node.isRuntimeLayer) {
        bool startActive = project->isChildSceneStartActive(node.ownerSceneId, node.childSceneId);
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ChildSceneText);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetStyle().FramePadding.y * 0.5f);
        ImGui::SetWindowFontScale(0.7f);
        ImGui::TextUnformatted(startActive ? ICON_FA_PLAY : ICON_FA_PAUSE);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip(startActive ? "Starts active" : "Starts inactive");
    }

    // Eye icon toggle for child scene inline loading (not while the row lists the running game)
    if (node.isChildScene && node.runtimeSceneId == 0 && !node.isNotRunning) {
        const SceneProject* childScene = project->getScene(node.childSceneId);
        bool isLoaded = childScene && childScene->expandedInline;

        const char* icon = isLoaded ? ICON_FA_EYE : ICON_FA_EYE_SLASH;
        float buttonWidth = ImGui::CalcTextSize(icon).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float buttonPosX = ImGui::GetWindowContentRegionMax().x - buttonWidth;

        // Ensure it doesn't overlap the text too much if the window is very narrow
        if (buttonPosX > ImGui::GetCursorPosX() + 10.0f) {
            ImGui::SameLine(buttonPosX);
        } else {
            ImGui::SameLine();
        }

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
        ImGui::PushStyleColor(ImGuiCol_Text, isLoaded ? ImVec4(0.55f, 0.80f, 0.85f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 0.5f));

        // Remove frame padding for this button so it doesn't stretch the tree node height
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));

        ImGui::PushID("ChildSceneEye");
        ImGui::PushID(node.childSceneId);

        if (ImGui::Button(icon)) {
            if (isLoaded) {
                project->unloadChildSceneInline(node.childSceneId);
            } else {
                project->loadChildSceneInline(node.childSceneId);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(isLoaded ? "Hide child scene contents" : "Show child scene contents");
        }

        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
    }

    // Prefab Mode's back arrow on its header (HierarchyPrefabMode.cpp)
    drawPrefabRowButtons(node, renamingNode);
    // ">" at the right end of an instance root's row: opens its prefab (HierarchyPrefabRows.cpp)
    if (!renamingNode) {
        showPrefabRowArrow(node);
    }
    // the eye and the hand at the row's left (HierarchyVisibilityGutter.cpp)
    drawVisibilityGutter(node, rowMinY, rowMaxY);

    if (nodeOpen) {
        for (auto& child : node.children) {
            showTreeNode(child);
        }
        ImGui::TreePop();
    }

    if (node.separator){
        ImGui::Separator();
    }

    popNodeImGuiId(node);
}

void editor::HierarchyWindow::revealEntity(uint32_t sceneId, Entity entity) {
    revealRequest = {sceneId, entity};
}

bool editor::HierarchyWindow::collectRevealPath(TreeNode& node) {
    bool isEntityNode = !node.isScene && !node.isChildScene;
    if (isEntityNode && node.id == revealRequest.entity && getNodeSceneId(node) == revealRequest.sceneId) {
        revealTargetNode = &node;
        return true;
    }

    for (auto& child : node.children) {
        if (collectRevealPath(child)) {
            revealOpenNodes.insert(&node);
            return true;
        }
    }

    return false;
}

void editor::HierarchyWindow::syncSceneWindowSelectionHierarchy(const TreeNode& node, uint32_t collapsedSceneId, Entity collapsedAncestor) {
    if (node.isScene) {
        sceneWindow->beginStructureVisibilityUpdate(node.id);
        collapsedSceneId = NULL_PROJECT_SCENE;
        collapsedAncestor = NULL_ENTITY;
    } else if (node.isChildScene) {
        sceneWindow->beginStructureVisibilityUpdate(node.runtimeSceneId != 0 ? node.runtimeSceneId : node.childSceneId);
        collapsedSceneId = NULL_PROJECT_SCENE;
        collapsedAncestor = NULL_ENTITY;
    } else {
        uint32_t sceneId = getNodeSceneId(node);
        if (collapsedAncestor != NULL_ENTITY && collapsedSceneId == sceneId) {
            sceneWindow->setStructureSelectionParent(sceneId, node.id, collapsedAncestor);
        } else {
            collapsedSceneId = NULL_PROJECT_SCENE;
            collapsedAncestor = NULL_ENTITY;
        }

        // Nearest collapsed ancestor, so the scene window can walk the chain outwards
        auto sceneIt = collapsedEntities.find(sceneId);
        if (!node.children.empty()
            && sceneIt != collapsedEntities.end() && sceneIt->second.find(node.id) != sceneIt->second.end()) {
            collapsedSceneId = sceneId;
            collapsedAncestor = node.id;
        }
    }

    for (const auto& child : node.children) {
        syncSceneWindowSelectionHierarchy(child, collapsedSceneId, collapsedAncestor);
    }
}

void editor::HierarchyWindow::pushNodeImGuiId(const TreeNode& node){
    if (node.isScene){
        ImGui::PushID("SceneNode");
        ImGui::PushID((int)node.id);
    }else if (node.isChildScene){
        ImGui::PushID("ChildSceneNode");
        ImGui::PushID((int)node.ownerSceneId);
        ImGui::PushID((int)node.childSceneId);
    }else if (node.entitySceneId != 0 && node.entitySceneId != project->getSelectedSceneId()){
        ImGui::PushID("ChildSceneEntityNode");
        ImGui::PushID((int)node.entitySceneId);
        ImGui::PushID((int)node.id);
    }else{
        ImGui::PushID("EntityNode");
        ImGui::PushID((int)project->getSelectedSceneId());
        ImGui::PushID((int)node.id);
    }
}

void editor::HierarchyWindow::popNodeImGuiId(const TreeNode& node){
    if (node.isScene){
        ImGui::PopID();
        ImGui::PopID();
    }else if (node.isChildScene){
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
    }else if (node.entitySceneId != 0 && node.entitySceneId != project->getSelectedSceneId()){
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
    }else{
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
    }
}

// Walks the scene (and any inline child scenes) into the TreeNode hierarchy the panel
// draws. Called only when the cache key changes, so it must not depend on frame state.
void editor::HierarchyWindow::rebuildEntityTree(SceneProject* sceneProject, TreeNode& root, std::unordered_set<Entity>& sceneEntitiesSet){

    sceneEntitiesSet = std::unordered_set<Entity>(sceneProject->entities.begin(), sceneProject->entities.end());

    // Whatever the scene spawned while running is listed alongside the authored entities, so a
    // script's object shows up under its parent instead of nowhere. It is never authored, so
    // it is drawn as read-only and disappears again once Stop destroys it.
    const std::vector<Entity> playCreated = project->getPlayCreatedEntities(sceneProject);
    cachedPlayCreatedEntities = std::unordered_set<Entity>(playCreated.begin(), playCreated.end());
    sceneEntitiesSet.insert(playCreated.begin(), playCreated.end());

    Entity mainCamera = sceneProject->mainCamera;
    size_t order = 0;
    std::unordered_map<Entity, TreeNode*> entityNodeMap;
    const std::unordered_map<Entity, std::filesystem::path> prefabEntityPaths =
        project->getPrefabPathsForScene(sceneProject->id);
    std::unordered_map<Entity, std::list<TreeNode>> virtualPrefabChildren;
    std::unordered_map<Entity, std::list<TreeNode>> virtualActionChildren;

    root = TreeNode();

    switch (sceneProject->sceneType) {
        case SceneType::SCENE_3D: root.icon = ICON_FA_CUBES; break;
        case SceneType::SCENE_2D: root.icon = ICON_FA_CUBES_STACKED; break;
        case SceneType::SCENE_UI: root.icon = ICON_FA_WINDOW_RESTORE; break;
        default: root.icon = ICON_FA_CUBES; break;
    }
    root.id = sceneProject->id;
    root.isScene = true;
    root.order = order++;
    root.name = sceneProject->name;
    if (project->isPrefabStage(sceneProject->id)) {
        // Prefab Mode: the header is the prefab's (Unity's Hierarchy header in a stage)
        root.icon = ICON_FA_CUBE;
    }

    // The GameObjects of a child scene under its node: the edit-time scene, or while the
    // game runs, its running copy with what the game made in it (read-only rows)
    auto addSceneContents = [&](TreeNode& sceneNode, SceneProject* contentScene, uint32_t contentSceneId) {
        Scene* contentRegistry = contentScene->scene;
        const std::vector<Entity> contentPlayCreated = project->getPlayCreatedEntities(contentScene);
        const std::unordered_set<Entity> contentPlayCreatedSet(contentPlayCreated.begin(), contentPlayCreated.end());
        std::unordered_set<Entity> childEntitiesSet(contentScene->entities.begin(), contentScene->entities.end());
        childEntitiesSet.insert(contentPlayCreated.begin(), contentPlayCreated.end());
        std::unordered_map<Entity, TreeNode*> childEntityNodeMap;

        std::vector<Entity> flatEntities = contentScene->entities;
        flatEntities.insert(flatEntities.end(), contentPlayCreated.begin(), contentPlayCreated.end());

        // non-hierarchical entities
        for (auto& entity : flatEntities) {
            Signature signature = contentRegistry->getSignature(entity);
            if (!signature.test(contentRegistry->getComponentId<Transform>())) {
                TreeNode enode;
                enode.icon = getObjectIcon(signature, contentRegistry);
                enode.id = entity;
                enode.entitySceneId = contentSceneId;
                enode.isMainCamera = (entity == contentScene->mainCamera);
                enode.hasScript = hasScripts(contentRegistry, entity);
                enode.isBone = signature.test(contentRegistry->getComponentId<BoneComponent>());
                enode.isPrefabRoot = signature.test(contentRegistry->getComponentId<PrefabComponent>());
                enode.isMissingPrefab = enode.isPrefabRoot && project->isMissingPrefab(contentSceneId, entity);
                enode.isPlayCreated = contentPlayCreatedSet.count(entity) > 0;
                enode.order = order++;
                enode.name = contentRegistry->getEntityName(entity);
                sceneNode.children.push_back(enode);
                childEntityNodeMap[entity] = &sceneNode.children.back();
            }
        }

        // hierarchical entities
        auto childTransforms = contentRegistry->getComponentArray<Transform>();
        for (int i = 0; i < childTransforms->size(); i++) {
            Transform& transform = childTransforms->getComponentFromIndex(i);
            Entity entity = childTransforms->getEntity(i);
            Signature signature = contentRegistry->getSignature(entity);

            if (childEntitiesSet.find(entity) != childEntitiesSet.end()) {
                TreeNode enode;
                enode.icon = getObjectIcon(signature, contentRegistry);
                enode.id = entity;
                enode.entitySceneId = contentSceneId;
                enode.isMainCamera = (entity == contentScene->mainCamera);
                enode.hasScript = hasScripts(contentRegistry, entity);
                enode.isBone = signature.test(contentRegistry->getComponentId<BoneComponent>());
                enode.isPrefabRoot = signature.test(contentRegistry->getComponentId<PrefabComponent>());
                enode.isMissingPrefab = enode.isPrefabRoot && project->isMissingPrefab(contentSceneId, entity);
                enode.hasTransform = true;
                enode.isPlayCreated = contentPlayCreatedSet.count(entity) > 0;
                enode.isLocked = enode.isPlayCreated || ProjectUtils::isEntityLocked(contentRegistry, entity);
                enode.order = order++;
                enode.name = contentRegistry->getEntityName(entity);

                auto parentIt = childEntityNodeMap.find(transform.parent);
                TreeNode* parentNode = parentIt != childEntityNodeMap.end() ? parentIt->second : nullptr;
                if (parentNode) {
                    enode.parent = parentNode->id;
                    parentNode->children.push_back(enode);
                    childEntityNodeMap[entity] = &parentNode->children.back();
                } else if (transform.parent == NULL_ENTITY || (enode.isPlayCreated && childEntitiesSet.count(transform.parent) == 0)) {
                    // a runtime entity can hang from an unlisted one, like the default camera
                    sceneNode.children.push_back(enode);
                    childEntityNodeMap[entity] = &sceneNode.children.back();
                }
            }
        }
    };

    auto sceneTypeIcon = [](SceneType sceneType) -> std::string {
        switch (sceneType) {
            case SceneType::SCENE_3D: return ICON_FA_CUBES;
            case SceneType::SCENE_2D: return ICON_FA_CUBES_STACKED;
            case SceneType::SCENE_UI: return ICON_FA_WINDOW_RESTORE;
        }
        return ICON_FA_CUBES;
    };

    // While the game runs, the child scenes list the scenes it runs (as Unity lists the
    // loaded scenes in Play mode): their running copies, live
    const bool gameRunning = project->getPlaySessionSceneId() == sceneProject->id &&
        (sceneProject->playState == ScenePlayState::PLAYING || sceneProject->playState == ScenePlayState::PAUSED);
    const std::vector<SceneProject*> runningLayers = gameRunning
        ? project->getRunningRuntimeLayerScenes(sceneProject->id) : std::vector<SceneProject*>();
    std::unordered_set<uint32_t> listedLayers;

    // child scenes (shown before entities)
    bool hasChildScenes = !sceneProject->childScenes.empty() || !runningLayers.empty();
    for (const ChildSceneRef& childSceneRef : sceneProject->childScenes) {
        uint32_t childSceneId = childSceneRef.id;
        SceneProject* childScene = project->getScene(childSceneId);
        if (!childScene) {
            continue; // Skip invalid child scene references
        }

        TreeNode childSceneNode;
        childSceneNode.icon = sceneTypeIcon(childScene->sceneType);
        childSceneNode.name = childScene->name;
        childSceneNode.isChildScene = true;
        childSceneNode.id = childSceneId;
        childSceneNode.childSceneId = childSceneId;
        childSceneNode.ownerSceneId = sceneProject->id;
        childSceneNode.order = order++;

        if (gameRunning) {
            SceneProject* runtime = nullptr;
            const uint32_t runtimeId = project->getPlayRuntimeSceneId(childSceneId);
            for (SceneProject* runningLayer : runningLayers) {
                if (runtimeId != NULL_PROJECT_SCENE && runningLayer->id == runtimeId) {
                    runtime = runningLayer;
                }
            }
            if (runtime && runtime->scene) {
                childSceneNode.runtimeSceneId = runtimeId;
                listedLayers.insert(runtimeId);
                addSceneContents(childSceneNode, runtime, runtimeId);
            } else {
                // not running: nothing of it is in the game
                childSceneNode.isNotRunning = true;
            }
        } else if (childScene->expandedInline && childScene->scene) {
            // If child scene is expanded inline, build entity tree nodes for it
            addSceneContents(childSceneNode, childScene, childSceneId);
        }

        root.children.push_back(childSceneNode);
    }

    // scenes the game runs that are no child scene of this one: nested deeper, or added by scripts
    // (the DontDestroyOnLoad scene goes last, below the played scene's GameObjects)
    SceneProject* persistentLayer = nullptr;
    for (SceneProject* runtime : runningLayers) {
        if (listedLayers.count(runtime->id) || !runtime->scene) {
            continue;
        }
        if (project->isPersistentPlayScene(runtime->id)) {
            persistentLayer = runtime;
            continue;
        }
        TreeNode layerNode;
        layerNode.icon = sceneTypeIcon(runtime->sceneType);
        layerNode.name = runtime->name;
        layerNode.isChildScene = true;
        layerNode.isRuntimeLayer = true;
        layerNode.id = project->getPlaySourceSceneId(runtime->id);
        layerNode.childSceneId = layerNode.id;
        layerNode.ownerSceneId = sceneProject->id;
        layerNode.runtimeSceneId = runtime->id;
        layerNode.order = order++;
        addSceneContents(layerNode, runtime, runtime->id);
        root.children.push_back(layerNode);
    }

    bool separatorAfterChildScenes = hasChildScenes;

    // non-hierarchical entities
    std::vector<Entity> flatEntities = sceneProject->entities;
    flatEntities.insert(flatEntities.end(), playCreated.begin(), playCreated.end());

    for (auto& entity : flatEntities) {
        Signature signature = sceneProject->scene->getSignature(entity);

        if (!signature.test(sceneProject->scene->getComponentId<Transform>())){
            if (separatorAfterChildScenes && !root.children.empty()) {
                root.children.back().separator = true;
                separatorAfterChildScenes = false;
            }

            TreeNode child;
            child.icon = getObjectIcon(signature, sceneProject->scene);
            child.id = entity;
            child.isMainCamera = (entity == mainCamera);
            child.hasScript = hasScripts(sceneProject->scene, entity);
            child.isBone = signature.test(sceneProject->scene->getComponentId<BoneComponent>());
            child.isPlayCreated = cachedPlayCreatedEntities.count(entity) > 0;
            child.isLocked = child.isPlayCreated || ProjectUtils::isEntityLocked(sceneProject->scene, entity);
            child.order = order++;
            child.name = sceneProject->scene->getEntityName(entity);
            auto prefabIt = prefabEntityPaths.find(entity);
            if (prefabIt != prefabEntityPaths.end()) {
                child.isPrefab = true;
                child.prefabFilepath = prefabIt->second;
                if (signature.test(sceneProject->scene->getComponentId<PrefabComponent>())) {
                    child.isPrefabRoot = true;
                    child.nestedPrefabFilepath = sceneProject->scene->getComponent<PrefabComponent>(entity).path;
                }
            }
            if (signature.test(sceneProject->scene->getComponentId<PrefabComponent>())) {
                child.isMissingPrefab = project->isMissingPrefab(sceneProject->id, entity);
            }

            bool addedAsVirtualChild = false;
            Entity actionTarget = ProjectUtils::getVirtualParent(sceneProject->scene, entity);
            if (actionTarget != NULL_ENTITY && sceneEntitiesSet.find(actionTarget) != sceneEntitiesSet.end()) {
                // Check if they are in the same prefab
                bool samePrefab = false;
                auto targetPrefabIt = prefabEntityPaths.find(actionTarget);
                if (prefabIt != prefabEntityPaths.end() && targetPrefabIt != prefabEntityPaths.end() && prefabIt->second == targetPrefabIt->second) {
                    samePrefab = true;
                }

                // Priority is Action target, but if both are prefabs, ensure it prefers organizing by the actual local parent element correctly.
                if (samePrefab || prefabIt == prefabEntityPaths.end()) {
                    child.parent = actionTarget;
                    if (samePrefab) {
                        child.isParentPrefab = true;
                        child.hasPrefabParent = true;
                    }
                    virtualActionChildren[actionTarget].push_back(std::move(child));
                    addedAsVirtualChild = true;
                }
            }
            
            if (!addedAsVirtualChild && prefabIt != prefabEntityPaths.end()) {
                Prefab* prefab = project->getPrefab(prefabIt->second);
                Entity prefabRoot = prefab ? prefab->getRootEntity(sceneProject->id, entity) : NULL_ENTITY;
                if (prefabRoot != NULL_ENTITY && prefabRoot != entity) {
                    child.parent = prefabRoot;
                    child.isParentPrefab = true;
                    virtualPrefabChildren[prefabRoot].push_back(std::move(child));
                    addedAsVirtualChild = true;
                }
            }
            
            if (!addedAsVirtualChild) {
                root.children.push_back(child);
                entityNodeMap[entity] = &root.children.back();
            }
        }
    }

    bool applySeparator = false;
    if (root.children.size() > 0){
        applySeparator = true;
    }

    // hierarchical entities
    auto transforms = sceneProject->scene->getComponentArray<Transform>();
    for (int i = 0; i < transforms->size(); i++){
        Transform& transform = transforms->getComponentFromIndex(i);
        Entity entity = transforms->getEntity(i);
        Signature signature = sceneProject->scene->getSignature(entity);

        if (sceneEntitiesSet.find(entity) != sceneEntitiesSet.end()){
            if (applySeparator){
                root.children.back().separator = true;
                applySeparator = false;
            }

            if (separatorAfterChildScenes && !root.children.empty()) {
                root.children.back().separator = true;
                separatorAfterChildScenes = false;
            }

            TreeNode child;
            child.icon = getObjectIcon(signature, sceneProject->scene);
            child.id = entity;
            child.isMainCamera = (entity == mainCamera);
            child.hasScript = hasScripts(sceneProject->scene, entity);
            child.isBone = signature.test(sceneProject->scene->getComponentId<BoneComponent>());
            child.hasTransform = true;
            child.isPlayCreated = cachedPlayCreatedEntities.count(entity) > 0;
            child.isLocked = child.isPlayCreated
                || ProjectUtils::isEntityLocked(sceneProject->scene, entity)
                || ProjectUtils::getModelBranchOwner(sceneProject->scene, entity) != NULL_ENTITY;
            child.canEditModelHierarchy = ProjectUtils::canEditModelBranch(sceneProject->scene, entity, &child.hierarchyReason);
            child.order = order++;
            child.name = sceneProject->scene->getEntityName(entity);
            auto prefabIt = prefabEntityPaths.find(entity);
            if (prefabIt != prefabEntityPaths.end()) {
                child.isPrefab = true;
                child.prefabFilepath = prefabIt->second;
            }
            if (signature.test(sceneProject->scene->getComponentId<PrefabComponent>())) {
                child.isPrefabRoot = true;
                child.nestedPrefabFilepath = sceneProject->scene->getComponent<PrefabComponent>(entity).path;
                child.isMissingPrefab = project->isMissingPrefab(sceneProject->id, entity);
            }
            auto parentIt = entityNodeMap.find(transform.parent);
            TreeNode* parent = parentIt != entityNodeMap.end() ? parentIt->second : nullptr;
            if (parent){
                child.parent = parent->id;
                auto parentPrefabIt = prefabEntityPaths.find(transform.parent);
                if (parentPrefabIt != prefabEntityPaths.end()) {
                    child.isParentPrefab = true;
                    child.hasPrefabParent = true;
                    // Locally added prefab roots treated as top-level visually, not nested
                    if (child.isPrefabRoot && child.prefabFilepath != parentPrefabIt->second) {
                        child.isParentPrefab = false;
                    }
                }
                parent->children.push_back(child);
                entityNodeMap[entity] = &parent->children.back();
            }else if (transform.parent == NULL_ENTITY || (child.isPlayCreated && sceneEntitiesSet.count(transform.parent) == 0)){
                // a runtime entity can hang from an unlisted one, like the default camera
                root.children.push_back(child);
                entityNodeMap[entity] = &root.children.back();
            }else{
                printf("ERROR: Could not find parent of entity %u\n", entity);
            }
        }
    }

    for (auto& [rootEntity, children] : virtualPrefabChildren) {
        auto parentIt = entityNodeMap.find(rootEntity);
        if (parentIt == entityNodeMap.end() || children.empty()) {
            continue;
        }

        TreeNode* parent = parentIt->second;
        if (!parent->children.empty()) {
            children.back().separator = true;
        }

        parent->children.splice(parent->children.begin(), children);
    }

    // Splice action children, resolving targets that may themselves be action children
    bool splicedNew;
    do {
        splicedNew = false;
        for (auto it = virtualActionChildren.begin(); it != virtualActionChildren.end(); ) {
            auto parentIt = entityNodeMap.find(it->first);
            if (parentIt == entityNodeMap.end() || it->second.empty()) {
                ++it;
                continue;
            }

            TreeNode* parent = parentIt->second;
            if (!parent->children.empty()) {
                it->second.back().separator = true;
            }

            // Splice and register in entityNodeMap (std::list nodes are stable)
            parent->children.splice(parent->children.begin(), it->second);
            for (auto& child : parent->children) {
                if (entityNodeMap.find(child.id) == entityNodeMap.end()) {
                    entityNodeMap[child.id] = &child;
                }
            }

            it = virtualActionChildren.erase(it);
            splicedNew = true;
        }
    } while (splicedNew);

    // Unity's DontDestroyOnLoad scene in Play: the persistent GameObjects, live
    if (persistentLayer) {
        TreeNode persistentNode;
        persistentNode.icon = ICON_FA_THUMBTACK;
        persistentNode.name = persistentLayer->name;
        persistentNode.isChildScene = true;
        persistentNode.isRuntimeLayer = true;
        persistentNode.isPersistent = true;
        persistentNode.id = project->getPlaySourceSceneId(persistentLayer->id);
        persistentNode.childSceneId = persistentNode.id;
        persistentNode.ownerSceneId = sceneProject->id;
        persistentNode.runtimeSceneId = persistentLayer->id;
        persistentNode.order = order++;
        addSceneContents(persistentNode, persistentLayer, persistentLayer->id);
        if (!root.children.empty()) {
            root.children.back().separator = true;
        }
        root.children.push_back(persistentNode);
    }

    markPrefabScripts(root);
    markPrefabRows(root);
}

void editor::HierarchyWindow::showPartialLoadBanner(const SceneProject& sceneProject) {
    const size_t dropped = sceneProject.loadReport.dropped();
    if (dropped == 0) {
        return;
    }
    const size_t kept = sceneProject.loadReport.outstandingPreserved(findPreservedStore(sceneProject.scene));
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
    ImGui::TextWrapped("This scene did not load completely: %zu value%s dropped, %zu kept. Auto Save is off for this scene.",
                       dropped, dropped == 1 ? "" : "s", kept);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("Details##PartialLoad")) {
        for (const LoadIssue& issue : sceneProject.loadReport.issues) {
            if (issue.fate == LoadIssueFate::Dropped) {
                Out::error(issue.consoleLine());
            }
        }
        DataSafetyDialogs::showFileInConsole(sceneProject.filepath.filename().string());
    }
    ImGui::SetItemTooltip("Show this scene's load issues in the Console");
    ImGui::Separator();
}

void editor::HierarchyWindow::show(){
    windowFocused = false;
    if (!windowOpen) {
        return;
    }

    SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->scene) {
        return;
    }

    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }

    bool wasOpen = windowOpen;

    if (!ImGui::Begin(HierarchyWindow::WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        if (wasOpen && !windowOpen) {
            setOpen(false);
        }
        return;
    }
    // Tree rows do not take window focus when clicked, so do it here: F2, F and
    // the Edit menu need to know the Hierarchy is the panel being worked in.
    if ((ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)
        && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_NoPopupHierarchy)
        && !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        ImGui::SetWindowFocus();
        focusedThisFrame = true;
    } else {
        focusedThisFrame = false;
    }
    windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    showPartialLoadBanner(*sceneProject);

    // A rename started for a scene that is no longer shown has nothing to edit
    if (renameTarget != RenameTarget::None && renameSceneId != sceneProject->id) {
        renameTarget = RenameTarget::None;
    }

    // Rebuild the tree only when it changed. Key = scene + structureVersion + entity count +
    // child-scene signature; a 0.5s rebuild catches the rest, runtime entities included.
    size_t childSignature = 0;
    auto hashCombine = [](size_t& h, size_t v){ h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
    for (const ChildSceneRef& childSceneRef : sceneProject->childScenes) {
        hashCombine(childSignature, (size_t)childSceneRef.id);
        const SceneProject* childScene = project->getScene(childSceneRef.id);
        if (childScene) {
            hashCombine(childSignature, childScene->expandedInline ? 1u : 0u);
            if (childScene->expandedInline) {
                hashCombine(childSignature, childScene->entities.size());
                hashCombine(childSignature, (size_t)childScene->structureVersion);
            }
        }
    }
    // the scenes the running game layers, as they come and go
    hashCombine(childSignature, (size_t)sceneProject->playState);
    if (project->getPlaySessionSceneId() == sceneProject->id) {
        for (const SceneProject* runtime : project->getRunningRuntimeLayerScenes(sceneProject->id)) {
            hashCombine(childSignature, (size_t)runtime->id);
            hashCombine(childSignature, runtime->entities.size());
            hashCombine(childSignature, (size_t)runtime->structureVersion);
        }
    }

    const double nowTime = ImGui::GetTime();
    const bool rebuildTree =
        cacheSceneId != sceneProject->id ||
        cacheStructureVersion != sceneProject->structureVersion ||
        cacheEntityCount != sceneProject->entities.size() ||
        cacheChildSignature != childSignature ||
        cacheBuildTime < 0.0 || (nowTime - cacheBuildTime) > 0.5;

    // root and sceneEntitiesSet are cached members reused across frames.
    TreeNode& root = cachedRoot;
    std::unordered_set<Entity>& sceneEntitiesSet = cachedSceneEntitiesSet;

    if (rebuildTree) {
        cacheSceneId = sceneProject->id;
        cacheStructureVersion = sceneProject->structureVersion;
        cacheEntityCount = sceneProject->entities.size();
        cacheChildSignature = childSignature;
        cacheBuildTime = nowTime;

        rebuildEntityTree(sceneProject, root, sceneEntitiesSet);
    }

    showToolbar();
    ImGui::BeginChild("StructureScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    updateSearchAndSelectionOrder(root, rebuildTree);

    revealOpenNodes.clear();
    revealTargetNode = nullptr;
    if (revealRequest.entity != NULL_ENTITY) {
        collectRevealPath(root);
        revealRequest = {};
    }

    // the rows leave room on their left for the visibility gutter
    const float gutterWidth = visibilityGutterWidth();
    ImGui::Indent(gutterWidth);
    showTreeNode(root);
    ImGui::Unindent(gutterWidth);
    syncSceneWindowSelectionHierarchy(root);
    float treeLastCursorY = ImGui::GetCursorScreenPos().y;

    // The renamed row is hidden (filtered out, deleted): drop the rename
    if (renameTarget != RenameTarget::None && ++renameMissedFrames > 2) {
        renameTarget = RenameTarget::None;
    }

    // Handle right-click in empty space
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ImGui::IsAnyItemHovered() && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        if (focusedThisFrame) {
            pendingEmptySpaceMenu = true;
        } else {
            ImGui::OpenPopup("EmptySpaceContextMenu");
        }
    } else if (pendingEmptySpaceMenu) {
        pendingEmptySpaceMenu = false;
        ImGui::OpenPopup("EmptySpaceContextMenu");
    }

    // Empty space context menu: paste and create at the scene root
    if (ImGui::BeginPopup("EmptySpaceContextMenu")) {
        if (ImGui::IsWindowAppearing()) {
            EntityClipboard::refresh();
        }
        if (ImGui::MenuItem(ICON_FA_PASTE "  Paste", shortcutText("edit.paste"), false, EntityClipboard::hasGameObjects(project))) {
            EntityClipboard::pasteGameObjects(project, sceneProject->id, EntityClipboard::Placement::Root, NULL_ENTITY);
        }
        ImGui::Separator();
        showCreateMenuItems(true, NULL_ENTITY, false);
        ImGui::EndPopup();
    }

    ImGui::EndChild();

    // Handle drag and drop for entity files and entities to empty space
    if (ImGui::GetMousePos().y > treeLastCursorY && ImGui::BeginDragDropTarget()) {

        // Handle entity drag-drop to move to root level (last position)
        if (const ImGuiPayload* payload = ImGui::GetDragDropPayload()) {
            if (payload->IsDataType("entity")) {
                if (payload->DataSize >= sizeof(EntityPayload)) {
                    const EntityPayload* p = reinterpret_cast<const EntityPayload*>(payload->Data);
                    Entity sourceEntity = p->entity;
                    bool sourceHasTransform = p->hasTransform;
                    const std::string problem = sameSceneDropProblem(*project, *p, project->getSelectedSceneId(), true);
                    if (!problem.empty()) {
                        ImGui::SetTooltip("%s", problem.c_str());
                    }

                    if (sourceHasTransform && problem.empty()) {
                        std::vector<Entity> movableEntities = getMovableDraggedEntities(sourceEntity, root, InsertionType::AFTER);
                        if (!movableEntities.empty() && ImGui::AcceptDragDropPayload("entity")) {
                            if (payload->IsDelivery()) {
                                moveDraggedEntitiesToRootLevel(movableEntities, sceneEntitiesSet);
                            }
                        }
                    }
                }
            }
        }

        // a sound onto the empty Hierarchy: a new GameObject with an AudioSource
        if (const ImGuiPayload* soundPayload = ImGui::GetDragDropPayload(); SoundDrop::accepts(soundPayload)) {
            if (const ImGuiPayload* dropped = ImGui::AcceptDragDropPayload(soundPayload->DataType)) {
                std::vector<std::string> sounds;
                std::string error;
                if (SoundDrop::resolve(project, dropped, sounds, error)) {
                    for (const std::string& sound : sounds) SoundDrop::createSource(project, project->getSelectedSceneId(), sound, false, Vector3(0, 0, 0), error);
                }
                if (!error.empty()) Out::warning("Sound drop: %s", error.c_str());
            }
        }

        bool allowResourceDragDrop = false;
        if (const ImGuiPayload* payload = ImGui::GetDragDropPayload()) {
            if (payload->IsDataType("resource_files")) {
                const char* data = (const char*)payload->Data;
                size_t dataSize = payload->DataSize;

                size_t offset = 0;
                while (offset < dataSize) {
                    std::string path(data + offset);
                    if (!path.empty()) {
                        if (Util::isSceneFile(path) || Util::isPrefabFile(path) || acceptsModelDrop(path) || Util::isTilemapImportFile(path) || Util::isTitleArtFile(path)) {
                            allowResourceDragDrop = true;
                            break;
                        }
                    }
                    offset += path.size() + 1;
                }
            }
        }

        if(allowResourceDragDrop) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
                // Parse the dropped file paths
                std::vector<std::string> droppedPaths;
                const char* data = (const char*)payload->Data;
                size_t dataSize = payload->DataSize;

                // Parse null-terminated strings from the payload
                size_t offset = 0;
                while (offset < dataSize) {
                    std::string path(data + offset);
                    if (!path.empty()) {
                        droppedPaths.push_back(path);
                    }
                    offset += path.size() + 1; // +1 for null terminator
                }

                handleSceneFilesDropAsChildScenes(droppedPaths, project->getSelectedSceneId());
                handleEntityFilesDrop(droppedPaths);
                // a .titleart (the Project window's, or the Title Designer's document chip): a
                // GameObject showing it, one step "Create Title Art (logo)" (S81 R6, S82 R61)
                for (const std::string& path : droppedPaths) {
                    if (!Util::isTitleArtFile(path)) continue;
                    std::string error;
                    if (createTitleArtObject(project, project->getSelectedSceneId(), path, NULL_ENTITY, &error) == NULL_ENTITY) {
                        Out::warning("Title Art drop: %s", error.c_str());
                    }
                }

                if (payload->IsDelivery()) {
                    ImGui::SetWindowFocus(HierarchyWindow::WINDOW_NAME);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    showReplacePrefabPopup();
    showRestructurePopup();
    showTilemapImportPreview();

    ImGui::End();

    if (wasOpen && !windowOpen) {
        setOpen(false);
    }
}
