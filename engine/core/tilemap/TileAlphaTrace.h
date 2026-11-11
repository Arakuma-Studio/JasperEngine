// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TILEMAP_TILE_ALPHA_TRACE_H
#define TILEMAP_TILE_ALPHA_TRACE_H

// Traces a tile's alpha into pixel-exact collision outlines (docs/specs/S50-grid-tilemap.md R15).

#include "Export.h"
#include "tilemap/Tileset.h"

#include <cstdint>
#include <vector>

namespace jasperengine{

    // The outlines of the pixels with alpha >= threshold in the rectangle (x, y, w, h) of an RGBA8
    // image whose first row is the top one; pixels outside the image count as transparent.
    // Result: closed polygons (the first point is not repeated) in tile pixels, origin at the
    // tile's bottom-left corner, y up, integer points, collinear points merged; solids
    // counter-clockwise, holes clockwise. Each polygon starts at its lowest, then leftmost point,
    // and the polygons are ordered by that point, a solid before a hole on the same point.
    // Pixels touching only at a corner are separate polygons.
    JASPERENGINE_API std::vector<TilePolygon> traceAlpha(const uint8_t* rgba, int imageW, int imageH, int x, int y, int w, int h, int threshold);

}

#endif // TILEMAP_TILE_ALPHA_TRACE_H
