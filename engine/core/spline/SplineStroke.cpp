// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Apps/Editor/Source/SceneView/SplineTool.cpp (FinalizeSpline,
// SimplifyPolyline, SnapPointsToSurface, TryExtendExistingSpline) and SplineStrokeCapture.cpp,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#include "SplineStroke.h"

#include "SplineCurve.h"
#include "SplineUtility.h"
#include <algorithm>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::spline;

namespace{

    // a point this close to the one before it repeats it
    const float REPEATED_POINT_SQ = 1e-12f;
    // a Catmull-Rom tangent (next - prev) / 2 as a Bezier handle (a third of it)
    const float AUTO_BEZIER_HANDLE_SCALE = 1.0f / 6.0f;

}

void spline::smoothPolyline(std::vector<Vector3>& points, uint32_t iterations){
    const size_t n = points.size();
    if (n < 3 || iterations == 0){
        return;
    }
    std::vector<Vector3> next(n);
    for (uint32_t it = 0; it < iterations; it++){
        next[0] = points[0];
        next[n - 1] = points[n - 1];
        for (size_t i = 1; i + 1 < n; i++){
            next[i] = points[i] * 0.5f + (points[i - 1] + points[i + 1]) * 0.25f;
        }
        std::swap(points, next);
    }
}

uint32_t spline::snapPoints(std::vector<Vector3>& points, const SplineSurfaceProbe& probe, const Vector3& upAxis){
    if (!probe){
        return 0;
    }
    const Vector3 up = curve::normalizedOr(upAxis, Vector3(0.0f, 1.0f, 0.0f));
    uint32_t hits = 0;
    for (Vector3& p : points){
        const SplineSurfaceSample sample = probe(p);
        if (sample.hit){
            p = p + up * (sample.height - curve::dot(p, up));
            hits++;
        }
    }
    return hits;
}

bool spline::shapeStroke(std::span<const Vector3> stroke, const SplineStrokeOptions& options,
                         const SplineSurfaceProbe* probe, SplineStrokeShape& out){
    out.points.clear();
    out.closed = false;

    std::vector<Vector3> dense;
    dense.reserve(stroke.size());
    for (const Vector3& p : stroke){
        if (dense.empty() || curve::lengthSq(p - dense.back()) > REPEATED_POINT_SQ){
            dense.push_back(p);
        }
    }
    if (dense.size() < 2){
        return false;
    }

    // closing: back on the start after leaving it (a short scribble at the start stays open)
    const float closeSq = options.closeRadius * options.closeRadius;
    bool leftStart = false;
    for (const Vector3& p : dense){
        leftStart = leftStart || curve::lengthSq(p - dense.front()) > 4.0f * closeSq;
    }
    const bool closes = options.closeRadius > 0.0f && dense.size() >= 4 && leftStart &&
                        curve::lengthSq(dense.back() - dense.front()) <= closeSq;

    smoothPolyline(dense, options.smoothIterations);
    simplifyPolyline(dense, options.simplifyTolerance, out.points);

    if (closes){
        while (out.points.size() > SplineData::MIN_CLOSED_KNOTS &&
               curve::lengthSq(out.points.back() - out.points.front()) <= closeSq){
            out.points.pop_back();
        }
        out.closed = out.points.size() >= SplineData::MIN_CLOSED_KNOTS;
    }

    if (probe){
        snapPoints(out.points, *probe, options.upAxis);
    }
    return out.points.size() >= SplineData::MIN_KNOTS;
}

bool spline::strokeToSpline(std::span<const Vector3> stroke, const SplineStrokeOptions& options,
                            const SplineSurfaceProbe* probe, SplineData& spline){
    SplineStrokeShape shape;
    if (!shapeStroke(stroke, options, probe, shape)){
        return false;
    }
    std::vector<SplineKnot> knots(shape.points.size());
    for (size_t i = 0; i < knots.size(); i++){
        knots[i].position = shape.points[i];
        knots[i].radius = options.knotRadius;
    }
    spline.setType(SplineType::CatmullRom);
    spline.setClosed(shape.closed);
    spline.setUpAxis(options.upAxis);
    spline.setKnots(std::move(knots));
    return true;
}

SplineStrokeExtension spline::extendWithStroke(SplineData& spline, std::span<const Vector3> points, float pickRadius){
    if (spline.closed() || spline.knotCount() == 0 || points.empty()){
        return SplineStrokeExtension::None;
    }
    const std::vector<SplineKnot>& knots = spline.knots();
    const Vector3& start = points[0];
    const float pickSq = pickRadius * pickRadius;
    const float toLast = curve::lengthSq(start - knots.back().position);
    const float toFirst = curve::lengthSq(start - knots.front().position);
    const bool append = toLast <= pickSq && toLast <= toFirst;
    const bool prepend = !append && toFirst <= pickSq;
    if (!append && !prepend){
        return SplineStrokeExtension::None;
    }

    // the new knots in stroke order, leaving the endpoint
    const SplineKnot& endpoint = append ? knots.back() : knots.front();
    std::vector<SplineKnot> added;
    Vector3 previous = endpoint.position;
    for (size_t i = 1; i < points.size(); i++){
        if (curve::lengthSq(points[i] - previous) <= REPEATED_POINT_SQ){
            continue;
        }
        SplineKnot k;
        k.position = points[i];
        k.radius = endpoint.radius;
        k.rollDeg = endpoint.rollDeg;
        k.mode = endpoint.mode;
        added.push_back(k);
        previous = points[i];
    }
    if (added.empty()){
        return SplineStrokeExtension::None;
    }

    if (spline.type() == SplineType::CubicBezier){
        // handles from centred differences along endpoint, added...; the curve runs away from
        // the endpoint when appending and toward it when prepending
        const float direction = append ? 1.0f : -1.0f;
        for (size_t j = 0; j < added.size(); j++){
            const Vector3& prev = j > 0 ? added[j - 1].position : endpoint.position;
            const Vector3& next = j + 1 < added.size() ? added[j + 1].position : added[j].position;
            const Vector3 handle = (next - prev) * (AUTO_BEZIER_HANDLE_SCALE * direction);
            added[j].tangentOut = handle;
            added[j].tangentIn = -handle;
        }
    }

    std::vector<SplineKnot> extended;
    extended.reserve(knots.size() + added.size());
    if (append){
        extended.insert(extended.end(), knots.begin(), knots.end());
        extended.insert(extended.end(), added.begin(), added.end());
    }else{
        extended.insert(extended.end(), added.rbegin(), added.rend());
        extended.insert(extended.end(), knots.begin(), knots.end());
    }
    spline.setKnots(std::move(extended));
    return append ? SplineStrokeExtension::Appended : SplineStrokeExtension::Prepended;
}
