// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_HOLES_H
#define TERRAIN_HOLES_H

// T3 R18, R19, R27 (the owner of what a hole means, programme D7): one optional R8 map, 0 = hole,
// 255 = solid, read with no filtering. T1 R92 creates this minimal part; T3 extends it with the
// lattice rules of its consumers. A plain view over texels, so a job's copied texels use the same
// isHole, and a tiled store supplies one per tile.

#include "Export.h"
#include "terrain/TerrainMapStore.h"
#include "terrain/TerrainShape.h"

#include <cmath>
#include <cstdint>

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
