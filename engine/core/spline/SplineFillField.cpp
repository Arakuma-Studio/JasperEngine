// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Source/SplineFillField.cpp,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#include "SplineFillField.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::spline;

namespace{

    bool finitePlanar(const SplineStation& station){
        return std::isfinite(station.position.x) && std::isfinite(station.position.y) && std::isfinite(station.position.z);
    }

    // a lattice coordinate as a corner index clamped to [low, high]; NaN lands on low
    int32_t clampCorner(float coordinate, int32_t low, int32_t high){
        if (!(coordinate >= static_cast<float>(low))){
            return low;
        }
        if (coordinate > static_cast<float>(high)){
            return high;
        }
        return static_cast<int32_t>(coordinate);
    }

    // Waterline at an arc position, linearly interpolated between the two stations bracketing it
    // and CLAMPED at both ends: past the last station the field holds its height, so a fill that
    // reaches open ground stops where the ground rises to it instead of chasing the slope away.
    float waterlineAtArc(std::span<const SplineStation> stations, float arc){
        if (arc <= stations.front().distance){
            return stations.front().position.y;
        }
        if (arc >= stations.back().distance){
            return stations.back().position.y;
        }
        const auto upper = std::upper_bound(stations.begin(), stations.end(), arc,
                                            [](float value, const SplineStation& s){ return value < s.distance; });
        const size_t hi = static_cast<size_t>(upper - stations.begin());
        const SplineStation& a = stations[hi - 1u];
        const SplineStation& b = stations[hi];
        const float span = b.distance - a.distance;
        if (!(span > 0.0f)){
            return a.position.y;
        }
        const float t = (arc - a.distance) / span;
        return a.position.y + (b.position.y - a.position.y) * t;
    }

    // horizontal unit forward and right of a station, falling back where its frame is degenerate in
    // plan (a run going straight up has no XZ travel): right = forward x up in XZ is (-fz, fx)
    void planarFrame(const SplineStation& station, float& fx, float& fz, float& rx, float& rz){
        fx = station.forward.x;
        fz = station.forward.z;
        const float forwardLength = std::sqrt(fx * fx + fz * fz);
        if (forwardLength > 1.0e-6f){
            fx /= forwardLength;
            fz /= forwardLength;
        }else{
            fx = 0.0f;
            fz = -1.0f;
        }

        rx = station.right.x;
        rz = station.right.z;
        const float rightLength = std::sqrt(rx * rx + rz * rz);
        if (rightLength > 1.0e-6f){
            rx /= rightLength;
            rz /= rightLength;
        }else{
            rx = -fz;
            rz = fx;
        }
    }

}

SplineFillResult spline::buildSplineFillField(std::span<const SplineStation> stations, const SplineGroundGrid& ground,
                                              const SplineFillParams& params){
    SplineFillResult result;
    if (stations.empty() || !ground.isValid()){
        return result;
    }

    const float reach = std::max(0.0f, params.maxHalfWidth);
    const float tuck = std::max(0.0f, params.edgeDrop);
    if (!(reach > 0.0f) || !std::isfinite(reach)){
        return result;
    }

    // the floor stands a hair above the level it names, so a mouth meeting a sea sits ON it
    const float seaFloor = params.seaLevelFloor + SPLINE_SEA_LEVEL_FLOOR_LIFT_METRES;

    // The halo matters: a corner just OUTSIDE the corridor still needs a waterline and a distance,
    // because the mesher interpolates the region boundary across the edge leaving the corridor.
    const float halo = std::sqrt(ground.spacingX * ground.spacingX + ground.spacingZ * ground.spacingZ);
    const float stampReach = reach + halo;
    const float stampReachSq = stampReach * stampReach;

    // ---- The block of corners the stamp can reach, plus one corner of margin ----
    float minX = std::numeric_limits<float>::infinity();
    float maxX = -std::numeric_limits<float>::infinity();
    float minZ = std::numeric_limits<float>::infinity();
    float maxZ = -std::numeric_limits<float>::infinity();
    for (const SplineStation& station : stations){
        if (!finitePlanar(station)){
            continue;
        }
        minX = std::min(minX, station.position.x);
        maxX = std::max(maxX, station.position.x);
        minZ = std::min(minZ, station.position.z);
        maxZ = std::max(maxZ, station.position.z);
    }
    if (!(minX <= maxX) || !(minZ <= maxZ)){
        return result;
    }
    const int32_t lastX = static_cast<int32_t>(ground.countX) - 1;
    const int32_t lastZ = static_cast<int32_t>(ground.countZ) - 1;
    // The margin keeps every corner on the block's border unstamped, so the block's border stands in
    // for the grid's: the escape and drain scans, which only ever cross unstamped dry corners out
    // there, give every corner of the block what a scan of the whole grid would.
    const int32_t x0 = clampCorner(std::floor((minX - stampReach - ground.originX) / ground.spacingX) - 1.0f, 0, lastX);
    const int32_t x1 = clampCorner(std::ceil((maxX + stampReach - ground.originX) / ground.spacingX) + 1.0f, 0, lastX);
    const int32_t z0 = clampCorner(std::floor((minZ - stampReach - ground.originZ) / ground.spacingZ) - 1.0f, 0, lastZ);
    const int32_t z1 = clampCorner(std::ceil((maxZ + stampReach - ground.originZ) / ground.spacingZ) + 1.0f, 0, lastZ);
    const uint32_t wx = static_cast<uint32_t>(x1 - x0 + 1);
    const uint32_t wz = static_cast<uint32_t>(z1 - z0 + 1);
    const uint32_t cornerCount = wx * wz;
    result.minCornerX = static_cast<uint32_t>(x0);
    result.minCornerZ = static_cast<uint32_t>(z0);
    result.countX = wx;
    result.countZ = wz;
    result.corners.assign(cornerCount, SplineFillCorner{});

    // the bed under the block, and each block corner's world position
    std::vector<float> bed(cornerCount);
    for (uint32_t lz = 0; lz < wz; lz++){
        const float* row = ground.heights.data() + ground.index(static_cast<uint32_t>(x0), static_cast<uint32_t>(z0) + lz);
        std::copy(row, row + wx, bed.begin() + static_cast<std::ptrdiff_t>(lz * wx));
    }
    const auto worldX = [&](uint32_t local){ return ground.worldX(static_cast<uint32_t>(x0) + local % wx); };
    const auto worldZ = [&](uint32_t local){ return ground.worldZ(static_cast<uint32_t>(z0) + local / wx); };

    // ---- Stamp: nearest station per corner, over a corridor plus one cell ----
    // Squared distances while it runs, one square root per corner at the end: every corner falls
    // inside dozens of stations' discs and all but one of those visits only lose a comparison.
    std::vector<float> bestSquared(cornerCount, std::numeric_limits<float>::infinity());

    for (uint32_t s = 0; s < stations.size(); s++){
        const SplineStation& station = stations[s];
        if (!finitePlanar(station)){
            continue;
        }
        const float sx = station.position.x;
        const float sz = station.position.z;

        const int32_t sz0 = clampCorner(std::floor((sz - stampReach - ground.originZ) / ground.spacingZ), z0, z1);
        const int32_t sz1 = clampCorner(std::ceil((sz + stampReach - ground.originZ) / ground.spacingZ), z0, z1);

        float fx, fz, rx, rz;
        planarFrame(station, fx, fz, rx, rz);

        for (int32_t cz = sz0; cz <= sz1; cz++){
            const float dz = ground.worldZ(static_cast<uint32_t>(cz)) - sz;
            const float dzSquared = dz * dz;
            if (dzSquared > stampReachSq){
                continue;
            }

            // the disc's column span on this row, so the inner loop walks corners already inside it
            const float halfSpan = std::sqrt(stampReachSq - dzSquared);
            const int32_t rx0 = clampCorner(std::ceil((sx - halfSpan - ground.originX) / ground.spacingX), x0, x1 + 1);
            const int32_t rx1 = clampCorner(std::floor((sx + halfSpan - ground.originX) / ground.spacingX), x0 - 1, x1);
            if (rx1 < rx0){
                continue;
            }

            const uint32_t rowBase = static_cast<uint32_t>(cz - z0) * wx;
            float dx = ground.worldX(static_cast<uint32_t>(rx0)) - sx;
            for (int32_t cx = rx0; cx <= rx1; cx++, dx += ground.spacingX){
                const float squared = dx * dx + dzSquared;
                const uint32_t index = rowBase + static_cast<uint32_t>(cx - x0);
                if (squared >= bestSquared[index]){
                    continue;
                }
                bestSquared[index] = squared;

                SplineFillCorner& corner = result.corners[index];
                corner.station = s;
                // projected into the station's own frame, so U and V are continuous within a
                // station's territory rather than quantised to the station spacing
                corner.arcDistance = station.distance + (dx * fx + dz * fz);
                corner.lateralSigned = dx * rx + dz * rz;
            }
        }
    }

    for (uint32_t i = 0; i < cornerCount; i++){
        if (result.corners[i].station != SPLINE_FILL_NO_STATION){
            result.corners[i].distance = std::sqrt(bestSquared[i]);
        }
    }

    // ---- Escape elevation: the bank the water would have to be over ----
    // One sweep in decreasing distance: every dependency of a corner is a neighbour FURTHER from
    // the centreline, so that order is a topological order and each corner settles in one visit.
    // (distance, index) packed in one word: a non-negative float's bits order like the float.
    std::vector<uint64_t> outward;
    outward.reserve(cornerCount);
    for (uint32_t i = 0; i < cornerCount; i++){
        SplineFillCorner& corner = result.corners[i];
        if (corner.distance > reach){
            // already outside the corridor: the water has left the domain here, and a rim decided
            // out here is one the run cannot actually see
            corner.escape = bed[i];
            corner.escapeUnseen = 1u;
            continue;
        }
        uint32_t key;
        std::memcpy(&key, &corner.distance, sizeof(key));
        outward.push_back((static_cast<uint64_t>(key) << 32) | i);
    }
    std::sort(outward.begin(), outward.end(), std::greater<uint64_t>());

    for (const uint64_t packed : outward){
        const uint32_t index = static_cast<uint32_t>(packed);
        const float here = result.corners[index].distance;
        float onward = std::numeric_limits<float>::infinity();
        uint8_t onwardUnseen = 0u;
        const uint32_t lx = index % wx;
        const uint32_t lz = index / wx;
        const auto consider = [&](uint32_t other){
            const SplineFillCorner& n = result.corners[other];
            if (n.distance > here && n.escape < onward){
                onward = n.escape;
                onwardUnseen = n.escapeUnseen;
            }
        };
        if (lx > 0u){
            consider(index - 1u);
        }
        if (lx + 1u < wx){
            consider(index + 1u);
        }
        if (lz > 0u){
            consider(index - wx);
        }
        if (lz + 1u < wz){
            consider(index + wx);
        }

        // no outward route at all leaves it at infinity: a pocket that drains only back into the
        // river is held by the river
        SplineFillCorner& corner = result.corners[index];
        if (bed[index] > onward){
            // the highest ground on the way out is HERE, inside the corridor: a bank the run sees
            corner.escape = bed[index];
            corner.escapeUnseen = 0u;
        }else{
            corner.escape = onward;
            corner.escapeUnseen = onwardUnseen;
        }
    }

    // ---- Waterline, then the inside-ness the mesher interpolates ----
    for (uint32_t i = 0; i < cornerCount; i++){
        SplineFillCorner& corner = result.corners[i];
        if (corner.station == SPLINE_FILL_NO_STATION){
            continue;
        }
        corner.waterline = std::max(waterlineAtArc(stations, corner.arcDistance), seaFloor);
        // Three constraints intersected, in metres so the mesher can interpolate their minimum to a
        // sub-cell shoreline: the ground is below the water, the corridor has not run out, and the
        // water does not stand over a bank it could flow across. The last EXCLUDES water rather than
        // lowering it; a sea level contains water the way a bank does, so it joins the escape.
        const float held = std::max(corner.escape, seaFloor);
        const float phi = corner.waterline - bed[i];
        corner.inside = std::min({phi + tuck, reach - corner.distance, held - corner.waterline});
    }

    // ---- Seeds: the station cells, at each station's own waterline ----
    std::vector<uint32_t> queue;
    const auto seed = [&](uint32_t index){
        SplineFillCorner& corner = result.corners[index];
        if (corner.wet || !(corner.inside > 0.0f)){
            return;
        }
        corner.wet = 1u;
        queue.push_back(index);
    };

    for (uint32_t s = 0; s < stations.size(); s++){
        const SplineStation& station = stations[s];
        if (!finitePlanar(station)){
            continue;
        }
        const float gx = (station.position.x - ground.originX) / ground.spacingX;
        const float gz = (station.position.z - ground.originZ) / ground.spacingZ;
        if (!(gx >= -0.5f) || !(gz >= -0.5f) || !(gx < static_cast<float>(ground.countX) - 0.5f) ||
            !(gz < static_cast<float>(ground.countZ) - 0.5f)){
            continue;
        }
        const uint32_t cx = static_cast<uint32_t>(std::max(0L, std::lround(gx)));
        const uint32_t cz = static_cast<uint32_t>(std::max(0L, std::lround(gz)));
        if (!result.contains(cx, cz)){
            continue;
        }

        result.diagnostics.seeds++;
        const uint32_t index = (cz - result.minCornerZ) * wx + (cx - result.minCornerX);
        // dry means the BED is not below the waterline here, the thing an author can act on
        if (bed[index] >= result.corners[index].waterline){
            if (result.diagnostics.drySeeds == 0u){
                result.diagnostics.firstDrySeed = station.position;
                result.diagnostics.firstDrySeedStation = s;
            }
            result.diagnostics.drySeeds++;
        }
        seed(index);
    }

    const auto refuse = [&](){
        result.corners.assign(cornerCount, SplineFillCorner{});
        result.diagnostics = SplineFillDiagnostics{};
        result.diagnostics.exceededBudget = true;
    };

    // ---- Flood: 4-connected, from the seeds, through inside > 0 ----
    for (size_t head = 0; head < queue.size(); head++){
        const uint32_t index = queue[head];
        const uint32_t lx = index % wx;
        const uint32_t lz = index / wx;
        if (lx > 0u){
            seed(index - 1u);
        }
        if (lx + 1u < wx){
            seed(index + 1u);
        }
        if (lz > 0u){
            seed(index - wx);
        }
        if (lz + 1u < wz){
            seed(index + wx);
        }
        if (queue.size() > params.maxWetCorners){
            // refuse rather than coarsen: the caller builds nothing and says so
            refuse();
            return result;
        }
    }

    // ---- Seal: a dry pocket the water encloses has nowhere to drain ----
    // The containment term presumes the outside is reachable; for a corner ringed by water it is
    // not. Reachability decides only which corners the containment term governs: ground above its
    // own waterline stays a dry-topped islet, and a dip at the region's edge still bites.
    std::vector<uint8_t> drains(cornerCount, 0u);
    std::vector<uint32_t> dryQueue;
    const auto reachDry = [&](uint32_t index){
        if (drains[index] != 0u || result.corners[index].wet != 0u){
            return;
        }
        drains[index] = 1u;
        dryQueue.push_back(index);
    };
    for (uint32_t lx = 0; lx < wx; lx++){
        reachDry(lx);
        reachDry((wz - 1u) * wx + lx);
    }
    for (uint32_t lz = 0; lz < wz; lz++){
        reachDry(lz * wx);
        reachDry(lz * wx + wx - 1u);
    }
    for (size_t head = 0; head < dryQueue.size(); head++){
        const uint32_t index = dryQueue[head];
        const uint32_t lx = index % wx;
        const uint32_t lz = index / wx;
        if (lx > 0u){
            reachDry(index - 1u);
        }
        if (lx + 1u < wx){
            reachDry(index + 1u);
        }
        if (lz > 0u){
            reachDry(index - wx);
        }
        if (lz + 1u < wz){
            reachDry(index + wx);
        }
    }

    for (uint32_t index = 0; index < cornerCount; index++){
        SplineFillCorner& corner = result.corners[index];
        if (corner.wet != 0u || drains[index] != 0u || corner.station == SPLINE_FILL_NO_STATION){
            continue;
        }
        // the same field minus the one term reachability just disqualified
        const float inside = std::min(corner.waterline - bed[index] + tuck, reach - corner.distance);
        if (!(inside > 0.0f)){
            continue;
        }
        corner.inside = inside;
        corner.wet = 1u;
        queue.push_back(index);
        result.diagnostics.sealedPocketCorners++;
    }

    if (queue.size() > params.maxWetCorners){
        refuse();
        return result;
    }

    result.diagnostics.wetCorners = static_cast<uint32_t>(queue.size());

    // ---- What the containment term finally refused, over the corners still dry ----
    float worstUnseenMetres = 0.0f;
    for (uint32_t index = 0; index < cornerCount; index++){
        const SplineFillCorner& corner = result.corners[index];
        if (corner.wet != 0u || corner.station == SPLINE_FILL_NO_STATION){
            continue;
        }
        const float held = std::max(corner.escape, seaFloor);
        if (!(corner.waterline - bed[index] + tuck > 0.0f) || !(reach - corner.distance > 0.0f) ||
            held - corner.waterline > 0.0f){
            continue;
        }
        // non-negative by the guard above, so the first refusal seeds the ranking: a run whose banks
        // stand exactly at the waterline still names a sample
        const float over = corner.waterline - held;
        const SplineFillWorstSample here{worldX(index), worldZ(index), corner.arcDistance};
        if (result.diagnostics.overBankCorners == 0u || over > result.diagnostics.maxOverBankMetres){
            result.diagnostics.maxOverBankMetres = over;
            result.diagnostics.worstOverBank = here;
        }
        result.diagnostics.overBankCorners++;
        // the rim that let it go was outside the corridor: a wider maxHalfWidth, not a deeper carve
        if (corner.escapeUnseen == 0u){
            continue;
        }
        if (result.diagnostics.unseenBankCorners == 0u || over > worstUnseenMetres){
            worstUnseenMetres = over;
            result.diagnostics.worstUnseenBank = here;
        }
        result.diagnostics.unseenBankCorners++;
    }

    // ---- Diagnostics over the settled region ----
    for (const uint32_t index : queue){
        const SplineFillCorner& corner = result.corners[index];
        result.diagnostics.maxDepthMetres = std::max(result.diagnostics.maxDepthMetres, corner.waterline - bed[index]);

        const uint32_t lx = index % wx;
        const uint32_t lz = index / wx;
        const auto neighbour = [&](uint32_t other){
            const SplineFillCorner& n = result.corners[other];
            if (n.wet){
                result.diagnostics.medialStepMetres = std::max(result.diagnostics.medialStepMetres,
                                                               std::abs(n.waterline - corner.waterline));
            }
        };
        if (lx > 0u){
            neighbour(index - 1u);
        }
        if (lx + 1u < wx){
            neighbour(index + 1u);
        }
        if (lz > 0u){
            neighbour(index - wx);
        }
        if (lz + 1u < wz){
            neighbour(index + wx);
        }
    }

    // ---- Validate the waterline instead of clamping it ----
    // clamping a rise would silently move water the author placed
    for (size_t i = 1; i < stations.size(); i++){
        const float rise = stations[i].position.y - stations[i - 1u].position.y;
        if (rise > result.diagnostics.waterlineRiseMetres){
            result.diagnostics.waterlineRiseMetres = rise;
            result.diagnostics.waterlineRiseAtArc = stations[i].distance;
            result.diagnostics.waterlineRiseStation = static_cast<uint32_t>(i);
        }
    }

    return result;
}
