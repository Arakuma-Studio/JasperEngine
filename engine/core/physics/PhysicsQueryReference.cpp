// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "PhysicsQueryReference.h"
#include "Log.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    constexpr size_t INSERTION_SORT_LIMIT = 32;

    bool overlapKeyLess(const OverlapHit& a, const OverlapHit& b){
        if (a.body != b.body) return a.body < b.body;
        return a.colliderIndex < b.colliderIndex;
    }

    bool castKeyLess(const RayReturn& a, const RayReturn& b){
        if (a.body != b.body) return a.body < b.body;
        if (a.colliderIndex != b.colliderIndex) return a.colliderIndex < b.colliderIndex;
        return a.distance < b.distance;
    }

    bool castOrderLess(const RayReturn& a, const RayReturn& b){
        if (a.distance != b.distance) return a.distance < b.distance;
        if (a.body != b.body) return a.body < b.body;
        return a.colliderIndex < b.colliderIndex;
    }

    // in place, no allocation: insertion sort for the usual handful of hits (D3)
    template<typename T, typename Less>
    void sortHits(T* hits, size_t count, Less less){
        if (count < INSERTION_SORT_LIMIT){
            for (size_t i = 1; i < count; i++){
                T value = hits[i];
                size_t j = i;
                while (j > 0 && less(value, hits[j - 1])){
                    hits[j] = hits[j - 1];
                    j--;
                }
                hits[j] = value;
            }
        }else{
            std::sort(hits, hits + count, less);
        }
    }

}

size_t physics::sortOverlapHits(OverlapHit* hits, size_t count){
    sortHits(hits, count, overlapKeyLess);
    // a compound child can be reported twice through different sub-shape paths
    size_t kept = 0;
    for (size_t i = 0; i < count; i++){
        if (kept == 0 || hits[kept - 1].body != hits[i].body || hits[kept - 1].colliderIndex != hits[i].colliderIndex){
            hits[kept++] = hits[i];
        }
    }
    return kept;
}

size_t physics::sortCastHits(RayReturn* hits, size_t count){
    // one hit per collider, its nearest; then nearest first
    sortHits(hits, count, castKeyLess);
    size_t kept = 0;
    for (size_t i = 0; i < count; i++){
        if (kept == 0 || hits[kept - 1].body != hits[i].body || hits[kept - 1].colliderIndex != hits[i].colliderIndex){
            hits[kept++] = hits[i];
        }
    }
    sortHits(hits, kept, castOrderLess);
    return kept;
}

void physics::capsule2DSegment(const Vector2& center, const Vector2& size, CapsuleDirection2D direction, float angleDegrees,
                               Vector2& point1, Vector2& point2, float& radius){
    // Unity: the round ends lie along direction; the size is the capsule's bounding box, so
    // the radius is half the other side and the segment the rest of this one
    const bool vertical = direction == CapsuleDirection2D::Vertical;
    const float along = std::fabs(vertical ? size.y : size.x);
    const float across = std::fabs(vertical ? size.x : size.y);
    radius = across * 0.5f;
    const float half = std::max(0.0f, along * 0.5f - radius);
    const float angle = angleDegrees * (float)M_PI / 180.0f;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const Vector2 localAxis = vertical ? Vector2(0.0f, half) : Vector2(half, 0.0f);
    const Vector2 axis(localAxis.x * c - localAxis.y * s, localAxis.x * s + localAxis.y * c);
    point1 = Vector2(center.x - axis.x, center.y - axis.y);
    point2 = Vector2(center.x + axis.x, center.y + axis.y);
}

void physics::referenceOverlap(Scene* scene, bool is2D, const QueryShape& shape, const PhysicsQueryOptions& options, std::vector<OverlapHit>& out){
    out.clear();
    if (shape.is2D() != is2D){
        Log::error("PhysicsQueryReference: a %s shape for the %s world", shape.is2D() ? "2D" : "3D", is2D ? "2D" : "3D");
        return;
    }
#ifdef JASPERENGINE_PHYSICS_3D
    if (!is2D){
        referenceOverlap3D(scene, shape, options, out);
    }
#endif
#ifdef JASPERENGINE_PHYSICS_2D
    if (is2D){
        referenceOverlap2D(scene, shape, options, out);
    }
#endif
    (void)scene;
    (void)options;
}

size_t physics::queryOverlap(Scene* scene, const QueryShape& shape, const PhysicsQueryOptions& options, std::vector<OverlapHit>& out){
    out.clear();
    switch (shape.type){
#ifdef JASPERENGINE_PHYSICS_3D
        case QueryShape::Type::Sphere:
            return Physics::overlapSphere(scene, shape.a, shape.radius, out, options);
        case QueryShape::Type::Box:
            return Physics::overlapBox(scene, shape.a, shape.halfExtents, shape.rotation, out, options);
        case QueryShape::Type::Capsule:
            return Physics::overlapCapsule(scene, shape.a, shape.b, shape.radius, out, options);
#endif
#ifdef JASPERENGINE_PHYSICS_2D
        case QueryShape::Type::Point2D:
            return Physics2D::overlapPointAll(scene, Vector2(shape.a.x, shape.a.y), out, options);
        case QueryShape::Type::Circle2D:
            return Physics2D::overlapCircleAll(scene, Vector2(shape.a.x, shape.a.y), shape.radius, out, options);
        case QueryShape::Type::Box2D:
            return Physics2D::overlapBoxAll(scene, Vector2(shape.a.x, shape.a.y), Vector2(shape.halfExtents.x * 2.0f, shape.halfExtents.y * 2.0f), shape.angle, out, options);
        case QueryShape::Type::Capsule2D:
            return Physics2D::overlapCapsuleAll(scene, Vector2(shape.a.x, shape.a.y), Vector2(shape.halfExtents.x * 2.0f, shape.halfExtents.y * 2.0f), shape.direction2D, shape.angle, out, options);
        case QueryShape::Type::Area2D:
            return Physics2D::overlapAreaAll(scene, Vector2(shape.a.x, shape.a.y), Vector2(shape.b.x, shape.b.y), out, options);
#endif
        default:
            (void)scene;
            (void)options;
            Log::error("PhysicsQueryReference: the scene's build has no physics backend for this shape");
            return 0;
    }
}

physics::ReferenceDiff physics::compare(std::span<const OverlapHit> query, std::span<const OverlapHit> reference){
    // both are in R6's order: a merge walk
    ReferenceDiff diff;
    diff.referenceCount = reference.size();
    size_t i = 0;
    size_t j = 0;
    while (i < query.size() || j < reference.size()){
        if (j >= reference.size() || (i < query.size() && overlapKeyLess(query[i], reference[j]))){
            diff.extra.push_back(query[i++]);
        }else if (i >= query.size() || overlapKeyLess(reference[j], query[i])){
            diff.missing.push_back(reference[j++]);
        }else{
            i++;
            j++;
        }
    }
    return diff;
}
