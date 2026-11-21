// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Contract (spatial-queries R6, R16)
// - The order every overlap and cast returns (sortOverlapHits, sortCastHits): the same world and
//   query give the same output whatever the broad phase's insertion history.
// - referenceOverlap answers an overlap by testing every body of the world, one by one, with the
//   same filters as the runtime query and in the same order; compare lists what differs. Used by
//   MCP query_physics {verify} and the tests, never by the runtime queries (it allocates).
// - Main thread, outside the physics step.

#ifndef PHYSICS_QUERY_REFERENCE_H
#define PHYSICS_QUERY_REFERENCE_H

#include "Export.h"
#include "registry/PhysicsLayers.h"
#include "math/Quaternion.h"
#include "math/Vector2.h"
#include "math/Vector3.h"

#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine{

    class Scene;

    namespace physics{

        // A query shape as the overlaps take it. 3D: Sphere (a, radius), Box (a, halfExtents,
        // rotation), Capsule (a, b, radius). 2D in scene units: Point2D (a), Circle2D (a,
        // radius), Box2D (a, halfExtents xy, angle in degrees), Capsule2D (a, halfExtents xy =
        // half the size, direction2D, angle), Area2D (corners a and b).
        struct QueryShape{
            enum class Type : uint8_t { Sphere, Box, Capsule, Point2D, Circle2D, Box2D, Capsule2D, Area2D };
            Type type = Type::Sphere;
            Vector3 a;
            Vector3 b;
            Vector3 halfExtents;
            Quaternion rotation;
            float radius = 0.0f;
            float angle = 0.0f;
            CapsuleDirection2D direction2D = CapsuleDirection2D::Vertical;

            bool is2D() const { return type >= Type::Point2D; }
        };

        struct ReferenceDiff{
            std::vector<OverlapHit> missing;   // in the reference, not in the query
            std::vector<OverlapHit> extra;     // in the query, not in the reference
            size_t referenceCount = 0;
            bool ok() const { return missing.empty() && extra.empty(); }
        };

        // R6's orders, in place and without allocating: overlaps by (body, colliderIndex) with
        // duplicates dropped; casts by distance then (body, colliderIndex), one hit per collider
        // (its nearest). Return the new count.
        JASPERENGINE_API size_t sortOverlapHits(OverlapHit* hits, size_t count);
        JASPERENGINE_API size_t sortCastHits(RayReturn* hits, size_t count);

        // A 2D capsule given as Unity's bounding size, direction and angle: the centres of its
        // round ends and its radius (scene units)
        JASPERENGINE_API void capsule2DSegment(const Vector2& center, const Vector2& size, CapsuleDirection2D direction, float angleDegrees,
                                               Vector2& point1, Vector2& point2, float& radius);

        // Every collider of the scene's 3D (is2D false) or 2D world touching the shape, tested body
        // by body with the query's filters, in R6's order
        JASPERENGINE_API void referenceOverlap(Scene* scene, bool is2D, const QueryShape& shape, const PhysicsQueryOptions& options, std::vector<OverlapHit>& out);
        // The runtime query of the shape (Physics / Physics2D ...All forms), for callers holding a QueryShape
        JASPERENGINE_API size_t queryOverlap(Scene* scene, const QueryShape& shape, const PhysicsQueryOptions& options, std::vector<OverlapHit>& out);
        JASPERENGINE_API ReferenceDiff compare(std::span<const OverlapHit> query, std::span<const OverlapHit> reference);

        // the halves of referenceOverlap, beside each world's queries (PhysicsQueries*.cpp)
#ifdef JASPERENGINE_PHYSICS_3D
        void referenceOverlap3D(Scene* scene, const QueryShape& shape, const PhysicsQueryOptions& options, std::vector<OverlapHit>& out);
#endif
#ifdef JASPERENGINE_PHYSICS_2D
        void referenceOverlap2D(Scene* scene, const QueryShape& shape, const PhysicsQueryOptions& options, std::vector<OverlapHit>& out);
#endif

    }

}

#endif // PHYSICS_QUERY_REFERENCE_H
