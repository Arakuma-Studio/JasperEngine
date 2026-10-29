// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Source/SplineStep.cpp,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#include "SplineStep.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::spline;

namespace{

    // a station this close to a point's distance stands on that point
    constexpr float ON_POINT_METRES = 1.0e-4f;

    inline float stepDot(const Vector3& a, const Vector3& b){
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    // stand a ring plumb: its up is the frame's vertical and its forward the level direction of
    // travel (right is already level)
    void plumb(SplineStation& station, const Vector3& up){
        const Vector3 level = station.forward - up * stepDot(station.forward, up);
        const float length = std::sqrt(stepDot(level, level));
        if (length > 1.0e-6f){
            station.forward = level * (1.0f / length);
        }
        station.up = up;
    }

    // the topOffset that puts a plumb station's top at `level` plus the wall's height
    float topOffsetFor(const SplineStation& station, float level, const Vector3& up){
        return level - stepDot(station.position, up);
    }

}

void spline::stepTopsAtPoints(std::vector<SplineStation>& stations, std::span<const float> pointDistances,
                              const Vector3& up, bool closedLoop, float stepHeight){
    const size_t count = stations.size();
    if (count < 2u){
        return;
    }
    for (SplineStation& station : stations){
        plumb(station, up);
    }
    const size_t pointCount = pointDistances.size();
    const size_t runCount = pointCount + 1u;

    // Each station's run, and the point it stands on (-1 for none). A station on point k starts in
    // run k, the run it ends; a Crease ring there, and any ring after it on the same point, is the
    // next run's.
    std::vector<size_t> run(count, 0u);
    std::vector<int32_t> onPoint(count, -1);
    size_t next = 0;
    for (size_t i = 0; i < count; i++){
        const float distance = stations[i].distance;
        while (next < pointCount && distance > pointDistances[next] + ON_POINT_METRES){
            next++;
        }
        run[i] = next;
        if (next < pointCount && std::abs(distance - pointDistances[next]) <= ON_POINT_METRES){
            onPoint[i] = static_cast<int32_t>(next);
        }
    }
    for (size_t i = 1; i < count; i++){
        if (onPoint[i] >= 0 && onPoint[i] == onPoint[i - 1u] &&
            (stations[i].join == SplineStationJoin::Crease || run[i - 1u] > run[i])){
            run[i] = static_cast<size_t>(onPoint[i]) + 1u;
        }
    }

    // each run's level: its highest station, counting the stations on the points at both its ends
    std::vector<float> level(runCount, std::numeric_limits<float>::lowest());
    for (size_t i = 0; i < count; i++){
        const float height = stepDot(stations[i].position, up);
        level[run[i]] = std::max(level[run[i]], height);
        if (onPoint[i] >= 0){
            const size_t k = static_cast<size_t>(onPoint[i]);
            level[k] = std::max(level[k], height);
            level[k + 1u] = std::max(level[k + 1u], height);
        }
    }
    if (std::isfinite(stepHeight) && stepHeight > 0.0f){
        for (float& value : level){
            if (value != std::numeric_limits<float>::lowest()){
                value = std::ceil(value / stepHeight) * stepHeight;
            }
        }
    }

    std::vector<SplineStation> stepped;
    stepped.reserve(count + runCount);
    size_t i = 0;
    while (i < count){
        if (onPoint[i] < 0){
            stepped.push_back(stations[i]);
            stepped.back().topOffset = topOffsetFor(stations[i], level[run[i]], up);
            i++;
            continue;
        }

        // the stations standing on one point
        const size_t first = i;
        size_t last = i;
        while (last + 1u < count && onPoint[last + 1u] == onPoint[first]){
            last++;
        }
        i = last + 1u;
        const size_t k = static_cast<size_t>(onPoint[first]);
        const float before = level[k];
        const float after = level[k + 1u];

        // a Mitre already splits into the run before and the run after at its Crease ring
        if (run[last] != run[first]){
            for (size_t s = first; s <= last; s++){
                stepped.push_back(stations[s]);
                stepped.back().topOffset = topOffsetFor(stations[s], level[run[s]], up);
            }
            continue;
        }

        // anything else on the point stands at the higher level, and a Crease copy on the lower
        // side carries the step
        const float high = std::max(before, after);
        if (before < after){
            stepped.push_back(stations[first]);
            stepped.back().topOffset = topOffsetFor(stations[first], before, up);
        }
        for (size_t s = first; s <= last; s++){
            stepped.push_back(stations[s]);
            if (s == first && before < after){
                stepped.back().join = SplineStationJoin::Crease;
            }
            stepped.back().topOffset = topOffsetFor(stations[s], high, up);
        }
        if (before > after){
            stepped.push_back(stations[last]);
            stepped.back().join = SplineStationJoin::Crease;
            stepped.back().topOffset = topOffsetFor(stations[last], after, up);
        }
    }

    // a welded loop's seam is a step when its first and last runs stand apart
    if (closedLoop && level.front() != level.back()){
        SplineStation seam = stepped.back();
        seam.join = SplineStationJoin::Crease;
        seam.topOffset = topOffsetFor(seam, level.front(), up);
        stepped.push_back(seam);
    }
    stations = std::move(stepped);
}
