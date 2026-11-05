// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "imgui.h"
#include "Project.h"
#include "object/Camera.h"
#include "sceneview/SceneViewController.h"
#include "window/ParticleEffectOverlay.h"
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>

namespace jasperengine::editor {

    class ImportPrefabCmd;

    class SceneWindow {
    private:
        // the Particle Effect panel and the Edit-mode particle preview it drives
        ParticleEffectOverlay particleOverlay;
        bool sceneScriptMenuPending = false;   // a short right click opens the editor scripts' Scene view menu
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
        // a right click that did not move the camera opens the lattice menu where it was
        std::map<uint32_t, float> rightClickTravel;
        std::map<uint32_t, bool> latticeMenuRequested;
        std::map<uint32_t, ImVec2> latticeMenuPos;
        std::map<uint32_t, bool> focusCanvasOnNextFrame;

        std::map<uint32_t, int> width;
        std::map<uint32_t, int> height;
        std::map<uint32_t, ImVec2> framebufferScale;
        // The view's rect in the editor window's client area (x, y, width, height), last frame
        std::map<uint32_t, ImVec4> viewRect;
        std::map<uint32_t, float> canvasDpiScale;

        std::map<uint32_t, bool> hasNotification;

        // Entity children hidden below a collapsed Hierarchy node select an ancestor
        // when picked in the viewport. Each maps to its nearest collapsed ancestor.
        std::unordered_map<uint32_t, std::unordered_map<Entity, Entity>> structureSelectionParents;

        std::vector<uint32_t> closeSceneQueue;

        // S26: picking flow, camera, bookmarks, panes, measures, Layers (SceneViewController.cpp)
        std::unique_ptr<SceneViewController> viewController;

        // Prefab Mode: the stage the Prefab Mode tab showed last frame (a new one takes focus,
        // leaving it gives the focus back to the scene)
        uint32_t shownPrefabStage = NULL_PROJECT_SCENE;

        // Prefab Mode's bar over its Scene view: breadcrumbs, Save, Auto Save, Context
        // (PrefabStageBar.cpp)
        void drawPrefabStageBar(SceneProject& sceneProject);
        // a scene, prefab or project.yaml another program changed on disk: Reload / Keep Mine (S05)
        void drawExternalChangeBar(SceneProject& sceneProject);
        std::string getPrefabStageWindowTitle(const SceneProject& sceneProject) const;
        // Leaves Prefab Mode down to `depth` stages, asking to save (deferred to the next frame)
        void requestExitPrefabStages(size_t depth);

        void handleCloseScene(uint32_t sceneId);
        void closeSceneInternal(uint32_t sceneId);
        void sceneEventHandler(SceneProject* sceneProject);
        void endLook(uint32_t sceneId);
        void handleResourceFileDragDrop(SceneProject* sceneProject);
        // R16: what a drop at (x, y) lands on (the preview's own entities ignored)
        SceneSurfaceHit getDropSurface(SceneProject* sceneProject, float x, float y, const std::unordered_set<Entity>& ignore);
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
        Vector3 getPrefabDropPosition(SceneProject* sceneProject, float x, float y, bool& keepZ, SceneSurfaceHit* surface = nullptr);
        bool handleViewportGizmoClick(SceneProject* sceneProject, float canvasX, float canvasY, int canvasWidth);
        // the LOD Level legend and the Stats box over the Scene view (spec S54 R11, R12)
        void drawDrawModeOverlays(SceneProject& sceneProject, ImVec2 imageMin, ImVec2 imageMax);
        // the Paint Dye brush's circle (splatRadius) over the image, under the pointer
        void drawFluidBrushCursor(SceneProject* sceneProject);
        // UI tests (UIProbe): the view's rectangle and the selected fluid's Sprite on it
        void probeSceneView(SceneProject* sceneProject);
        // the pointer was over a Scene view's canvas this frame (the SceneView shortcut context)
        bool viewHovered = false;
        void focusSceneWindow(const SceneProject& sceneProject) const;
        void requestOpenParentScene(uint32_t childSceneId, uint32_t parentSceneId);
        void drawParentSceneButton(const SceneProject& sceneProject);
        void drawGizmosMenu(SceneProject& sceneProject);
        // S41 R21: a 2D scene whose main camera has a Pixel Perfect Camera, with Pixel Zoom on
        bool pixelZoomActive(const SceneProject* sceneProject) const;
        // the toolbar's Pixel Zoom toggle, its readout and list, 1x and Game
        void drawPixelZoomToolbar(SceneProject& sceneProject);
    public:
        // the Scene view at k view pixels per base pixel, anchored at the view's centre (the
        // 1x and Game buttons, sceneView.pixelZoom1x / pixelZoomGame)
        void setPixelZoom(SceneProject& sceneProject, float scale);
        // the Game view's current k (its Pixel Perfect Camera's layout), 1 without one
        float gamePixelZoom(SceneProject& sceneProject) const;
    private:
        // the Pixel Grid overlay (from 6x): 1-pixel lines on art-pixel edges
        void drawPixelGrid(SceneProject& sceneProject, const ImVec2& canvasMin, const ImVec2& canvasAvail);
        // R22: the main camera's Pixel Perfect Camera frames: its reference resolution, and in
        // Integer (Expand) the frame the Game view's size shows (dashed)
        void drawPixelPerfectFrames(SceneProject& sceneProject, const ImVec2& canvasMin);
        // the right-click menu of the selected lattice (LatticeSceneTools draws its items)
        void drawLatticeContextMenu(SceneProject& sceneProject);
        // the camera moves to show bounds whole (focusOnEntities' framing)
        void focusOnBounds(SceneProject* sceneProject, const AABB& bounds);
        std::string getWindowTitle(const SceneProject& sceneProject) const;
        Entity resolveStructureSelection(uint32_t viewportSceneId, uint32_t sceneId, Entity entity) const;
        Entity findSelectableObjectByRay(uint32_t sceneId, float x, float y, uint32_t* outSceneId = nullptr);
        bool selectObjectByRay(uint32_t sceneId, float x, float y, bool shiftPressed, bool ctrlPressed = false);
        ImVec2 canvasFramebufferScale(uint32_t sceneId) const;
        ImVec2 canvasEngineScale(uint32_t sceneId) const;
        float canvasDpi(uint32_t sceneId) const;
        void toEngineCanvas(uint32_t sceneId, float& x, float& y) const;


    public:
        // The ImGui id of Prefab Mode's tab: one tab, whichever stage it shows
        static constexpr const char* PREFAB_STAGE_WINDOW_ID = "###PrefabMode";

        SceneWindow(Project* project);
        ~SceneWindow();

        SceneViewController& getViewController() { return *viewController; }
        // after the frame's draw of a scene: its inactive Split/Quad panes that need it
        void renderInactivePanes(SceneProject& sceneProject);

        void show();
        bool isFocused() const;
        ParticleEffectOverlay& getParticleOverlay() { return particleOverlay; }
        // a camera drag (look, orbit, pan) is running in a Scene view
        bool isNavigatingCamera() const;
        // A left click (no drag, no pose handle) at engine canvas (x, y) of a posing Scene view:
        // a click off the posed character leaves posing (PoseViewport::clicked with the selection
        // pick and the raw hit). True when posing ended. Also the MCP test hook's click.
        // `picked` / `rawHit` (optional) receive the two picks.
        bool poseClick(uint32_t sceneId, float x, float y, Entity* picked = nullptr, Entity* rawHit = nullptr);
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
        // Frame All (Alt+F, Home): what the view shows and can pick, in any scene type (S26 R18)
        void frameAll(uint32_t sceneId);
        // The numpad views (3D): the camera looks along direction (Front: +Z)
        void viewFrom(uint32_t sceneId, const Vector3& direction);
        // A Scene view's canvas was under the pointer last frame
        bool isHovered() const;
        bool viewThroughCamera(uint32_t sceneId, Entity cameraEntity);
        void stopViewingCamera(uint32_t sceneId);

        // Where the scene's view sits in the editor window (client-area pixels, as the OS
        // pointer sees them): false before it was drawn
        bool getViewRect(uint32_t sceneId, float& x, float& y, float& w, float& h) const;
        int getWidth(uint32_t sceneId) const;
        int getHeight(uint32_t sceneId) const;
        float getOverlayScale(uint32_t sceneId) const;
    };
}
