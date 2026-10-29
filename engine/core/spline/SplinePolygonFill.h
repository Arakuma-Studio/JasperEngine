// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SPLINE_SPLINEPOLYGONFILL_H
#define SPLINE_SPLINEPOLYGONFILL_H

// The flat fill of a closed spline (S18 R7 Fill, D10): our own ear clipper over the uniformly
// resampled curve, and the mesh it becomes (a lake surface in 3D, a filled shape in 2D).

#include "Export.h"
#include "SplineData.h"
#include "SplineVertex.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine{ namespace spline{

    enum class SplineFillError : uint8_t{
        None = 0,
        // fewer than three distinct points
        TooFewPoints,
        // no area (every point on one line)
        ZeroArea,
        // two edges cross or touch (also a vertex on another edge, or an edge folding back)
        SelfIntersecting,
        // no ear found: the input is not a simple polygon to floating-point precision
        NoEar
    };

    // Why a fill was refused; edgeA, edgeB name the offending edges (edge i runs from point i to
    // the next distinct point) for SelfIntersecting.
    struct SplineFillIssue{
        SplineFillError error = SplineFillError::None;
        uint32_t edgeA = 0;
        uint32_t edgeB = 0;
    };

    // a sentence for the inspector warning
    JASPERENGINE_API const char* fillErrorReason(SplineFillError error);

    // Triangulates the simple polygon (clockwise or counter-clockwise, without the closing
    // repeat; consecutive repeats are skipped) by ear clipping. triangles receives index triples
    // into polygon, every one counter-clockwise in the polygon's own (x, y) frame; a point on a
    // straight run between its neighbours is left out of the triangles (no sliver). A
    // self-intersecting or self-touching input is refused (false, issue set, triangles empty).
    JASPERENGINE_API bool triangulatePolygon(std::span<const Vector2> polygon, std::vector<uint32_t>& triangles,
                                             SplineFillIssue* issue = nullptr);

    // The plane a fill lies in.
    enum class SplineFillPlane : uint8_t{
        // 3D: y = height, normal +Y; the outline's x, z
        XZ = 0,
        // 2D: z = height, normal +Z; the outline's x, y
        XY
    };

    struct SplineFillOptions{
        SplineFillPlane plane = SplineFillPlane::XZ;
        // the plane's offset along its normal
        float height = 0.0f;
        // UV = world position in the plane (x, -z in XZ; x, y in XY) times these
        float tilingU = 1.0f;
        float tilingV = 1.0f;
    };

    // The flat fill mesh of a closed outline (sampleFillOutline): one vertex per outline point,
    // flattened onto the plane, with the plane's normal, tangent +X, UVs in world units x tiling
    // and white color, and triangles whose front faces (counter-clockwise in our right-handed
    // renderer) face along the normal: (b - a) x (c - a) agrees with it. False (outputs empty)
    // when triangulatePolygon refuses.
    JASPERENGINE_API bool buildPolygonFill(std::span<const Vector3> outline, const SplineFillOptions& options,
                                           std::vector<SplineVertex>& vertices, std::vector<uint32_t>& indices,
                                           SplineFillIssue* issue = nullptr);

    // The closed curve resampled uniformly by arc length, at most spacing apart (at least three
    // points), without the closing repeat. Empty for an invalid spline or a spacing <= 0.
    JASPERENGINE_API void sampleFillOutline(const SplineData& spline, float spacing, std::vector<Vector3>& outline);

    // the mean knot coordinate along the up axis: the 3D fill's height before its offset
    JASPERENGINE_API float meanKnotHeight(const SplineData& spline);

}}

#endif // SPLINE_SPLINEPOLYGONFILL_H
