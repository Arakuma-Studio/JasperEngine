// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/Spline/Include/Spline/SplineTypes.h, Copyright (c) 2026
// Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef SPLINE_SPLINETYPES_H
#define SPLINE_SPLINETYPES_H

#include "math/Vector3.h"
#include <cstdint>
#include <functional>

namespace jasperengine{ namespace spline{

    // How the curve runs between knots (S18 R1). Plain int-backed, like TangentMode: the editor
    // writes enum properties as int.
    enum class SplineType{
        // straight segments, constant speed
        Linear = 0,
        // centripetal Catmull-Rom (alpha 0.5) through every knot, tangents automatic
        CatmullRom,
        // cubic Bezier through every knot, shaped by each knot's handles
        CubicBezier
    };

    // How a Bezier knot's two handles follow each other when one is edited (Unity semantics).
    enum class TangentMode{
        // opposite directions, equal lengths
        Mirrored = 0,
        // opposite directions, each keeps its own length
        Aligned,
        // independent
        Broken
    };

    // One control point. Positions are spline-local (the component wrappers convert to and from
    // world space, S18 R3).
    struct SplineKnot{
        Vector3 position = Vector3(0.0f, 0.0f, 0.0f);
        // Bezier handles as offsets from position (Unity): the curve leaves along tangentOut and
        // arrives along -tangentIn. CatmullRom and Linear ignore them.
        Vector3 tangentIn = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 tangentOut = Vector3(0.0f, 0.0f, 0.0f);
        // half-width in world units, >= 0
        float radius = 2.0f;
        // bank about forward, degrees; positive raises the right side
        float rollDeg = 0.0f;
        TangentMode mode = TangentMode::Mirrored;
    };

    // The curve at one parameter. right and up include the roll; with roll 0 they are the R4
    // basis: right = normalize(forward x upAxis), up = right x forward.
    struct SplineFrame{
        Vector3 position = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 forward = Vector3(0.0f, 0.0f, -1.0f);
        Vector3 right = Vector3(1.0f, 0.0f, 0.0f);
        Vector3 up = Vector3(0.0f, 1.0f, 0.0f);
        // the channels, interpolated linearly per segment (never overshooting the knots)
        float radius = 0.0f;
        float rollDeg = 0.0f;
    };

    // A closest-point result.
    struct SplineHit{
        // global parameter in [0, 1]
        float t = 0.0f;
        // from the query to position (in the XZ queries, measured in XZ)
        float distance = 0.0f;
        // the full 3D point on the curve
        Vector3 position = Vector3(0.0f, 0.0f, 0.0f);
        uint32_t segment = 0;
    };

    // What a surface probe found under a point: the conform of stations (terrain) and the snap of
    // strokes (terrain or any surface). height is the coordinate along the spline's up axis
    // (world Y for a 3D spline); normal is the surface normal there.
    struct SplineSurfaceSample{
        bool hit = false;
        float height = 0.0f;
        Vector3 normal = Vector3(0.0f, 1.0f, 0.0f);
    };

    // Probes the surface under a point (the point's own height is ignored).
    using SplineSurfaceProbe = std::function<SplineSurfaceSample(const Vector3& point)>;

}}

#endif // SPLINE_SPLINETYPES_H
