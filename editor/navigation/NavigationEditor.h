// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The editor's side of navigation bakes and assets (docs/navigation.md):
// - bakes run on the Background lane while the editor keeps drawing; update() (every frame)
//   lands them and writes the .navmesh through S03 atomic writes, at the surface's asset or at
//   the default location (assets/<scene dir>/<SceneName>/NavMesh-<Entity>.navmesh), with an
//   S04 GUID, and points the surface's navMeshData at it;
// - a scene save first writes the .navgrid of every grid whose cells changed;
// - the Inspector, the Navigation window and the MCP tools start, cancel and clear bakes here.

#include "Scene.h"
#include "subsystem/NavigationSystem.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;
    struct SceneProject;

    class NavigationEditor {
    public:
        using BakeCallback = std::function<void(const NavBakeResult& result)>;

        static NavigationEditor& get();

        // The scene's navigation system, set up for the editor (tile records kept, Background
        // lane); null without a scene
        static NavigationSystem* systemOf(SceneProject& sceneProject);

        // Starts a bake (Not baked / Dirty: the changed tiles; FULL: every tile; CLEAR: none,
        // the asset removed). onDone runs on the main thread once it ended (saved or not).
        bool bake(Project* project, uint32_t sceneId, Entity surface, NavBakeMode mode, std::string& error,
                  BakeCallback onDone = nullptr);
        // Every surface of the scene (Bake All)
        int bakeAll(Project* project, uint32_t sceneId, NavBakeMode mode, std::string& error);
        bool cancel(Project* project, uint32_t sceneId, Entity surface);
        // Removes the navmesh and its asset (Unity's Clear)
        bool clear(Project* project, uint32_t sceneId, Entity surface, std::string& error);

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

        // ---- the overlay's query path (visualize_navigation path), world points
        void setQueryPath(uint32_t sceneId, std::vector<Vector3> points) { queryPaths[sceneId] = std::move(points); }
        const std::vector<Vector3>& getQueryPath(uint32_t sceneId) const {
            static const std::vector<Vector3> none;
            auto it = queryPaths.find(sceneId);
            return it != queryPaths.end() ? it->second : none;
        }

        // Stale bake (spec R32, R42): tiles whose inputs differ from the baked ones, -1 when
        // not collected; cached per scene revision
        int dirtyTiles(Project* project, uint32_t sceneId, Entity surface);

    private:
        struct Pending {
            uint32_t sceneId = 0;
            Entity surface = NULL_ENTITY;
            NavBakeMode mode = NavBakeMode::DIRTY;
            std::vector<BakeCallback> callbacks;
        };
        std::vector<Pending> pending;

        struct DirtyCache {
            uint32_t sceneId = 0;
            Entity surface = NULL_ENTITY;
            uint64_t revision = 0;
            double checkedAt = 0.0;
            int dirty = -1;
        };
        std::vector<DirtyCache> dirtyCache;

        std::map<uint32_t, std::vector<Vector3>> queryPaths;
        uint32_t brushScene = 0;
        Entity brushGrid = NULL_ENTITY;
        Brush brush;

        void finish(Project* project, Pending& entry, const NavBakeResult& result);
    };

}
