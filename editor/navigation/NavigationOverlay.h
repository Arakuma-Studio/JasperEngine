// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Scene view's "AI Navigation" overlay (spec S19 R33, R34, D8): the navmeshes in their area
// colours (translucent triangles, edge lines; the tiles intersecting the view frustum, at most
// 64k triangles), tile bounds, the detail (height) mesh, NavGrid cells, agent paths and
// avoidance, links, obstacles, and the selected components' gizmos (surface and modifier
// volumes, link start/end, obstacle shapes, the agent's cylinder). Editor-only objects in the
// scene they annotate (the Scene view draws them, never a game or camera image); a shipped
// player has none of this.

#include "Scene.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "navigation/NavDiagnostics.h"

#include "yaml-cpp/yaml.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace jasperengine {
    class Camera;
    class LineRenderer;
    class Mesh;
}

namespace jasperengine::nav {
    struct NavDebugGeometry;
}

namespace jasperengine::editor {

    struct SceneProject;

    // Saved with the workspace (per scene, with the Scene view's display settings)
    struct NavOverlaySettings {
        enum class Paths { None, Selected, All };
        // what the navmesh's triangles are tinted by (S42 R27)
        enum class ColorBy { Areas, Islands, Tiles };

        bool enabled = true;
        bool showNavMesh = true;
        bool showTileBounds = false;
        bool showHeightMesh = false;
        bool showGrid = true;
        Paths showPaths = Paths::Selected;
        bool showAvoidance = false;
        bool showLinks = true;
        bool showObstacles = true;
        // ---- S55 areas: runtime modifier volumes always, script volumes in play (R41)
        bool showAreaOverrides = true;
        // ---- S54 diagnostics (R27, R38: written only when not default)
        ColorBy colorBy = ColorBy::Areas;
        // collected triangles steeper than the agent type's Max Slope
        bool showSteepSlopes = false;
        // "12.4 ms · 210 polys · 18.5 KB" at every visible tile
        bool showTileLabels = false;

        bool operator==(const NavOverlaySettings& other) const {
            return enabled == other.enabled && showNavMesh == other.showNavMesh && showTileBounds == other.showTileBounds &&
                   showHeightMesh == other.showHeightMesh && showGrid == other.showGrid && showPaths == other.showPaths &&
                   showAvoidance == other.showAvoidance && showLinks == other.showLinks && showObstacles == other.showObstacles &&
                   showAreaOverrides == other.showAreaOverrides && colorBy == other.colorBy && showSteepSlopes == other.showSteepSlopes &&
                   showTileLabels == other.showTileLabels;
        }
        bool operator!=(const NavOverlaySettings& other) const { return !(*this == other); }
    };

    // only what differs from the defaults ("navigation" under the scene's view settings)
    YAML::Node encodeNavOverlaySettings(const NavOverlaySettings& settings);
    void decodeNavOverlaySettings(const YAML::Node& node, NavOverlaySettings& settings);

    // One tile's label of the last update (Show Tile Labels): its last build in this session
    struct NavTileLabel {
        Entity surface = NULL_ENTITY;
        int x = 0, y = 0;
        // "12.4 ms · 210 polys · 18.5 KB" ("– ms" when not built since the navmesh loaded)
        std::string text;
        // the tile's centre, at the bottom of its bounds
        Vector3 world;
        // its inputs changed since the bake (outlined orange)
        bool dirty = false;
    };

    class NavigationOverlay {
    public:
        static constexpr size_t MAX_TRIANGLES = 65536;
        // Show Tile Labels: at most this many per update (the tiles in the view first by entity)
        static constexpr size_t MAX_TILE_LABELS = 256;

        explicit NavigationOverlay(Scene* scene);
        ~NavigationOverlay();

        void update(SceneProject& sceneProject, Camera* camera, const std::set<Entity>& selected, const NavOverlaySettings& settings);
        void hide();

        // what the last update drew (tests, visualize_navigation)
        size_t getTriangleCount() const { return drawnTriangles; }
        size_t getLineCount() const { return drawnLines; }
        // the last update's visible tile labels (Show Tile Labels; the Scene view draws them as
        // ImGui text, visualize_navigation lists them)
        const std::vector<NavTileLabel>& getTileLabels() const { return tileLabels; }
        // Show Steep Slopes' triangles of the last update
        size_t getSlopeTriangleCount() const { return drawnSlopeTriangles; }
        // the Mesh and LineRenderer objects it holds (tests: options off create none)
        size_t getObjectCount() const;
        // Color By Islands: the island of the query's start the last update kept coloured (the
        // others grey), -1: none
        int getStartIsland(Entity surface) const {
            auto it = surfaces.find(surface);
            return it != surfaces.end() ? it->second.startIsland : -1;
        }

        // a navmesh triangle's tint by Color By: its area, its island (grey when startIsland >= 0
        // and it is another island, or it has none) or its tile
        static std::vector<Vector4> triangleColors(const nav::NavDebugGeometry& geometry, NavOverlaySettings::ColorBy colorBy,
                                                   int startIsland);
        // "12.4 ms · 210 polys · 18.5 KB" ("– ms" when not built since the navmesh loaded)
        static std::string tileLabelText(const nav::NavTileReport& report);

        // ---- S55 areas: each area override box the last update drew, and its colour (tests)
        struct AreaOverrideDraw {
            Entity modifier = NULL_ENTITY;
            uint32_t handle = 0;
            Vector4 color;
            size_t lines = 0;
        };
        const std::vector<AreaOverrideDraw>& getAreaOverrideDraws() const { return areaOverrideDraws; }

    private:
        struct SurfaceDraw {
            std::vector<Mesh*> meshes;
            LineRenderer* edges = nullptr;
            uint64_t revision = 0;
            uint64_t settingsVersion = 0;
            std::vector<uint32_t> visibleKey;
            bool detail = false;
            size_t triangles = 0;
            NavOverlaySettings::ColorBy colorBy = NavOverlaySettings::ColorBy::Areas;
            // Color By Islands: the island of the query's start (the others grey), -1: none
            int startIsland = -1;
            // tile bounds drawn, and the dirty tiles outlined (x, y)
            bool tileBounds = false;
            bool dirtyOutlines = false;
            std::vector<std::pair<int, int>> dirty;
            // Show Steep Slopes: the collected triangles steeper than the agent's Max Slope, and
            // the inputs they were made from (their tile hashes and configuration)
            std::vector<Mesh*> slopes;
            uint64_t slopeKey = 0;
            size_t slopeTriangles = 0;
            // Show Tile Labels: the tiles' last builds, read again when the navmesh changed
            uint64_t reportsRevision = 0;
            std::vector<nav::NavTileReport> reports;
        };
        struct GridDraw {
            std::vector<Mesh*> meshes;
            LineRenderer* lines = nullptr;
            uint64_t revision = 0;
            uint64_t settingsVersion = 0;
            // the GameObject's world position and rotation it was drawn at
            float pose[7] = {0, 0, 0, 0, 0, 0, 0};
        };

        Scene* scene;
        std::map<Entity, SurfaceDraw> surfaces;
        std::map<Entity, GridDraw> grids;
        // paths, links, obstacles and gizmos: rebuilt each update
        LineRenderer* lines = nullptr;
        size_t drawnTriangles = 0;
        size_t drawnLines = 0;
        std::vector<AreaOverrideDraw> areaOverrideDraws;
        size_t drawnSlopeTriangles = 0;
        std::vector<NavTileLabel> tileLabels;

        Mesh* newMesh();
        LineRenderer* newLines(unsigned int maxLines);
        void fillMeshes(std::vector<Mesh*>& meshes, const std::vector<float>& triangles, const std::vector<Vector4>& colors,
                        float lift);
        void updateSurface(Entity entity, SurfaceDraw& draw, SceneProject& sceneProject, Camera* camera, const NavOverlaySettings& settings,
                           size_t& triangleBudget);
        void updateSlopes(Entity entity, SurfaceDraw& draw, SceneProject& sceneProject, const NavOverlaySettings& settings, size_t& triangleBudget);
        void updateGrid(Entity entity, GridDraw& draw, SceneProject& sceneProject, const NavOverlaySettings& settings);
        void drawGizmos(SceneProject& sceneProject, const std::set<Entity>& selected, const NavOverlaySettings& settings);
        static void destroy(SurfaceDraw& draw);
        static void destroy(GridDraw& draw);
    };

}
