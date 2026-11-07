// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Scene view's behaviour beyond drawing the scene (docs/specs/S26-scene-view.md R13-R25,
// R32-R34): the click flow over ScenePicker's hits (root, drill, same-spot cycling, Ctrl+wheel,
// the "name n/m" pill, clicks waiting on a building BVH), the editor camera (Persp/Iso, axis
// views, Frame All, eased moves, the Scene Camera settings, the fly speed), camera bookmarks,
// the Single/Split/Quad layouts and their panes, measures, rulers, the Layers filter and the
// per-GameObject hide and pick lock, and the surface a drop lands on.
//
// SceneWindow owns one and calls it from its toolbar, input and canvas code; the MCP tools
// (ViewTools, SceneViewTools) drive the same functions, so an agent sees what a user does.
// The state that outlives a session is the scene's SceneViewState (workspace.yaml); what is
// here besides is per session (tweens, the last click, pane cameras and framebuffers).

#include "Project.h"
#include "picking/ScenePicker.h"
#include "sceneview/SceneViewMath.h"
#include "sceneview/SceneViewState.h"
#include "texture/Framebuffer.h"
#include "texture/Texture.h"

#include "imgui.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace jasperengine {
    class Camera;
}

namespace jasperengine::editor {

    // the bindings of a Scene view command of the catalog as tooltips show them ("M", "Alt+H";
    // shortcuts/DefaultCommands.cpp), "" when unbound
    std::string sceneViewShortcut(const char* id);

    struct SceneClickInput {
        float x = 0.0f;               // engine pixels of the view (the active pane)
        float y = 0.0f;
        bool shift = false;
        bool ctrl = false;            // Cmd on macOS
        // the click waits for (or builds) a BVH still building instead of deferring itself
        bool waitForBvh = false;
        // the selection is changed (else only decided: pick_at's "none")
        bool apply = true;
        std::optional<bool> exactPick;  // the toolbar's Exact Pick when unset
    };

    struct SceneClickOutcome {
        ScenePickResult result;
        ScenePickClick click;
        bool pending = false;         // deferred until the BVH of the hit mesh is built
        bool applied = false;
    };

    // where a drop or a measure point lands (R16)
    struct SceneSurfaceHit {
        enum class Source { NONE, SURFACE, PLANE, ALONG_RAY };
        Source source = Source::NONE;
        bool pending = false;         // the surface is a mesh whose BVH is still building
        Vector3 point;
        Vector3 normal = Vector3(0, 1, 0);
        uint32_t sceneId = 0;
        Entity entity = NULL_ENTITY;
        PickHit hit;
    };

    enum class MeasureSnap { NONE, SURFACE, VERTEX, GRID };

    class SceneViewController {
    public:
        static constexpr float PILL_SECONDS = 1.5f;
        static constexpr double PENDING_STATUS_SECONDS = 0.1;
        static constexpr float VERTEX_SNAP_PIXELS = 12.0f;
        static constexpr float DROP_FALLBACK_DISTANCE = 10.0f;
        static constexpr int BOOKMARK_PREVIEW_WIDTH = 160;
        static constexpr int BOOKMARK_PREVIEW_HEIGHT = 90;
        static constexpr float MIN_ORTHO_SIZE = 0.001f;
        static constexpr float MAX_ORTHO_SIZE = 100000.0f;
        // the engine view ids of the panes (SCENE_VIEW_ID and GAME_VIEW_ID are 1 and 2)
        static constexpr uint32_t PANE_VIEW_ID_BASE = 16;

        SceneViewController(Project* project);
        ~SceneViewController();

        // the outermost collapsed Hierarchy row holding an entity (SceneWindow's structure
        // selection), for pick roots
        std::function<Entity(uint32_t viewSceneId, uint32_t sceneId, Entity entity)> collapsedRow;
        // the size and scale of a scene's view (SceneWindow), engine pixels
        std::function<void(uint32_t sceneId, int& width, int& height)> viewSize;

        // ---- lifecycle
        void forgetScene(uint32_t sceneId);
        void forgetAll();
        // Once per editor frame for each shown scene, before its input: the view state onto
        // the camera and scene the first time, the pane a hover chose, tweens, the projection's
        // clips, the automatic axis ortho, a deferred click
        void beginFrame(SceneProject& sceneProject);
        // the view state onto the camera and scene, once per Scene view (MCP calls it too)
        void ensureApplied(SceneProject& sceneProject);

        // ---- picking (R11-R15, R33)
        ScenePickResult pickAt(uint32_t sceneId, float x, float y, bool exactPick, bool waitForBvh);
        SceneClickOutcome click(uint32_t sceneId, const SceneClickInput& input);
        // Ctrl/Cmd+wheel: the next (step 1) or previous candidate under the cursor is selected,
        // the next plain click there confirms it
        bool cycleCandidates(uint32_t sceneId, float x, float y, int step);
        bool hasPendingClick(uint32_t sceneId) const;
        // the next click is a first one (no same-spot cycling, no Ctrl+wheel step to confirm)
        void resetClickHistory(uint32_t sceneId);
        // the busy cursor and status text of a waiting click
        std::string pendingStatus(uint32_t sceneId) const;

        // ---- camera (R17, R18, R21)
        SceneCameraPose currentPose(const SceneProject& sceneProject) const;
        // a pose at once, or eased (Camera Easing) when animate
        void moveTo(SceneProject& sceneProject, const SceneCameraPose& pose, bool animate);
        // an eased move lands now (an agent's camera changes do not animate)
        void finishTween(SceneProject& sceneProject);
        // moves the camera to look at target from position (frame selection): eased, the
        // projection and orthographic size following the distance
        void lookFrom(SceneProject& sceneProject, const Vector3& position, const Vector3& target, const Vector3& up);
        bool isOrthographic(const SceneProject& sceneProject) const;
        // The view state's projection on the camera of a 3D scene's Scene view, for a view of that
        // size: also the scene's Default Camera (a scene with no Camera), which its code generates
        static void projectCamera(SceneProject& sceneProject, int width, int height, bool force);
        void setProjection(SceneProject& sceneProject, bool orthographic);
        void toggleProjection(SceneProject& sceneProject);
        void setAxisView(SceneProject& sceneProject, SceneViewAxis axis);
        // the view direction's axis (FREE when it looks along none)
        SceneViewAxis viewAxis(const SceneProject& sceneProject) const;
        // the active pane is an axis pane: pans and zooms, never orbits
        bool isAxisLocked(const SceneProject& sceneProject) const;
        bool frameAll(SceneProject& sceneProject);
        void setOrthoSize(SceneProject& sceneProject, float orthoSize);
        // the wheel over an orthographic view: its half height scaled
        void orthoZoom(SceneProject& sceneProject, float wheel);
        // what camera moves scale with: the distance to the target, or for an orthographic
        // view the distance whose perspective framing equals it
        float navigationDistance(const SceneProject& sceneProject) const;
        void applyCameraSettings(SceneProject& sceneProject, const SceneCameraSettings& settings);
        // the fly speed this frame (Camera Speed, Shift, Acceleration); flying: a WASDQE key is
        // held (the acceleration restarts when none is)
        float flySpeed(SceneProject& sceneProject, bool flying, bool shift, float dt);
        void stepFlySpeed(SceneProject& sceneProject, float wheel);

        // ---- bookmarks (R19)
        bool storeBookmark(SceneProject& sceneProject, int slot, std::string& error);
        bool recallBookmark(SceneProject& sceneProject, int slot, std::string& error);
        bool deleteBookmark(SceneProject& sceneProject, int slot, std::string& error);
        bool renameBookmark(SceneProject& sceneProject, int slot, const std::string& name, std::string& error);
        std::filesystem::path bookmarkPreviewPath(const SceneProject& sceneProject, int slot) const;
        // a slot's display name: its own, "Home" or its number
        static std::string bookmarkName(const SceneViewState& state, int slot);

        // ---- layouts and panes (R20, R31)
        void setLayout(SceneProject& sceneProject, SceneViewLayout layout);
        void setPaneAxis(SceneProject& sceneProject, int pane, SceneViewAxis axis);
        void setPaneShading(SceneProject& sceneProject, int pane, PaneShading shading);
        void toggleMaximized(SceneProject& sceneProject, int pane);
        // the pane under the pointer becomes the active one at the next frame
        void requestActivePane(SceneProject& sceneProject, int pane);
        void setActivePane(SceneProject& sceneProject, int pane);
        // How many pane slots the canvas draws this frame (1 when Single or maximized), and
        // for each: whether it is the active one (the caller then draws the Scene view as
        // before inside it, and endPane closes it) or another (drawn here)
        int paneSlotCount(const SceneProject& sceneProject) const;
        bool beginPane(SceneProject& sceneProject, int slot);
        void endPane(SceneProject& sceneProject, int slot);
        // after the frame's draw of the scene: the inactive panes that need it redraw
        void renderInactivePanes(SceneProject& sceneProject);
        // the pane of a slot, the engine size of a pane's view
        int paneOfSlot(const SceneProject& sceneProject, int slot) const;
        bool paneViewSize(const SceneProject& sceneProject, int pane, int& width, int& height) const;
        uint64_t paneDrawCount(const SceneProject& sceneProject, int pane) const;
        // the framebuffer an inactive pane draws into (null for the active one: the Scene view's)
        Framebuffer* paneFramebuffer(SceneProject& sceneProject, int pane);
        // a pane's camera pose (the active pane's is the editor camera's)
        SceneCameraPose panePose(const SceneProject& sceneProject, int pane) const;
        // a ray through (x, y) of a pane's view (engine pixels), for picking in it
        bool paneRay(const SceneProject& sceneProject, int pane, float x, float y, Ray& ray) const;

        // ---- measures and rulers (R24, R25)
        uint32_t addMeasure(SceneProject& sceneProject, const Vector3& from, const Vector3& to);
        bool deleteMeasure(SceneProject& sceneProject, uint32_t id);
        void clearMeasures(SceneProject& sceneProject);
        bool isMeasuring(uint32_t sceneId) const;
        bool hasSelectedMeasure(uint32_t sceneId) const;
        // the selected measure's id, 0 when none
        uint32_t selectedMeasure(uint32_t sceneId) const;
        void setMeasuring(SceneProject& sceneProject, bool measuring);
        // a measure end at (x, y): the surface (R16 order), the hit triangle's nearest corner
        // within 12 px (vertex), the grid (snap-to-grid)
        bool measurePoint(SceneProject& sceneProject, float x, float y, MeasureSnap snap, Vector3& point);
        static bool rulersApply(const SceneProject& sceneProject, bool orthographicAxisView);

        // ---- visibility and pick lock (R22, R23)
        void setLayerFilter(SceneProject& sceneProject, uint32_t visible, uint32_t pickable);
        void hideSelection(SceneProject& sceneProject);
        void showAll(SceneProject& sceneProject);
        void togglePickLockSelection(SceneProject& sceneProject);

        // ---- placement (R16)
        // what a drop at (x, y) of the view lands on, ignoring `ignore` (the dragged preview)
        SceneSurfaceHit surfaceAt(SceneProject& sceneProject, float x, float y, std::span<const Entity> ignore, bool waitForBvh);
        // ---- ImGui (SceneViewToolbar.cpp)
        // the R32 items after the view's existing ones, before post FX and Gizmos
        void drawToolbar(SceneProject& sceneProject, float rightLimit);
        // over the active pane's view: the pill, rulers, measures, the "N hidden" banner
        void drawOverlays(SceneProject& sceneProject, ImVec2 viewMin, ImVec2 viewSize, float engineScaleX, float engineScaleY);
        // the shortcuts and tools that take the pointer (measure); true when the event is used
        bool handleInput(SceneProject& sceneProject, float x, float y, bool hovered, bool focused);
        // the Scene Camera rows (the camera popup; the Gizmos popup links here)
        void drawCameraSettings(SceneProject& sceneProject);
        void openCameraSettings(uint32_t sceneId);
        // the status bar's short message on the left, for `seconds`
        void notifyStatus(const std::string& text, double seconds = 3.0);

        // ---- resource jasper://scene-view (MCP)
        SceneLayerFilter getLayerFilter(const SceneProject& sceneProject) const { return sceneProject.viewState.layerFilter; }

    private:
        struct Tween {
            bool active = false;
            SceneCameraPose from;
            SceneCameraPose to;
            double start = 0.0;
        };

        struct PendingClick {
            bool active = false;
            SceneClickInput input;
            double started = 0.0;
            std::string meshName;
            bool announced = false;
        };

        struct Pane {
            Camera* camera = nullptr;          // the pane's own (not the active one's)
            Framebuffer framebuffer;
            int width = 0;                     // engine pixels of its view
            int height = 0;
            bool dirty = true;
            uint64_t draws = 0;
            ImVec2 imageMin;                   // where its view was drawn (screen)
            ImVec2 imageSize;
        };

        struct Runtime {
            bool applied = false;
            const void* appliedRender = nullptr;
            // the click before (same-spot cycling) and a Ctrl+wheel step to confirm
            float lastClickX = -1.0e9f;
            float lastClickY = -1.0e9f;
            bool depthAnchor = false;
            std::vector<ScenePickEntry> wheelCandidates;
            int wheelIndex = -1;
            // the "name n/m" pill
            std::string pillText;
            ImVec2 pillPosition;               // view pixels (logical)
            double pillUntil = 0.0;
            PendingClick pending;
            Tween tween;
            double flyStart = -1.0;
            // the projection the camera was last set to (applied on change)
            bool projectedOrtho = false;
            float projectedSize = 0.0f;
            int projectedWidth = 0;
            int projectedHeight = 0;
            // measure tool
            bool measuring = false;
            bool measureDragging = false;
            Vector3 measureFrom;
            Vector3 measureTo;
            bool measureValid = false;
            uint32_t selectedMeasure = 0;
            // panes
            std::array<Pane, SceneViewState::MAX_PANES> panes;
            int requestedPane = -1;
            SceneCameraPose renderedPose;      // the editor camera when the panes last drew
            bool renderedPoseValid = false;
            SceneViewLayout renderedLayout = SceneViewLayout::SINGLE;
            uint64_t renderedSelection = 0;
            // bookmark previews loaded for the hover
            std::array<std::unique_ptr<Texture>, SceneViewState::BOOKMARK_SLOTS> previews;
            std::array<uint32_t, SceneViewState::BOOKMARK_SLOTS> previewRevision{};
            std::array<uint32_t, SceneViewState::BOOKMARK_SLOTS> loadedRevision{};
            bool cameraSettingsOpen = false;
            int renameSlot = -1;
            char renameBuffer[64] = {};
        };

        Project* project;
        std::map<uint32_t, Runtime> runtimes;
        uint32_t cameraPopupScene = NULL_PROJECT_SCENE;

        Runtime& runtime(uint32_t sceneId);
        const Runtime* findRuntime(uint32_t sceneId) const;
        void applyViewState(SceneProject& sceneProject);
        // the camera's projection from the view state (ortho half height, clips, fov)
        void applyProjection(SceneProject& sceneProject, Runtime& rt, bool force);
        void applyPoseNow(SceneProject& sceneProject, const SceneCameraPose& pose);
        void updateTween(SceneProject& sceneProject, Runtime& rt);
        void updateAxisOrtho(SceneProject& sceneProject);
        void processPendingClick(SceneProject& sceneProject, Runtime& rt);
        SceneClickOutcome runClick(uint32_t sceneId, const SceneClickInput& input, Runtime& rt);
        void applyClick(uint32_t sceneId, const ScenePickClick& click, const ScenePickResult& result);
        void showPill(Runtime& rt, const ScenePickClick& click, const ScenePickResult& result, float x, float y);
        ScenePickContext clickContext(uint32_t sceneId, const SceneClickInput& input, const Runtime& rt) const;
        ScenePickRequest pickRequest(uint32_t sceneId, float x, float y, bool exactPick, bool waitForBvh) const;
        void ensurePaneCamera(SceneProject& sceneProject, int pane);
        void writePaneCamera(SceneProject& sceneProject, int pane);
        void destroyPanes(Runtime& rt);
        void markPanesDirty(Runtime& rt);
        bool panesNeedRedraw(SceneProject& sceneProject, Runtime& rt);
        void drawInactivePane(SceneProject& sceneProject, int pane);
        bool drawPaneHeader(SceneProject& sceneProject, int pane);
        Texture* bookmarkPreview(SceneProject& sceneProject, int slot);
        void saveWorkspace();
        std::string sceneKey(const SceneProject& sceneProject) const;
    };

}
