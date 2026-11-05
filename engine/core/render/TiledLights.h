// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TILEDLIGHTS_H
#define TILEDLIGHTS_H

#include "Export.h"
#include "math/Matrix4.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

#include <cstdint>
#include <vector>

namespace jasperengine {

    // Screen-tile light lists (Forward+, docs/specs/S57-forge-lighting.md R1), after The Forge's
    // LightClusters.comp rewritten as a CPU build: each light's bounding sphere projects to a
    // screen rect, whose tiles list the light. The lists and the lights' data share one RGBA32F
    // data texture per view, read with texelFetch by includes/lights.glsl (USE_TILED_LIGHTS):
    //
    //   texels [0, LIGHT_TEXELS)  the view's lights, LIGHT_RECORD_TEXELS each (view-local index k
    //                             at k * LIGHT_RECORD_TEXELS): the per-object block's position_type,
    //                             direction_inner, color_outer and spot_shadow, then x and y the
    //                             light's culling mask (low and high 16 bits), z 1 for a light shaded
    //                             per vertex (LightShading::VERTEX)
    //   then one tile after another, TILE_TEXELS each: x the tile's count, then its view-local
    //                             light indices, four a texel, ascending (scene order)
    //
    // The tiles cover the pass's viewport from its gl_FragCoord origin with an even size, so a
    // 2x2 quad never straddles two tiles (the shaders loop over the tile's count in uniform control
    // flow per quad, as the derivatives in lightShadow need).
    class JASPERENGINE_API TiledLights {
    public:
        static constexpr int MAX_LIGHTS_PER_TILE = 32;
        static constexpr int MAX_LIGHTS_PER_VIEW = 256;
        static constexpr int LIGHT_RECORD_TEXELS = 5;
        static constexpr int TEXTURE_WIDTH = 512;
        static constexpr int LIGHT_TEXELS = 1536;  // MAX_LIGHTS_PER_VIEW * LIGHT_RECORD_TEXELS, rounded up to rows
        static constexpr int TILE_TEXELS = 1 + MAX_LIGHTS_PER_TILE / 4;
        // the grids: Medium (and the default) 16x9, High and above 32x18
        static constexpr int GRID_COLUMNS = 16;
        static constexpr int GRID_ROWS = 9;
        static constexpr int GRID_COLUMNS_HIGH = 32;
        static constexpr int GRID_ROWS_HIGH = 18;

        // One light of the scene as the tiles take it
        struct Light {
            Vector4 record[LIGHT_RECORD_TEXELS];
            Vector3 position;
            float range = 0.0f;   // the sphere's radius; global ignores it
            bool global = false;  // directional, or a point or spot of unlimited range: every tile
            int sceneIndex = 0;   // RenderSystem::sceneLights index, the order the tiles list lights in
        };

        // The grid of a pass: columns x rows tiles of tileWidth x tileHeight pixels (even sizes)
        struct Grid {
            int columns = 0;
            int rows = 0;
            int tileWidth = 0;
            int tileHeight = 0;
            int tileCount() const { return columns * rows; }
            bool operator==(const Grid& o) const{
                return columns == o.columns && rows == o.rows && tileWidth == o.tileWidth && tileHeight == o.tileHeight;
            }
            bool operator!=(const Grid& o) const{ return !(*this == o); }
        };

        // The view the lists are built for
        struct View {
            Matrix4 viewMatrix;
            Matrix4 projectionMatrix;   // the logical one (not Y-flipped for an offscreen GL target)
            bool perspective = true;
            float nearClip = 0.1f;
            float farClip = 1000.0f;
            Vector3 cameraPosition;
            int width = 0;              // the colour pass's viewport, pixels
            int height = 0;
            // gl_FragCoord.y grows towards the image's top (GL drawing into the swapchain, or an
            // unflipped GL target); false where row 0 is the top (GL's flipped offscreen targets,
            // the other backends)
            bool fragYUp = false;
            Grid grid;
        };

        struct Stats {
            int lights = 0;             // listed in the view (at most MAX_LIGHTS_PER_VIEW)
            int droppedFromView = 0;    // past MAX_LIGHTS_PER_VIEW (the farthest)
            int droppedFromTiles = 0;   // tile entries past MAX_LIGHTS_PER_TILE (the farthest)
            int maxPerTile = 0;
            float averagePerTile = 0.0f;
            int tiles = 0;
        };

        // the grid of a viewport: tiles of even size covering it
        static Grid makeGrid(int width, int height, int columns, int rows);
        // the texture's height for a grid (TEXTURE_WIDTH texels a row)
        static int textureRows(const Grid& grid);
        // the screen rect a light's sphere covers, in pixels of the pass's gl_FragCoord space
        // (x0, y0, x1, y1, inclusive of what it touches); false when it misses the view. A sphere
        // reaching the near plane, or holding the camera, covers the whole viewport
        static bool projectSphere(const View& view, const Vector3& center, float radius, float rect[4]);

        // The lists of a view: the nearest MAX_LIGHTS_PER_VIEW lights (global ones first), each
        // tile the nearest MAX_LIGHTS_PER_TILE of those reaching it, listed in scene order. Writes
        // the whole texture (TEXTURE_WIDTH * textureRows(view.grid) texels, RGBA), and the scene
        // index of each view-local light into viewLights. Pure CPU, allocation-free once warm.
        void build(const View& view, const std::vector<Light>& lights, std::vector<float>& texels, Stats& stats);

        // what build listed: the scene light index of view-local index k
        const std::vector<int>& getViewLights() const { return viewLights; }

    private:
        struct Candidate {
            float distance;   // to the camera, past the sphere's surface (global lights: -1)
            int light;        // index into the input
            int rect[4];      // tiles x0, y0, x1, y1
        };
        std::vector<Candidate> candidates;
        std::vector<int> viewLights;
        std::vector<uint8_t> tileCounts;
    };

}

#endif // TILEDLIGHTS_H
