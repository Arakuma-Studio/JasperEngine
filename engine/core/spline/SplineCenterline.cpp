// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineLayout/Source/CenterlineSampling.cpp and
// TileLayout.cpp (HoldSurfaceAcrossGaps), Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors, MIT License. See NOTICE.md.

#include "SplineCenterline.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::spline;

void spline::holdSurfaceAcrossGaps(std::span<SplineCenterSample> center){
    const auto measured = std::find_if(center.begin(), center.end(),
                                       [](const SplineCenterSample& s){ return s.surfaceValid; });
    if (measured == center.end()){
        return;
    }

    for (auto it = center.begin(); it != measured; ++it){
        it->position.y = measured->position.y;
        it->normal = measured->normal;
    }
    for (auto it = measured + 1; it != center.end(); ++it){
        if (it->surfaceValid){
            continue;
        }
        it->position.y = (it - 1)->position.y;
        it->normal = (it - 1)->normal;
    }
}

void spline::centerlineFromStations(std::span<const SplineStation> stations, std::vector<SplineCenterSample>& out){
    out.resize(stations.size());
    for (size_t i = 0; i < stations.size(); i++){
        out[i].position = stations[i].position;
        out[i].normal = stations[i].up;
        out[i].surfaceValid = true;
    }
}

float spline::largestAxisScale(const Matrix4& worldMatrix){
    const auto axisScale = [&worldMatrix](int column){
        const float* axis = worldMatrix[column];
        return std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    };
    return std::max({axisScale(0), axisScale(1), axisScale(2)});
}

float spline::worldCenterlineLength(float localArcLength, const Matrix4& worldMatrix){
    return localArcLength * largestAxisScale(worldMatrix);
}

uint32_t spline::centerlineSampleCount(float worldLength){
    constexpr uint32_t minSamples = 2u;
    if (!(worldLength > 0.0f)){
        return minSamples;
    }

    // compare before casting: a length past the budget, or infinite, would otherwise narrow to
    // an out-of-range uint32
    const float steps = worldLength / SPLINE_CENTERLINE_STEP;
    if (steps >= static_cast<float>(SPLINE_MAX_CENTERLINE_SAMPLES)){
        return SPLINE_MAX_CENTERLINE_SAMPLES;
    }

    return std::max(static_cast<uint32_t>(steps) + 2u, minSamples);
}
