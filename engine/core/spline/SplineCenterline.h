// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineLayout/Include/SplineLayout/CenterlineSampling.h
// and TileLayout.h (CenterSample, HoldSurfaceAcrossGaps), Copyright (c) 2026 Gamecrafters Guild
// Limited and contributors, MIT License. See NOTICE.md.

#ifndef SPLINE_SPLINECENTERLINE_H
#define SPLINE_SPLINECENTERLINE_H

#include "Export.h"
#include "math/Vector3.h"
#include "math/Matrix4.h"
#include "SplineStation.h"
#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine{ namespace spline{

    // Metres between dense samples of a draped centerline. Pinned: layout stations come from the
    // measured length of the draped polyline, so anything that varies this step varies how many
    // pieces the user gets.
    inline constexpr float SPLINE_CENTERLINE_STEP = 0.5f;

    // Longest centerline the pinned step covers within the sample budget. Past it the step
    // coarsens, the drape measures short and pieces are lost, so the caller warns.
    inline constexpr float SPLINE_MAX_CENTERLINE_LENGTH = 4096.0f;

    // One sample per step, plus the endpoint, plus one to close the final partial step.
    inline constexpr uint32_t SPLINE_MAX_CENTERLINE_SAMPLES =
        static_cast<uint32_t>(SPLINE_MAX_CENTERLINE_LENGTH / SPLINE_CENTERLINE_STEP) + 2u;

    // One sample of the draped (conformed) centerline the layouts walk, in world space.
    struct SplineCenterSample{
        Vector3 position = Vector3(0.0f, 0.0f, 0.0f);
        // the surface normal under the sample (world up where nothing conforms)
        Vector3 normal = Vector3(0.0f, 1.0f, 0.0f);
        // false marks a sample whose conform probe found no surface (off the terrain, a hole):
        // position.y and normal are then the authored fallback rather than measured ground.
        // Stations come from this polyline's 3D length, so measuring that fallback would let one
        // miss re-parameterise (and so re-count) the whole spline; the layouts hold the nearest
        // measured altitude across the gap instead (holdSurfaceAcrossGaps).
        bool surfaceValid = true;
    };

    // The single conform-miss rule (S18 R9): replaces the altitude and normal of every unmeasured
    // sample with the nearest measured one's: the run before it, or, for a leading gap (nothing
    // before it), the first measured sample after it. The gap then contributes the length of a
    // plausible surface instead of a round trip to the authored altitude. A polyline with no
    // measured sample at all is left untouched: its authored altitudes are the only surface
    // there is. The surfaceValid flags are not modified.
    JASPERENGINE_API void holdSurfaceAcrossGaps(std::span<SplineCenterSample> center);

    // The stations' path as a centerline: position and up (as the normal) per station, every
    // sample measured. The conforming caller overwrites normal and surfaceValid where it sampled
    // the terrain.
    JASPERENGINE_API void centerlineFromStations(std::span<const SplineStation> stations,
                                                 std::vector<SplineCenterSample>& out);

    // Largest per-axis scale of the transform's basis: the local-to-world factor for scalar
    // spline quantities (arc lengths, widths). Exact under uniform scale; under non-uniform scale
    // the largest axis wins, so a world quantity derived from it is never under-scaled.
    JASPERENGINE_API float largestAxisScale(const Matrix4& worldMatrix);

    // A spline's local arc length in world metres (largestAxisScale), so world-space sampling is
    // at least as fine as the step asks for under any scale.
    JASPERENGINE_API float worldCenterlineLength(float localArcLength, const Matrix4& worldMatrix);

    // Dense sample count for a world-space centerline length at the pinned step, clamped to the
    // budget. Non-positive and non-finite lengths degrade to the two-sample minimum (an infinite
    // one to the budget) rather than to an out-of-range cast.
    JASPERENGINE_API uint32_t centerlineSampleCount(float worldLength);

}}

#endif // SPLINE_SPLINECENTERLINE_H
