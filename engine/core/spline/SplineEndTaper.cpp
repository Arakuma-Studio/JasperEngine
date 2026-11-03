// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Source/SplineEndTaper.cpp,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#include "SplineEndTaper.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::spline;

void spline::applyEndTaper(std::span<SplineStation> stations, const SplineEndTaperParams& params){
    if (params.closedLoop || stations.size() < 2u){
        return;
    }
    // off is a full early-out rather than a scale of one: "off changes nothing" is the contract
    if (!std::isfinite(params.taperMetres) || !(params.taperMetres > 0.0f)){
        return;
    }

    const float front = stations.front().distance;
    const float back = stations.back().distance;
    if (!std::isfinite(front) || !std::isfinite(back) || !(back > front)){
        return;
    }

    const float drop = std::isfinite(params.heightDrop) ? params.heightDrop : 0.0f;

    for (SplineStation& station : stations){
        // whichever end is nearer owns this station, which lets the two pinches meet on a short run
        const float fromEnd = std::min(station.distance - front, back - station.distance);
        // a rejection, so a non-finite distance falls through untouched instead of scaling by NaN
        if (!(fromEnd < params.taperMetres)){
            continue;
        }

        const float t = std::clamp(fromEnd / params.taperMetres, 0.0f, 1.0f);
        const float scale = t * t * (3.0f - 2.0f * t);
        station.halfWidthLeft *= scale;
        station.halfWidthRight *= scale;
        station.position = station.position - station.up * (drop * (1.0f - scale));
    }
}
