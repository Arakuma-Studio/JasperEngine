// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/Spline/Source/SplineEvaluator.cpp, Copyright (c) 2026
// Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#include "SplineEvaluator.h"

#include "SplineCurve.h"
#include "SplineUtility.h"
#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::spline;

namespace{

    const float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;
    const float INF = std::numeric_limits<float>::infinity();

    // R4: forward is parallel to the up axis when |forward x up| (the sine between them) < 1e-4
    const float PARALLEL_SIN_SQ = 1e-8f;
    // a derivative this short has no usable direction
    const float MIN_DERIVATIVE_SQ = 1e-12f;

    // the parallel-transport search: scan steps per segment, then bisections of the boundary
    const uint32_t TRANSPORT_SCAN_STEPS = 64;
    const uint32_t TRANSPORT_BISECTIONS = 24;

    // closest point: coarse samples per segment, then per local minimum golden-section steps
    // over its bracket (to 3e-3 of the bracket) and Newton steps on (B - p) . B'
    const uint32_t CLOSEST_SAMPLES = 16;
    const uint32_t CLOSEST_GOLDEN_STEPS = 12;
    const uint32_t CLOSEST_NEWTON_STEPS = 3;
    const float GOLDEN = 0.6180339887498949f;

    // a Hermite slope of the arc inversion past 3 could make it non-monotone (Fritsch-Carlson)
    const float MAX_HERMITE_SLOPE = 3.0f;

    float clamp01(float v){
        return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f;
    }

    curve::Segment segmentOf(const SplineData& s, const SplineCache& c, uint32_t segment){
        const std::vector<SplineKnot>& k = s.knots();
        const size_t next = segment + 1 < k.size() ? segment + 1 : 0;
        return curve::Segment{k[segment].position, c.controls[size_t(segment) * 2], c.controls[size_t(segment) * 2 + 1], k[next].position};
    }

    const SplineKnot& segmentEnd(const SplineData& s, uint32_t segment){
        const size_t next = segment + 1 < s.knotCount() ? segment + 1 : 0;
        return s.knot(next);
    }

    // a direction perpendicular to up, for a frame nothing else orients: -Z for +Y up, +Y for +Z
    Vector3 fallbackForward(const Vector3& up){
        return curve::normalizedOr(curve::cross(up, Vector3(1.0f, 0.0f, 0.0f)), Vector3(0.0f, 0.0f, -1.0f));
    }

    bool wellDefinedRight(const Vector3& forward, const Vector3& up){
        return curve::lengthSq(curve::cross(forward, up)) >= PARALLEL_SIN_SQ;
    }

    // the unit tangent of one segment at u; false where the segment gives none
    bool segmentForward(const curve::Segment& s, float u, Vector3& forward){
        const Vector3 d = s.derivative(u);
        const float dSq = curve::lengthSq(d);
        if (dSq >= MIN_DERIVATIVE_SQ && std::isfinite(dSq)){
            forward = d / std::sqrt(dSq);
            return true;
        }
        // a vanishing derivative (a zero Bezier handle at an end, a cusp): near it B' runs along
        // B'' (after the point) or -B'' (before it)
        Vector3 dd = s.secondDerivative(u);
        if (u > 0.5f){
            dd = -dd;
        }
        const float ddSq = curve::lengthSq(dd);
        if (ddSq >= MIN_DERIVATIVE_SQ && std::isfinite(ddSq)){
            forward = dd / std::sqrt(ddSq);
            return true;
        }
        const Vector3 chord = s.p3 - s.p0;
        const float chordSq = curve::lengthSq(chord);
        if (chordSq >= MIN_DERIVATIVE_SQ && std::isfinite(chordSq)){
            forward = chord / std::sqrt(chordSq);
            return true;
        }
        return false;
    }

    // the unit tangent at (segment, u): the segment's own, else (a zero-length segment) the
    // nearest segment's that has one (after, then before), else fallbackForward
    Vector3 forwardAt(const SplineData& s, const SplineCache& c, uint32_t segment, float u, const curve::Segment& seg){
        Vector3 forward;
        if (segmentForward(seg, u, forward)){
            return forward;
        }
        const uint32_t segments = c.segmentCount;
        const bool ring = s.effectivelyClosed();
        for (uint32_t k = 1; k < segments; k++){
            if (ring || segment + k < segments){
                if (segmentForward(segmentOf(s, c, (segment + k) % segments), 0.0f, forward)){
                    return forward;
                }
            }
            if (ring || segment >= k){
                if (segmentForward(segmentOf(s, c, (segment + segments - k) % segments), 1.0f, forward)){
                    return forward;
                }
            }
        }
        return fallbackForward(s.upAxis());
    }

    // global segment parameter x in [0, segments] -> (segment, u), wrapping on a ring
    void paramToSegment(float x, uint32_t segments, bool ring, uint32_t& segment, float& u){
        const float n = float(segments);
        if (ring && std::isfinite(x)){
            x = x - std::floor(x / n) * n;
        }
        x = x > 0.0f ? (x < n ? x : n) : 0.0f;
        segment = static_cast<uint32_t>(x);
        if (segment >= segments){
            segment = segments - 1;
        }
        u = clamp01(x - float(segment));
    }

    Vector3 forwardAtParam(const SplineData& s, const SplineCache& c, float x){
        uint32_t segment;
        float u;
        paramToSegment(x, c.segmentCount, s.effectivelyClosed(), segment, u);
        return forwardAt(s, c, segment, u, segmentOf(s, c, segment));
    }

    // R4 where forward is parallel to up: the right of the last well-defined frame before
    // (segment, u) (at an open start: the first after it), carried over by parallel transport.
    // Inside the 1e-4 cone forward turns by at most 2e-4 rad, so projecting that right onto the
    // plane normal to forward is the minimal rotation. A spline that is vertical everywhere gets
    // right = forward x -fallbackForward(up) (+X for a +Y run up the +Y axis).
    Vector3 transportedRight(const SplineData& s, const SplineCache& c, uint32_t segment, float u, const Vector3& forward){
        const Vector3& up = s.upAxis();
        const uint32_t segments = c.segmentCount;
        const bool ring = s.effectivelyClosed();
        const float x = float(segment) + u;
        const float step = 1.0f / float(TRANSPORT_SCAN_STEPS);
        const uint32_t scanSteps = segments * TRANSPORT_SCAN_STEPS;
        const float directions[2] = {-1.0f, 1.0f};
        for (float direction : directions){
            for (uint32_t k = 1; k <= scanSteps; k++){
                const float px = x + direction * float(k) * step;
                if (!ring && (px < 0.0f || px > float(segments))){
                    break;
                }
                if (!wellDefinedRight(forwardAtParam(s, c, px), up)){
                    continue;
                }
                float good = px;
                float bad = px - direction * step;
                for (uint32_t b = 0; b < TRANSPORT_BISECTIONS; b++){
                    const float mid = 0.5f * (good + bad);
                    if (wellDefinedRight(forwardAtParam(s, c, mid), up)){
                        good = mid;
                    }else{
                        bad = mid;
                    }
                }
                const Vector3 r = curve::normalizedOr(curve::cross(forwardAtParam(s, c, good), up), Vector3(1.0f, 0.0f, 0.0f));
                return curve::normalizedOr(r - forward * curve::dot(forward, r), Vector3(1.0f, 0.0f, 0.0f));
            }
            if (ring){
                // the backward scan went once around the loop
                break;
            }
        }
        return curve::normalizedOr(curve::cross(forward, -fallbackForward(up)), Vector3(1.0f, 0.0f, 0.0f));
    }

    void applyRoll(SplineFrame& frame){
        if (frame.rollDeg != 0.0f){
            const float a = frame.rollDeg * DEG_TO_RAD;
            const float cs = std::cos(a);
            const float sn = std::sin(a);
            const Vector3 right = frame.right * cs + frame.up * sn;
            const Vector3 up = frame.up * cs - frame.right * sn;
            frame.right = right;
            frame.up = up;
        }
    }

    // the frame of a spline without a segment: its one knot, or the default frame
    SplineFrame pointFrame(const SplineData& s){
        SplineFrame frame;
        if (s.knotCount() == 0){
            return frame;
        }
        const SplineKnot& k = s.knot(0);
        frame.position = k.position;
        frame.forward = fallbackForward(s.upAxis());
        frame.right = curve::normalizedOr(curve::cross(frame.forward, s.upAxis()), Vector3(1.0f, 0.0f, 0.0f));
        frame.up = curve::cross(frame.right, frame.forward);
        frame.radius = k.radius;
        frame.rollDeg = k.rollDeg;
        applyRoll(frame);
        return frame;
    }

    SplineFrame segmentFrame(const SplineData& s, const SplineCache& c, uint32_t segment, float u){
        const curve::Segment seg = segmentOf(s, c, segment);
        const SplineKnot& k0 = s.knot(segment);
        const SplineKnot& k1 = segmentEnd(s, segment);
        SplineFrame frame;
        frame.position = seg.position(u);
        frame.forward = forwardAt(s, c, segment, u, seg);
        const Vector3 r = curve::cross(frame.forward, s.upAxis());
        if (curve::lengthSq(r) >= PARALLEL_SIN_SQ){
            frame.right = r / std::sqrt(curve::lengthSq(r));
        }else{
            frame.right = transportedRight(s, c, segment, u, frame.forward);
        }
        frame.up = curve::cross(frame.right, frame.forward);
        frame.radius = curve::lerp(k0.radius, k1.radius, u);
        frame.rollDeg = curve::lerp(k0.rollDeg, k1.rollDeg, u);
        applyRoll(frame);
        return frame;
    }

    // global t -> (segment, u); t clamps to [0, 1]
    void decompose(uint32_t segments, float t, uint32_t& segment, float& u){
        const float x = clamp01(t) * float(segments);
        segment = static_cast<uint32_t>(x);
        if (segment >= segments){
            segment = segments - 1;
        }
        u = clamp01(x - float(segment));
    }

    // t outside [0, 1] wraps on a ring
    float wrapT(const SplineData& s, float t){
        if (s.effectivelyClosed() && std::isfinite(t) && (t < 0.0f || t > 1.0f)){
            t = t - std::floor(t);
        }
        return t;
    }

    // d into [0, length]: wrapped on a ring, clamped otherwise (NaN to 0)
    float sanitizedDistance(const SplineData& s, const SplineCache& c, float d){
        const float total = c.length;
        if (!(total > 0.0f)){
            return 0.0f;
        }
        if (s.effectivelyClosed() && std::isfinite(d) && (d < 0.0f || d > total)){
            d = d - std::floor(d / total) * total;
        }
        return d > 0.0f ? (d < total ? d : total) : 0.0f;
    }

    // arc distance d (in [0, length]) -> (segment, u): the LUT interval by binary search, then
    // a cubic Hermite of u over it whose end slopes are 1 / speed, which is exact to fourth order
    void locate(const SplineData& s, const SplineCache& c, float d, uint32_t& segment, float& u){
        const size_t count = c.arc.size();
        const float* arc = c.arc.data();
        size_t index = static_cast<size_t>(std::lower_bound(arc, arc + count, d) - arc);
        if (index >= count){
            index = count - 1;
        }
        const uint32_t steps = c.arcSteps;
        segment = static_cast<uint32_t>(index / steps);
        const uint32_t i = static_cast<uint32_t>(index % steps);
        const float s0 = index > 0 ? arc[index - 1] : 0.0f;
        const float h = arc[index] - s0;
        if (!(h > 0.0f)){
            u = float(i) / float(steps);
            return;
        }
        const float x = clamp01((d - s0) / h);
        const curve::Segment seg = segmentOf(s, c, segment);
        const float du = 1.0f / float(steps);
        const float v0 = seg.speed(float(i) / float(steps));
        const float v1 = seg.speed(float(i + 1) / float(steps));
        float m0 = v0 > 0.0f ? h / (du * v0) : MAX_HERMITE_SLOPE;
        float m1 = v1 > 0.0f ? h / (du * v1) : MAX_HERMITE_SLOPE;
        m0 = m0 < MAX_HERMITE_SLOPE ? (m0 > 0.0f ? m0 : 0.0f) : MAX_HERMITE_SLOPE;
        m1 = m1 < MAX_HERMITE_SLOPE ? (m1 > 0.0f ? m1 : 0.0f) : MAX_HERMITE_SLOPE;
        const float x2 = x * x;
        const float x3 = x2 * x;
        const float hermite = (x3 - 2.0f * x2 + x) * m0 + (3.0f * x2 - 2.0f * x3) + (x3 - x2) * m1;
        u = clamp01((float(i) + hermite) / float(steps));
    }

    struct Closest{
        float distSq = INF;
        uint32_t segment = 0;
        float u = 0.0f;
        Vector3 position;
    };

    float metricSq(const Vector3& a, const Vector3& p, bool xz){
        const float dx = a.x - p.x;
        const float dz = a.z - p.z;
        if (xz){
            return dx * dx + dz * dz;
        }
        const float dy = a.y - p.y;
        return dx * dx + dy * dy + dz * dz;
    }

    float boundsSq(const SplineSegmentBounds& b, const Vector3& p, bool xz){
        const float dx = std::max(0.0f, std::max(b.min.x - p.x, p.x - b.max.x));
        const float dz = std::max(0.0f, std::max(b.min.z - p.z, p.z - b.max.z));
        if (xz){
            return dx * dx + dz * dz;
        }
        const float dy = std::max(0.0f, std::max(b.min.y - p.y, p.y - b.max.y));
        return dx * dx + dy * dy + dz * dz;
    }

    // every local minimum of the coarse scan of one segment, refined; best keeps the nearest
    void refineSegment(const curve::Segment& seg, uint32_t segment, const Vector3& p, bool xz, Closest& best){
        float samples[CLOSEST_SAMPLES + 1];
        for (uint32_t i = 0; i <= CLOSEST_SAMPLES; i++){
            samples[i] = metricSq(seg.position(float(i) / float(CLOSEST_SAMPLES)), p, xz);
        }
        const auto f = [&](float u){ return metricSq(seg.position(u), p, xz); };
        for (uint32_t i = 0; i <= CLOSEST_SAMPLES; i++){
            const float left = i > 0 ? samples[i - 1] : INF;
            const float right = i < CLOSEST_SAMPLES ? samples[i + 1] : INF;
            if (!(samples[i] <= left && samples[i] <= right) || (i > 0 && samples[i] == left)){
                continue;
            }
            const float lo = float(i > 0 ? i - 1 : 0) / float(CLOSEST_SAMPLES);
            const float hi = float(i < CLOSEST_SAMPLES ? i + 1 : CLOSEST_SAMPLES) / float(CLOSEST_SAMPLES);

            float a = lo;
            float b = hi;
            float c1 = b - GOLDEN * (b - a);
            float c2 = a + GOLDEN * (b - a);
            float f1 = f(c1);
            float f2 = f(c2);
            for (uint32_t g = 0; g < CLOSEST_GOLDEN_STEPS; g++){
                if (f1 < f2){
                    b = c2;
                    c2 = c1;
                    f2 = f1;
                    c1 = b - GOLDEN * (b - a);
                    f1 = f(c1);
                }else{
                    a = c1;
                    c1 = c2;
                    f1 = f2;
                    c2 = a + GOLDEN * (b - a);
                    f2 = f(c2);
                }
            }
            float uBest = f1 < f2 ? c1 : c2;
            float fBest = f1 < f2 ? f1 : f2;
            if (samples[i] < fBest){
                uBest = float(i) / float(CLOSEST_SAMPLES);
                fBest = samples[i];
            }

            float u = uBest;
            for (uint32_t n = 0; n < CLOSEST_NEWTON_STEPS; n++){
                Vector3 e = seg.position(u) - p;
                Vector3 d1 = seg.derivative(u);
                Vector3 d2 = seg.secondDerivative(u);
                if (xz){
                    e.y = 0.0f;
                    d1.y = 0.0f;
                    d2.y = 0.0f;
                }
                const float g = curve::dot(e, d1);
                const float gp = curve::dot(d1, d1) + curve::dot(e, d2);
                if (!(gp > 0.0f)){
                    break;
                }
                const float un = std::clamp(u - g / gp, lo, hi);
                const float fn = f(un);
                if (!(fn < fBest)){
                    break;
                }
                uBest = un;
                fBest = fn;
                u = un;
            }

            if (fBest < best.distSq){
                best.distSq = fBest;
                best.segment = segment;
                best.u = uBest;
                best.position = seg.position(uBest);
            }
        }
    }

    Closest closestImpl(const SplineData& s, const SplineCache& c, const Vector3& p, bool xz){
        Closest best;
        if (s.knotCount() == 0){
            return best;
        }
        const uint32_t segments = c.segmentCount;
        if (segments == 0){
            best.position = s.knot(0).position;
            best.distSq = metricSq(best.position, p, xz);
            return best;
        }
        // the segment whose box is nearest goes first, so the rest are culled against it
        uint32_t seed = 0;
        float seedSq = INF;
        for (uint32_t i = 0; i < segments; i++){
            const float b = boundsSq(c.bounds[i], p, xz);
            if (b < seedSq){
                seedSq = b;
                seed = i;
            }
        }
        refineSegment(segmentOf(s, c, seed), seed, p, xz, best);
        for (uint32_t i = 0; i < segments; i++){
            if (i != seed && boundsSq(c.bounds[i], p, xz) < best.distSq){
                refineSegment(segmentOf(s, c, i), i, p, xz, best);
            }
        }
        return best;
    }

    SplineHit toHit(const SplineCache& c, const Closest& closest){
        SplineHit hit;
        hit.distance = std::sqrt(closest.distSq);
        hit.position = closest.position;
        hit.segment = closest.segment;
        hit.t = c.segmentCount > 0 ? (float(closest.segment) + closest.u) / float(c.segmentCount) : 0.0f;
        return hit;
    }

    float radiusAt(const SplineData& s, const SplineCache& c, const Closest& closest){
        if (c.segmentCount == 0){
            return s.knotCount() > 0 ? s.knot(0).radius : 0.0f;
        }
        return curve::lerp(s.knot(closest.segment).radius, segmentEnd(s, closest.segment).radius, closest.u);
    }

    // Is (x, z) enclosed by the closed curve? Within the polygon's error of the curve the polygon
    // cannot tell, so there the side of the curve's tangent at the closest point decides (the
    // loop's orientation is the polygon's area sign); elsewhere an even-odd crossing test.
    bool enclosedXZ(const SplineData& s, const SplineCache& c, const Closest& closest, float x, float z){
        if (std::sqrt(closest.distSq) <= c.polygonError){
            const Vector3 d = segmentOf(s, c, closest.segment).derivative(closest.u);
            if (d.x * d.x + d.z * d.z > MIN_DERIVATIVE_SQ){
                const float side = d.x * (z - closest.position.z) - d.z * (x - closest.position.x);
                if (side != 0.0f){
                    return (side > 0.0f) == (c.polygonArea > 0.0f);
                }
            }
        }
        const std::vector<float>& poly = c.polygonXZ;
        const size_t vertices = poly.size() / 2;
        bool inside = false;
        for (size_t i = 0, j = vertices - 1; i < vertices; j = i++){
            const float xi = poly[i * 2];
            const float zi = poly[i * 2 + 1];
            const float xj = poly[j * 2];
            const float zj = poly[j * 2 + 1];
            if ((zi > z) != (zj > z)){
                const float crossing = (xj - xi) * (z - zi) / (zj - zi) + xi;
                if (x < crossing){
                    inside = !inside;
                }
            }
        }
        return inside;
    }

}

SplineFrame spline::evaluate(const SplineData& spline, float t){
    const SplineCache& c = spline.ensureCache();
    if (c.segmentCount == 0){
        return pointFrame(spline);
    }
    uint32_t segment;
    float u;
    decompose(c.segmentCount, wrapT(spline, t), segment, u);
    return segmentFrame(spline, c, segment, u);
}

SplineFrame spline::evaluateSegment(const SplineData& spline, uint32_t segment, float u){
    const SplineCache& c = spline.ensureCache();
    if (c.segmentCount == 0){
        return pointFrame(spline);
    }
    return segmentFrame(spline, c, std::min(segment, c.segmentCount - 1), clamp01(u));
}

SplineFrame spline::evaluateAtDistance(const SplineData& spline, float d){
    const SplineCache& c = spline.ensureCache();
    if (c.segmentCount == 0){
        return pointFrame(spline);
    }
    uint32_t segment;
    float u;
    locate(spline, c, sanitizedDistance(spline, c, d), segment, u);
    return segmentFrame(spline, c, segment, u);
}

Vector3 spline::evaluatePosition(const SplineData& spline, float t){
    const SplineCache& c = spline.ensureCache();
    if (c.segmentCount == 0){
        return spline.knotCount() > 0 ? spline.knot(0).position : Vector3(0.0f, 0.0f, 0.0f);
    }
    uint32_t segment;
    float u;
    decompose(c.segmentCount, wrapT(spline, t), segment, u);
    return segmentOf(spline, c, segment).position(u);
}

float spline::length(const SplineData& spline){
    return spline.ensureCache().length;
}

float spline::tToDistance(const SplineData& spline, float t){
    const SplineCache& c = spline.ensureCache();
    if (c.segmentCount == 0){
        return 0.0f;
    }
    uint32_t segment;
    float u;
    decompose(c.segmentCount, t, segment, u);
    const uint32_t steps = c.arcSteps;
    uint32_t i = static_cast<uint32_t>(u * float(steps));
    if (i >= steps){
        i = steps - 1;
    }
    const size_t index = size_t(segment) * steps + i;
    const float s0 = index > 0 ? c.arc[index - 1] : 0.0f;
    const float u0 = float(i) / float(steps);
    if (!(u > u0)){
        return s0;
    }
    return s0 + curve::arcLength(segmentOf(spline, c, segment), u0, u);
}

float spline::distanceToT(const SplineData& spline, float d){
    const SplineCache& c = spline.ensureCache();
    if (c.segmentCount == 0){
        return 0.0f;
    }
    uint32_t segment;
    float u;
    locate(spline, c, sanitizedDistance(spline, c, d), segment, u);
    return (float(segment) + u) / float(c.segmentCount);
}

void spline::distanceToSegment(const SplineData& spline, float d, uint32_t& segment, float& u){
    const SplineCache& c = spline.ensureCache();
    segment = 0;
    u = 0.0f;
    if (c.segmentCount > 0){
        locate(spline, c, sanitizedDistance(spline, c, d), segment, u);
    }
}

void spline::tToSegment(const SplineData& spline, float t, uint32_t& segment, float& u){
    const SplineCache& c = spline.ensureCache();
    segment = 0;
    u = 0.0f;
    if (c.segmentCount > 0){
        decompose(c.segmentCount, wrapT(spline, t), segment, u);
    }
}

float spline::segmentToT(const SplineData& spline, uint32_t segment, float u){
    const uint32_t segments = spline.segmentCount();
    if (segments == 0){
        return 0.0f;
    }
    return clamp01((float(std::min(segment, segments - 1)) + clamp01(u)) / float(segments));
}

SplineHit spline::closestPoint(const SplineData& spline, const Vector3& p){
    const SplineCache& c = spline.ensureCache();
    return toHit(c, closestImpl(spline, c, p, false));
}

SplineHit spline::closestPointXZ(const SplineData& spline, float x, float z){
    const SplineCache& c = spline.ensureCache();
    return toHit(c, closestImpl(spline, c, Vector3(x, 0.0f, z), true));
}

float spline::signedDistance(const SplineData& spline, const Vector3& p){
    const SplineCache& c = spline.ensureCache();
    const Closest closest = closestImpl(spline, c, p, false);
    return std::sqrt(closest.distSq) - radiusAt(spline, c, closest);
}

float spline::signedDistanceXZ(const SplineData& spline, float x, float z){
    const SplineCache& c = spline.ensureCache();
    const Closest closest = closestImpl(spline, c, Vector3(x, 0.0f, z), true);
    const float d = std::sqrt(closest.distSq);
    const float r = radiusAt(spline, c, closest);
    if (spline.effectivelyClosed() && enclosedXZ(spline, c, closest, x, z)){
        return -(d + r);
    }
    return d - r;
}

float spline::signedDistanceBandXZ(const SplineData& spline, float x, float z){
    const SplineCache& c = spline.ensureCache();
    const Closest closest = closestImpl(spline, c, Vector3(x, 0.0f, z), true);
    return std::sqrt(closest.distSq) - radiusAt(spline, c, closest);
}

void spline::sampleUniform(const SplineData& spline, uint32_t count, std::vector<SplineFrame>& frames){
    frames.clear();
    if (count == 0){
        return;
    }
    frames.reserve(count);
    const float total = length(spline);
    if (count == 1){
        frames.push_back(evaluateAtDistance(spline, 0.0f));
        return;
    }
    for (uint32_t i = 0; i < count; i++){
        const float d = i + 1 == count ? total : total * (float(i) / float(count - 1));
        frames.push_back(evaluateAtDistance(spline, d));
    }
}

bool spline::resample(SplineData& spline, float spacing, float simplifyTolerance){
    const float total = length(spline);
    if (!spline.isValid() || !(total > 0.0f) || !std::isfinite(total) || !(spacing > 0.0f)){
        return false;
    }
    const bool ring = spline.effectivelyClosed();
    const float maxIntervals = 65535.0f;
    const float wanted = std::ceil(total / spacing);
    uint32_t intervals = wanted < maxIntervals ? static_cast<uint32_t>(wanted) : static_cast<uint32_t>(maxIntervals);
    intervals = std::max(intervals, ring ? uint32_t(SplineData::MIN_CLOSED_KNOTS) : uint32_t(1));

    std::vector<SplineFrame> frames;
    sampleUniform(spline, intervals + 1, frames);
    if (simplifyTolerance > 0.0f){
        // a ring simplifies as the open run that returns to its start, which keeps the start
        std::vector<Vector3> positions(frames.size());
        for (size_t i = 0; i < frames.size(); i++){
            positions[i] = frames[i].position;
        }
        std::vector<uint32_t> kept;
        simplifyIndices(positions, simplifyTolerance, kept);
        std::vector<SplineFrame> keptFrames;
        keptFrames.reserve(kept.size());
        for (uint32_t index : kept){
            keptFrames.push_back(frames[index]);
        }
        if (ring && keptFrames.size() < SplineData::MIN_CLOSED_KNOTS + 1){
            // a loop needs three knots: keep the samples a third and two thirds around
            keptFrames = {frames[0], frames[intervals / 3], frames[(2 * intervals) / 3], frames[intervals]};
        }
        frames = std::move(keptFrames);
    }
    if (ring){
        // the last sample repeats the first
        frames.pop_back();
    }
    spline.replaceKnotsFromFrames(frames);
    return true;
}
