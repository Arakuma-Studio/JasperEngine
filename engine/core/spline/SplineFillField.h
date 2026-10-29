// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Include/SplineGeometry/SplineFillField.h,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef SPLINE_SPLINEFILLFIELD_H
#define SPLINE_SPLINEFILLFIELD_H

#include "Export.h"
#include "math/Vector3.h"
#include "SplineStation.h"
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace jasperengine{ namespace spline{

    // FitToBanks water (S18 R7 WaterBanks). A water surface is not a swept cross-section: its extent
    // is decided by the GROUND, the curve only says where the water is and how high it stands, so
    // the shape is found by flooding a region. A region cannot self-intersect, reaches pockets no
    // station's perpendicular does, and two legs of one hairpin over one basin are one region.
    // This half solves the field and the connectivity; SplineFillMesher turns it into triangles.

    // Ground heights on the terrain's OWN sample lattice (S18 D9: the heightmap texels, spacing =
    // terrainSize / (N - 1), world transform applied), so the shoreline is compared against the
    // data the renderer draws. World XZ, Y up.
    struct SplineGroundGrid{
        // world XZ of corner (0, 0), and the metres between adjacent corners
        float originX = 0.0f;
        float originZ = 0.0f;
        float spacingX = 0.0f;
        float spacingZ = 0.0f;
        // corner counts, NOT cell counts: N corners make N - 1 cells
        uint32_t countX = 0;
        uint32_t countZ = 0;
        // world-space ground altitude per corner, row-major with x fastest; countX * countZ of them
        std::span<const float> heights;

        uint32_t cornerCount() const { return countX * countZ; }
        uint32_t index(uint32_t cx, uint32_t cz) const { return cz * countX + cx; }
        float worldX(uint32_t cx) const { return originX + static_cast<float>(cx) * spacingX; }
        float worldZ(uint32_t cz) const { return originZ + static_cast<float>(cz) * spacingZ; }
        bool isValid() const{
            return countX >= 2u && countZ >= 2u && spacingX > 0.0f && spacingZ > 0.0f &&
                   heights.size() == static_cast<size_t>(countX) * countZ;
        }
    };

    // a waterline rising downstream by more than this is an authoring error worth naming
    inline constexpr float SPLINE_WATERLINE_RISE_TOLERANCE_METRES = 0.05f;

    // "No floor": also the arithmetic identity of the clamp that applies it, so the disabled state
    // needs neither a branch nor an equality test
    inline constexpr float SPLINE_NO_SEA_LEVEL_FLOOR = std::numeric_limits<float>::lowest();

    // Metres the sea-level floor is lifted above the level it names: a mouth exactly coplanar with
    // the sea z-fights it in lattice-aligned stripes. At the disabled floor's magnitude one ULP is
    // ~2e31 m, so the addition cannot move it.
    inline constexpr float SPLINE_SEA_LEVEL_FLOOR_LIFT_METRES = 0.02f;

    inline constexpr uint32_t SPLINE_FILL_NO_STATION = 0xFFFFFFFFu;

    struct SplineFillParams{
        // radius of the corridor around the stations (MaxHalfWidth): it bounds the SEARCH, and is
        // never a wall; water reaching it over lower ground is left out and counted as unseen bank
        float maxHalfWidth = 12.0f;
        // the contour runs at phi = -edgeDrop, so the shoreline sits past the waterline crossing
        // into the bank, and the mesher sinks the rim vertex edgeDrop below the surface
        float edgeDrop = 0.1f;
        // the waterline never falls below this (a river mouth meeting a sea); no floor by default
        float seaLevelFloor = SPLINE_NO_SEA_LEVEL_FLOOR;
        // hard ceiling on the region: past it the run builds nothing and says so rather than
        // silently coarsening
        uint32_t maxWetCorners = 262144u;
    };

    // where a refusal was worst: the world XZ of the sample and how far along the run it sits
    struct SplineFillWorstSample{
        float x = 0.0f;
        float z = 0.0f;
        float arcDistance = 0.0f;
    };

    struct SplineFillDiagnostics{
        uint32_t seeds = 0;
        // stations whose own ground is not below their waterline (carve deeper or lift the offset)
        uint32_t drySeeds = 0;
        Vector3 firstDrySeed;
        // the station of firstDrySeed, whose splineT names the knot
        uint32_t firstDrySeedStation = SPLINE_FILL_NO_STATION;
        uint32_t wetCorners = 0;
        // corners the ground and the corridor would both have taken, refused because the water
        // there would stand over a bank it could flow across, and the worst overshoot in metres
        uint32_t overBankCorners = 0;
        float maxOverBankMetres = 0.0f;
        SplineFillWorstSample worstOverBank;
        // the subset whose escape topped out at the CORRIDOR EDGE rather than at ground the run can
        // see: widen maxHalfWidth, rather than carve deeper
        uint32_t unseenBankCorners = 0;
        SplineFillWorstSample worstUnseenBank;
        // corners the containment term refused that the region then enclosed on every side, so
        // they were taken after all (water cannot drain through water)
        uint32_t sealedPocketCorners = 0;
        float maxDepthMetres = 0.0f;
        // largest waterline difference between two adjacent wet corners (two legs of one run
        // sharing a basin at different altitudes)
        float medialStepMetres = 0.0f;
        // metres the waterline RISES going downstream, where, and at which station (whose splineT
        // names the knot); compare against SPLINE_WATERLINE_RISE_TOLERANCE_METRES
        float waterlineRiseMetres = 0.0f;
        float waterlineRiseAtArc = 0.0f;
        uint32_t waterlineRiseStation = SPLINE_FILL_NO_STATION;
        // the region exceeded maxWetCorners: no corner is wet and the caller builds nothing
        bool exceededBudget = false;
    };

    // the stamped and flooded field at one lattice corner
    struct SplineFillCorner{
        // world y of the water surface here, from the nearest station's arc position (an INPUT the
        // fill reads and never writes: water it cannot hold is removed, not lowered)
        float waterline = 0.0f;
        // the lowest bank a drop here must clear to leave the corridor away from the centreline;
        // infinity where no outward path exists (a pocket the river itself holds)
        float escape = std::numeric_limits<float>::infinity();
        // XZ metres to the nearest station; infinity where no station stamped this corner
        float distance = std::numeric_limits<float>::infinity();
        // draped distance along the run and signed metres across it (positive on the right of
        // travel), continuous within a station's territory
        float arcDistance = 0.0f;
        float lateralSigned = 0.0f;
        // how far INSIDE the region this corner is, in metres, before connectivity:
        // min(phi + edgeDrop, maxHalfWidth - distance, max(escape, seaFloor) - waterline);
        // the mesher interpolates it to place sub-cell shoreline vertices
        float inside = -std::numeric_limits<float>::infinity();
        // the governing station, or SPLINE_FILL_NO_STATION
        uint32_t station = SPLINE_FILL_NO_STATION;
        // reached by the 4-connected flood from a station seed
        uint8_t wet = 0;
        // escape was decided at the corridor edge rather than at ground inside it
        uint8_t escapeUnseen = 0;
    };

    // The field over the block of lattice corners the corridor can reach (its stations' bounds
    // plus maxHalfWidth and one corner of margin, clamped to the grid): no corner outside it can be
    // stamped, so the field there is the unstamped dry corner and nothing is stored for it, which
    // keeps a 1025^2 terrain from costing a full-grid pass per rebuild.
    struct SplineFillResult{
        // corner (minCornerX + i, minCornerZ + j) of the grid is corners[j * countX + i]
        uint32_t minCornerX = 0;
        uint32_t minCornerZ = 0;
        uint32_t countX = 0;
        uint32_t countZ = 0;
        std::vector<SplineFillCorner> corners;
        SplineFillDiagnostics diagnostics;

        bool hasRegion() const { return diagnostics.wetCorners > 0u; }
        bool contains(uint32_t cx, uint32_t cz) const{
            return cx >= minCornerX && cz >= minCornerZ && cx - minCornerX < countX && cz - minCornerZ < countZ;
        }
        // the field at grid corner (cx, cz); an unstamped dry corner outside the block
        const SplineFillCorner& corner(uint32_t cx, uint32_t cz) const{
            static const SplineFillCorner unstamped;
            if (!contains(cx, cz)){
                return unstamped;
            }
            return corners[static_cast<size_t>(cz - minCornerZ) * countX + (cx - minCornerX)];
        }
    };

    // Stamp the waterline field from the stations (a closest-point transform at station
    // resolution: the height at the closest point on the curve in XZ, the rule the terrain bake uses
    // for its flatten target), solve the escape elevation (a drainage path must move AWAY from the
    // centreline: water leaving along the flow is the river working, across the bank a flood), then
    // flood the region 4-connected from the station seeds (8-connected leaks through a single
    // diagonal gap). Stations need ascending distance; their position y is the waterline.
    JASPERENGINE_API SplineFillResult buildSplineFillField(std::span<const SplineStation> stations,
                                                           const SplineGroundGrid& ground, const SplineFillParams& params);

}}

#endif // SPLINE_SPLINEFILLFIELD_H
