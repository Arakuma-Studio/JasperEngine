// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Apps/Editor/Source/SceneView/SplineTool.cpp (FinalizeSpline,
// SimplifyPolyline, SnapPointsToSurface, TryExtendExistingSpline) and SplineStrokeCapture.cpp,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef SPLINE_SPLINESTROKE_H
#define SPLINE_SPLINESTROKE_H

// Freehand stroke -> spline (S18 R26): the Draw Spline tool's and edit_spline's stroke path.
// Every point is in the target spline's local space (the caller converts); a 2D caller converts
// its pixel tolerances to units first (4 px for the simplify tolerance).

#include "Export.h"
#include "SplineData.h"
#include "SplineTypes.h"
#include "math/Vector3.h"
#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine{ namespace spline{

    struct SplineStrokeOptions{
        // Ramer-Douglas-Peucker tolerance (units)
        float simplifyTolerance = 0.5f;
        // Laplacian smoothing iterations (endpoints fixed)
        uint32_t smoothIterations = 2;
        // a stroke ending this close to its start closes the spline
        float closeRadius = 1.0f;
        // the knots' radius
        float knotRadius = 2.0f;
        // the axis a snap probe's height is measured along (+Z for a 2D spline)
        Vector3 upAxis = Vector3(0.0f, 1.0f, 0.0f);
    };

    // A stroke reduced to knot positions.
    struct SplineStrokeShape{
        std::vector<Vector3> points;
        // the stroke ended on its start: points does not repeat the start
        bool closed = false;
    };

    // Laplacian smoothing: each iteration moves every interior point halfway to the mean of its
    // two neighbours (all at once); the endpoints stay.
    JASPERENGINE_API void smoothPolyline(std::vector<Vector3>& points, uint32_t iterations);

    // Puts every point on the surface the probe finds under it (its height along upAxis is
    // replaced); a miss leaves the point. Returns the hits.
    JASPERENGINE_API uint32_t snapPoints(std::vector<Vector3>& points, const SplineSurfaceProbe& probe, const Vector3& upAxis);

    // Stroke -> knot positions: consecutive repeats dropped, smoothed (on the dense stroke, where
    // the Laplacian denoises without pulling corners and arcs inward, as it would on the few
    // simplified points), simplified (RDP), closed when it ends within closeRadius of its start
    // after leaving that circle (the tail inside it is trimmed, three points remain at least),
    // then snapped when a probe is given. False when fewer than two points remain.
    JASPERENGINE_API bool shapeStroke(std::span<const Vector3> stroke, const SplineStrokeOptions& options,
                                      const SplineSurfaceProbe* probe, SplineStrokeShape& out);

    // A new CatmullRom spline from a stroke (shapeStroke): its knots, closed flag and upAxis.
    // False (spline unchanged) when the stroke is too short.
    JASPERENGINE_API bool strokeToSpline(std::span<const Vector3> stroke, const SplineStrokeOptions& options,
                                         const SplineSurfaceProbe* probe, SplineData& spline);

    enum class SplineStrokeExtension : uint8_t{
        None = 0,
        // after the last knot
        Appended,
        // before the first knot (reversed, so the curve still runs one way)
        Prepended
    };

    // Extends an open spline with a shaped stroke whose first point lies within pickRadius of one
    // of its endpoints (the nearer; the end on a tie): the stroke's other points become new knots
    // past that endpoint, taking its radius, roll and mode (Bezier knots get centred-difference
    // handles). The existing knots stay bit-identical. A closed spline, an empty one or a stroke
    // starting elsewhere is not extended (None, nothing changes).
    JASPERENGINE_API SplineStrokeExtension extendWithStroke(SplineData& spline, std::span<const Vector3> points, float pickRadius);

}}

#endif // SPLINE_SPLINESTROKE_H
