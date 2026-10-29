// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Include/SplineGeometry/SplineFillMesher.h,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef SPLINE_SPLINEFILLMESHER_H
#define SPLINE_SPLINEFILLMESHER_H

#include "Export.h"
#include "math/AABB.h"
#include "SplineFillField.h"
#include "SplineStation.h"
#include "SplineVertex.h"
#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine{ namespace spline{

    // Turns a flooded region into a water surface: marching squares used as an AREA fill, each
    // cell emitting the polygon of itself inside the region, so there is no boundary polygon to
    // extract and no polygon-with-holes triangulator for a region with islands. By construction:
    //  - no T-junctions: a shoreline vertex belongs to the lattice EDGE it crosses, so the two cells
    //    sharing that edge index the same vertex;
    //  - watertight at saddles: the cell-centre decider only chooses how one cell's interior
    //    connects, the edge crossings are untouched;
    //  - a buried rim: the contour runs at inside = 0 (phi = -edgeDrop) and the boundary vertex sits
    //    edgeDrop below the waterline.
    //
    // uv0 is a MEASUREMENT, not a texture chart (S18 D9): uv0.x = arc length along the flow, uv0.y =
    // still-water depth under the vertex, both world metres (a rim vertex reads -edgeDrop where the
    // ground decides the shoreline, so a shader clamping at zero puts its foam on the waterline). A
    // caller moving the vertices into a local space must not transform uv0 with them. The vertex
    // colour carries the flow: rg = the governing station's forward in XZ (0.5 + 0.5 d), b = the
    // speed scale. The tangent is that forward (U runs along the flow) with the strip builder's
    // handedness (-1), so a filled river and a Channel one agree about their frame.

    // 64 cells a side per chunk by default; at most 147, the most whose corner and edge vertices fit
    // the uint16 indices of a physics mesh collider ((n + 1)^2 + 2n(n + 1) <= 65535)
    inline constexpr uint32_t SPLINE_FILL_CELLS_PER_CHUNK = 64u;
    inline constexpr uint32_t SPLINE_FILL_MAX_CELLS_PER_CHUNK = 147u;

    struct SplineFillMeshParams{
        // metres the shoreline vertex sits below its waterline: the field's own edgeDrop
        float edgeDrop = 0.1f;
        // the vertex colour's b, clamped to [0, 1]
        float flowSpeedScale = 1.0f;
    };

    // A block of CELLS, in grid corner coordinates. Chunks run on the lattice rather than on arc
    // length: their boundaries fall on lattice lines, both sides evaluate the same field at the same
    // corner, and the duplicated boundary vertices are BIT-IDENTICAL.
    struct SplineFillChunkRange{
        uint32_t minCornerX = 0;
        uint32_t minCornerZ = 0;
        // one past the last CORNER: a range covers maxCornerX - minCornerX - 1 cells across
        uint32_t maxCornerX = 0;
        uint32_t maxCornerZ = 0;
    };

    struct SplineFillMesh{
        std::vector<SplineVertex> vertices;
        std::vector<uint32_t> indices;
        AABB bounds;

        bool isValid() const { return !vertices.empty() && !indices.empty(); }
    };

    // Mesh one lattice block of the flooded region, in the space the ground and stations were given
    // in. An empty mesh means the block holds no wet cell: a hole in the chunk plan, not a failure.
    JASPERENGINE_API SplineFillMesh buildSplineFillMesh(const SplineFillResult& field, const SplineGroundGrid& ground,
                                                        std::span<const SplineStation> stations,
                                                        const SplineFillChunkRange& range, const SplineFillMeshParams& params);

    // Cover the field's block of corners in chunks of at most `cellsPerChunk` cells a side (clamped
    // to [1, SPLINE_FILL_MAX_CELLS_PER_CHUNK]), aligned to whole multiples of it on the grid so a
    // chunk keeps its cells while the field's block grows or shrinks around it, in row-major order.
    JASPERENGINE_API std::vector<SplineFillChunkRange> carveFillChunks(const SplineFillResult& field,
                                                                       uint32_t cellsPerChunk = SPLINE_FILL_CELLS_PER_CHUNK);

}}

#endif // SPLINE_SPLINEFILLMESHER_H
