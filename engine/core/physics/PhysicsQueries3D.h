// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef PHYSICSQUERIES3D_H
#define PHYSICSQUERIES3D_H

// Unity's 3D shape queries on the scene's Jolt world (docs/specs/S96-physics-gaps.md REQ-10,
// D4): sphere, box and capsule casts, overlaps and checks, ClosestPoint and Rigidbody.SweepTest.
// Layer-masked like Physics.raycast, with its QueryOptions (ignore list, triggers, start-inside,
// back faces). Casts return the hit's distance in world units along the normalized direction.
// From the main thread or a job between steps, and from inside a step (a synchronous contact
// callback) through the no-lock query. The ...All and overlap forms fill a vector the caller
// owns and allocate nothing when it has the capacity.

#include "Export.h"
#include "Entity.h"
#include "math/Quaternion.h"
#include "math/Ray.h"
#include "math/Vector3.h"
#include "object/physics/Collision.h"
#include "physics/PhysicsQueryOptions.h"

#include <cstddef>
#include <vector>

namespace jasperengine{

    class Scene;

#ifdef JASPERENGINE_PHYSICS_3D
    class JASPERENGINE_API PhysicsQueries3D{
    public:
        // The nearest hit of a shape moved from its start along direction for maxDistance, or
        // Ray::NO_HIT. A collider the shape overlaps at the start is skipped (Unity) unless
        // options.hitFromInside, which reports it at distance 0, point = the start's centre,
        // normal = -direction.
        static RayReturn sphereCast(Scene* scene, Vector3 origin, float radius, Vector3 direction, float maxDistance, const QueryOptions& options = {});
        static RayReturn boxCast(Scene* scene, Vector3 center, Vector3 halfExtents, Vector3 direction, Quaternion orientation, float maxDistance, const QueryOptions& options = {});
        static RayReturn capsuleCast(Scene* scene, Vector3 point1, Vector3 point2, float radius, Vector3 direction, float maxDistance, const QueryOptions& options = {});
        // Every collider hit, nearest first, one hit per collider, at most options.maxHits (the
        // nearest); `hits` is cleared first. Returns the count.
        static size_t sphereCastAll(Scene* scene, Vector3 origin, float radius, Vector3 direction, float maxDistance, std::vector<RayReturn>& hits, const QueryOptions& options = {});
        static size_t boxCastAll(Scene* scene, Vector3 center, Vector3 halfExtents, Vector3 direction, Quaternion orientation, float maxDistance, std::vector<RayReturn>& hits, const QueryOptions& options = {});
        static size_t capsuleCastAll(Scene* scene, Vector3 point1, Vector3 point2, float radius, Vector3 direction, float maxDistance, std::vector<RayReturn>& hits, const QueryOptions& options = {});

        // The colliders touching or inside the shape, each once, ordered by GameObject then
        // collider index, at most options.maxHits; `colliders` is cleared first. Back faces of
        // meshes count. A CharacterController's inner body is its GameObject's collider 0.
        static size_t overlapSphere(Scene* scene, Vector3 center, float radius, std::vector<ColliderRef>& colliders, const QueryOptions& options = {});
        static size_t overlapBox(Scene* scene, Vector3 center, Vector3 halfExtents, Quaternion orientation, std::vector<ColliderRef>& colliders, const QueryOptions& options = {});
        static size_t overlapCapsule(Scene* scene, Vector3 point1, Vector3 point2, float radius, std::vector<ColliderRef>& colliders, const QueryOptions& options = {});
        // the colliders a point is inside (solid; a mesh only if closed)
        static size_t overlapPoint(Scene* scene, Vector3 point, std::vector<ColliderRef>& colliders, const QueryOptions& options = {});
        // overlap* is not empty, stopping at the first collider
        static bool checkSphere(Scene* scene, Vector3 center, float radius, const QueryOptions& options = {});
        static bool checkBox(Scene* scene, Vector3 center, Vector3 halfExtents, Quaternion orientation, const QueryOptions& options = {});
        static bool checkCapsule(Scene* scene, Vector3 point1, Vector3 point2, float radius, const QueryOptions& options = {});
        static bool checkPoint(Scene* scene, Vector3 point, const QueryOptions& options = {});

        // Unity Physics.ClosestPoint: the point of a convex collider (sphere, box, capsule,
        // cylinder, convex hull) nearest to `point`, `point` itself when inside. False for a
        // mesh or heightfield collider, a collider gone or a body not built yet.
        static bool closestPoint(Scene* scene, Vector3 point, const ColliderRef& collider, Vector3& closest);

        // Unity Rigidbody.SweepTest / SweepTestAll: the body's solid colliders cast from where
        // the body is, never hitting the body itself (options.ignore adds others)
        static RayReturn sweepTest(Scene* scene, Entity body, Vector3 direction, float maxDistance, const QueryOptions& options = {});
        static size_t sweepTestAll(Scene* scene, Entity body, Vector3 direction, float maxDistance, std::vector<RayReturn>& hits, const QueryOptions& options = {});

        // Why the arguments of a query cannot be used ("radius must be a positive finite number"),
        // or null: what the Lua bindings raise and the C++ queries log
        static const char* invalidDirection(const Vector3& direction, float maxDistance);
        static const char* invalidRadius(float radius);
        static const char* invalidHalfExtents(const Vector3& halfExtents);
        static const char* invalidPoint(const Vector3& point);
    };
#endif

}

#endif //PHYSICSQUERIES3D_H
