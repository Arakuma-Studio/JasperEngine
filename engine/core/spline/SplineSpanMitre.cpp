// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineLayout/Source/SpanMitre.cpp and SeamShear.cpp
// (SignedGroundYaw), Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License.
// See NOTICE.md.

#include "SplineSpanMitre.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

using namespace jasperengine;
using namespace jasperengine::spline;

namespace{

    constexpr float MITRE_PI = 3.14159265358979f;
    constexpr float RADIANS_PER_DECI_DEGREE = MITRE_PI / 1800.0f;
    constexpr float METRES_PER_TENTH_MILLIMETRE = 1.0e-4f;
    constexpr float LENGTH_SCALE_STEP = 1.0e-3f;
    // below this share of the plane's normal along travel the plane runs nearly parallel to the
    // span, and no overhang reaches it
    constexpr float MIN_ALONG_SHARE = 1.0e-3f;

    int32_t roundToStep(float value, float step){
        return static_cast<int32_t>(std::lround(value / step));
    }

    // The plane in one span's frame, and how far that span's end must reach.
    //
    // A point at (x, y, a) in the frame (x along right, y along up from the base line, a along
    // forward from the base line's end) is past the plane when c.x*x + c.y*y + c.z*a > d, with c
    // the plane's normal in the frame and d its offset there. Along travel the plane stands at
    //     a(x, y) = (d - c.x*x - c.y*y) / c.z.
    // The end is laid at the furthest a(x, y) over the four corners of the cross-section (x = +-
    // halfThickness, y = bottom or top), and of a registered crest's on top of it, so the cut,
    // not a short end, makes the face. On a racked span the frame is pitched and the plane plumb,
    // so the top corners sit top*tan(pitch) further along or back than the bottom ones.
    bool endPlaneInFrame(const SplineSpanJoinSide& span, const Vector3& outward, const Vector3& planePoint,
                         bool atStart, SplineFenceEndPlane& outPlane, float& outOverhangMetres){
        const SplineLayoutPose& pose = span.pose;
        Vector3 c(outward.dotProduct(pose.right), outward.dotProduct(pose.up), outward.dotProduct(pose.forward));
        float d = outward.dotProduct(planePoint - span.baseLineEnd);
        const float length = std::sqrt(c.dotProduct(c));
        if (!(length > 0.0f) || !std::isfinite(length)){
            return false;
        }
        c = c * (1.0f / length);
        d /= length;
        const float along = atStart ? -c.z : c.z;
        if (!(along > MIN_ALONG_SHARE)){
            return false;
        }
        outPlane.turnRadians = 2.0f * std::asin(std::clamp(c.x, -1.0f, 1.0f));
        outPlane.leanRadians = atStart ? std::atan2(c.y, -c.z) : std::atan2(-c.y, c.z);

        // signed reach past the base line's end, positive away from the span
        const float sign = atStart ? -1.0f : 1.0f;
        const auto reach = [&](float x, float y){ return sign * (d - c.x * x - c.y * y) / c.z; };
        float furthest = 0.0f;
        for (const float x : {-span.halfThickness, span.halfThickness}){
            for (const float y : {span.bottom, span.top}){
                furthest = std::max(furthest, reach(x, y));
            }
        }
        if (span.crestHalfThickness > 0.0f){
            for (const float x : {-span.crestHalfThickness, span.crestHalfThickness}){
                for (const float y : {span.crestBottom, span.crestTop}){
                    furthest = std::max(furthest, reach(x, y));
                }
            }
        }
        outOverhangMetres = furthest;
        // from the extended end back to where the plane crosses the base line
        outPlane.stationInset = furthest - reach(0.0f, 0.0f);
        return true;
    }

    // The plane's coefficients in the frame, rebuilt from a quantised end: the turn gives its
    // share across travel, and the two points where it crosses the piece's centre line (at the
    // base line and at the top, `alongRise` metres further along travel over `height`) give its
    // tilt about right. A variant is then a function of its key alone.
    Vector3 endPlaneCoefficients(const SplineMitreEndKey& end, bool atStart, float height, float alongRise){
        const float halfTurn = 0.5f * static_cast<float>(end.turnDeciDegrees) * RADIANS_PER_DECI_DEGREE;
        const float square = std::cos(halfTurn);
        const float side = std::sin(halfTurn);
        const float sign = atStart ? -1.0f : 1.0f;
        const float run = std::sqrt(height * height + alongRise * alongRise);
        if (!(height > MIN_ALONG_SHARE) || !(run > 0.0f)){
            return Vector3(side, 0.0f, sign * square);
        }
        // normal to the rise (0, height, alongRise) within the up-forward plane
        const float forward = sign * square * height / run;
        return Vector3(side, -forward * alongRise / height, forward);
    }

    float lengthScaleOf(const SplineMitreShape& shape){
        return static_cast<float>(shape.lengthScaleThousandths) * LENGTH_SCALE_STEP;
    }

    // A piece's own axes as the frame lays them: which local component runs along travel, which
    // across it, and the sign that turns the across component into metres to the RIGHT of travel
    // (an X-run piece meets right through its local +Z, a Z-run piece through its local -X).
    struct PieceAxes{
        int along = 2;
        int across = 0;
        float rightSign = -1.0f;
    };

    PieceAxes axesOf(const SplinePiece& piece){
        if (piece.axis() == SplinePieceAxis::X){
            return {0, 2, 1.0f};
        }
        return {2, 0, -1.0f};
    }

    SplineMitreEndKey quantizeEnd(const SplineFenceEndPlane& plane){
        SplineMitreEndKey key;
        if (!plane.mitred){
            return key;
        }
        key.mitred = true;
        constexpr float degreesPerRadian = 180.0f / MITRE_PI;
        key.turnDeciDegrees = roundToStep(plane.turnRadians * degreesPerRadian, 0.1f);
        key.insetTenthMillimetres = roundToStep(plane.stationInset, METRES_PER_TENTH_MILLIMETRE);
        key.topInsetTenthMillimetres = roundToStep(plane.topInset, METRES_PER_TENTH_MILLIMETRE);
        if (key.topInsetTenthMillimetres != key.insetTenthMillimetres){
            key.stationLocalYTenthMillimetres = roundToStep(plane.stationLocalY, METRES_PER_TENTH_MILLIMETRE);
        }
        return key;
    }

    // a strict weak order over variant keys, for the first-use index
    struct VariantKeyLess{
        static auto tie(const SplineMitreEndKey& e){
            return std::make_tuple(e.mitred, e.turnDeciDegrees, e.insetTenthMillimetres, e.topInsetTenthMillimetres,
                                   e.stationLocalYTenthMillimetres);
        }
        bool operator()(const SplineMitreVariantKey& a, const SplineMitreVariantKey& b) const{
            const auto ta = std::make_tuple(static_cast<int>(a.role), a.poolSlot, a.shape.lengthScaleThousandths);
            const auto tb = std::make_tuple(static_cast<int>(b.role), b.poolSlot, b.shape.lengthScaleThousandths);
            if (ta != tb){
                return ta < tb;
            }
            if (tie(a.shape.start) != tie(b.shape.start)){
                return tie(a.shape.start) < tie(b.shape.start);
            }
            return tie(a.shape.end) < tie(b.shape.end);
        }
    };

    // Assigns one piece its variant, adding the variant the first time its shape is met. The
    // plan's order is first use; the map only finds a shape already met.
    class VariantAssigner{
    public:
        explicit VariantAssigner(SplineMitreVariantPlan& plan): target(plan) {}

        uint32_t assign(SplineMitrePieceRole role, uint32_t poolSlot, float lengthScale,
                        const SplineFenceEndPlane& start, const SplineFenceEndPlane& end){
            if (!start.mitred && !end.mitred){
                return SPLINE_NO_MITRE_VARIANT;
            }
            const SplineMitreVariantKey key{role, poolSlot, quantizeMitreShape(lengthScale, start, end)};
            const auto found = index.find(key);
            if (found != index.end()){
                return found->second;
            }
            const uint32_t added = static_cast<uint32_t>(target.variants.size());
            target.variants.push_back(key);
            index.emplace(key, added);
            return added;
        }

    private:
        SplineMitreVariantPlan& target;
        std::map<SplineMitreVariantKey, uint32_t, VariantKeyLess> index;
    };

}

float spline::signedGroundYaw(const Vector3& a, const Vector3& b){
    const float aLenSq = a.x * a.x + a.z * a.z;
    const float bLenSq = b.x * b.x + b.z * b.z;
    if (!std::isfinite(aLenSq) || !std::isfinite(bLenSq) || aLenSq < 1.0e-8f || bLenSq < 1.0e-8f){
        return 0.0f;
    }
    // -Y component of a x b: positive when b lies to the right of a (right = forward x up)
    const float cross = a.x * b.z - a.z * b.x;
    const float dot = a.x * b.x + a.z * b.z;
    return std::atan2(cross, dot);
}

bool spline::measureSpanJoin(const SplineSpanJoinSide& before, const SplineSpanJoinSide& after, const Vector3& station,
                             SplineSpanJoinMeasure& out){
    const Vector3 none(0.0f, 0.0f, 0.0f);
    const Vector3 groundBefore = normalizedOrFallback(Vector3(before.pose.forward.x, 0.0f, before.pose.forward.z), none);
    const Vector3 groundAfter = normalizedOrFallback(Vector3(after.pose.forward.x, 0.0f, after.pose.forward.z), none);
    // facing past the closing span
    const Vector3 normal = normalizedOrFallback(groundBefore + groundAfter, none);
    if (normal.dotProduct(normal) < 0.5f){
        return false;
    }

    SplineSpanJoinMeasure measure;
    measure.turnRadians = signedGroundYaw(before.pose.forward, after.pose.forward);
    measure.notchMetres = (before.halfThickness + after.halfThickness) * std::tan(0.5f * std::abs(measure.turnRadians));
    if (!endPlaneInFrame(before, normal, station, false, measure.beforeEnd, measure.beforeOverhangMetres) ||
        !endPlaneInFrame(after, normal * -1.0f, station, true, measure.afterStart, measure.afterOverhangMetres)){
        return false;
    }
    measure.beforeEnd.mitred = true;
    measure.afterStart.mitred = true;
    out = measure;
    return true;
}

SplineMitreShape spline::quantizeMitreShape(float lengthScale, const SplineFenceEndPlane& start, const SplineFenceEndPlane& end){
    SplineMitreShape shape;
    shape.lengthScaleThousandths = roundToStep(lengthScale, LENGTH_SCALE_STEP);
    shape.start = quantizeEnd(start);
    shape.end = quantizeEnd(end);
    return shape;
}

SplineMitrePlane spline::mitreEndCutPlane(const SplinePiece& piece, const SplineMitreShape& shape, bool atStart){
    const SplineMitreEndKey& end = atStart ? shape.start : shape.end;
    const PieceAxes axes = axesOf(piece);
    const float scale = lengthScaleOf(shape);
    const float inset = static_cast<float>(end.insetTenthMillimetres) * METRES_PER_TENTH_MILLIMETRE;
    const float topInset = static_cast<float>(end.topInsetTenthMillimetres) * METRES_PER_TENTH_MILLIMETRE;
    const float stationY = static_cast<float>(end.stationLocalYTenthMillimetres) * METRES_PER_TENTH_MILLIMETRE;
    const float top = piece.center.y + piece.halfExtents.y;
    // from the base line to the top the crossing moves along travel by the difference of the
    // insets, outward at a closing end for a smaller inset
    const float alongRise = (atStart ? -1.0f : 1.0f) * scale * (inset - topInset);
    const Vector3 c = endPlaneCoefficients(end, atStart, top - stationY, alongRise);
    // the station's position along travel from the piece's centre, in metres
    const float stationAlong = (atStart ? -1.0f : 1.0f) * scale * (0.5f * piece.length() - inset);
    const float centreAlong = piece.center[static_cast<unsigned>(axes.along)];
    const float centreAcross = piece.center[static_cast<unsigned>(axes.across)];

    // c.x * right + c.y * (y - stationY) + c.z * (along - stationAlong) = 0 in the frame's metres,
    // with right = rightSign * (across - centreAcross) and along = scale * (localAlong -
    // centreAlong), rewritten on local coordinates
    SplineMitrePlane plane;
    plane.normal[static_cast<unsigned>(axes.along)] = c.z * scale;
    plane.normal[static_cast<unsigned>(axes.across)] = c.x * axes.rightSign;
    plane.normal[1] = c.y;
    plane.offset = c.x * axes.rightSign * centreAcross + c.y * stationY + c.z * (scale * centreAlong + stationAlong);
    return plane;
}

SplineMitreVariantPlan spline::planMitreVariants(const SplineFenceLayoutResult& layout){
    SplineMitreVariantPlan plan;
    VariantAssigner assigner(plan);
    plan.spanVariants.reserve(layout.spans.size());
    for (const SplineFenceSpan& span : layout.spans){
        plan.spanVariants.push_back(assigner.assign(span.isGate ? SplineMitrePieceRole::Gate : SplineMitrePieceRole::Span,
                                                    span.poolSlot, span.lengthScale, span.start, span.end));
    }
    plan.crestVariants.reserve(layout.crests.size());
    for (const SplineFenceCrest& crest : layout.crests){
        plan.crestVariants.push_back(assigner.assign(SplineMitrePieceRole::Crest, crest.poolSlot, crest.lengthScale,
                                                     crest.start, crest.end));
    }
    return plan;
}
