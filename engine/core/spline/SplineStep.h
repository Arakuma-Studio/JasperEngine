// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Include/SplineGeometry/SplineStep.h,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef SPLINE_SPLINESTEP_H
#define SPLINE_SPLINESTEP_H

#include "Export.h"
#include "math/Vector3.h"
#include "SplineStation.h"
#include <span>
#include <vector>

namespace jasperengine{ namespace spline{

    // The Stepped grade of a wall (S18 R6): each run between two consecutive authored points keeps
    // a level top, the profile's height above its highest station, so no stretch of it stands lower
    // than that, and the top steps where two runs meet, as coursed masonry does. A positive
    // `stepHeight` rounds each run's level up to a whole multiple of it (measured along `up` from 0),
    // so every step is a whole number of courses and runs within one course of each other share a
    // level.
    //
    // `pointDistances` are the distances of the stations standing on the run's interior authored
    // points, ascending; on a welded loop the seam point is the stream's two ends and is not listed.
    // A Mitre's first ring takes the run before it and its Crease ring the run after; any other
    // station on a point (a round corner's fan, a smooth point) takes the higher of the two levels and
    // gains a Crease copy on the side of the lower one, so every step is a Crease ring whose top
    // differs from the ring before it, which the strip builder closes with a riser. A welded loop
    // whose first and last runs stand apart gains a Crease copy of its last station.
    //
    // Every ring is stood plumb first (up = `up`, forward level), so each top rises straight above
    // its base. Run it after the corners and the face turn.
    JASPERENGINE_API void stepTopsAtPoints(std::vector<SplineStation>& stations, std::span<const float> pointDistances,
                                           const Vector3& up, bool closedLoop, float stepHeight = 0.0f);

}}

#endif // SPLINE_SPLINESTEP_H
