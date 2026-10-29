// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Include/SplineGeometry/SplineStation.h,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.
// Handedness: JasperEngine is right-handed, right = normalize(forward x up) (S18 D6); profile
// points with a positive lateral coordinate lie on the right of travel.

#ifndef SPLINE_SPLINESTATION_H
#define SPLINE_SPLINESTATION_H

#include "math/Vector3.h"
#include <cstdint>

namespace jasperengine{ namespace spline{

    // How a station's ring meets the ring before it.
    enum class SplineStationJoin : uint8_t{
        // stitched to the previous ring; its distance must advance past it
        Advance = 0,
        // a crease across the run: the ring stands at the previous ring's distance and is NOT
        // stitched to it, so the faces either side keep their own normals up to the shared edge
        // (a mitred corner is such a pair)
        Crease,
        // another ring of a corner that turns about one point: the previous ring's distance, its
        // own shape, stitched to it (a round corner's fan)
        Pivot
    };

    // sides of travel a station's mitreRight applies to (bit flags)
    inline constexpr uint8_t SPLINE_MITRE_LEFT_SIDE = 1u;
    inline constexpr uint8_t SPLINE_MITRE_RIGHT_SIDE = 2u;
    inline constexpr uint8_t SPLINE_MITRE_BOTH_SIDES = SPLINE_MITRE_LEFT_SIDE | SPLINE_MITRE_RIGHT_SIDE;

    // One station of the draped path: the frame a cross-section is placed into, plus the
    // per-station channels that shape it there. Stations are the shared seam between every
    // consumer of a spline (the swept strip, tile and fence placement, the water region fill and
    // the terrain bake): they walk the same draped, gap-held, width-carrying polyline by true 3D
    // length (S18 D5).
    struct SplineStation{
        Vector3 position = Vector3(0.0f, 0.0f, 0.0f);
        // orthonormal and ROLL-FREE: the strip builder applies rollRadians about forward itself
        Vector3 right = Vector3(1.0f, 0.0f, 0.0f);
        Vector3 up = Vector3(0.0f, 1.0f, 0.0f);
        Vector3 forward = Vector3(0.0f, 0.0f, -1.0f);
        // half-widths per side of travel: profile points with a negative lateral coordinate
        // scale by halfWidthLeft / profile.nominalHalfWidth, the rest by halfWidthRight / ...
        float halfWidthLeft = 1.0f;
        float halfWidthRight = 1.0f;
        // bank about forward, radians (the spline's roll channel)
        float rollRadians = 0.0f;
        // distance along the run, strictly ascending; the DRAPED 3D length (it drives U)
        float distance = 0.0f;
        // the spline parameter this station was sampled at (knot index + fraction; global t of
        // the editor's knot warnings); -1 for stations a builder inserted
        float splineT = -1.0f;

        SplineStationJoin join = SplineStationJoin::Advance;
        // the lateral axis that PLACES profile points on the sides named by mitreSides, where it
        // differs from right (right, up, forward still shade them)
        Vector3 mitreRight = Vector3(0.0f, 0.0f, 0.0f);
        uint8_t mitreSides = 0;
        // metres along up added to Grounded profile points only
        float groundOffset = 0.0f;
        // metres along up added to every point NOT marked Grounded (a stepped wall's top)
        float topOffset = 0.0f;
        // per side, the length a side face has run beyond the centreline per metre it stands from
        // it (cumulative turn in radians, positive outside); 0 = centreline U on every face
        float faceTurnLeft = 0.0f;
        float faceTurnRight = 0.0f;
        // how far the top's U has run ahead of the centreline around round corners
        float topArcAllowance = 0.0f;
        // a corner ring's face turn relative to the leg before it (applyCorners writes it,
        // accumulateFaceTurn reads it); the builder never reads it
        float cornerTurnLeft = 0.0f;
        float cornerTurnRight = 0.0f;
    };

}}

#endif // SPLINE_SPLINESTATION_H
