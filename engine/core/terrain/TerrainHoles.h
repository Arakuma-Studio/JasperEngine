// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_HOLES_H
#define TERRAIN_HOLES_H

// T3 R18, R19, R22, R23, R27 (the owner of what a hole means, programme D7): one optional R8 map,
// 0 = hole, 255 = solid, read with no filtering. A plain view over texels, so a job's copied texels
// use the same isHole, and a tiled store supplies one per tile.

#include "Export.h"
#include "terrain/TerrainMapStore.h"
#include "terrain/TerrainShape.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace jasperengine::terrain {

    inline constexpr uint8_t HOLE_VALUE = 0;
    inline constexpr uint8_t SOLID_VALUE = 255;

    struct HoleMapView {
        const uint8_t* texels = nullptr;
        int width = 0, height = 0;
    };

    // T3 R19, the only definition: texel min(floor(u * W), W - 1), with u = (localX + half) / size.
    // A point on a texel edge belongs to the higher texel; outside the terrain is not a hole.
    inline bool isHole(const HoleMapView& map, float u, float v) {
        if (!map.texels || map.width <= 0 || map.height <= 0) return false;
        // written so that NaN, like outside, is not a hole
        if (!(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)) return false;
        const int x = static_cast<int>(std::floor(u * static_cast<float>(map.width)));
        const int y = static_cast<int>(std::floor(v * static_cast<float>(map.height)));
        const int texelX = x < map.width - 1 ? x : map.width - 1;
        const int texelY = y < map.height - 1 ? y : map.height - 1;
        return map.texels[static_cast<size_t>(texelY) * map.width + texelX] < 128;
    }

    // T3 R22: lattice square (i, j) of a samples x samples lattice over the terrain is open when every
    // hole texel its local rectangle overlaps is a hole. open holds (samples - 1)^2 flags, row by row.
    // A negative mirror is the collider's mirrored axis, where sample i sits at -(-half + spacing * i).
    JASPERENGINE_API void openSquares(const HoleMapView& map, int samples, float mirrorX, float mirrorZ, std::vector<uint8_t>& open);
    // T3 R22: sample (x, y) has no collision when all its existing adjacent squares (4, 2 at an edge,
    // 1 at a corner) are open. out holds samples^2 flags, row by row, and the return is how many are set.
    JASPERENGINE_API int noCollisionSamples(const std::vector<uint8_t>& open, int samples, std::vector<uint8_t>& out);
    // The same rule for one sample (x, y) of a samples x samples lattice, read from the map: what a
    // collider block that a refresh samples asks, with no whole-lattice pass
    JASPERENGINE_API bool noCollisionSample(const HoleMapView& map, int samples, float mirrorX, float mirrorZ, int x, int y);
    // T3 R23: lattice cell (i, j) overlaps any hole texel. cells holds (samples - 1)^2 flags, row by
    // row; the return is how many are set.
    JASPERENGINE_API int navHoleCells(const HoleMapView& map, int samples, std::vector<uint8_t>& cells);
    // T3 R43: how many hole texels of the map lie in texels [x0, x1) x [y0, y1), clipped to the map
    JASPERENGINE_API int64_t countHoleTexels(const HoleMapView& map, int x0, int y0, int x1, int y1);

    // T3 R30: what the Holes overlay draws, per texel of the map: HOLE_OVERLAY_DEEP where the collider is
    // open, HOLE_OVERLAY_RIM where it stays under the drawn hole, HOLE_OVERLAY_NAV on ground the navmesh
    // leaves out, SOLID_VALUE elsewhere. Read as a hole map (under one half is a hole).
    inline constexpr uint8_t HOLE_OVERLAY_DEEP = 0;
    inline constexpr uint8_t HOLE_OVERLAY_RIM = 64;
    inline constexpr uint8_t HOLE_OVERLAY_NAV = 200;
    JASPERENGINE_API void holeOverlayClasses(const HoleMapView& map, int collisionSamples, int navSamples, std::vector<uint8_t>& out);

    struct HoleRegionResult {
        int changed = 0;             // texels that took the value
        int64_t holeTexels = 0;      // holes in the map after the call
        float holeFraction = 0.0f;   // of the map's texels
        bool mapCreated = false;
        bool mapRemoved = false;
        int64_t filteredOut = 0;     // in the region, kept by the filter
        int64_t inRegion = 0;        // in the region
    };

    // T3 R27's texel-centre rule: writes value (0 cut, 255 fill) to MapKind::Hole through the store.
    // A cut creates the map at the height map's resolution when it would change a texel, and a fill
    // that leaves no hole removes it, if the store lets it. No falloff and no strength, so a second
    // identical call changes 0 texels.
    JASPERENGINE_API int rasterizeRegion(TerrainMapStore& store, const Shape& region, uint8_t value, TexelRect* changed,
                                         const Filter* filter = nullptr, HoleRegionResult* detail = nullptr);

}

#endif // TERRAIN_HOLES_H
