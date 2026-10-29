// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Include/SplineGeometry/SplineProfile.h,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef SPLINE_SPLINEPROFILE_H
#define SPLINE_SPLINEPROFILE_H

#include "Export.h"
#include "math/Vector2.h"
#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine{ namespace spline{

    // Cross-section presets (S18 R6). Each is a generator over a handful of dimensions rather
    // than a baked point list, so widening a road is one float instead of re-authoring points.
    enum class SplineProfileShape : uint8_t{
        Flat = 0,  // ribbon, water: width (two points, open)
        Bevel,     // path: width, edgeDrop, edgeInset (open)
        Crown,     // road: width, crownRise, shoulderWidth, shoulderDrop (open)
        Rectangle, // wall: thickness, height (closed, capped)
        Tube,      // pipe: tubeSides around a unit radius the radius channel scales (closed, capped)
        Custom     // an authored point list (buildCustomProfile)
    };

    inline constexpr uint32_t SPLINE_PROFILE_MAX_CUSTOM_POINTS = 32;
    inline constexpr uint32_t SPLINE_PROFILE_MIN_TUBE_SIDES = 3;
    inline constexpr uint32_t SPLINE_PROFILE_MAX_TUBE_SIDES = 64;

    // One cross-section vertex in the station frame's plane: x = lateral (along the station's
    // right, positive on the right of travel), y = vertical (along its up), both in metres.
    struct SplineProfilePoint{
        Vector2 position = Vector2(0.0f, 0.0f);
        // a crease: the vertex is emitted twice per ring with split normals, so a wall's top edge
        // stays sharp instead of smoothing into a pillow
        uint8_t hard = 0;
        // stands on the ground: the station's groundOffset moves it and no other point
        uint8_t grounded = 0;
    };

    // POINT ORDER IS LOAD-BEARING, one rule for open and closed profiles: the outward normal of an
    // edge is its direction rotated +90 degrees in the (lateral, vertical) plane, edge (dx, dy) =>
    // normal (-dy, dx).
    //   - An OPEN strip is ordered left to right (-x to +x), so its normals point up.
    //   - A CLOSED loop is ordered clockwise in that plane (up the left face, right across the top,
    //     down the right face, left across the bottom), so every face points away from the solid.
    struct SplineProfile{
        std::vector<SplineProfilePoint> points;
        // a closed polygon caps its ends; derived from the profile, never authored
        bool closed = false;
        // the half-width the points are authored at: the width channel scales lateral coordinates
        // by halfWidth / nominalHalfWidth. The widest |x|, or 1 for a profile with no lateral extent.
        float nominalHalfWidth = 1.0f;
        // a closed profile's end cap as point index triples, in the profile's own winding; empty
        // for an open profile and for an outline that cannot be ear-clipped (it crosses itself),
        // which then builds no caps
        std::vector<uint32_t> capTriangles;

        bool isValid() const { return points.size() >= 2u; }
    };

    // Preset dimensions (defaults from OpenEngine's SplineExtrude and SplineWall recipes).
    struct SplineProfileParams{
        SplineProfileShape shape = SplineProfileShape::Bevel;
        // Flat, Bevel, Crown: total width of the carriageway, metres
        float width = 2.0f;
        // Bevel: how far the outer edge drops below the walkable surface, and how far in from it
        // the surface stays flat. Both zero collapse it to the Flat ribbon.
        float edgeDrop = 0.1f;
        float edgeInset = 0.15f;
        // Crown: camber rise at the centreline, the shoulder beyond the width on each side and
        // how far it drops
        float crownRise = 0.12f;
        float shoulderWidth = 0.5f;
        float shoulderDrop = 0.2f;
        // Rectangle: wall thickness and height above the station
        float thickness = 0.6f;
        float height = 3.0f;
        // Tube: sides of the polygon around the centreline, clamped to
        // [SPLINE_PROFILE_MIN_TUBE_SIDES, SPLINE_PROFILE_MAX_TUBE_SIDES]. The tube is authored at
        // radius 1, so it is swept with SplineProfileScale::Uniform and the radius channel as its
        // half-width.
        uint32_t tubeSides = 12;

        bool operator==(const SplineProfileParams&) const = default;
    };

    // Resolve a preset into the point list a sweep consumes. Consecutive coincident points are
    // dropped, so a degenerate preset (a Bevel with no drop and no inset) collapses to a valid
    // smaller profile instead of emitting zero-length edges. Custom returns an empty profile:
    // authored points go through buildCustomProfile.
    JASPERENGINE_API SplineProfile buildProfile(const SplineProfileParams& params);

    // An authored point list (at most SPLINE_PROFILE_MAX_CUSTOM_POINTS; later points are ignored),
    // with the same cleanup and derived nominal half-width. The caller owns the winding rule.
    JASPERENGINE_API SplineProfile buildCustomProfile(std::span<const SplineProfilePoint> points, bool closed);

    // What a swept run does to the terrain under it (OpenEngine's ResolveGroundAuthority): a road
    // owns the ground it is cut into, a path or ribbon grades it to its own heights, and walls,
    // tubes, custom shapes and water filled to its banks leave it alone (a fill reads the ground
    // and must never flatten the basin it needs to find its extent).
    enum class SplineGroundAuthority : uint8_t{
        None = 0,
        Grades,
        Owns
    };

    JASPERENGINE_API SplineGroundAuthority resolveGroundAuthority(SplineProfileShape shape, bool fitsToBanks);

}}

#endif // SPLINE_SPLINEPROFILE_H
