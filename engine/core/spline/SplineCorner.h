// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Include/SplineGeometry/SplineCorner.h,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.
//
// Handedness (S18 D6): right of travel is normalize(forward x up) here (OpenEngine: up x forward),
// so a turn toward the right has cross(out, in) along +up.

#ifndef SPLINE_SPLINECORNER_H
#define SPLINE_SPLINECORNER_H

#include "Export.h"
#include "math/Vector3.h"
#include "SplineStation.h"
#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine{ namespace spline{

    // What a sweep builds where its run turns at an authored point.
    enum class SplineCornerStyle : uint8_t{
        // the stations stay where they were sampled and the sweep rounds the turn over whatever
        // spacing they have (paths and roads)
        None = 0,
        // one ring in the bisector plane, its lateral axis scaled by 1 / cos(theta/2) so the faces of
        // both legs meet on one edge, emitted twice (a crease) so each leg keeps its own normals
        Mitre,
        // a fan of rings pivoting about the corner point from the incoming frame to the outgoing
        // one: the outside becomes an arc, the inside meets at the inner mitre point
        Round
    };

    // A mitre ring's lateral scale 1 / cos(theta/2) at which a Mitre corner is built Round instead
    // (and reported): 2 is a 120 degree turn, past which the mitre point runs away to infinity.
    inline constexpr float SPLINE_MITRE_LIMIT = 2.0f;
    inline constexpr float SPLINE_MITRE_LIMIT_TURN_DEGREES = 120.0f;

    // A Round corner has ceil(theta / step) rings, both ends included: a 90 degree corner is 9.
    inline constexpr float SPLINE_ROUND_CORNER_STEP_DEGREES = 10.0f;
    inline constexpr uint32_t SPLINE_MIN_ROUND_CORNER_RINGS = 2;
    inline constexpr uint32_t SPLINE_MAX_ROUND_CORNER_RINGS = 18;

    // a station of the stream that stands on an authored corner point (knot)
    struct SplineCornerSite{
        uint32_t stationIndex = 0;
        // the knot, so a report names what the author sees
        uint32_t pointIndex = 0;
    };

    enum class SplineCornerIssueKind : uint8_t{
        // asked to be a Mitre but turning past SPLINE_MITRE_LIMIT: built Round
        PastMitreLimit = 0,
        // the leg between the corner and an open end of the run is shorter than the corner's
        // reach, so the inside face folds back past the end
        LegShorterThanReach,
        // this corner and the next (otherPointIndex) turn the same way closer together than the sum
        // of their reaches, so their inside faces fold through each other on the leg between them
        CornersOverlap,
        // between knots pointIndex and otherPointIndex the curve bends tighter than the inside
        // half-width, so the inside face folds through itself
        InnerFold
    };

    // what an author is told about a corner the sweep could not build as asked
    struct SplineCornerIssue{
        SplineCornerIssueKind kind = SplineCornerIssueKind::PastMitreLimit;
        uint32_t pointIndex = 0;
        uint32_t otherPointIndex = 0;
        float turnDegrees = 0.0f;
    };

    // Give the stream's corners the shape `style` asks for, in place. Each site's station becomes
    // its corner's rings (a crease pair for a Mitre, a fan for a Round), and the stations within
    // the corner's reach on either leg are removed: the inside faces meet at the inner mitre point,
    // halfWidth * tan(theta/2) before the corner along each leg, and a ring between it and the
    // corner would fold the inside face back through itself (one within a few centimetres beyond
    // it goes too). Directions are read from neighbouring stations, so the stream must already
    // carry the draped positions; `up` is the frame's vertical and the plane the turn is measured in.
    // On a closed loop (closed by closeStationLoop) station 0 may be a site: its corner is split
    // across the seam. Sites must be in ascending station order. Returns the corners built Round
    // past the mitre limit and the legs too short for their corners' reach.
    JASPERENGINE_API std::vector<SplineCornerIssue> applyCorners(std::vector<SplineStation>& stations,
            std::span<const SplineCornerSite> sites, SplineCornerStyle style, const Vector3& up, bool closedLoop);

    // Give every station the length its side faces have run beyond the centreline (faceTurnLeft /
    // faceTurnRight), so each face takes U along its own length, and the top's arc allowance around
    // round corners. Run it after applyCorners; running it again gives the same result.
    JASPERENGINE_API void accumulateFaceTurn(std::span<SplineStation> stations, const Vector3& up);

    // The stations where a curve bends tighter than the wall is wide: the plan radius through the
    // station and its neighbours is smaller than the inside half-width. Stations flagged in `skip`
    // (a corner and its neighbours, which a corner reshapes) are not measured. Ascending indices.
    JASPERENGINE_API std::vector<uint32_t> findInnerFolds(std::span<const SplineStation> stations,
            std::span<const uint8_t> skip, const Vector3& up);

    // The inner folds as InnerFold issues, one per knot interval: a fold station's splineT names the
    // knots either side of it (knot floor(t) and the next; on a closed spline of `knotCount` knots
    // the last interval ends on knot 0). Stations without a splineT (-1) are skipped.
    JASPERENGINE_API std::vector<SplineCornerIssue> innerFoldIssues(std::span<const SplineStation> stations,
            std::span<const uint32_t> folds, uint32_t knotCount, bool closedSpline);

    // Close a sampled loop on itself: the stream's last station is the loop's start reached again,
    // and this makes the two the same station in every field but distance and splineT, the first
    // ring's frame looking along the loop across the seam. Fewer than three stations are left
    // untouched.
    JASPERENGINE_API void closeStationLoop(std::span<SplineStation> stations, const Vector3& up);

}}

#endif // SPLINE_SPLINECORNER_H
