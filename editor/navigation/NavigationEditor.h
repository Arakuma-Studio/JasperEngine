// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The editor's side of navigation bakes and assets (docs/navigation.md):
// - bakes run on the Background lane while the editor keeps drawing; update() (every frame)
//   lands them and writes the .navmesh through S03 atomic writes, at the surface's asset or at
//   the default location (assets/<scene dir>/<SceneName>/NavMesh-<Entity>.navmesh), with an
//   S04 GUID, and points the surface's navMeshData at it;
// - a scene save first writes the .navgrid of every grid whose cells changed;
// - the Inspector, the Navigation window and the MCP tools start, cancel and clear bakes here;
//   every bake that ends neither ok nor cancelled logs one Console error naming its surface
//   (a refused start too: callers do not log), and its result is kept for the session
//   (lastBake: the Inspector's "Bake failed" line, the Surfaces tab, get_navigation_info).

#include "Scene.h"
#include "subsystem/NavigationSystem.h"
#include "math/Ray.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;
    struct SceneProject;
    class NavigationOverlay;

    // What the overlay draws for a query: its path (corners, status, gap) and its ends
    struct NavQueryDraw {
        nav::NavPath path;
        Vector3 from, to;
        bool hasFrom = false, hasTo = false;
    };

    class NavigationEditor {
    public:
        using BakeCallback = std::function<void(const NavBakeResult& result)>;

        static NavigationEditor& get();

        // The scene's navigation system, set up for the editor (tile records kept, Background
        // lane); null without a scene
        static NavigationSystem* systemOf(SceneProject& sceneProject);

        // Starts a bake (Not baked / Dirty: the changed tiles; FULL: every tile; CLEAR: none,
        // the asset removed). onDone runs on the main thread once it ended (saved or not). A
        // refused start is logged and recorded here (onDone does not run for it).
        bool bake(Project* project, uint32_t sceneId, Entity surface, NavBakeMode mode, std::string& error,
                  BakeCallback onDone = nullptr);
        // Every surface of the scene (Bake All)
        int bakeAll(Project* project, uint32_t sceneId, NavBakeMode mode, std::string& error);
        bool cancel(Project* project, uint32_t sceneId, Entity surface);
        // Removes the navmesh and its asset (Unity's Clear); a failure is logged and recorded
        bool clear(Project* project, uint32_t sceneId, Entity surface, std::string& error);

        // The last bake of a surface this session (spec S74 R3): a refused start and an
        // editor-side failure (the save, the scene closed) count. Null when the surface has
        // none (never baked here, or cleared since); entries of closed scenes are dropped.
        struct LastBake {
            bool ok = false;
            bool cancelled = false;
            std::string error;
            std::vector<std::string> warnings;
            int tilesBuilt = 0;
            int polys = 0;
            double ms = 0.0;
        };
        const LastBake* lastBake(uint32_t sceneId, Entity surface) const;

        // Every frame: pumps the bakes, saves the finished ones
        void update(Project* project);
        // a bake is running (the editor keeps drawing frames)
        bool isBusy() const { return !pending.empty(); }
        bool isBaking(uint32_t sceneId, Entity surface) const;

        // Before a scene is encoded: the .navgrid of every grid whose cells changed is written
        // and its gridData set
        void saveSceneAssets(Project* project, SceneProject& sceneProject);

        // assets/<scene dir>/<SceneName>/<prefix>-<EntityName>.<extension> (absolute)
        static std::filesystem::path defaultAssetPath(Project* project, const SceneProject& sceneProject, Entity entity,
                                                      const std::string& prefix, const std::string& extension);
        // Writes bytes to the asset (absolute path) atomically, gives it a GUID and returns its
        // reference (path relative to the assets folder)
        static bool writeAsset(Project* project, const std::filesystem::path& file, const std::vector<uint8_t>& bytes,
                               AssetRef& ref, std::string& error);
        // The surface's navmesh file: its asset, else the default location
        static std::filesystem::path navMeshFileOf(Project* project, const SceneProject& sceneProject, Entity surface);

        // ---- grids (undoable: NavGridCellsCmd)
        // Rebuild from the grid's source, keeping brush-painted cells ("Rebuild Nav Grid")
        bool rebuildGrid(Project* project, uint32_t sceneId, Entity grid, std::string* error = nullptr);
        // Clears the override bits and rebuilds ("Reset Nav Grid Overrides")
        bool resetGridOverrides(Project* project, uint32_t sceneId, Entity grid, std::string* error = nullptr);

        // ---- the nav-grid brush (spec R35; the Scene view routes it like the terrain brush)
        enum class BrushMode { Paint, Erase, Fill };
        enum class BrushShape { Circle, Square };
        struct Brush {
            BrushMode mode = BrushMode::Paint;
            BrushShape shape = BrushShape::Circle;
            int area = nav::AREA_NOT_WALKABLE;
            // diameter in cells
            int size = 1;
        };
        void setBrushGrid(uint32_t sceneId, Entity grid) { brushScene = sceneId; brushGrid = grid; }
        void stopBrush() { brushGrid = NULL_ENTITY; }
        bool isBrushActive(uint32_t sceneId) const { return brushGrid != NULL_ENTITY && brushScene == sceneId; }
        Entity getBrushGrid() const { return brushGrid; }
        Brush& getBrush() { return brush; }
        // the brush holds the Scene view's left button in this scene (a NavGrid to paint)
        bool isBrushPainting(Scene* scene) const;
        // A stroke: press, drag, release (one undo step "Paint Nav Grid"). Shift held while
        // dragging paints a straight line from the press to the cursor (Shift on the press: from
        // the last stroke's end). Fill floods the cell pressed.
        void beginBrushStroke(Project* project, Scene* scene, const Ray& ray, bool shift);
        void paintBrushStroke(Scene* scene, const Ray& ray, bool shift = false);
        void endBrushStroke(Project* project);
        // ---- NavMesh Link handles (spec R34): a press near a selected link's start or end drags
        // it on the link's horizontal plane; the release is one undo step
        bool beginLinkHandleDrag(Project* project, uint32_t sceneId, Scene* scene, const Ray& ray, const std::vector<Entity>& selected);
        void dragLinkHandle(Scene* scene, const Ray& ray);
        void endLinkHandleDrag(Project* project);
        bool isDraggingLinkHandle() const { return linkDrag.active; }

        // the grid cell under a ray (false: it misses the grid's plane)
        static bool cellUnderRay(Scene* scene, Entity grid, const Ray& ray, nav::GridCell& cell);

        // ---- the overlay's query (the Path Tester, visualize_navigation path; S75 R25): start
        // green, end red, a Complete path magenta, a Partial one orange with a red gap to the end
        void setQueryDraw(uint32_t sceneId, NavQueryDraw draw);
        void clearQueryDraw(uint32_t sceneId);
        // null: nothing to draw in the scene
        const NavQueryDraw* getQueryDraw(uint32_t sceneId) const;
        // the scene's AI Navigation overlay (getTileLabels); null before its Scene view drew it
        NavigationOverlay* overlayOf(uint32_t sceneId);

        // Stale bake (spec R32, R42): tiles whose inputs differ from the baked ones, -1 when
        // not collected (inputReport's count)
        int dirtyTiles(Project* project, uint32_t sceneId, Entity surface);
        // The surface's inputs collected against its navmesh (S75 R15, R24, R29): the dirty tiles
        // and their coordinates, the tile grid and voxel size of the Inspector's estimate.
        // Collected at most once a second (it reads the whole scene); null while it bakes or in
        // play. keepInput: with the collected input itself and its sources (the slope overlay,
        // the Path Tester's probe); an input nobody asked for in 2 s is let go (update()).
        const NavigationSystem::InputReport* inputReport(SceneProject& sceneProject, Entity surface, bool keepInput = false);
        // inputReport's input (keepInput), null when it cannot be collected
        std::shared_ptr<const nav::NavBakeInput> keptInput(SceneProject& sceneProject, Entity surface);

    private:
        struct Pending {
            uint32_t sceneId = 0;
            Entity surface = NULL_ENTITY;
            // for the error line once the scene closed under the bake
            std::string surfaceName;
            NavBakeMode mode = NavBakeMode::DIRTY;
            std::vector<BakeCallback> callbacks;
        };
        std::vector<Pending> pending;
        std::map<std::pair<uint32_t, Entity>, LastBake> lastBakes;

        struct InputCache {
            uint32_t sceneId = 0;
            // a scene reloaded under the same id is another scene
            const Scene* scene = nullptr;
            Entity surface = NULL_ENTITY;
            double checkedAt = 0.0;
            // the last time someone asked for the input itself
            double inputAskedAt = 0.0;
            NavigationSystem::InputReport report;
        };
        std::vector<InputCache> inputCache;

        std::map<uint32_t, NavQueryDraw> queryDraws;
        struct LinkDrag {
            bool active = false;
            uint32_t sceneId = 0;
            Entity link = NULL_ENTITY;
            bool end = false;
            Vector3 original;
            Vector3 planePoint;
            Vector3 planeNormal;
        };
        LinkDrag linkDrag;
        uint32_t brushScene = 0;
        Entity brushGrid = NULL_ENTITY;
        Brush brush;
        struct BrushStroke;
        std::shared_ptr<BrushStroke> stroke;
        nav::GridCell lastStrokeEnd;
        bool hasLastStrokeEnd = false;
        void strokeTo(Scene* scene, nav::GridCell cell);

        void finish(Project* project, Pending& entry, const NavBakeResult& result);
        // the one Console line of a result (unless the engine logged it) and its LastBake
        void report(uint32_t sceneId, Entity surface, const std::string& surfaceName, const NavBakeResult& result);
        // a refused start or clear: logged and recorded as a failed bake
        void refuse(uint32_t sceneId, Entity surface, const std::string& surfaceName, const std::string& error);
    };

}
