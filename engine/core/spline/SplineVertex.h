// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Include/SplineGeometry/SplineVertex.h,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef SPLINE_SPLINEVERTEX_H
#define SPLINE_SPLINEVERTEX_H

#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

namespace jasperengine{ namespace spline{

    // One vertex of a generated spline mesh: the standard mesh layout's attributes (position,
    // normal, tangent, uv0, color), so the conversion at the engine boundary is a copy while
    // this module stays free of the render headers. Shared by every emitter (the swept strip,
    // the region fill, the polygon fill).
    struct SplineVertex{
        Vector3 position = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 normal = Vector3(0.0f, 1.0f, 0.0f);
        Vector2 uv = Vector2(0.0f, 0.0f);
        // xyz direction plus handedness in w (glTF). Analytic: every emitter knows the surface
        // frame it places a vertex into.
        Vector4 tangent = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
        // the water encoding (S18 D9): rg = flow direction XZ (0.5 + 0.5 d), b = speed scale;
        // white elsewhere
        Vector4 color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    };

}}

#endif // SPLINE_SPLINEVERTEX_H
