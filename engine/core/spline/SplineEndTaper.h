// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Include/SplineGeometry/SplineEndTaper.h,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef SPLINE_SPLINEENDTAPER_H
#define SPLINE_SPLINEENDTAPER_H

#include "Export.h"
#include "SplineStation.h"
#include <span>

namespace jasperengine{ namespace spline{

    // How a run's OPEN ends stop. Left alone each end is a square cut across the whole
    // cross-section; the taper pinches it to a point instead. It works on the final per-station
    // half-widths, whatever produced them.
    struct SplineEndTaperParams{
        // Length of the pinch at each open end, measured on SplineStation::distance. Zero (the
        // default) is off, and off means UNTOUCHED: the stations come back bit-identical.
        float taperMetres = 0.0f;
        // how far the pinched ring sinks along its station's up, so the end tucks under the
        // surface it came from instead of stopping flush with it; zero pinches in-plane
        float heightDrop = 0.0f;
        // a closed loop has no end: pinching its seam would cut the loop open
        bool closedLoop = false;
    };

    // Pinch `stations` to nothing over the last taperMetres of run at each open end, smoothly
    // (smoothstep: a linear ramp meets the run at a crease that reads as a line across a water
    // surface). Both half-widths scale by one factor, so an asymmetric channel keeps its ratio. On a
    // run shorter than two tapers every station takes the nearer end, so the pinches meet.
    JASPERENGINE_API void applyEndTaper(std::span<SplineStation> stations, const SplineEndTaperParams& params);

}}

#endif // SPLINE_SPLINEENDTAPER_H
