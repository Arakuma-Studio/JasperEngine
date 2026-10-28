// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SplinePolygonFill.h"

#include "SplineCurve.h"
#include "SplineEvaluator.h"
#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::spline;

namespace{

    // |sin| of the turn below which a vertex continues a straight run
    const double COLLINEAR_SIN = 1e-10;
    // the most points sampleFillOutline emits
    const float MAX_OUTLINE_POINTS = 65536.0f;

    // the arithmetic runs in double: exact for the differences and products of float inputs that
    // decide orientation, so the result does not depend on rounding
    struct Point{
        double x;
        double y;
    };

    double cross(const Point& o, const Point& a, const Point& b){
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    }

    int orientation(const Point& o, const Point& a, const Point& b){
        const double c = cross(o, a, b);
        return (c > 0.0) - (c < 0.0);
    }

    // p, known collinear with a -> b, lies on that segment
    bool onSegment(const Point& a, const Point& b, const Point& p){
        return p.x >= std::min(a.x, b.x) && p.x <= std::max(a.x, b.x) &&
               p.y >= std::min(a.y, b.y) && p.y <= std::max(a.y, b.y);
    }

    // the closed segments a -> b and c -> d share a point
    bool segmentsTouch(const Point& a, const Point& b, const Point& c, const Point& d){
        const int o1 = orientation(a, b, c);
        const int o2 = orientation(a, b, d);
        const int o3 = orientation(c, d, a);
        const int o4 = orientation(c, d, b);
        if (o1 * o2 < 0 && o3 * o4 < 0){
            return true;
        }
        return (o1 == 0 && onSegment(a, b, c)) || (o2 == 0 && onSegment(a, b, d)) ||
               (o3 == 0 && onSegment(c, d, a)) || (o4 == 0 && onSegment(c, d, b));
    }

    // The first pair of edges that cross, touch or (adjacent ones) fold back onto each other.
    // Edges are swept in order of their smallest x, each tested against those whose x range
    // overlaps its own.
    bool findSelfIntersection(const std::vector<Point>& pts, uint32_t& edgeA, uint32_t& edgeB){
        const uint32_t n = static_cast<uint32_t>(pts.size());
        std::vector<double> minX(n);
        std::vector<double> maxX(n);
        std::vector<uint32_t> order(n);
        for (uint32_t i = 0; i < n; i++){
            const Point& a = pts[i];
            const Point& b = pts[(i + 1) % n];
            minX[i] = std::min(a.x, b.x);
            maxX[i] = std::max(a.x, b.x);
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [&](uint32_t l, uint32_t r){
            return minX[l] < minX[r] || (minX[l] == minX[r] && l < r);
        });
        for (uint32_t oi = 0; oi < n; oi++){
            const uint32_t i = order[oi];
            for (uint32_t oj = oi + 1; oj < n && minX[order[oj]] <= maxX[i]; oj++){
                const uint32_t j = order[oj];
                const uint32_t lo = std::min(i, j);
                const uint32_t hi = std::max(i, j);
                bool bad;
                if (hi == lo + 1 || (lo == 0 && hi == n - 1)){
                    // adjacent: the shared vertex v, the far ends a and b; overlapping when all
                    // three lie on one line with a and b on the same side of v
                    const uint32_t first = (hi == lo + 1) ? lo : hi;
                    const Point& a = pts[first];
                    const Point& v = pts[(first + 1) % n];
                    const Point& b = pts[(first + 2) % n];
                    bad = orientation(a, v, b) == 0 && (a.x - v.x) * (b.x - v.x) + (a.y - v.y) * (b.y - v.y) > 0.0;
                }else{
                    bad = segmentsTouch(pts[i], pts[(i + 1) % n], pts[j], pts[(j + 1) % n]);
                }
                if (bad){
                    edgeA = lo;
                    edgeB = hi;
                    return true;
                }
            }
        }
        return false;
    }

    bool fail(SplineFillIssue* issue, SplineFillError error, uint32_t edgeA = 0, uint32_t edgeB = 0){
        if (issue){
            issue->error = error;
            issue->edgeA = edgeA;
            issue->edgeB = edgeB;
        }
        return false;
    }

}

const char* spline::fillErrorReason(SplineFillError error){
    switch (error){
        case SplineFillError::None: return "";
        case SplineFillError::TooFewPoints: return "the shape has fewer than three distinct points";
        case SplineFillError::ZeroArea: return "the shape encloses no area";
        case SplineFillError::SelfIntersecting: return "the shape crosses or touches itself";
        case SplineFillError::NoEar: return "the shape could not be triangulated";
    }
    return "";
}

bool spline::triangulatePolygon(std::span<const Vector2> polygon, std::vector<uint32_t>& triangles, SplineFillIssue* issue){
    triangles.clear();
    if (issue){
        *issue = SplineFillIssue();
    }

    // the distinct points (consecutive repeats and the closing repeat skipped), as indices
    std::vector<uint32_t> source;
    std::vector<Point> pts;
    source.reserve(polygon.size());
    pts.reserve(polygon.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(polygon.size()); i++){
        const Point p{double(polygon[i].x), double(polygon[i].y)};
        if (!std::isfinite(p.x) || !std::isfinite(p.y)){
            return fail(issue, SplineFillError::TooFewPoints);
        }
        if (pts.empty() || p.x != pts.back().x || p.y != pts.back().y){
            source.push_back(i);
            pts.push_back(p);
        }
    }
    while (pts.size() > 1 && pts.back().x == pts.front().x && pts.back().y == pts.front().y){
        pts.pop_back();
        source.pop_back();
    }
    const uint32_t n = static_cast<uint32_t>(pts.size());
    if (n < 3){
        return fail(issue, SplineFillError::TooFewPoints);
    }

    double area2 = 0.0;
    double extent = 0.0;
    for (uint32_t i = 0; i < n; i++){
        const Point& a = pts[i];
        const Point& b = pts[(i + 1) % n];
        area2 += a.x * b.y - b.x * a.y;
        extent = std::max({extent, std::fabs(a.x - pts[0].x), std::fabs(a.y - pts[0].y)});
    }
    if (!(std::fabs(area2) > 1e-12 * extent * extent)){
        return fail(issue, SplineFillError::ZeroArea);
    }
    uint32_t edgeA = 0;
    uint32_t edgeB = 0;
    if (findSelfIntersection(pts, edgeA, edgeB)){
        return fail(issue, SplineFillError::SelfIntersecting, edgeA, edgeB);
    }

    // clip ears of the counter-clockwise loop (a clockwise input runs backwards)
    const bool ccw = area2 > 0.0;
    std::vector<uint32_t> prev(n);
    std::vector<uint32_t> next(n);
    for (uint32_t i = 0; i < n; i++){
        prev[i] = ccw ? (i + n - 1) % n : (i + 1) % n;
        next[i] = ccw ? (i + 1) % n : (i + n - 1) % n;
    }
    const auto earIsEmpty = [&](uint32_t a, uint32_t b, uint32_t c){
        for (uint32_t v = next[c]; v != a; v = next[v]){
            if (cross(pts[a], pts[b], pts[v]) >= 0.0 && cross(pts[b], pts[c], pts[v]) >= 0.0 &&
                cross(pts[c], pts[a], pts[v]) >= 0.0){
                return false;
            }
        }
        return true;
    };
    const auto unlink = [&](uint32_t v){
        next[prev[v]] = next[v];
        prev[next[v]] = prev[v];
    };
    // v continues a straight run between its neighbours: it adds no area, only slivers
    const auto straight = [&](uint32_t v){
        const Point& pa = pts[prev[v]];
        const Point& pb = pts[v];
        const Point& pc = pts[next[v]];
        const double e1 = std::hypot(pb.x - pa.x, pb.y - pa.y);
        const double e2 = std::hypot(pc.x - pb.x, pc.y - pb.y);
        return std::fabs(cross(pa, pb, pc)) <= COLLINEAR_SIN * e1 * e2 &&
               (pb.x - pa.x) * (pc.x - pb.x) + (pb.y - pa.y) * (pc.y - pb.y) > 0.0;
    };

    uint32_t remaining = n;
    // drop the straight-run vertices first (rechecking the one before each removal)
    uint32_t current = 0;
    for (uint32_t checked = 0; checked < remaining && remaining > 3;){
        if (straight(current)){
            const uint32_t before = prev[current];
            unlink(current);
            remaining--;
            current = before;
            checked = 0;
        }else{
            current = next[current];
            checked++;
        }
    }

    triangles.reserve(size_t(remaining - 2) * 3);
    uint32_t stalled = 0;
    while (remaining > 3){
        const uint32_t a = prev[current];
        const uint32_t c = next[current];
        if (straight(current)){
            // clipping an ear can leave a neighbour on a straight run
            unlink(current);
            remaining--;
            current = c;
            stalled = 0;
            continue;
        }
        if (cross(pts[a], pts[current], pts[c]) > 0.0 && earIsEmpty(a, current, c)){
            triangles.push_back(source[a]);
            triangles.push_back(source[current]);
            triangles.push_back(source[c]);
            unlink(current);
            remaining--;
            current = c;
            stalled = 0;
            continue;
        }
        current = c;
        if (++stalled > remaining){
            triangles.clear();
            return fail(issue, SplineFillError::NoEar);
        }
    }
    const uint32_t a = prev[current];
    const uint32_t c = next[current];
    if (cross(pts[a], pts[current], pts[c]) > 0.0){
        triangles.push_back(source[a]);
        triangles.push_back(source[current]);
        triangles.push_back(source[c]);
    }
    return true;
}

bool spline::buildPolygonFill(std::span<const Vector3> outline, const SplineFillOptions& options,
                              std::vector<SplineVertex>& vertices, std::vector<uint32_t>& indices,
                              SplineFillIssue* issue){
    vertices.clear();
    indices.clear();
    // the 2D frame: counter-clockwise in (x, -z) faces +Y, in (x, y) faces +Z
    const bool xz = options.plane == SplineFillPlane::XZ;
    std::vector<Vector2> polygon;
    polygon.reserve(outline.size());
    for (const Vector3& p : outline){
        polygon.push_back(xz ? Vector2(p.x, -p.z) : Vector2(p.x, p.y));
    }
    if (!triangulatePolygon(polygon, indices, issue)){
        return false;
    }
    vertices.resize(outline.size());
    for (size_t i = 0; i < outline.size(); i++){
        const Vector3& p = outline[i];
        SplineVertex& v = vertices[i];
        v.position = xz ? Vector3(p.x, options.height, p.z) : Vector3(p.x, p.y, options.height);
        v.normal = xz ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(0.0f, 0.0f, 1.0f);
        // tangent +X along U; the bitangent normal x tangent (-Z in XZ, +Y in XY) runs along V
        v.tangent = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
        v.uv = Vector2(polygon[i].x * options.tilingU, polygon[i].y * options.tilingV);
        v.color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    }
    return true;
}

void spline::sampleFillOutline(const SplineData& spline, float spacing, std::vector<Vector3>& outline){
    outline.clear();
    const float total = length(spline);
    if (!spline.isValid() || !(spacing > 0.0f) || !(total > 0.0f) || !std::isfinite(total)){
        return;
    }
    const float wanted = std::ceil(total / spacing);
    const uint32_t count = std::max(3u, wanted < MAX_OUTLINE_POINTS ? static_cast<uint32_t>(wanted) : static_cast<uint32_t>(MAX_OUTLINE_POINTS));
    outline.reserve(count);
    for (uint32_t i = 0; i < count; i++){
        outline.push_back(evaluateAtDistance(spline, total * (float(i) / float(count))).position);
    }
}

float spline::meanKnotHeight(const SplineData& spline){
    if (spline.knotCount() == 0){
        return 0.0f;
    }
    double sum = 0.0;
    for (const SplineKnot& k : spline.knots()){
        sum += double(curve::dot(k.position, spline.upAxis()));
    }
    return float(sum / double(spline.knotCount()));
}
