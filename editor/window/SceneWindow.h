// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "imgui.h"
#include "Project.h"
#include "object/Camera.h"
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>

namespace jasperengine::editor {

    class ImportPrefabCmd;

    class SceneWindow {
    private:
        Project* project;
        bool windowFocused;

        bool mouseLeftDown = false;
        Vector2 mouseLeftStartPos;
        Vector2 mouseLeftDragPos;
        bool mouseLeftDraggedInside;

        std::map<uint32_t, bool> draggingMouse;
        std::map<uint32_t, bool> suppressLeftMouseUntilRelease;
        std::map<uint32_t, bool> subSelectionClickConsumesRelease;
        // Physics Snap: a Shift + LMB press on a selected object; past the drag threshold it
        // snaps the selection along the surfaces under the cursor instead of box selecting.
        // Begun (or refused) once per press.
        std::map<uint32_t, bool> physicsSnapArmed;
        std::map<uint32_t, bool> physicsSnapBegun;
        // a menu or popup was open when the last frame's Scene views were drawn (its Esc is not
        // the Physics tool's)
        bool popupOpenLastFrame = false;
        std::map<uint32_t, bool> resyncLookDelta;
        std::map<uint32_t, bool> lookActive;
        std::map<uint32_t, ImVec2> lookReturnPos;
        std::map<uint32_t, bool> focusCanvasOnNextFrame;

        std::map<uint32_t, int> width;
        std::map<uint32_t, int> height;
        std::map<uint32_t, ImVec2> framebufferScale;
        std::map<uint32_t, float> canvasDpiScale;

        std::map<uint32_t, bool> hasNotification;

        // Entity children hidden below a collapsed Hierarchy node select an ancestor
        // when picked in the viewport. Each maps to its nearest collapsed ancestor.
        std::unordered_map<uint32_t, std::unordered_map<Entity, Entity>> structureSelectionParents;

        std::vector<uint32_t> closeSceneQueue;

        // Prefab Mode: the stage the Prefab Mode tab showed last frame (a new one takes focus,
        // leaving it gives the focus back to the scene)
        uint32_t shownPrefabStage = NULL_PROJECT_SCENE;

        // Prefab Mode's bar over its Scene view: breadcrumbs, Save, Auto Save, Context
        // (PrefabStageBar.cpp)
        void drawPrefabStageBar(SceneProject& sceneProject);
        std::string getPrefabStageWindowTitle(const SceneProject& sceneProject) const;
        // Leaves Prefab Mode down to `depth` stages, asking to save (deferred to the next frame)
        void requestExitPrefabStages(size_t depth);

        void handleCloseScene(uint32_t sceneId);
        void closeSceneInternal(uint32_t sceneId);
        void sceneEventHandler(SceneProject* sceneProject);
        void endLook(uint32_t sceneId);
        void handleResourceFileDragDrop(SceneProject* sceneProject);
        Vector3 getModelDropPosition(SceneProject* sceneProject, float x, float y, Entity hitEntity);
        void handleTileRectDragDrop(SceneProject* sceneProject);

        // Prefab dragged from the Project window over a Scene view (Unity's way): an instance
        // follows the pointer (on what it points at in 3D, at its world position in 2D), the
        // drop makes it for real as one undo step, leaving the view takes it away
        ImportPrefabCmd* prefabDragPreview = nullptr;
        uint32_t prefabDragSceneId = NULL_PROJECT_SCENE;
        std::string prefabDragPath;
        std::unordered_set<Entity> prefabDragEntities;
        void updatePrefabDrag(SceneProject* sceneProject, const std::vector<std::string>& paths, float x, float y, bool delivery);
        void cancelPrefabDrag();
        Vector3 getPrefabDropPosition(SceneProject* sceneProject, float x, float y, bool& keepZ);
        bool handleViewportGizmoClick(SceneProject* sceneProject, float canvasX, float canvasY, int canvasWidth);
        void snapCameraToDirection(Camera* camera, const Vector3& direction);
        void focusSceneWindow(const SceneProject& sceneProject) const;
        void requestOpenParentScene(uint32_t childSceneId, uint32_t parentSceneId);
        void drawParentSceneButton(const SceneProject& sceneProject);
        void drawGizmosMenu(SceneProject& sceneProject);
        std::string getWindowTitle(const SceneProject& sceneProject) const;
        Entity resolveStructureSelection(uint32_t viewportSceneId, uint32_t sceneId, Entity entity) const;
        Entity findSelectableObjectByRay(uint32_t sceneId, float x, float y, uint32_t* outSceneId = nullptr);
        bool selectObjectByRay(uint32_t sceneId, float x, float y, bool shiftPressed);
        ImVec2 canvasFramebufferScale(uint32_t sceneId) const;
        ImVec2 canvasEngineScale(uint32_t sceneId) const;
        float canvasDpi(uint32_t sceneId) const;
        void toEngineCanvas(uint32_t sceneId, float& x, float& y) const;
        
    public:
        // The ImGui id of Prefab Mode's tab: one tab, whichever stage it shows
        static constexpr const char* PREFAB_STAGE_WINDOW_ID = "###PrefabMode";

        SceneWindow(Project* project);

        void show();
        bool isFocused() const;
        void resetProjectState();
        void clearSceneState(uint32_t sceneId);
        void requestPlayFocus(uint32_t sceneId);
        void beginStructureVisibilityUpdate(uint32_t sceneId);
        void setStructureSelectionParent(uint32_t sceneId, Entity entity, Entity selectionParent);

        // Frames entities of the scene itself in its Scene view: the editor camera moves
        // to show them whole (3D: around them, 2D: centered and zoomed to fit)
        void focusOnEntities(SceneProject* sceneProject, const std::vector<Entity>& entities);
        // Frame Selected (F) in the Scene view of sceneId: the GameObjects selected in it
        // or in one of its layer scenes (inline child scenes, a running game's layers)
        void frameSelection(uint32_t sceneId);
        // Frames entities of entitySceneId (the scene itself or one of its layer scenes)
        void frameEntities(uint32_t viewSceneId, uint32_t entitySceneId, const std::vector<Entity>& entities);
        // The selection Frame Selected works on in the Scene view of sceneId
        bool hasFrameableSelection(uint32_t sceneId);
        bool viewThroughCamera(uint32_t sceneId, Entity cameraEntity);
        void stopViewingCamera(uint32_t sceneId);

        int getWidth(uint32_t sceneId) const;
        int getHeight(uint32_t sceneId) const;
        float getOverlayScale(uint32_t sceneId) const;
    };
}
