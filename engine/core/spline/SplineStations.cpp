// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/TerrainECS/Source/TerrainRoutePolyline.cpp and the
// station frames of Apps/Editor/Source/Placement/SplineSweepStations.cpp, Copyright (c) 2026
// Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#include "SplineStations.h"

#include "SplineCurve.h"
#include "SplineEvaluator.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::spline;

namespace{

    const float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;
    const float INF = std::numeric_limits<float>::infinity();
    // R4: forward parallel to up within 1e-4
    const float PARALLEL_SIN_SQ = 1e-8f;
    // segments shorter than this in XZ have no direction to project onto
    const float MIN_SEGMENT_XZ_SQ = 1e-12f;
    // the grid's cell budget
    const int64_t MAX_GRID_CELLS = int64_t(1) << 22;

    Vector3 transformPoint(const Matrix4* m, const Vector3& p){
        if (!m){
            return p;
        }
        const Matrix4& a = *m;
        return Vector3(a[0][0] * p.x + a[1][0] * p.y + a[2][0] * p.z + a[3][0],
                       a[0][1] * p.x + a[1][1] * p.y + a[2][1] * p.z + a[3][1],
                       a[0][2] * p.x + a[1][2] * p.y + a[2][2] * p.z + a[3][2]);
    }

    Vector3 transformDirection(const Matrix4* m, const Vector3& d){
        if (!m){
            return d;
        }
        const Matrix4& a = *m;
        return Vector3(a[0][0] * d.x + a[1][0] * d.y + a[2][0] * d.z,
                       a[0][1] * d.x + a[1][1] * d.y + a[2][1] * d.z,
                       a[0][2] * d.x + a[1][2] * d.y + a[2][2] * d.z);
    }

    // the largest axis scale of the basis (S18 D4): radius scale and arc scale
    float largestScale(const Matrix4* m){
        if (!m){
            return 1.0f;
        }
        const Matrix4& a = *m;
        float best = 0.0f;
        for (int c = 0; c < 3; c++){
            const float s = std::sqrt(a[c][0] * a[c][0] + a[c][1] * a[c][1] + a[c][2] * a[c][2]);
            best = s > best ? s : best;
        }
        return best;
    }

    // the right for a frame whose forward is parallel to up and that has no neighbour to carry
    // one over from: as the evaluator's (+X for a run up +Y)
    Vector3 lastResortRight(const Vector3& forward, const Vector3& up){
        const Vector3 fallbackForward = curve::normalizedOr(curve::cross(up, Vector3(1.0f, 0.0f, 0.0f)), Vector3(0.0f, 0.0f, -1.0f));
        return curve::normalizedOr(curve::cross(forward, -fallbackForward), Vector3(1.0f, 0.0f, 0.0f));
    }

    // r carried to the plane normal to forward (parallel transport between close frames)
    Vector3 transported(const Vector3& r, const Vector3& forward, const Vector3& fallback){
        return curve::normalizedOr(r - forward * curve::dot(forward, r), fallback);
    }

    float heightOf(const Vector3& p, const Vector3& up){
        return curve::dot(p, up);
    }

    struct Sample{
        uint32_t segment = 0;
        float u = 0.0f;
        // local arc distance, for the midpoints of subdivided intervals
        float localDistance = 0.0f;
        bool knot = false;
        Vector3 world;
        // the curve's own tangent (world), for the turn criterion and as the frame's fallback
        Vector3 forward;
        float radius = 0.0f;
        float rollDeg = 0.0f;
        SplineSurfaceSample surface;
    };

    class Sampler{
    public:
        Sampler(const SplineData& spline, const SplineStationOptions& options):
            spline(spline),
            options(options),
            conforming(options.conform != SplineConform::None && bool(options.probe)),
            cosTurn(std::cos(std::clamp(options.maxTurnDeg, 0.0f, 180.0f) * DEG_TO_RAD)){
        }

        bool isConforming() const { return conforming; }

        Sample at(uint32_t segment, float u, float localDistance, bool knot, bool probe) const{
            const SplineFrame frame = evaluateSegment(spline, segment, u);
            Sample s;
            s.segment = segment;
            s.u = u;
            s.localDistance = localDistance;
            s.knot = knot;
            s.world = transformPoint(options.localToWorld, frame.position);
            s.forward = curve::normalizedOr(transformDirection(options.localToWorld, frame.forward), frame.forward);
            s.radius = frame.radius;
            s.rollDeg = frame.rollDeg;
            if (conforming && probe){
                s.surface = options.probe(s.world);
            }
            return s;
        }

        // the sample halfway (in arc length) between a and b, both in segment
        Sample middle(const Sample& a, const Sample& b, uint32_t segment) const{
            const float d = 0.5f * (a.localDistance + b.localDistance);
            uint32_t s;
            float u;
            distanceToSegment(spline, d, s, u);
            if (s != segment || !(u > a.u && u < b.u)){
                // the LUT put the midpoint across the segment boundary: halve the parameter
                u = 0.5f * (a.u + b.u);
            }
            return at(segment, u, d, false, true);
        }

        // the stations strictly between a and b that the turn and drape criteria ask for, in order
        void subdivide(const Sample& a, const Sample& b, uint32_t segment, uint32_t depth, std::vector<Sample>& out) const{
            if (depth >= options.maxSubdivisions){
                return;
            }
            bool split = curve::dot(a.forward, b.forward) < cosTurn;
            Sample m;
            bool haveMiddle = false;
            if (!split && conforming){
                m = middle(a, b, segment);
                haveMiddle = true;
                if (a.surface.hit && b.surface.hit && m.surface.hit){
                    const float chord = 0.5f * (a.surface.height + b.surface.height);
                    split = std::fabs(m.surface.height - chord) > options.maxHeightDelta;
                }
            }
            if (!split){
                return;
            }
            if (!haveMiddle){
                m = middle(a, b, segment);
            }
            subdivide(a, m, segment, depth + 1, out);
            out.push_back(m);
            subdivide(m, b, segment, depth + 1, out);
        }

    private:
        const SplineData& spline;
        const SplineStationOptions& options;
        const bool conforming;
        const float cosTurn;
    };

    // the station of segment i of a polyline nearest to (x, z) in XZ; false without XZ extent
    bool segmentXZ(std::span<const SplineStation> stations, size_t i, float x, float z,
                   float& distance, float& fraction, float& lateral){
        const Vector3& a = stations[i].position;
        const Vector3& b = stations[i + 1].position;
        const float dx = b.x - a.x;
        const float dz = b.z - a.z;
        const float lenSq = dx * dx + dz * dz;
        if (!(lenSq >= MIN_SEGMENT_XZ_SQ)){
            return false;
        }
        float t = ((x - a.x) * dx + (z - a.z) * dz) / lenSq;
        t = t > 0.0f ? (t < 1.0f ? t : 1.0f) : 0.0f;
        const float ox = x - (a.x + dx * t);
        const float oz = z - (a.z + dz * t);
        distance = std::sqrt(ox * ox + oz * oz);
        fraction = t;
        // right of travel = forward x +Y = (-dz, dx) in XZ
        lateral = (oz * dx - ox * dz) / std::sqrt(lenSq);
        return true;
    }

    SplineStationHitXZ hitOf(std::span<const SplineStation> stations, uint32_t segment, float distance, float fraction, float lateral){
        const SplineStation& a = stations[segment];
        const SplineStation& b = stations[segment + 1];
        SplineStationHitXZ hit;
        hit.valid = true;
        hit.segment = segment;
        hit.fraction = fraction;
        hit.distanceXZ = distance;
        hit.lateral = lateral;
        hit.height = curve::lerp(a.position.y, b.position.y, fraction);
        hit.distance = curve::lerp(a.distance, b.distance, fraction);
        hit.halfWidthLeft = curve::lerp(a.halfWidthLeft, b.halfWidthLeft, fraction);
        hit.halfWidthRight = curve::lerp(a.halfWidthRight, b.halfWidthRight, fraction);
        return hit;
    }

    SplineStation lerpStation(const SplineStation& a, const SplineStation& b, float f){
        SplineStation s;
        s.position = curve::lerp(a.position, b.position, f);
        s.forward = curve::normalizedOr(curve::lerp(a.forward, b.forward, f), a.forward);
        s.right = transported(curve::lerp(a.right, b.right, f), s.forward, a.right);
        s.up = curve::cross(s.right, s.forward);
        s.halfWidthLeft = curve::lerp(a.halfWidthLeft, b.halfWidthLeft, f);
        s.halfWidthRight = curve::lerp(a.halfWidthRight, b.halfWidthRight, f);
        s.rollRadians = curve::lerp(a.rollRadians, b.rollRadians, f);
        s.distance = curve::lerp(a.distance, b.distance, f);
        s.groundOffset = curve::lerp(a.groundOffset, b.groundOffset, f);
        s.topOffset = curve::lerp(a.topOffset, b.topOffset, f);
        s.splineT = -1.0f;
        s.join = SplineStationJoin::Advance;
        return s;
    }

}

SplineStationReport spline::buildStations(const SplineData& spline, const SplineStationOptions& options,
                                          std::vector<SplineStation>& out){
    SplineStationReport report;
    out.clear();
    if (!spline.isValid()){
        return report;
    }
    const uint32_t segments = spline.segmentCount();
    const float total = length(spline);
    float spacing = options.stationSpacing > 0.0f ? options.stationSpacing : 1.0f;
    // half the budget for the even spacing, the rest for the subdivisions
    const float maxBase = float(SPLINE_MAX_STATIONS / 2);
    if (std::isfinite(total) && total / spacing > maxBase){
        spacing = total / maxBase;
        report.capped = true;
    }

    // 1. samples along the curve: the knots, the even spacing between them, the subdivisions
    const Sampler sampler(spline, options);
    std::vector<Sample> samples;
    for (uint32_t segment = 0; segment < segments; segment++){
        const float d0 = tToDistance(spline, segmentToT(spline, segment, 0.0f));
        const float d1 = tToDistance(spline, segmentToT(spline, segment, 1.0f));
        const float span = d1 - d0;
        const float wanted = span > 0.0f ? std::ceil(span / spacing) : 1.0f;
        const uint32_t intervals = wanted < maxBase ? std::max(1u, static_cast<uint32_t>(wanted)) : static_cast<uint32_t>(maxBase);

        Sample a;
        if (segment == 0){
            a = sampler.at(segment, 0.0f, d0, true, true);
            samples.push_back(a);
        }else{
            // the knot is already a station (the previous segment's end); the start of this
            // segment adds its own outgoing tangent for the turn criterion
            a = sampler.at(segment, 0.0f, d0, true, false);
            a.surface = samples.back().surface;
        }
        for (uint32_t j = 1; j <= intervals; j++){
            Sample b;
            if (j == intervals){
                b = sampler.at(segment, 1.0f, d1, true, true);
            }else{
                const float d = d0 + span * (float(j) / float(intervals));
                uint32_t s;
                float u;
                distanceToSegment(spline, d, s, u);
                if (s != segment){
                    u = s < segment ? 0.0f : 1.0f;
                }
                b = sampler.at(segment, u, d, false, true);
            }
            sampler.subdivide(a, b, segment, 0, samples);
            samples.push_back(b);
            a = b;
        }
    }

    // 2. the drape: probed heights, misses holding the nearest measured one
    const Vector3& up = spline.upAxis();
    std::vector<Vector3> normals(samples.size(), up);
    if (sampler.isConforming()){
        size_t first = samples.size();
        for (size_t i = 0; i < samples.size(); i++){
            if (samples[i].surface.hit){
                first = std::min(first, i);
            }else{
                report.probeMisses++;
            }
        }
        if (first == samples.size()){
            report.allMissed = true;
        }else{
            float held = samples[first].surface.height;
            Vector3 heldNormal = samples[first].surface.normal;
            for (size_t i = 0; i < samples.size(); i++){
                if (samples[i].surface.hit){
                    held = samples[i].surface.height;
                    heldNormal = samples[i].surface.normal;
                }
                const Vector3& w = samples[i].world;
                samples[i].world = w + up * (held + options.verticalOffset - heightOf(w, up));
                normals[i] = curve::dot(heldNormal, up) < 0.0f ? -heldNormal : heldNormal;
            }
        }
    }

    // 3. stations by draped length, coincident points dropped (a knot overrides the sample it
    // coincides with, so every knot keeps its splineT)
    const float widthScale = largestScale(options.localToWorld);
    out.reserve(samples.size());
    std::vector<Vector3> stationNormals;
    std::vector<Vector3> fallbackForwards;
    stationNormals.reserve(samples.size());
    fallbackForwards.reserve(samples.size());
    for (size_t i = 0; i < samples.size(); i++){
        const Sample& s = samples[i];
        SplineStation station;
        station.position = s.world;
        station.halfWidthLeft = options.constantWidth ? options.halfWidth : s.radius * widthScale;
        station.halfWidthRight = station.halfWidthLeft;
        station.rollRadians = s.rollDeg * DEG_TO_RAD;
        station.splineT = float(s.segment) + s.u;
        if (!out.empty()){
            SplineStation& last = out.back();
            const float d = last.distance + std::sqrt(curve::lengthSq(s.world - last.position));
            if (!(d > last.distance)){
                if (s.knot){
                    station.distance = last.distance;
                    last = station;
                }
                continue;
            }
            station.distance = d;
        }
        out.push_back(station);
        stationNormals.push_back(normals[i]);
        fallbackForwards.push_back(s.forward);
    }

    // 4. roll-free frames from the draped polyline
    const size_t n = out.size();
    const bool ring = spline.effectivelyClosed() && n >= 3;
    const bool slope = sampler.isConforming() && !report.allMissed && options.conform == SplineConform::HeightAndSlope;
    const float blend = std::clamp(options.slopeBlend, 0.0f, 1.0f);
    std::vector<uint8_t> defined(n, 0);
    for (size_t i = 0; i < n; i++){
        const size_t prev = i > 0 ? i - 1 : (ring ? n - 2 : 0);
        const size_t next = i + 1 < n ? i + 1 : (ring ? 1 : n - 1);
        SplineStation& st = out[i];
        st.forward = curve::normalizedOr(out[next].position - out[prev].position, fallbackForwards[i]);
        Vector3 surfaceUp = up;
        if (slope){
            surfaceUp = curve::normalizedOr(up * (1.0f - blend) + stationNormals[i] * blend, up);
        }
        const Vector3 r = curve::cross(st.forward, surfaceUp);
        if (curve::lengthSq(r) >= PARALLEL_SIN_SQ){
            st.right = r / std::sqrt(curve::lengthSq(r));
            defined[i] = 1;
        }else if (i > 0 && defined[i - 1]){
            st.right = transported(out[i - 1].right, st.forward, st.right);
            defined[i] = 1;
        }
    }
    // a run that starts parallel to up takes the first defined right, carried back
    size_t firstDefined = n;
    for (size_t i = 0; i < n; i++){
        if (defined[i]){
            firstDefined = i;
            break;
        }
    }
    if (firstDefined < n){
        for (size_t i = firstDefined; i-- > 0;){
            out[i].right = transported(out[i + 1].right, out[i].forward, lastResortRight(out[i].forward, up));
            defined[i] = 1;
        }
    }
    for (size_t i = 0; i < n; i++){
        if (!defined[i]){
            out[i].right = i > 0 ? transported(out[i - 1].right, out[i].forward, lastResortRight(out[i].forward, up))
                                 : lastResortRight(out[i].forward, up);
        }
        out[i].up = curve::cross(out[i].right, out[i].forward);
    }
    if (ring){
        out[n - 1].forward = out[0].forward;
        out[n - 1].right = out[0].right;
        out[n - 1].up = out[0].up;
    }
    return report;
}

void spline::resampleStations(std::span<const SplineStation> stations, float maxSpacing, std::vector<SplineStation>& out){
    out.clear();
    if (stations.size() < 2){
        out.assign(stations.begin(), stations.end());
        return;
    }
    float spacing = maxSpacing > SPLINE_MIN_STATION_SPACING ? maxSpacing : SPLINE_MIN_STATION_SPACING;
    const float total = stations.back().distance - stations.front().distance;
    const float budget = float(SPLINE_MAX_STATIONS - stations.size());
    if (std::isfinite(total) && total / spacing > budget){
        spacing = total / budget;
    }
    out.reserve(stations.size());
    out.push_back(stations[0]);
    for (size_t i = 1; i < stations.size(); i++){
        const SplineStation& a = stations[i - 1];
        const SplineStation& b = stations[i];
        const float span = b.distance - a.distance;
        const float wanted = span > spacing ? std::ceil(span / spacing) : 1.0f;
        const uint32_t parts = wanted < budget ? static_cast<uint32_t>(wanted) : 1u;
        for (uint32_t j = 1; j < parts; j++){
            out.push_back(lerpStation(a, b, float(j) / float(parts)));
        }
        out.push_back(b);
    }
}

SplineStationHitXZ spline::closestStationSegmentXZ(std::span<const SplineStation> stations, float x, float z){
    SplineStationHitXZ best;
    best.distanceXZ = INF;
    for (size_t i = 0; i + 1 < stations.size(); i++){
        float distance;
        float fraction;
        float lateral;
        if (segmentXZ(stations, i, x, z, distance, fraction, lateral) && distance < best.distanceXZ){
            best = hitOf(stations, static_cast<uint32_t>(i), distance, fraction, lateral);
        }
    }
    return best;
}

void SplineStationGridXZ::build(std::span<const SplineStation> stationList, float cellSize){
    stations = stationList;
    cellStart.clear();
    items.clear();
    columns = 0;
    rows = 0;
    if (stations.size() < 2){
        return;
    }
    float maxX = stations[0].position.x;
    float maxZ = stations[0].position.z;
    minX = maxX;
    minZ = maxZ;
    double segmentLengths = 0.0;
    for (size_t i = 0; i < stations.size(); i++){
        const Vector3& p = stations[i].position;
        if (!std::isfinite(p.x) || !std::isfinite(p.z)){
            // a polyline with non-finite points has no grid; closest() reports nothing
            stations = {};
            return;
        }
        minX = std::min(minX, p.x);
        minZ = std::min(minZ, p.z);
        maxX = std::max(maxX, p.x);
        maxZ = std::max(maxZ, p.z);
        if (i > 0){
            const Vector3& q = stations[i - 1].position;
            segmentLengths += std::sqrt(double(p.x - q.x) * double(p.x - q.x) + double(p.z - q.z) * double(p.z - q.z));
        }
    }
    const float extent = std::max(maxX - minX, maxZ - minZ);
    cell = cellSize > 0.0f ? cellSize : float(2.0 * segmentLengths / double(stations.size() - 1));
    cell = std::max({cell, extent / 1024.0f, 1e-3f});
    int64_t cx = int64_t((maxX - minX) / cell) + 1;
    int64_t cz = int64_t((maxZ - minZ) / cell) + 1;
    while (cx * cz > MAX_GRID_CELLS){
        cell *= 2.0f;
        cx = int64_t((maxX - minX) / cell) + 1;
        cz = int64_t((maxZ - minZ) / cell) + 1;
    }
    columns = int32_t(cx);
    rows = int32_t(cz);

    // two passes over the segments' boxes: the counts, then the segment indices (in order)
    const auto cellRange = [&](size_t i, int32_t& x0, int32_t& z0, int32_t& x1, int32_t& z1){
        const Vector3& a = stations[i].position;
        const Vector3& b = stations[i + 1].position;
        x0 = std::clamp(int32_t((std::min(a.x, b.x) - minX) / cell), 0, columns - 1);
        x1 = std::clamp(int32_t((std::max(a.x, b.x) - minX) / cell), 0, columns - 1);
        z0 = std::clamp(int32_t((std::min(a.z, b.z) - minZ) / cell), 0, rows - 1);
        z1 = std::clamp(int32_t((std::max(a.z, b.z) - minZ) / cell), 0, rows - 1);
    };
    cellStart.assign(size_t(columns) * size_t(rows) + 1, 0);
    for (size_t i = 0; i + 1 < stations.size(); i++){
        int32_t x0, z0, x1, z1;
        cellRange(i, x0, z0, x1, z1);
        for (int32_t gz = z0; gz <= z1; gz++){
            for (int32_t gx = x0; gx <= x1; gx++){
                cellStart[size_t(gz) * size_t(columns) + size_t(gx) + 1]++;
            }
        }
    }
    for (size_t c = 1; c < cellStart.size(); c++){
        cellStart[c] += cellStart[c - 1];
    }
    items.resize(cellStart.back());
    std::vector<uint32_t> fill(cellStart.begin(), cellStart.end() - 1);
    for (size_t i = 0; i + 1 < stations.size(); i++){
        int32_t x0, z0, x1, z1;
        cellRange(i, x0, z0, x1, z1);
        for (int32_t gz = z0; gz <= z1; gz++){
            for (int32_t gx = x0; gx <= x1; gx++){
                items[fill[size_t(gz) * size_t(columns) + size_t(gx)]++] = static_cast<uint32_t>(i);
            }
        }
    }
}

SplineStationHitXZ SplineStationGridXZ::closest(float x, float z, float maxDistance) const{
    SplineStationHitXZ none;
    none.distanceXZ = INF;
    if (columns == 0 || !std::isfinite(x) || !std::isfinite(z)){
        return none;
    }
    // the query's cell (it may lie outside the grid)
    const float limit = 1e9f;
    const int64_t qx = int64_t(std::clamp(std::floor((x - minX) / cell), -limit, limit));
    const int64_t qz = int64_t(std::clamp(std::floor((z - minZ) / cell), -limit, limit));
    const int64_t lastX = columns - 1;
    const int64_t lastZ = rows - 1;
    const int64_t maxRing = std::max({std::abs(qx), std::abs(qx - lastX), std::abs(qz), std::abs(qz - lastZ)});
    // rings nearer than this hold no cell of the grid
    const int64_t firstRing = std::max({int64_t(0), -qx, qx - lastX, -qz, qz - lastZ});

    float best = INF;
    uint32_t bestSegment = 0;
    float bestFraction = 0.0f;
    float bestLateral = 0.0f;
    const auto visit = [&](int64_t gx, int64_t gz){
        const size_t c = size_t(gz) * size_t(columns) + size_t(gx);
        for (uint32_t k = cellStart[c]; k < cellStart[c + 1]; k++){
            const uint32_t i = items[k];
            float distance;
            float fraction;
            float lateral;
            if (segmentXZ(stations, i, x, z, distance, fraction, lateral) &&
                (distance < best || (distance == best && i < bestSegment))){
                best = distance;
                bestSegment = i;
                bestFraction = fraction;
                bestLateral = lateral;
            }
        }
    };
    for (int64_t r = firstRing; r <= maxRing; r++){
        // the cells of ring r are at least r - 1 cells from the query's
        if (r > 0 && float(r - 1) * cell > maxDistance){
            break;
        }
        // the ring clipped to the grid: its bottom and top rows, then its two side columns
        const int64_t x0 = std::max(qx - r, int64_t(0));
        const int64_t x1 = std::min(qx + r, lastX);
        const int64_t rowsAt[2] = {qz - r, qz + r};
        for (int64_t gz : rowsAt){
            if (gz >= 0 && gz <= lastZ){
                for (int64_t gx = x0; gx <= x1; gx++){
                    visit(gx, gz);
                }
            }
            if (r == 0){
                break;
            }
        }
        const int64_t z0 = std::max(qz - r + 1, int64_t(0));
        const int64_t z1 = std::min(qz + r - 1, lastZ);
        const int64_t columnsAt[2] = {qx - r, qx + r};
        for (int64_t gx : columnsAt){
            if (r > 0 && gx >= 0 && gx <= lastX){
                for (int64_t gz = z0; gz <= z1; gz++){
                    visit(gx, gz);
                }
            }
        }
        // every cell not visited yet is at least r cells from the query's
        if (best < float(r) * cell){
            break;
        }
    }
    if (!(best <= maxDistance)){
        return none;
    }
    return hitOf(stations, bestSegment, best, bestFraction, bestLateral);
}
