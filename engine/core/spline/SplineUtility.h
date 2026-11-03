// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/Spline/Include/Spline/SplineUtility.h, Copyright (c)
// 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef SPLINE_SPLINEUTILITY_H
#define SPLINE_SPLINEUTILITY_H

#include "Export.h"
#include "math/Vector3.h"
#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine{ namespace spline{

    // squared distance from p to the segment a -> b (3D)
    JASPERENGINE_API float pointSegmentDistanceSq(const Vector3& p, const Vector3& a, const Vector3& b);

    // Ramer-Douglas-Peucker simplification reporting the KEPT INDICES into points (strictly
    // increasing; the first and the last are always kept), so callers with per-sample payloads
    // (frames, channels) can map every kept point back to its sample. Keeps the shape within
    // tolerance (world units). Iterative (no recursion). kept is replaced.
    JASPERENGINE_API void simplifyIndices(std::span<const Vector3> points, float tolerance, std::vector<uint32_t>& kept);

    // the positions simplifyIndices keeps; out is replaced
    JASPERENGINE_API void simplifyPolyline(std::span<const Vector3> points, float tolerance, std::vector<Vector3>& out);

}}

#endif // SPLINE_SPLINEUTILITY_H
