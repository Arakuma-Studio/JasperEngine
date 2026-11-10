// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TILEMAP_CHUNK_BUILDER_H
#define TILEMAP_CHUNK_BUILDER_H

// One chunk's instances (docs/specs/S50-grid-tilemap.md R8, R31, R32, R35, R36, D7): each non-empty
// cell becomes an instance of the layer's unit quad (positions (0, 0)..(1, 1), UV (qx, 1 - qy)), in
// layer space, the GameObject's matrix being the batch transform. Reads only: chunks build in
// parallel.
//
// A tile's transform bits are applied in image space, as Tiled's: transpose (swap x and y, image y
// down) first, then flip X, then flip Y. Flips are a negative texture rect, transpose swaps the
// quad's axes in the instance matrix (exact zeros: S41's every-vertex snap applies).
//
// Edges (S41 R13): in pixels, through pixelart::snap under a Pixel Perfect Camera, from values that
// coincide exactly for neighbouring tiles, so a shared edge lands on one pixel at every camera
// position; the UVs are the tile's rect with no inset (the shaders clamp inside the frame: 'Tcl').

#include "Export.h"
#include "math/AABB.h"
#include "render/sprite/SpriteInstanceBatch.h"
#include "tilemap/TileCells.h"
#include "tilemap/TileGridLayout.h"
#include "tilemap/TilesetPool.h"

#include <vector>

namespace jasperengine{

    // one animated instance of a chunk: where it is, and the tile (and transform bits) to re-pick
    struct TilemapAnimatedInstance{
        uint32_t instance = 0;
        int32_t tile = 0;
        uint32_t flags = 0;
        int64_t startMs = 0;             // random_start: the cell's hash offset
    };

    struct TilemapChunkParams{
        const TileCells* cells = nullptr;
        TileGridSpec grid;               // world units (the Grid's cell size)
        const TilesetRuntime* tileset = nullptr;
        // the project's texels per world unit: tile images are pixels / ppu in the world
        double pixelsPerUnit = 1.0;
        double tileAnchorX = 0.0;        // in cells (R2 tileAnchor)
        double tileAnchorY = 0.0;
        bool dualGrid = false;
        bool snapEdges = false;
        uint32_t seed = 0;               // the random_start hash's
        int64_t animationMs = 0;
        bool individual = false;         // Individual mode: painter's order, and out.rows
    };

    // one anchor row of a chunk's instances (Individual mode: a sort item of its own)
    struct TilemapChunkRow{
        uint32_t first = 0;
        uint32_t count = 0;
        double anchorU = 0.0;            // layer space, world units
        double anchorV = 0.0;
    };

    struct TilemapChunkBuild{
        std::vector<TilemapAnimatedInstance> animated;
        std::vector<SpritePageRun> runs;
        std::vector<TilemapChunkRow> rows;
        // the earliest time (in animationMs) an animated instance changes frame; INT64_MAX: never
        int64_t nextFrameMs = INT64_MAX;
        // an animated tile needs another page than the one it was built with (the chunk rebuilds)
    };

    namespace tilechunk{

        // fills `batch` (setCount, instances, pivots when enabled, page runs, markInstancesChanged) with the
        // chunk's instances, in the painter's order where tiles may overlap, else grouped by page
        JASPERENGINE_API void build(const TilemapChunkParams& params, ChunkCoord coord, SpriteInstanceBatch& batch, TilemapChunkBuild& out);
        // rewrites the texture rects of the animated instances whose frame changed at `ms`; false
        // when a frame lies on another page than the instance's run (the chunk must rebuild)
        JASPERENGINE_API bool updateAnimation(const TilemapChunkParams& params, SpriteInstanceBatch& batch, TilemapChunkBuild& build, int64_t ms, bool& changed);
        // the frame rect (u0, v0, w, h; negative w or h mirrors) of a tile with its flag bits
        JASPERENGINE_API Rect tileUvRect(const TileRenderInfo& tile, uint32_t flags);
        // the chunk's bounds in layer space: its cells' polygons moved by the layer's
        // tilegrid::tileShift and grown by `overhang` world units
        JASPERENGINE_API AABB chunkBounds(const TileGridSpec& grid, const CellRect& rect, double overhangX, double overhangY, TilePoint shift);
    }

}

#endif // TILEMAP_CHUNK_BUILDER_H
