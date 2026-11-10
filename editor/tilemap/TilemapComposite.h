// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// A deterministic CPU composite of a Tilemap Layer region (docs/specs/S50-grid-tilemap.md R76's
// `image`, R83): the cells of the region placed as the chunk builder places them (R8's anchors,
// the tile's offset, the layer's tile anchor, the dual-grid shift), each tile's flips and transpose
// in Tiled's image-space order, drawn in the painter's order with alpha over. No GPU: the same
// arguments give the same bytes, and it is the reference the GPU goldens compare against.
//
// Positions are in pixels (world units x pixelsPerUnit) rounded to the pixel grid (floor(v + 0.5))
// for every tile; the GPU path keeps fractions unless the Pixel Perfect Camera snaps them.

#pragma once

#include "pixelart/PixelImage.h"
#include "pixelart/PixelProblem.h"
#include "tilemap/TileCells.h"
#include "tilemap/TileGridLayout.h"
#include "tilemap/TilesetEdit.h"

#include <cstddef>
#include <cstdint>

namespace jasperengine::editor::tilemap {

    // the most pixels a composite image holds (4096 x 4096, 64 MB of RGBA), after its scale
    constexpr int64_t MAX_COMPOSITE_PIXELS = 16777216;

    struct CompositeParams {
        const TileCells* cells = nullptr;
        const TilesetRuntime* tileset = nullptr;
        TileGridSpec grid;
        CellRect region;
        int scale = 1;                   // 1 to 8
        double pixelsPerUnit = 1.0;
        double tileAnchorX = 0.0;        // the layer's tileAnchor, in cells
        double tileAnchorY = 0.0;
        bool dualGrid = false;
    };

    struct CompositeResult {
        PixelImage image;
        // the plane position (pixels) of the image's top-left corner, so a caller maps cells to
        // image pixels; multiply by the scale for the scaled image
        int originX = 0;
        int originY = 0;
        // cells whose tile has no image (R90: the id is missing, or its page did not load)
        size_t missing = 0;
    };

    // A copy of a tile image with its transform bits applied, in Tiled's order: transpose (swap x
    // and y, image y down) first, then flip X, then flip Y
    PixelImage transformTileImage(const PixelImage& image, uint32_t flags);

    // The region (cells with tiles, in the painter's order) on an image covering the region's cells
    // and every tile drawn in it; an empty region gives an empty image. False with `region_too_large`
    // for a region or an image past the limits (cells, or MAX_COMPOSITE_PIXELS), or `parse` for a scale outside 1 to 8.
    bool compositeLayer(const CompositeParams& params, TilesetPagePixels& pages, CompositeResult& out, PixelProblem& error);

}
