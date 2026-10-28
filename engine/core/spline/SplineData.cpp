// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/Spline/Source/SplineData.cpp and the cache rebuild of
// Source/SplineEvaluator.cpp, Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT
// License. See NOTICE.md.

#include "SplineData.h"

#include "SplineCurve.h"
#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::spline;

namespace{

    // R22: the cache may hold this much plus CACHE_BYTES_PER_KNOT per knot
    const size_t CACHE_BASE_BYTES = 4096;
    const size_t CACHE_BYTES_PER_KNOT = 64;
    const uint32_t MAX_ARC_STEPS = 32;

    // the Catmull-Rom tangent (next - prev) / 2 as a cubic Bezier handle (a third of it)
    const float AUTO_BEZIER_HANDLE_SCALE = 1.0f / 6.0f;

    // below this a centripetal knot distance (|chord|^0.5) has no usable direction
    const float MIN_KNOT_DISTANCE = 1e-6f;

    float sanitizedRadius(float radius){
        return radius > 0.0f ? radius : 0.0f;
    }

    // The two inner control points of a centripetal Catmull-Rom segment p1 -> p2 (alpha 0.5,
    // Yuksel et al.): tangents m1, m2 of the Barry-Goldman curve over the segment's own knot
    // interval, as Hermite -> Bezier handles. A neighbour coincident with its knot gives the chord
    // as that end's tangent, which is what a reflected phantom point would give.
    void catmullRomControls(const Vector3& p0, const Vector3& p1, const Vector3& p2, const Vector3& p3,
                            Vector3& b1, Vector3& b2){
        const Vector3 chord = p2 - p1;
        const float d1 = std::sqrt(std::sqrt(curve::lengthSq(p1 - p0)));
        const float d2 = std::sqrt(std::sqrt(curve::lengthSq(chord)));
        const float d3 = std::sqrt(std::sqrt(curve::lengthSq(p3 - p2)));
        if (!(d2 > MIN_KNOT_DISTANCE)){
            b1 = p1;
            b2 = p2;
            return;
        }
        Vector3 m1 = chord;
        if (d1 > MIN_KNOT_DISTANCE){
            m1 = chord + ((p1 - p0) / d1 - (p2 - p0) / (d1 + d2)) * d2;
        }
        Vector3 m2 = chord;
        if (d3 > MIN_KNOT_DISTANCE){
            m2 = chord + ((p3 - p2) / d3 - (p3 - p1) / (d2 + d3)) * d2;
        }
        b1 = p1 + m1 * (1.0f / 3.0f);
        b2 = p2 - m2 * (1.0f / 3.0f);
    }

    // exact bounds of a cubic Bezier: its ends plus the roots of B' per axis
    SplineSegmentBounds segmentBounds(const curve::Segment& s){
        SplineSegmentBounds b;
        b.min = Vector3(std::min(s.p0.x, s.p3.x), std::min(s.p0.y, s.p3.y), std::min(s.p0.z, s.p3.z));
        b.max = Vector3(std::max(s.p0.x, s.p3.x), std::max(s.p0.y, s.p3.y), std::max(s.p0.z, s.p3.z));
        for (unsigned axis = 0; axis < 3; axis++){
            // B'(u) / 3 = qa u^2 + qb u + qc
            const float a = s.p1[axis] - s.p0[axis];
            const float bb = s.p2[axis] - s.p1[axis];
            const float c = s.p3[axis] - s.p2[axis];
            const float qa = a - 2.0f * bb + c;
            const float qb = 2.0f * (bb - a);
            const float qc = a;
            float roots[2];
            int count = 0;
            if (std::fabs(qa) < 1e-12f){
                if (std::fabs(qb) > 1e-12f){
                    roots[count++] = -qc / qb;
                }
            }else{
                const float disc = qb * qb - 4.0f * qa * qc;
                if (disc >= 0.0f){
                    const float sq = std::sqrt(disc);
                    roots[count++] = (-qb + sq) / (2.0f * qa);
                    roots[count++] = (-qb - sq) / (2.0f * qa);
                }
            }
            for (int r = 0; r < count; r++){
                if (roots[r] > 0.0f && roots[r] < 1.0f){
                    const float v = s.position(roots[r])[axis];
                    b.min[axis] = std::min(b.min[axis], v);
                    b.max[axis] = std::max(b.max[axis], v);
                }
            }
        }
        return b;
    }

    // distance in XZ from q to the chord a -> b
    float chordDistanceXZ(float ax, float az, float bx, float bz, float qx, float qz){
        const float dx = bx - ax;
        const float dz = bz - az;
        const float lenSq = dx * dx + dz * dz;
        float t = 0.0f;
        if (lenSq > 0.0f){
            t = ((qx - ax) * dx + (qz - az) * dz) / lenSq;
            t = t > 0.0f ? (t < 1.0f ? t : 1.0f) : 0.0f;
        }
        const float ex = qx - (ax + dx * t);
        const float ez = qz - (az + dz * t);
        return std::sqrt(ex * ex + ez * ez);
    }

    template<typename T>
    void sizeExactly(std::vector<T>& v, size_t n){
        v.resize(n);
        if (v.capacity() > n){
            v.shrink_to_fit();
        }
    }

    template<typename T>
    size_t heapBytes(const std::vector<T>& v){
        return v.capacity() * sizeof(T);
    }

}

void spline::applyTangentMode(SplineKnot& knot, SplineHandle edited){
    const Vector3& leader = edited == SplineHandle::Out ? knot.tangentOut : knot.tangentIn;
    Vector3& follower = edited == SplineHandle::Out ? knot.tangentIn : knot.tangentOut;
    if (knot.mode == TangentMode::Mirrored){
        follower = -leader;
    }else if (knot.mode == TangentMode::Aligned){
        const float leaderSq = curve::lengthSq(leader);
        if (leaderSq > 0.0f && std::isfinite(leaderSq)){
            const float followerLength = std::sqrt(curve::lengthSq(follower));
            follower = leader * (-followerLength / std::sqrt(leaderSq));
        }
    }
}

SplineData::SplineData(const SplineData& other):
    splineType(other.splineType),
    isClosed(other.isClosed),
    splineUpAxis(other.splineUpAxis),
    splineKnots(other.splineKnots),
    dataVersion(other.dataVersion){
}

SplineData& SplineData::operator=(const SplineData& other){
    if (this != &other){
        splineType = other.splineType;
        isClosed = other.isClosed;
        splineUpAxis = other.splineUpAxis;
        splineKnots = other.splineKnots;
        // a new version, so a cache built for this object's previous definition is not reused
        dataVersion = std::max(dataVersion, other.dataVersion) + 1;
    }
    return *this;
}

void SplineData::setType(SplineType type){
    if (splineType != type){
        splineType = type;
        markDirty();
    }
}

void SplineData::setClosed(bool closed){
    if (isClosed != closed){
        isClosed = closed;
        markDirty();
    }
}

void SplineData::setUpAxis(const Vector3& axis){
    const Vector3 n = curve::normalizedOr(axis, Vector3(0.0f, 0.0f, 0.0f));
    if (curve::lengthSq(n) == 0.0f || n == splineUpAxis){
        return;
    }
    splineUpAxis = n;
    markDirty();
}

void SplineData::setKnots(std::vector<SplineKnot> knots){
    for (SplineKnot& k : knots){
        k.radius = sanitizedRadius(k.radius);
    }
    splineKnots = std::move(knots);
    markDirty();
}

bool SplineData::setKnot(size_t index, const SplineKnot& knot){
    if (index >= splineKnots.size()){
        return false;
    }
    splineKnots[index] = knot;
    splineKnots[index].radius = sanitizedRadius(knot.radius);
    markDirty();
    return true;
}

bool SplineData::setKnotPosition(size_t index, const Vector3& position){
    if (index >= splineKnots.size()){
        return false;
    }
    splineKnots[index].position = position;
    markDirty();
    return true;
}

bool SplineData::setKnotRadius(size_t index, float radius){
    if (index >= splineKnots.size()){
        return false;
    }
    splineKnots[index].radius = sanitizedRadius(radius);
    markDirty();
    return true;
}

bool SplineData::setKnotRoll(size_t index, float rollDeg){
    if (index >= splineKnots.size()){
        return false;
    }
    splineKnots[index].rollDeg = rollDeg;
    markDirty();
    return true;
}

bool SplineData::setKnotTangents(size_t index, const Vector3& tangentIn, const Vector3& tangentOut){
    if (index >= splineKnots.size()){
        return false;
    }
    splineKnots[index].tangentIn = tangentIn;
    splineKnots[index].tangentOut = tangentOut;
    markDirty();
    return true;
}

bool SplineData::setKnotTangentIn(size_t index, const Vector3& tangentIn){
    if (index >= splineKnots.size()){
        return false;
    }
    splineKnots[index].tangentIn = tangentIn;
    applyTangentMode(splineKnots[index], SplineHandle::In);
    markDirty();
    return true;
}

bool SplineData::setKnotTangentOut(size_t index, const Vector3& tangentOut){
    if (index >= splineKnots.size()){
        return false;
    }
    splineKnots[index].tangentOut = tangentOut;
    applyTangentMode(splineKnots[index], SplineHandle::Out);
    markDirty();
    return true;
}

bool SplineData::setKnotMode(size_t index, TangentMode mode){
    if (index >= splineKnots.size()){
        return false;
    }
    splineKnots[index].mode = mode;
    applyTangentMode(splineKnots[index], SplineHandle::Out);
    markDirty();
    return true;
}

void SplineData::addKnot(const SplineKnot& knot){
    insertKnot(splineKnots.size(), knot);
}

void SplineData::insertKnot(size_t index, const SplineKnot& knot){
    index = std::min(index, splineKnots.size());
    SplineKnot k = knot;
    k.radius = sanitizedRadius(k.radius);
    splineKnots.insert(splineKnots.begin() + static_cast<std::ptrdiff_t>(index), k);
    markDirty();
}

bool SplineData::removeKnot(size_t index){
    if (index >= splineKnots.size() || splineKnots.size() <= MIN_KNOTS){
        return false;
    }
    splineKnots.erase(splineKnots.begin() + static_cast<std::ptrdiff_t>(index));
    markDirty();
    return true;
}

void SplineData::replaceKnotsFromFrames(std::span<const SplineFrame> frames){
    std::vector<SplineKnot> knots(frames.size());
    for (size_t i = 0; i < frames.size(); i++){
        knots[i].position = frames[i].position;
        knots[i].radius = frames[i].radius;
        knots[i].rollDeg = frames[i].rollDeg;
        knots[i].mode = TangentMode::Mirrored;
    }
    if (splineType == SplineType::CubicBezier && frames.size() >= 2){
        const size_t n = frames.size();
        const bool ring = isClosed && n >= MIN_CLOSED_KNOTS;
        for (size_t i = 0; i < n; i++){
            const size_t prev = i > 0 ? i - 1 : (ring ? n - 1 : 0);
            const size_t next = i + 1 < n ? i + 1 : (ring ? 0 : n - 1);
            const Vector3 handle = (frames[next].position - frames[prev].position) * AUTO_BEZIER_HANDLE_SCALE;
            knots[i].tangentOut = handle;
            knots[i].tangentIn = -handle;
        }
    }
    setKnots(std::move(knots));
}

uint32_t SplineData::segmentCount() const{
    if (splineKnots.size() < MIN_KNOTS){
        return 0;
    }
    return static_cast<uint32_t>(effectivelyClosed() ? splineKnots.size() : splineKnots.size() - 1);
}

const SplineCache& SplineData::ensureCache() const{
    const uint64_t current = dataVersion;
    if (cacheVersion.load(std::memory_order_acquire) != current){
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (cacheVersion.load(std::memory_order_relaxed) != current){
            rebuildCache(cache);
            cacheVersion.store(current, std::memory_order_release);
        }
    }
    return cache;
}

size_t SplineData::cacheBytes() const{
    const SplineCache& c = ensureCache();
    return sizeof(SplineCache) + heapBytes(c.controls) + heapBytes(c.bounds) + heapBytes(c.arc) + heapBytes(c.polygonXZ);
}

void SplineData::rebuildCache(SplineCache& c) const{
    const uint32_t segments = segmentCount();
    const size_t n = splineKnots.size();
    const bool ring = effectivelyClosed();
    c.segmentCount = segments;
    c.length = 0.0f;
    c.polygonError = 0.0f;
    c.polygonArea = 0.0f;
    if (segments == 0){
        c.arcSteps = 0;
        sizeExactly(c.controls, 0);
        sizeExactly(c.bounds, 0);
        sizeExactly(c.arc, 0);
        sizeExactly(c.polygonXZ, 0);
        return;
    }

    // the finest arc resolution the R22 budget affords: 2 controls + bounds per segment, then
    // per arc step one float (+ an x, z polygon vertex on a ring)
    const size_t budget = CACHE_BASE_BYTES + CACHE_BYTES_PER_KNOT * n;
    const size_t fixedBytes = sizeof(SplineCache) + segments * (2 * sizeof(Vector3) + sizeof(SplineSegmentBounds));
    const size_t stepBytes = segments * (sizeof(float) + (ring ? 2 * sizeof(float) : 0));
    size_t steps = budget > fixedBytes ? (budget - fixedBytes) / stepBytes : 1;
    steps = std::clamp<size_t>(steps, 1, MAX_ARC_STEPS);
    const uint32_t arcSteps = static_cast<uint32_t>(steps);
    c.arcSteps = arcSteps;

    sizeExactly(c.controls, size_t(segments) * 2);
    sizeExactly(c.bounds, segments);
    sizeExactly(c.arc, size_t(segments) * arcSteps);
    sizeExactly(c.polygonXZ, ring ? size_t(segments) * arcSteps * 2 : 0);

    for (uint32_t s = 0; s < segments; s++){
        const size_t i1 = s;
        const size_t i2 = s + 1 < n ? s + 1 : 0;
        const Vector3& p1 = splineKnots[i1].position;
        const Vector3& p2 = splineKnots[i2].position;
        Vector3 b1;
        Vector3 b2;
        if (splineType == SplineType::Linear){
            b1 = curve::lerp(p1, p2, 1.0f / 3.0f);
            b2 = curve::lerp(p1, p2, 2.0f / 3.0f);
        }else if (splineType == SplineType::CubicBezier){
            b1 = p1 + splineKnots[i1].tangentOut;
            b2 = p2 + splineKnots[i2].tangentIn;
        }else{
            // open ends reflect their neighbour (phantom p_-1 = 2 p_0 - p_1): the end tangent is
            // the chord to the next knot
            Vector3 p0;
            Vector3 p3;
            if (ring){
                p0 = splineKnots[s > 0 ? s - 1 : n - 1].position;
                p3 = splineKnots[(s + 2) % n].position;
            }else{
                p0 = s > 0 ? splineKnots[s - 1].position : p1 * 2.0f - p2;
                p3 = s + 2 < n ? splineKnots[s + 2].position : p2 * 2.0f - p1;
            }
            catmullRomControls(p0, p1, p2, p3, b1, b2);
        }
        c.controls[size_t(s) * 2] = b1;
        c.controls[size_t(s) * 2 + 1] = b2;

        const curve::Segment segment{p1, b1, b2, p2};
        c.bounds[s] = segmentBounds(segment);

        for (uint32_t i = 0; i < arcSteps; i++){
            const float u0 = float(i) / float(arcSteps);
            const float u1 = float(i + 1) / float(arcSteps);
            c.length += curve::arcLength(segment, u0, u1);
            c.arc[size_t(s) * arcSteps + i] = c.length;
            if (ring){
                const Vector3 p = segment.position(u0);
                c.polygonXZ[(size_t(s) * arcSteps + i) * 2] = p.x;
                c.polygonXZ[(size_t(s) * arcSteps + i) * 2 + 1] = p.z;
            }
        }
    }

    if (ring){
        // how far the polygon strays from the curve: the chord-to-curve distance at the middle of
        // every interval, with a margin for the rest of the interval
        const size_t vertices = c.polygonXZ.size() / 2;
        float error = 0.0f;
        double area = 0.0;
        for (size_t v = 0; v < vertices; v++){
            const size_t w = v + 1 < vertices ? v + 1 : 0;
            const float ax = c.polygonXZ[v * 2];
            const float az = c.polygonXZ[v * 2 + 1];
            const float bx = c.polygonXZ[w * 2];
            const float bz = c.polygonXZ[w * 2 + 1];
            area += double(ax) * double(bz) - double(bx) * double(az);
            const uint32_t s = static_cast<uint32_t>(v / arcSteps);
            const uint32_t i = static_cast<uint32_t>(v % arcSteps);
            const size_t i2 = s + 1 < n ? s + 1 : 0;
            const curve::Segment segment{splineKnots[s].position, c.controls[size_t(s) * 2], c.controls[size_t(s) * 2 + 1], splineKnots[i2].position};
            const Vector3 mid = segment.position((float(i) + 0.5f) / float(arcSteps));
            const float e = chordDistanceXZ(ax, az, bx, bz, mid.x, mid.z);
            error = e > error ? e : error;
        }
        c.polygonError = error * 1.5f + 1e-4f;
        c.polygonArea = float(area * 0.5);
    }
}
