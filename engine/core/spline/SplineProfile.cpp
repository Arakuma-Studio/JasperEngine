// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Source/SplineProfile.cpp,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#include "SplineProfile.h"

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace jasperengine;
using namespace jasperengine::spline;

namespace{

    // Two profile points closer than this are the same point: well below any authorable
    // dimension (a tenth of a millimetre), so a real cross-section is never collapsed.
    constexpr float COINCIDENT_EPSILON = 1.0e-4f;

    // Authored floats reach the generators unsanitized; a NaN would propagate into the mesh
    // bounds, where a NaN AABB takes culling with it.
    float finiteOr(float v, float fallback){
        return std::isfinite(v) ? v : fallback;
    }

    // an extent is non-negative: a negative one would mirror the profile and invert its winding
    float nonNegative(float v){
        return std::max(0.0f, finiteOr(v, 0.0f));
    }

    bool coincident(const Vector2& a, const Vector2& b){
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        return (dx * dx + dy * dy) < (COINCIDENT_EPSILON * COINCIDENT_EPSILON);
    }

    // Drop consecutive duplicates, and for a closed loop the wrap pair too: a zero-length edge
    // has no direction, so no normal, and adds no V.
    void dropCoincidentPoints(std::vector<SplineProfilePoint>& points, bool closed){
        std::vector<SplineProfilePoint> kept;
        kept.reserve(points.size());
        for (const SplineProfilePoint& p : points){
            if (!std::isfinite(p.position.x) || !std::isfinite(p.position.y)){
                continue;
            }
            // the surviving point stands for both, so it inherits a dropped duplicate's crease
            if (!kept.empty() && coincident(kept.back().position, p.position)){
                kept.back().hard = static_cast<uint8_t>(kept.back().hard | p.hard);
                kept.back().grounded = static_cast<uint8_t>(kept.back().grounded | p.grounded);
                continue;
            }
            kept.push_back(p);
        }
        if (closed && kept.size() >= 2u && coincident(kept.front().position, kept.back().position)){
            kept.front().hard = static_cast<uint8_t>(kept.front().hard | kept.back().hard);
            kept.front().grounded = static_cast<uint8_t>(kept.front().grounded | kept.back().grounded);
            kept.pop_back();
        }
        points.swap(kept);
    }

    // twice the signed area of (o, a, b): positive when it turns counter-clockwise
    float turnOf(const Vector2& o, const Vector2& a, const Vector2& b){
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    }

    // Ear clipping of the closed outline: triangles as point index triples in the outline's own
    // winding (a fan is only right for a convex outline, and a Custom one need not be). Empty when
    // the outline has no area or no ear can be found (it crosses itself).
    std::vector<uint32_t> clipEars(const std::vector<SplineProfilePoint>& points){
        std::vector<uint32_t> triangles;
        const size_t count = points.size();
        if (count < 3u){
            return triangles;
        }
        float twiceArea = 0.0f;
        for (size_t i = 0; i < count; i++){
            const Vector2& a = points[i].position;
            const Vector2& b = points[(i + 1u) % count].position;
            twiceArea += a.x * b.y - b.x * a.y;
        }
        if (!(std::abs(twiceArea) > COINCIDENT_EPSILON * COINCIDENT_EPSILON)){
            return triangles;
        }
        const float orientation = twiceArea > 0.0f ? 1.0f : -1.0f;

        std::vector<uint32_t> remaining(count);
        for (size_t i = 0; i < count; i++){
            remaining[i] = static_cast<uint32_t>(i);
        }
        triangles.reserve((count - 2u) * 3u);
        const auto position = [&](uint32_t index) -> const Vector2& { return points[index].position; };

        while (remaining.size() > 3u){
            const size_t m = remaining.size();
            bool clipped = false;
            for (size_t i = 0; i < m && !clipped; i++){
                const uint32_t p = remaining[(i + m - 1u) % m];
                const uint32_t c = remaining[i];
                const uint32_t n = remaining[(i + 1u) % m];
                const float turn = turnOf(position(p), position(c), position(n)) * orientation;
                if (turn < 0.0f){
                    continue; // reflex
                }
                bool blocked = false;
                if (turn > 0.0f){
                    for (const uint32_t q : remaining){
                        if (q == p || q == c || q == n){
                            continue;
                        }
                        const Vector2& point = position(q);
                        if (turnOf(position(p), position(c), point) * orientation >= 0.0f &&
                            turnOf(position(c), position(n), point) * orientation >= 0.0f &&
                            turnOf(position(n), position(p), point) * orientation >= 0.0f){
                            blocked = true;
                            break;
                        }
                    }
                }
                if (blocked){
                    continue;
                }
                // a collinear point is removed without a zero-area triangle
                if (turn > 0.0f){
                    triangles.insert(triangles.end(), {p, c, n});
                }
                remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
                clipped = true;
            }
            if (!clipped){
                triangles.clear();
                return triangles;
            }
        }
        if (turnOf(position(remaining[0]), position(remaining[1]), position(remaining[2])) * orientation > 0.0f){
            triangles.insert(triangles.end(), {remaining[0], remaining[1], remaining[2]});
        }
        return triangles;
    }

    SplineProfile finalizeProfile(std::vector<SplineProfilePoint> points, bool closed){
        dropCoincidentPoints(points, closed);

        SplineProfile profile;
        profile.points = std::move(points);
        // a "closed" polygon of fewer than three points is a line segment: closing it would emit
        // each edge twice, back to back, and cap a zero area
        profile.closed = closed && profile.points.size() >= 3u;

        float widest = 0.0f;
        for (const SplineProfilePoint& p : profile.points){
            widest = std::max(widest, std::abs(p.position.x));
        }
        profile.nominalHalfWidth = (widest > COINCIDENT_EPSILON) ? widest : 1.0f;

        if (profile.closed){
            profile.capTriangles = clipEars(profile.points);
        }
        return profile;
    }

    SplineProfile buildFlat(const SplineProfileParams& params){
        const float halfWidth = nonNegative(params.width) * 0.5f;
        std::vector<SplineProfilePoint> points = {
            {Vector2(-halfWidth, 0.0f), 0u, 0u},
            {Vector2(halfWidth, 0.0f), 0u, 0u},
        };
        return finalizeProfile(std::move(points), false);
    }

    SplineProfile buildBevel(const SplineProfileParams& params){
        const float halfWidth = nonNegative(params.width) * 0.5f;
        const float edgeDrop = nonNegative(params.edgeDrop);
        // past half the width the two inset points would cross and mirror the middle span; at
        // exactly half they coincide and collapse to one
        const float inset = std::min(nonNegative(params.edgeInset), halfWidth);

        // Left to right, so the walkable span faces up. The outer points are open ends and split
        // their normals anyway; the inset points, where the surface breaks into the drop, crease.
        std::vector<SplineProfilePoint> points = {
            {Vector2(-halfWidth, -edgeDrop), 0u, 0u},
            {Vector2(-halfWidth + inset, 0.0f), 1u, 0u},
            {Vector2(halfWidth - inset, 0.0f), 1u, 0u},
            {Vector2(halfWidth, -edgeDrop), 0u, 0u},
        };
        return finalizeProfile(std::move(points), false);
    }

    SplineProfile buildCrown(const SplineProfileParams& params){
        const float halfWidth = nonNegative(params.width) * 0.5f;
        const float crownRise = nonNegative(params.crownRise);
        const float shoulderWidth = nonNegative(params.shoulderWidth);
        const float shoulderDrop = nonNegative(params.shoulderDrop);

        // Left to right. The camber apex stays smooth so the carriageway reads as one curved
        // surface; the two shoulder joins are creases.
        std::vector<SplineProfilePoint> points = {
            {Vector2(-halfWidth - shoulderWidth, -shoulderDrop), 0u, 0u},
            {Vector2(-halfWidth, 0.0f), 1u, 0u},
            {Vector2(0.0f, crownRise), 0u, 0u},
            {Vector2(halfWidth, 0.0f), 1u, 0u},
            {Vector2(halfWidth + shoulderWidth, -shoulderDrop), 0u, 0u},
        };
        return finalizeProfile(std::move(points), false);
    }

    SplineProfile buildRectangle(const SplineProfileParams& params){
        const float halfThickness = nonNegative(params.thickness) * 0.5f;
        const float height = nonNegative(params.height);

        // Clockwise in (lateral, vertical): up the left face, across the top, down the right face,
        // and the wrap edge closes the bottom. Every corner creases; the base stands on the ground.
        std::vector<SplineProfilePoint> points = {
            {Vector2(-halfThickness, 0.0f), 1u, 1u},
            {Vector2(-halfThickness, height), 1u, 0u},
            {Vector2(halfThickness, height), 1u, 0u},
            {Vector2(halfThickness, 0.0f), 1u, 1u},
        };
        return finalizeProfile(std::move(points), true);
    }

    SplineProfile buildTube(const SplineProfileParams& params){
        const uint32_t sides = std::clamp(params.tubeSides, SPLINE_PROFILE_MIN_TUBE_SIDES, SPLINE_PROFILE_MAX_TUBE_SIDES);

        // A unit circle about the centreline, clockwise from the bottom (the winding rule), every
        // point smooth so the tube shades round.
        std::vector<SplineProfilePoint> points;
        points.reserve(sides);
        const double step = 2.0 * std::numbers::pi / static_cast<double>(sides);
        for (uint32_t k = 0; k < sides; k++){
            const double angle = -0.5 * std::numbers::pi - step * static_cast<double>(k);
            points.push_back({Vector2(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))), 0u, 0u});
        }
        SplineProfile profile = finalizeProfile(std::move(points), true);
        // the radius channel is the tube's radius: the circle the polygon is inscribed in, which an
        // odd side count or a turned polygon does not reach on the lateral axis
        profile.nominalHalfWidth = 1.0f;
        return profile;
    }

}

SplineProfile spline::buildProfile(const SplineProfileParams& params){
    switch (params.shape){
        case SplineProfileShape::Flat:
            return buildFlat(params);
        case SplineProfileShape::Bevel:
            return buildBevel(params);
        case SplineProfileShape::Crown:
            return buildCrown(params);
        case SplineProfileShape::Rectangle:
            return buildRectangle(params);
        case SplineProfileShape::Tube:
            return buildTube(params);
        case SplineProfileShape::Custom:
            break;
    }
    // Custom carries its points beside the params, not inside them
    return SplineProfile{};
}

SplineProfile spline::buildCustomProfile(std::span<const SplineProfilePoint> points, bool closed){
    const size_t count = std::min(points.size(), static_cast<size_t>(SPLINE_PROFILE_MAX_CUSTOM_POINTS));
    return finalizeProfile(std::vector<SplineProfilePoint>(points.begin(), points.begin() + static_cast<std::ptrdiff_t>(count)), closed);
}

SplineGroundAuthority spline::resolveGroundAuthority(SplineProfileShape shape, bool fitsToBanks){
    // a filled water region reads the ground and must never write it: a Flat river would
    // otherwise grade away the basin the fill needs to find where the water reaches
    if (fitsToBanks){
        return SplineGroundAuthority::None;
    }
    switch (shape){
        case SplineProfileShape::Crown:
            return SplineGroundAuthority::Owns;
        case SplineProfileShape::Flat:
        case SplineProfileShape::Bevel:
            return SplineGroundAuthority::Grades;
        case SplineProfileShape::Rectangle:
        case SplineProfileShape::Tube:
        case SplineProfileShape::Custom:
            break;
    }
    return SplineGroundAuthority::None;
}
