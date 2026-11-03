// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/Spline/Include/Spline/SplineEvaluator.h, Copyright (c)
// 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef SPLINE_SPLINEEVALUATOR_H
#define SPLINE_SPLINEEVALUATOR_H

// The queries of a spline (S18 R3), in spline-local space.
//
// Parameters. The global parameter t in [0, 1] is uniform PER SEGMENT: segment s covers
// [s / n, (s + 1) / n] of the n segments and its local parameter is u = t * n - s, so t = k / n
// is exactly knot k. Arc length is the other parameter: tToDistance / distanceToT convert, and
// evaluateAtDistance moves at constant speed. On an open spline t and distances clamp to the
// ends; on an effectively closed one they wrap (t = 1 and d = length stay the end of the loop).
//
// Every query reads the spline's cache and rebuilds it first when the spline changed (R5), so a
// stale cache is never read. The queries do not allocate (sampleUniform and resample fill their
// outputs), and a spline of fewer than two knots answers with its one knot (or a default frame).

#include "Export.h"
#include "SplineData.h"
#include "SplineTypes.h"
#include <cstdint>
#include <vector>

namespace jasperengine{ namespace spline{

    // the frame at global t; right and up per R4: right = normalize(forward x upAxis), up =
    // right x forward, both rolled by rollDeg about forward (positive raises the right side).
    // Where forward is parallel to upAxis (within 1e-4) the basis is the last well-defined one
    // before it (the first after it at an open start), parallel-transported, so vertical
    // stretches never flip.
    JASPERENGINE_API SplineFrame evaluate(const SplineData& spline, float t);
    // the frame at segment-local u in [0, 1] of one segment (clamped)
    JASPERENGINE_API SplineFrame evaluateSegment(const SplineData& spline, uint32_t segment, float u);
    // the frame at arc distance d from the start
    JASPERENGINE_API SplineFrame evaluateAtDistance(const SplineData& spline, float d);
    // the position only (no frame)
    JASPERENGINE_API Vector3 evaluatePosition(const SplineData& spline, float t);

    JASPERENGINE_API float length(const SplineData& spline);
    JASPERENGINE_API float tToDistance(const SplineData& spline, float t);
    JASPERENGINE_API float distanceToT(const SplineData& spline, float d);
    // arc distance d as (segment, u)
    JASPERENGINE_API void distanceToSegment(const SplineData& spline, float d, uint32_t& segment, float& u);
    // global t as (segment, u), and back
    JASPERENGINE_API void tToSegment(const SplineData& spline, float t, uint32_t& segment, float& u);
    JASPERENGINE_API float segmentToT(const SplineData& spline, uint32_t segment, float u);

    // The closest point of the curve: segment boxes as the broad phase, then per remaining
    // segment a coarse scan, golden-section and Newton refinement of every local minimum.
    // An empty spline answers distance = +infinity.
    JASPERENGINE_API SplineHit closestPoint(const SplineData& spline, const Vector3& p);
    // the same in the XZ projection (the curve's altitude never enters the distance; position
    // stays the full 3D point, so its y is the height to grade to)
    JASPERENGINE_API SplineHit closestPointXZ(const SplineData& spline, float x, float z);

    // distance to the swept tube: closest distance minus the radius there (< 0 inside)
    JASPERENGINE_API float signedDistance(const SplineData& spline, const Vector3& p);
    // XZ distance to the footprint minus the radius; a point enclosed by an effectively closed
    // spline is always < 0 (-(distance + radius)), so a closed spline is an area
    JASPERENGINE_API float signedDistanceXZ(const SplineData& spline, float x, float z);
    // XZ distance to the swept band only: a closed spline stays a ring, never a disc (equal to
    // signedDistanceXZ on an open spline)
    JASPERENGINE_API float signedDistanceBandXZ(const SplineData& spline, float x, float z);

    // count frames evenly spaced by arc length, the first at 0 and the last at length() (on a
    // closed spline it repeats the first position); frames is replaced
    JASPERENGINE_API void sampleUniform(const SplineData& spline, uint32_t count, std::vector<SplineFrame>& frames);

    // Replaces the knots with knots evenly spaced by arc length, at most spacing apart, carrying
    // the radius and roll channels (SplineData::replaceKnotsFromFrames); a simplifyTolerance > 0
    // then drops the ones the curve does not need (simplifyIndices on the sampled positions).
    // The type and the closed flag stay. False (nothing changes) for an invalid or zero-length
    // spline or a spacing <= 0.
    JASPERENGINE_API bool resample(SplineData& spline, float spacing, float simplifyTolerance = 0.0f);

}}

#endif // SPLINE_SPLINEEVALUATOR_H
