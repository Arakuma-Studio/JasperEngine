// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/Spline/Include/Spline/SplineUtility.h, Copyright (c)
// 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#include "SplineUtility.h"

#include "SplineCurve.h"

using namespace jasperengine;
using namespace jasperengine::spline;

float spline::pointSegmentDistanceSq(const Vector3& p, const Vector3& a, const Vector3& b){
    const Vector3 ab = b - a;
    const float abSq = curve::lengthSq(ab);
    if (!(abSq >= 1e-12f)){
        return curve::lengthSq(p - a);
    }
    float t = curve::dot(p - a, ab) / abSq;
    t = t > 0.0f ? (t < 1.0f ? t : 1.0f) : 0.0f;
    return curve::lengthSq(p - (a + ab * t));
}

void spline::simplifyIndices(std::span<const Vector3> points, float tolerance, std::vector<uint32_t>& kept){
    kept.clear();
    const uint32_t n = static_cast<uint32_t>(points.size());
    if (n <= 2){
        for (uint32_t i = 0; i < n; i++){
            kept.push_back(i);
        }
        return;
    }

    const float toleranceSq = tolerance * tolerance;
    struct Range{
        uint32_t first;
        uint32_t last;
    };
    std::vector<Range> stack;
    std::vector<uint8_t> keep(n, 0);
    keep[0] = 1;
    keep[n - 1] = 1;
    stack.push_back({0, n - 1});

    while (!stack.empty()){
        const Range range = stack.back();
        stack.pop_back();
        if (range.last <= range.first + 1){
            continue;
        }
        float farthestSq = 0.0f;
        uint32_t farthest = range.first;
        for (uint32_t i = range.first + 1; i < range.last; i++){
            const float d = pointSegmentDistanceSq(points[i], points[range.first], points[range.last]);
            if (d > farthestSq){
                farthestSq = d;
                farthest = i;
            }
        }
        if (farthestSq > toleranceSq){
            keep[farthest] = 1;
            stack.push_back({range.first, farthest});
            stack.push_back({farthest, range.last});
        }
    }

    for (uint32_t i = 0; i < n; i++){
        if (keep[i]){
            kept.push_back(i);
        }
    }
}

void spline::simplifyPolyline(std::span<const Vector3> points, float tolerance, std::vector<Vector3>& out){
    std::vector<uint32_t> kept;
    simplifyIndices(points, tolerance, kept);
    out.clear();
    out.reserve(kept.size());
    for (uint32_t index : kept){
        out.push_back(points[index]);
    }
}
