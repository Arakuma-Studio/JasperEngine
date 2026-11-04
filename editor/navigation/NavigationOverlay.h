// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Scene view's "AI Navigation" overlay (spec S19 R33, R34, D8): the navmeshes in their area
// colours (translucent triangles, edge lines; the tiles intersecting the view frustum, at most
// 64k triangles), tile bounds, the detail (height) mesh, NavGrid cells, agent paths and
// avoidance, links, obstacles, and the selected components' gizmos (surface and modifier
// volumes, link start/end, obstacle shapes, the agent's cylinder); in play, the NavGrid cells
// agents reserved at the current fixed tick (S53 R22). Editor-only objects in the
// scene they annotate (the Scene view draws them, never a game or camera image); a shipped
// player has none of this.

#include "Scene.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "navigation/NavTypes.h"

#include "yaml-cpp/yaml.h"

#include <array>
#include <map>
#include <set>
#include <vector>

namespace jasperengine {
    namespace nav {
        class NavGridMap;
    }
    class Camera;
    class LineRenderer;
    class Mesh;
}

namespace jasperengine::editor {

    struct SceneProject;

    // Saved with the workspace (per scene, with the Scene view's display settings)
    struct NavOverlaySettings {
        enum class Paths { None, Selected, All };

        bool enabled = true;
        bool showNavMesh = true;
        bool showTileBounds = false;
        bool showHeightMesh = false;
        bool showGrid = true;
        Paths showPaths = Paths::Selected;
        bool showAvoidance = false;
        bool showLinks = true;
        bool showObstacles = true;

        bool operator==(const NavOverlaySettings& other) const {
            return enabled == other.enabled && showNavMesh == other.showNavMesh && showTileBounds == other.showTileBounds &&
                   showHeightMesh == other.showHeightMesh && showGrid == other.showGrid && showPaths == other.showPaths &&
                   showAvoidance == other.showAvoidance && showLinks == other.showLinks && showObstacles == other.showObstacles;
        }
        bool operator!=(const NavOverlaySettings& other) const { return !(*this == other); }
    };

    // only what differs from the defaults ("navigation" under the scene's view settings)
    YAML::Node encodeNavOverlaySettings(const NavOverlaySettings& settings);
    void decodeNavOverlaySettings(const YAML::Node& node, NavOverlaySettings& settings);

    class NavigationOverlay {
    public:
        static constexpr size_t MAX_TRIANGLES = 65536;
        // NavGrid cells: their area's colour at this opacity (brush-painted overrides more opaque)
        static constexpr float CELL_ALPHA = 0.4f;
        static constexpr float OVERRIDE_CELL_ALPHA = 0.6f;
        // agent paths; a reserved cell is tinted halfway to it
        static inline const Vector4 AGENT_PATH_COLOR = Vector4(1.0f, 0.85f, 0.2f, 1.0f);

        // One colour per cell of the map (by cell index): the cell's area colour (rgb of
        // areaColors) at CELL_ALPHA or OVERRIDE_CELL_ALPHA, and for the cells in `reserved` (cell
        // indices; repeats and indices outside the map ignored) a 50 % blend of that rgb toward
        // AGENT_PATH_COLOR. Pure: no scene, no GPU.
        static void gridCellColors(const nav::NavGridMap& map, const std::array<Vector4, nav::AREA_COUNT>& areaColors,
                                   const std::vector<int>& reserved, std::vector<Vector4>& out);

        explicit NavigationOverlay(Scene* scene);
        ~NavigationOverlay();

        void update(SceneProject& sceneProject, Camera* camera, const std::set<Entity>& selected, const NavOverlaySettings& settings);
        void hide();

        // what the last update drew (tests, visualize_navigation)
        size_t getTriangleCount() const { return drawnTriangles; }
        size_t getLineCount() const { return drawnLines; }

    private:
        struct SurfaceDraw {
            std::vector<Mesh*> meshes;
            LineRenderer* edges = nullptr;
            uint64_t revision = 0;
            uint64_t settingsVersion = 0;
            std::vector<uint32_t> visibleKey;
            bool detail = false;
            size_t triangles = 0;
        };
        struct GridDraw {
            std::vector<Mesh*> meshes;
            LineRenderer* lines = nullptr;
            uint64_t revision = 0;
            uint64_t settingsVersion = 0;
            // the GameObject's world position and rotation it was drawn at
            float pose[7] = {0, 0, 0, 0, 0, 0, 0};
            // the reserved cells it was drawn with (empty out of play)
            std::vector<int> reserved;
        };

        Scene* scene;
        std::map<Entity, SurfaceDraw> surfaces;
        std::map<Entity, GridDraw> grids;
        // paths, links, obstacles and gizmos: rebuilt each update
        LineRenderer* lines = nullptr;
        size_t drawnTriangles = 0;
        size_t drawnLines = 0;
        // per update, reused: a grid's reserved cells and cell colours
        std::vector<int> reservedScratch;
        std::vector<Vector4> cellColors;

        Mesh* newMesh();
        LineRenderer* newLines(unsigned int maxLines);
        void fillMeshes(std::vector<Mesh*>& meshes, const std::vector<float>& triangles, const std::vector<Vector4>& colors,
                        float lift);
        void updateSurface(Entity entity, SurfaceDraw& draw, SceneProject& sceneProject, Camera* camera, const NavOverlaySettings& settings,
                           size_t& triangleBudget);
        void updateGrid(Entity entity, GridDraw& draw, SceneProject& sceneProject, const NavOverlaySettings& settings);
        void drawGizmos(SceneProject& sceneProject, const std::set<Entity>& selected, const NavOverlaySettings& settings);
        static void destroy(SurfaceDraw& draw);
        static void destroy(GridDraw& draw);
    };

}
