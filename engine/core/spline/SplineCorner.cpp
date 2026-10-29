// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Source/SplineCorner.cpp,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#include "SplineCorner.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

using namespace jasperengine;
using namespace jasperengine::spline;

namespace{

    constexpr float DEGREES_PER_RADIAN = 180.0f / std::numbers::pi_v<float>;

    // relative slack on the mitre limit, so a corner authored at exactly the limit (whose float
    // cos(60) makes the scale 2.0000001) is the Mitre the limit promises: about 0.007 degrees
    constexpr float MITRE_LIMIT_TOLERANCE = 1.0e-4f;

    // below this turn the two legs are one straight run: a crease would split coplanar normals
    constexpr float MIN_CORNER_TURN_RADIANS = 1.0e-3f;

    // nearest a neighbouring station may stand to the corner and still name its leg's direction
    constexpr float MIN_LEG_PROBE_METRES = 0.05f;

    // below this a turn between two stations' forwards is the rounding of sampled positions, not
    // a bend (a real bend this gentle has a radius past 50 km)
    constexpr float MIN_FACE_TURN_RADIANS = 1.0e-5f;

    // a turn measured from sampled positions lands a few ULPs off a whole number of steps
    constexpr float ROUND_CORNER_RING_TOLERANCE = 1.0e-3f;

    // Shortest band the inside face keeps before (and after) the inner mitre point: a station a
    // hair beyond it leaves a band too short to see that still carries its whole U span.
    constexpr float MIN_INSIDE_BAND_METRES = 0.05f;

    const Vector3 CORNER_FALLBACK_FORWARD(0.0f, 0.0f, -1.0f);
    const Vector3 CORNER_FALLBACK_RIGHT(1.0f, 0.0f, 0.0f);

    inline Vector3 crossOf(const Vector3& a, const Vector3& b){
        return Vector3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    }

    inline float dotOf(const Vector3& a, const Vector3& b){
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    Vector3 normalizedOr(const Vector3& v, const Vector3& fallback){
        const float lengthSquared = dotOf(v, v);
        if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-12f){
            return fallback;
        }
        return v * (1.0f / std::sqrt(lengthSquared));
    }

    Vector3 inPlane(const Vector3& v, const Vector3& up){
        return v - up * dotOf(v, up);
    }

    // The sweep's frame: right is the right of travel in the plane the turn is measured in,
    // normalize(forward x up), and up completes the basis, so a forward that climbs tilts up with it.
    struct Frame{
        Vector3 forward;
        Vector3 right;
        Vector3 up;
    };

    Frame frameAlong(const Vector3& forward, const Vector3& up){
        Frame frame;
        frame.forward = forward;
        frame.right = normalizedOr(crossOf(inPlane(forward, up), up), CORNER_FALLBACK_RIGHT);
        frame.up = crossOf(frame.right, frame.forward);
        return frame;
    }

    // The turn from `from` to `to` in radians in the plane `up` defines: positive toward the right
    // of travel, zero below MIN_FACE_TURN_RADIANS so a straight run gains no face turn at all.
    float planTurn(const Vector3& from, const Vector3& to, const Vector3& up){
        const Vector3 a = inPlane(from, up);
        const Vector3 b = inPlane(to, up);
        const float turn = std::atan2(dotOf(crossOf(b, a), up), dotOf(a, b));
        return std::abs(turn) < MIN_FACE_TURN_RADIANS ? 0.0f : turn;
    }

    // rise per metre of plan travel along a direction
    float gradeOf(const Vector3& direction, const Vector3& up){
        const Vector3 plan = inPlane(direction, up);
        const float planLength = std::sqrt(dotOf(plan, plan));
        return planLength > 1.0e-6f ? dotOf(direction, up) / planLength : 0.0f;
    }

    // one corner, measured once from the stream as sampled
    struct CornerTurn{
        Vector3 in;      // direction of the incoming leg
        Vector3 out;     // direction of the outgoing leg
        Vector3 inPlan;  // the same in the turn's plane, unit length
        Vector3 outPlan;
        float turn = 0.0f; // radians, 0 = straight on, pi = a hairpin
        bool rightTurn = true;
        bool buildRound = false;
        bool pastMitreLimit = false;
        bool fallback = false; // a Mitre past the limit
        // reach shortened to what the legs hold, so the inside face narrows toward the corner
        // instead of folding past a leg
        bool bounded = false;
        // lateral axis placing the faces of both legs on one edge: the bisector's right scaled by
        // 1 / cos(theta/2), which puts the inside at the inner mitre point
        Vector3 mitreRight;
        // how far before and after the corner, along each leg, the inside faces meet
        float reach = 0.0f;
        // the same per metre of half-width
        float reachPerMetre = 0.0f;
    };

    // the station a leg's direction is read from: the nearest at least MIN_LEG_PROBE_METRES of run
    // away from the corner, walking `step` (-1 back, +1 ahead) and wrapping round a closed loop
    uint32_t legProbe(std::span<const SplineStation> stations, uint32_t corner, int32_t step, bool closedLoop){
        const uint32_t count = static_cast<uint32_t>(stations.size());
        // on a closed loop the last station IS the first, so walking back from station 0 starts at
        // the end, whose distance is the loop's length
        const bool wrapsBack = closedLoop && corner == 0u && step < 0;
        const float cornerDistance = wrapsBack ? stations[count - 1u].distance : stations[corner].distance;
        int32_t index = wrapsBack ? static_cast<int32_t>(count) - 1 : static_cast<int32_t>(corner);
        const int32_t last = static_cast<int32_t>(count) - 1;
        for (;;){
            const int32_t next = index + step;
            if (next < 0 || next > last){
                return static_cast<uint32_t>(index);
            }
            index = next;
            if (std::abs(stations[static_cast<uint32_t>(index)].distance - cornerDistance) >= MIN_LEG_PROBE_METRES){
                return static_cast<uint32_t>(index);
            }
        }
    }

    CornerTurn measureCorner(std::span<const SplineStation> stations, uint32_t corner, SplineCornerStyle style,
                             const Vector3& up, bool closedLoop){
        const SplineStation& here = stations[corner];
        const SplineStation& before = stations[legProbe(stations, corner, -1, closedLoop)];
        const SplineStation& after = stations[legProbe(stations, corner, +1, closedLoop)];

        CornerTurn turn;
        turn.in = normalizedOr(here.position - before.position, here.forward);
        turn.out = normalizedOr(after.position - here.position, here.forward);
        turn.inPlan = normalizedOr(inPlane(turn.in, up), CORNER_FALLBACK_FORWARD);
        turn.outPlan = normalizedOr(inPlane(turn.out, up), turn.inPlan);
        turn.turn = std::acos(std::clamp(dotOf(turn.inPlan, turn.outPlan), -1.0f, 1.0f));
        // cross(out, in) points along +up for a turn toward the right of travel
        turn.rightTurn = dotOf(crossOf(turn.outPlan, turn.inPlan), up) >= 0.0f;

        const float halfTurnCosine = std::cos(turn.turn * 0.5f);
        const float mitreScale = halfTurnCosine > 1.0e-6f ? 1.0f / halfTurnCosine : std::numeric_limits<float>::max();
        // past the limit a Mitre is built Round, and its inner mitre point runs away toward a
        // hairpin: applyCorners bounds its reach by the legs
        turn.pastMitreLimit = mitreScale > SPLINE_MITRE_LIMIT * (1.0f + MITRE_LIMIT_TOLERANCE);
        turn.fallback = style == SplineCornerStyle::Mitre && turn.pastMitreLimit;
        turn.buildRound = style == SplineCornerStyle::Round || turn.fallback;

        const Vector3 bisector = normalizedOr(turn.inPlan + turn.outPlan, turn.inPlan);
        turn.mitreRight = normalizedOr(crossOf(bisector, up), CORNER_FALLBACK_RIGHT) * mitreScale;
        const float insideHalfWidth = turn.rightTurn ? here.halfWidthRight : here.halfWidthLeft;
        turn.reachPerMetre = std::tan(turn.turn * 0.5f);
        turn.reach = std::max(0.0f, insideHalfWidth) * turn.reachPerMetre;
        return turn;
    }

    // Stamp a corner ring with its face turn relative to the leg before the corner
    // (accumulateFaceTurn makes it cumulative): `outsideTurn` on the side it turns away from, the
    // inside face's shortfall on the side it turns toward.
    void stampCornerFaceTurn(SplineStation& ring, const CornerTurn& turn, float outsideTurn){
        const float inside = -turn.reachPerMetre;
        ring.cornerTurnLeft = turn.rightTurn ? outsideTurn : inside;
        ring.cornerTurnRight = turn.rightTurn ? inside : outsideTurn;
    }

    // the ring every corner ring starts from: the corner station itself (width, roll and distance
    // carry over), placed with the corner's shared vertical
    SplineStation cornerRing(const SplineStation& corner, const Frame& frame, const Vector3& cornerUp){
        SplineStation ring = corner;
        ring.forward = frame.forward;
        ring.right = frame.right;
        // one vertical for every ring of the corner, so rings sharing a position share every vertex
        // position: the legs' own ups differ when they climb at different grades
        ring.up = cornerUp;
        return ring;
    }

    // a Mitre: the corner ring twice, shaded once as the incoming leg and once as the outgoing one
    void appendMitre(std::vector<SplineStation>& out, const SplineStation& corner, const CornerTurn& turn,
                     const Vector3& up, bool incomingHalf, bool outgoingHalf){
        const Frame in = frameAlong(turn.in, up);
        const Frame outgoing = frameAlong(turn.out, up);
        const Vector3 cornerUp = normalizedOr(in.up + outgoing.up, up);

        SplineStation incoming = cornerRing(corner, in, cornerUp);
        incoming.mitreRight = turn.mitreRight;
        incoming.mitreSides = SPLINE_MITRE_BOTH_SIDES;
        // the outer face reaches the mitre edge tan(theta/2) per metre of half-width beyond the
        // centreline, and the inner one falls as far short of it
        stampCornerFaceTurn(incoming, turn, turn.reachPerMetre);
        SplineStation leaving = cornerRing(corner, outgoing, cornerUp);
        leaving.mitreRight = turn.mitreRight;
        leaving.mitreSides = SPLINE_MITRE_BOTH_SIDES;
        stampCornerFaceTurn(leaving, turn, turn.reachPerMetre);

        if (incomingHalf){
            out.push_back(incoming);
        }
        if (outgoingHalf){
            // a crease straight after the incoming ring; opening a stream (a closed loop's seam)
            // there is nothing before it to crease against
            leaving.join = incomingHalf ? SplineStationJoin::Crease : corner.join;
            out.push_back(leaving);
        }
    }

    // intervals of a Round corner's fan: one fewer than its rings
    uint32_t roundSegments(const CornerTurn& turn){
        const float steps = turn.turn * DEGREES_PER_RADIAN / SPLINE_ROUND_CORNER_STEP_DEGREES;
        const uint32_t rings = std::clamp(static_cast<uint32_t>(std::ceil(steps - ROUND_CORNER_RING_TOLERANCE)),
                                          SPLINE_MIN_ROUND_CORNER_RINGS, SPLINE_MAX_ROUND_CORNER_RINGS);
        return rings - 1u;
    }

    // a Round: a fan of rings turning about the corner point, the outside sweeping an arc of the
    // wall's own half-width and the inside staying on the inner mitre point
    void appendRound(std::vector<SplineStation>& out, const SplineStation& corner, const CornerTurn& turn,
                     const Vector3& up, uint32_t firstRing){
        const Frame in = frameAlong(turn.in, up);
        const Frame outgoing = frameAlong(turn.out, up);
        const Vector3 cornerUp = normalizedOr(in.up + outgoing.up, up);

        const uint32_t segments = roundSegments(turn);
        // toward the inside of the turn, perpendicular to the incoming leg
        const Vector3 rightOfTravel = normalizedOr(crossOf(turn.inPlan, up), CORNER_FALLBACK_RIGHT);
        const Vector3 towardTurn = turn.rightTurn ? rightOfTravel : rightOfTravel * -1.0f;
        const float gradeIn = gradeOf(turn.in, up);
        const float gradeOut = gradeOf(turn.out, up);
        const uint8_t insideSide = turn.rightTurn ? SPLINE_MITRE_RIGHT_SIDE : SPLINE_MITRE_LEFT_SIDE;

        for (uint32_t i = firstRing; i <= segments; i++){
            Vector3 forward;
            if (i == 0u){
                forward = turn.in;
            }else if (i == segments){
                forward = turn.out;
            }else{
                const float fraction = static_cast<float>(i) / static_cast<float>(segments);
                const float angle = turn.turn * fraction;
                const Vector3 plan = turn.inPlan * std::cos(angle) + towardTurn * std::sin(angle);
                const float grade = gradeIn + (gradeOut - gradeIn) * fraction;
                forward = normalizedOr(plan + up * grade, plan);
            }
            SplineStation ring = cornerRing(corner, frameAlong(forward, up), cornerUp);
            ring.mitreRight = turn.mitreRight;
            ring.mitreSides = insideSide;
            // the outside sweeps an arc of the half-width: radians of turn so far
            stampCornerFaceTurn(ring, turn, turn.turn * static_cast<float>(i) / static_cast<float>(segments));
            ring.join = (i == firstRing) ? corner.join : SplineStationJoin::Pivot;
            out.push_back(ring);
        }
    }

    // Mark the stations between a corner and the inner mitre point on each leg, and those within
    // MIN_INSIDE_BAND_METRES beyond it. Scans stop at another corner and never take a stream end.
    void markWithinReach(std::span<const SplineStation> stations, uint32_t corner, float reach, bool closedLoop,
                         const std::vector<uint8_t>& isSite, std::vector<uint8_t>& drop){
        if (reach <= 0.0f){
            return;
        }
        reach += MIN_INSIDE_BAND_METRES;
        const uint32_t count = static_cast<uint32_t>(stations.size());
        const float ahead = stations[corner].distance;
        for (uint32_t i = corner + 1u; i + 1u < count; i++){
            if (isSite[i] || stations[i].distance - ahead > reach){
                break;
            }
            drop[i] = 1u;
        }
        const bool wrapsBack = closedLoop && corner == 0u;
        const float behind = wrapsBack ? stations[count - 1u].distance : ahead;
        const uint32_t start = wrapsBack ? count - 1u : corner;
        for (uint32_t i = start; i-- > 1u;){
            if (isSite[i] || behind - stations[i].distance > reach){
                break;
            }
            drop[i] = 1u;
        }
    }

    // Bound each corner past the mitre limit to the reach its legs hold: where a leg is shorter
    // than halfWidth * tan(theta/2), the inside point moves in along the bisector until it stands
    // only as far back as the shorter leg allows, and the inside face narrows toward the corner
    // rather than folding back past the leg. A leg holds its length less the inside band, and
    // toward a corner that turns the same way, less that corner's reach too.
    void boundReachPastTheLimit(std::span<const SplineStation> stations, std::span<CornerTurn> turns,
                                std::span<const SplineCornerSite> sites, bool closedLoop){
        const size_t count = turns.size();
        const float runStart = stations.front().distance;
        const float runEnd = stations.back().distance;
        const float loopLength = runEnd - runStart;
        const auto at = [&](size_t t){ return stations[sites[t].stationIndex].distance; };
        // room on the leg from corner t toward its neighbour `step` away: the neighbouring corner,
        // or the open end
        const auto room = [&](size_t t, int32_t step) -> float{
            const bool hasNeighbour = closedLoop ? count > 1u : (step < 0 ? t > 0u : t + 1u < count);
            if (!hasNeighbour){
                if (closedLoop){
                    return 0.5f * loopLength - MIN_INSIDE_BAND_METRES;
                }
                const float leg = step < 0 ? at(t) - runStart : runEnd - at(t);
                return leg - MIN_INSIDE_BAND_METRES;
            }
            const size_t other = step < 0 ? (t + count - 1u) % count : (t + 1u) % count;
            float leg = step < 0 ? at(t) - at(other) : at(other) - at(t);
            if (leg < 0.0f){
                leg += loopLength;
            }
            const float shared = turns[other].rightTurn == turns[t].rightTurn ? turns[other].reach : 0.0f;
            return leg - shared - MIN_INSIDE_BAND_METRES;
        };
        for (size_t t = 0; t < count; t++){
            CornerTurn& turn = turns[t];
            if (!turn.pastMitreLimit){
                continue;
            }
            const float held = std::max(0.0f, std::min(room(t, -1), room(t, +1)));
            if (turn.reach <= held){
                continue;
            }
            const SplineStation& here = stations[sites[t].stationIndex];
            const float insideHalfWidth = turn.rightTurn ? here.halfWidthRight : here.halfWidthLeft;
            const float halfTurnSine = std::sin(turn.turn * 0.5f);
            if (!(insideHalfWidth > 0.0f) || !(halfTurnSine > 0.0f)){
                continue;
            }
            turn.reach = held;
            turn.reachPerMetre = held / insideHalfWidth;
            // a point s * halfWidth out along the bisector's right stands s * halfWidth * sin(theta/2)
            // back along each leg
            const float scale = held / (insideHalfWidth * halfTurnSine);
            turn.mitreRight = normalizedOr(turn.mitreRight, CORNER_FALLBACK_RIGHT) * scale;
            turn.bounded = true;
        }
    }

    // Report the legs too short for their corners' reach: between a corner and an open end of the
    // run, and between two corners that turn the same way (opposite turns put their inside faces on
    // opposite sides of the leg). A corner whose reach was bounded fits its legs by construction.
    void reportFoldingLegs(std::span<const SplineStation> stations, std::span<const CornerTurn> turns,
                           std::span<const SplineCornerSite> sites, bool closedLoop, std::vector<SplineCornerIssue>& issues){
        const size_t count = turns.size();
        const float runStart = stations.front().distance;
        const float runEnd = stations.back().distance;
        const float loopLength = runEnd - runStart;
        for (size_t t = 0; t < count; t++){
            const float at = stations[sites[t].stationIndex].distance;
            const float reach = turns[t].reach;
            if (!closedLoop){
                const bool first = t == 0u;
                const bool last = t + 1u == count;
                if (!turns[t].bounded && ((first && at - runStart < reach + MIN_INSIDE_BAND_METRES) ||
                                          (last && runEnd - at < reach + MIN_INSIDE_BAND_METRES))){
                    issues.push_back({SplineCornerIssueKind::LegShorterThanReach, sites[t].pointIndex, 0u,
                                      turns[t].turn * DEGREES_PER_RADIAN});
                }
            }
            if (t + 1u == count && (!closedLoop || count < 2u)){
                continue;
            }
            const size_t next = (t + 1u) % count;
            if (turns[next].rightTurn != turns[t].rightTurn || turns[t].bounded || turns[next].bounded){
                continue;
            }
            float leg = stations[sites[next].stationIndex].distance - at;
            if (next == 0u){
                leg += loopLength;
            }
            if (leg < reach + turns[next].reach + MIN_INSIDE_BAND_METRES){
                issues.push_back({SplineCornerIssueKind::CornersOverlap, sites[t].pointIndex, sites[next].pointIndex,
                                  turns[t].turn * DEGREES_PER_RADIAN});
            }
        }
    }

}

std::vector<SplineCornerIssue> spline::applyCorners(std::vector<SplineStation>& stations,
        std::span<const SplineCornerSite> sites, SplineCornerStyle style, const Vector3& up, bool closedLoop){
    std::vector<SplineCornerIssue> issues;
    const uint32_t count = static_cast<uint32_t>(stations.size());
    if (style == SplineCornerStyle::None || sites.empty() || count < 3u){
        return issues;
    }

    // every corner is measured on the stream as sampled, before any of them reshapes it
    std::vector<uint8_t> isSite(count, 0u);
    std::vector<int32_t> turnOfStation(count, -1);
    std::vector<CornerTurn> turns;
    std::vector<SplineCornerSite> turnSites;
    turns.reserve(sites.size());
    turnSites.reserve(sites.size());
    for (const SplineCornerSite& site : sites){
        const uint32_t index = site.stationIndex;
        const bool interior = index > 0u && index + 1u < count;
        const bool seam = closedLoop && index == 0u;
        if (!interior && !seam){
            continue;
        }
        const CornerTurn turn = measureCorner(stations, index, style, up, closedLoop);
        if (turn.turn < MIN_CORNER_TURN_RADIANS){
            continue;
        }
        if (turn.fallback){
            issues.push_back({SplineCornerIssueKind::PastMitreLimit, site.pointIndex, 0u, turn.turn * DEGREES_PER_RADIAN});
        }
        isSite[index] = 1u;
        turnOfStation[index] = static_cast<int32_t>(turns.size());
        turns.push_back(turn);
        turnSites.push_back(site);
    }
    if (turns.empty()){
        return issues;
    }
    boundReachPastTheLimit(stations, turns, turnSites, closedLoop);
    reportFoldingLegs(stations, turns, turnSites, closedLoop, issues);

    std::vector<uint8_t> drop(count, 0u);
    for (uint32_t i = 0; i < count; i++){
        if (turnOfStation[i] >= 0){
            markWithinReach(stations, i, turns[static_cast<uint32_t>(turnOfStation[i])].reach, closedLoop, isSite, drop);
        }
    }

    // the seam corner of a closed loop opens the stream with its outgoing half and closes it with
    // its incoming half
    const int32_t seamTurn = closedLoop ? turnOfStation[0] : -1;

    std::vector<SplineStation> reshaped;
    reshaped.reserve(count + turns.size() * (SPLINE_MAX_ROUND_CORNER_RINGS + 1u));
    for (uint32_t i = 0; i < count; i++){
        if (drop[i]){
            continue;
        }
        const bool closingSeam = seamTurn >= 0 && i + 1u == count;
        const int32_t turnIndex = closingSeam ? seamTurn : turnOfStation[i];
        if (turnIndex < 0){
            reshaped.push_back(stations[i]);
            continue;
        }
        const CornerTurn& turn = turns[static_cast<uint32_t>(turnIndex)];
        const bool openingSeam = seamTurn >= 0 && i == 0u;
        if (turn.buildRound){
            appendRound(reshaped, stations[i], turn, up, openingSeam ? roundSegments(turn) : 0u);
        }else{
            appendMitre(reshaped, stations[i], turn, up, !openingSeam, !closingSeam);
        }
    }
    stations = std::move(reshaped);
    return issues;
}

std::vector<uint32_t> spline::findInnerFolds(std::span<const SplineStation> stations, std::span<const uint8_t> skip,
                                             const Vector3& up){
    std::vector<uint32_t> folds;
    for (size_t i = 1; i + 1u < stations.size(); i++){
        if (i < skip.size() && skip[i] != 0u){
            continue;
        }
        const Vector3 a = inPlane(stations[i - 1u].position, up);
        const Vector3 b = inPlane(stations[i].position, up);
        const Vector3 c = inPlane(stations[i + 1u].position, up);
        const Vector3 ab = b - a;
        const Vector3 bc = c - b;
        const Vector3 ca = a - c;
        // twice the triangle's signed area, positive for a bend to the right
        const float twiceArea = dotOf(crossOf(bc, ab), up);
        if (std::abs(twiceArea) < 1.0e-12f){
            continue;
        }
        const float radius = std::sqrt(dotOf(ab, ab) * dotOf(bc, bc) * dotOf(ca, ca)) / (2.0f * std::abs(twiceArea));
        const float insideHalfWidth = twiceArea > 0.0f ? stations[i].halfWidthRight : stations[i].halfWidthLeft;
        if (radius < insideHalfWidth){
            folds.push_back(static_cast<uint32_t>(i));
        }
    }
    return folds;
}

std::vector<SplineCornerIssue> spline::innerFoldIssues(std::span<const SplineStation> stations,
        std::span<const uint32_t> folds, uint32_t knotCount, bool closedSpline){
    std::vector<SplineCornerIssue> issues;
    if (knotCount < 2u){
        return issues;
    }
    for (const uint32_t fold : folds){
        if (fold >= stations.size()){
            continue;
        }
        const float t = stations[fold].splineT;
        if (!(t >= 0.0f)){
            continue;
        }
        uint32_t knot = std::min(static_cast<uint32_t>(std::floor(t)), knotCount - 1u);
        uint32_t other = knot + 1u;
        if (closedSpline){
            other = other % knotCount;
        }else if (other >= knotCount){
            other = knot;
            knot = knot - 1u;
        }
        const bool reported = std::any_of(issues.begin(), issues.end(), [&](const SplineCornerIssue& issue){
            return issue.pointIndex == knot && issue.otherPointIndex == other;
        });
        if (!reported){
            issues.push_back({SplineCornerIssueKind::InnerFold, knot, other, 0.0f});
        }
    }
    return issues;
}

void spline::accumulateFaceTurn(std::span<SplineStation> stations, const Vector3& up){
    // Running face turn per side over the plain stations. A corner's rings are measured from the
    // base the leg brought them to; past the corner a side whose rings stand on the mitre point
    // gains its stamp once more (the other leg's stretch to or from that point), while a side that
    // swept an arc ends where its last ring stands.
    float left = 0.0f;
    float right = 0.0f;
    float baseLeft = 0.0f;
    float baseRight = 0.0f;
    float exitLeft = 0.0f;
    float exitRight = 0.0f;
    // the top's arc allowance: running, as the corner opened, and as it leaves
    float allowance = 0.0f;
    float baseAllowance = 0.0f;
    float exitAllowance = 0.0f;
    bool inCorner = false;
    Vector3 previousForward = stations.empty() ? Vector3() : stations.front().forward;
    for (size_t i = 0; i < stations.size(); i++){
        SplineStation& station = stations[i];
        const float turn = i == 0u ? 0.0f : planTurn(previousForward, station.forward, up);
        previousForward = station.forward;
        const bool cornerRingHere = station.mitreSides != 0u;
        const bool opensCorner = cornerRingHere && (!inCorner || station.join == SplineStationJoin::Advance);
        if (inCorner && (!cornerRingHere || opensCorner)){
            left = baseLeft + exitLeft;
            right = baseRight + exitRight;
            allowance = baseAllowance + exitAllowance;
            inCorner = false;
        }
        if (!cornerRingHere){
            left += turn;
            right -= turn;
            station.faceTurnLeft = left;
            station.faceTurnRight = right;
            station.topArcAllowance = allowance;
            continue;
        }

        const float stampLeft = station.cornerTurnLeft;
        const float stampRight = station.cornerTurnRight;
        // a round fan's rings stand on the mitre point on the inside only; their outside sweeps an
        // arc of the outside half-width, `arc` so far
        float arc = 0.0f;
        if (station.mitreSides == SPLINE_MITRE_RIGHT_SIDE){
            arc = station.halfWidthLeft * stampLeft;
        }else if (station.mitreSides == SPLINE_MITRE_LEFT_SIDE){
            arc = station.halfWidthRight * stampRight;
        }
        if (opensCorner){
            // a stream that opens on a corner ring starts at zero there
            baseLeft = i == 0u ? -stampLeft : left + turn;
            baseRight = i == 0u ? -stampRight : right - turn;
            baseAllowance = i == 0u ? -arc : allowance;
            inCorner = true;
        }
        station.topArcAllowance = baseAllowance + arc;
        exitAllowance = arc;
        station.faceTurnLeft = baseLeft + stampLeft;
        station.faceTurnRight = baseRight + stampRight;
        exitLeft = (station.mitreSides & SPLINE_MITRE_LEFT_SIDE) != 0u ? 2.0f * stampLeft : stampLeft;
        exitRight = (station.mitreSides & SPLINE_MITRE_RIGHT_SIDE) != 0u ? 2.0f * stampRight : stampRight;
    }
}

void spline::closeStationLoop(std::span<SplineStation> stations, const Vector3& up){
    const size_t count = stations.size();
    if (count < 3u){
        return;
    }
    // station 0 and the last are the same place, so the frame there looks from the station before
    // the end to the one after the start
    const Vector3 across = normalizedOr(stations[1].position - stations[count - 2u].position, stations[0].forward);
    const Frame frame = frameAlong(across, up);
    stations[0].forward = frame.forward;
    stations[0].right = frame.right;
    stations[0].up = frame.up;

    const float loopLength = stations[count - 1u].distance;
    const float loopT = stations[count - 1u].splineT;
    stations[count - 1u] = stations[0];
    stations[count - 1u].distance = loopLength;
    stations[count - 1u].splineT = loopT;
}
