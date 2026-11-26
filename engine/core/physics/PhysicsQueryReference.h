// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Contract (spatial-queries R16; the queries are S96's PhysicsQueries3D / PhysicsQueries2D)
// - QueryShape: one overlap's or cast's shape, as the debug draw ring keeps it and the reference
//   takes it.
// - referenceOverlap answers an overlap by testing every body of the world, one by one, with the
//   filters of S96's overlaps (layer mask, triggers, ignore list, back faces), resolving each hit
//   to its collider as they do, in their order (GameObject, then collider index); compare lists
//   what differs. Used by MCP query_physics {mode: verify} and the tests, never by the runtime
//   queries (it allocates).
// - Main thread, outside the physics step.

#ifndef PHYSICS_QUERY_REFERENCE_H
#define PHYSICS_QUERY_REFERENCE_H

#include "Export.h"
#include "Entity.h"
#include "math/Quaternion.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "physics/PhysicsQueryOptions.h"

#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine{

    class Scene;

    namespace physics{

        // A query shape. 3D: Sphere (a, radius), Box (a, halfExtents, rotation), Capsule (a, b,
        // radius), Point (a). 2D in scene units: Point2D (a), Circle2D (a, radius), Box2D (a,
        // halfExtents xy = half the size, angle in degrees), Capsule2D (a, halfExtents xy,
        // direction2D, angle), Area2D (corners a and b).
        struct QueryShape{
            enum class Type : uint8_t { Sphere, Box, Capsule, Point, Point2D, Circle2D, Box2D, Capsule2D, Area2D };
            Type type = Type::Sphere;
            Vector3 a;
            Vector3 b;
            Vector3 halfExtents;
            Quaternion rotation;
            float radius = 0.0f;
            float angle = 0.0f;
            CapsuleDirection2D direction2D = CapsuleDirection2D::VERTICAL;

            bool is2D() const { return type >= Type::Point2D; }

            JASPERENGINE_API static QueryShape sphere(const Vector3& center, float radius);
            JASPERENGINE_API static QueryShape box(const Vector3& center, const Vector3& halfExtents, const Quaternion& rotation);
            JASPERENGINE_API static QueryShape capsule(const Vector3& point1, const Vector3& point2, float radius);
            JASPERENGINE_API static QueryShape point(const Vector3& point);
            // 2D, as PhysicsQueries2D takes them: sizes whole, angles in the engine's angle unit
            JASPERENGINE_API static QueryShape point2D(const Vector2& point);
            JASPERENGINE_API static QueryShape circle2D(const Vector2& center, float radius);
            JASPERENGINE_API static QueryShape box2D(const Vector2& center, const Vector2& size, float angle);
            JASPERENGINE_API static QueryShape capsule2D(const Vector2& center, const Vector2& size, CapsuleDirection2D direction, float angle);
            JASPERENGINE_API static QueryShape area2D(const Vector2& pointA, const Vector2& pointB);
        };

        // One collider an overlap found: its GameObject and its index there (a ColliderRef's or a
        // ColliderRef2D's entity and index)
        struct QueryCollider{
            Entity entity = NULL_ENTITY;
            int index = -1;

            bool operator==(const QueryCollider& other) const { return entity == other.entity && index == other.index; }
        };

        struct ReferenceDiff{
            std::vector<QueryCollider> missing;   // in the reference, not in the query
            std::vector<QueryCollider> extra;     // in the query, not in the reference
            size_t referenceCount = 0;
            bool ok() const { return missing.empty() && extra.empty(); }
        };

        // A 2D capsule given as Unity's bounding size, direction and angle (degrees): the centres
        // of its round ends and its radius (scene units)
        JASPERENGINE_API void capsule2DSegment(const Vector2& center, const Vector2& size, CapsuleDirection2D direction, float angleDegrees,
                                               Vector2& point1, Vector2& point2, float& radius);

        // Every collider of the scene's 3D or 2D world (the shape's) touching the shape, tested
        // body by body with the overlaps' filters, in their order
        JASPERENGINE_API void referenceOverlap(Scene* scene, const QueryShape& shape, const QueryOptions& options, std::vector<QueryCollider>& out);
        // The runtime overlap of the shape (PhysicsQueries3D::overlap*, PhysicsQueries2D::overlap*All)
        JASPERENGINE_API size_t queryOverlap(Scene* scene, const QueryShape& shape, const QueryOptions& options, std::vector<QueryCollider>& out);
        JASPERENGINE_API ReferenceDiff compare(std::span<const QueryCollider> query, std::span<const QueryCollider> reference);

    }

}

#endif // PHYSICS_QUERY_REFERENCE_H
