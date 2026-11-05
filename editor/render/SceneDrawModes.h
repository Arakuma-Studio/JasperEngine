// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Scene view's Draw Mode (spec S54 R11) and the detail level lines of the Stats overlays
// (R12): editor only, nothing of it is in a game. Shaded, LOD Level and Overdraw are drawn by the
// renderer (RenderSystem::ViewDrawMode); the wires of Wireframe and Shaded Wireframe are an
// editor line overlay (SceneWireframe) over the visible meshes, built only in those modes.

#include "ecs/Entity.h"
#include "math/Vector3.h"
#include "render/ViewDrawMode.h"

#include "imgui.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace jasperengine{
    class Scene;
    class LineRenderer;
    struct CameraComponent;
    struct MeshComponent;
    struct LodFrameStats;
}

namespace jasperengine::editor{

    constexpr int SCENE_DRAW_MODE_COUNT = 5;

    // "shaded", "wireframe", "shaded_wireframe", "lod_level", "overdraw" (saved, MCP)
    const char* drawModeKey(ViewDrawMode mode);
    bool drawModeFromKey(std::string_view key, ViewDrawMode& mode);
    // "Shaded", "Wireframe", "Shaded Wireframe", "LOD Level", "Overdraw" (the toolbar)
    const char* drawModeLabel(ViewDrawMode mode);
    bool drawModeHasWires(ViewDrawMode mode);

    // The wires of the meshes a camera sees, each at the level its view draws: an edge list per
    // submesh and level, made the first time it is drawn and kept while its geometry is the same
    class SceneWireframe{
    public:
        // the most edges drawn (the rest of a dense scene is left out, nearest meshes first)
        static constexpr size_t MAX_EDGES = 250000;

        // lines gets the wires of scene's visible meshes through camera; levelOf gives the level
        // a mesh draws in the view
        void update(Scene* scene, LineRenderer* lines, CameraComponent& camera, const Vector3& eye, const std::function<unsigned int(Entity)>& levelOf);
        // drops every edge list (leaving the mode)
        void release();
        size_t getEdgeCount() const { return edgesDrawn; }
        bool isTruncated() const { return truncated; }

    private:
        struct Edges{
            uint64_t lodKey = 0;
            std::vector<Vector3> positions;
            std::vector<std::pair<uint32_t, uint32_t>> edges;
        };
        // by entity, submesh and level
        std::map<std::tuple<Entity, unsigned int, unsigned int>, Edges> cache;
        size_t edgesDrawn = 0;
        bool truncated = false;

        const Edges* edgesOf(MeshComponent& mesh, Entity entity, unsigned int submesh, unsigned int level);
    };

    // the LOD Level legend in a view's corner: LOD 0..3 in their colours
    void drawLodLegend(ImDrawList* drawList, ImVec2 imageMin, ImVec2 imageMax);
    // a Stats box in the top right corner of an image
    void drawStatsBox(ImDrawList* drawList, ImVec2 imageMin, ImVec2 imageMax, const std::vector<std::string>& lines);
    // "Verts: ...", "LOD 0: 120, 1: 40, 2: 8, 3: 2", "Fading: 3  Culled by size: 2", "Mesh memory: 86.0 MB"
    // (the LOD lines only while the scene's Mesh LOD is on)
    void appendMeshStatsLines(std::vector<std::string>& lines, uint64_t vertices, const LodFrameStats& lod, bool lodEnabled);

}
