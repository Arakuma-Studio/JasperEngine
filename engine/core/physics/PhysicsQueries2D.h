// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef PHYSICSQUERIES2D_H
#define PHYSICSQUERIES2D_H

// Unity's Physics2D shape queries on the scene's Box2D world (docs/specs/S96-physics-gaps.md
// REQ-11, D4): circle, box and capsule casts, the Overlap* family and Rigidbody2D.Cast.
// Positions, sizes and distances are in points (scene units, Physics2D.raycast's), angles in
// the engine's angle unit. Layer-masked like Physics2D.raycast, with its QueryOptions (ignore
// list, triggers, startInColliders). Main thread or a job between steps (Box2D refuses a
// query inside its step).

#include "Export.h"
#include "Entity.h"
#include "math/Ray.h"
#include "math/Vector2.h"
#include "object/physics/Collision2D.h"
#include "physics/PhysicsQueryOptions.h"

#include <cstddef>
#include <vector>

namespace jasperengine{

    class Scene;

#ifdef JASPERENGINE_PHYSICS_2D
    class JASPERENGINE_API PhysicsQueries2D{
    public:
        // The nearest hit of a shape moved from `origin` along direction for `distance`, or
        // Ray::NO_HIT. A collider the shape starts in is a hit at distance 0 (point = origin,
        // normal = -direction) only with startInColliders.
        static RayReturn circleCast(Scene* scene, Vector2 origin, float radius, Vector2 direction, float distance, const QueryOptions& options = {});
        // `size` is the whole box, turned by `angle`
        static RayReturn boxCast(Scene* scene, Vector2 origin, Vector2 size, float angle, Vector2 direction, float distance, const QueryOptions& options = {});
        // Unity's CapsuleCollider2D shape: `size` the whole capsule, its round ends on the
        // capsuleDirection axis (a circle when that side is the shorter)
        static RayReturn capsuleCast(Scene* scene, Vector2 origin, Vector2 size, CapsuleDirection2D capsuleDirection, float angle, Vector2 direction, float distance, const QueryOptions& options = {});
        // Every collider hit, nearest first, one per collider, at most options.maxHits; `hits`
        // is cleared first
        static size_t circleCastAll(Scene* scene, Vector2 origin, float radius, Vector2 direction, float distance, std::vector<RayReturn>& hits, const QueryOptions& options = {});
        static size_t boxCastAll(Scene* scene, Vector2 origin, Vector2 size, float angle, Vector2 direction, float distance, std::vector<RayReturn>& hits, const QueryOptions& options = {});
        static size_t capsuleCastAll(Scene* scene, Vector2 origin, Vector2 size, CapsuleDirection2D capsuleDirection, float angle, Vector2 direction, float distance, std::vector<RayReturn>& hits, const QueryOptions& options = {});

        // Every collider touching or inside the shape, each once, by GameObject then collider
        // index, at most options.maxHits; `colliders` is cleared first. overlapArea's box spans
        // two opposite corners.
        static size_t overlapCircleAll(Scene* scene, Vector2 point, float radius, std::vector<ColliderRef2D>& colliders, const QueryOptions& options = {});
        static size_t overlapBoxAll(Scene* scene, Vector2 point, Vector2 size, float angle, std::vector<ColliderRef2D>& colliders, const QueryOptions& options = {});
        static size_t overlapCapsuleAll(Scene* scene, Vector2 point, Vector2 size, CapsuleDirection2D capsuleDirection, float angle, std::vector<ColliderRef2D>& colliders, const QueryOptions& options = {});
        static size_t overlapPointAll(Scene* scene, Vector2 point, std::vector<ColliderRef2D>& colliders, const QueryOptions& options = {});
        static size_t overlapAreaAll(Scene* scene, Vector2 pointA, Vector2 pointB, std::vector<ColliderRef2D>& colliders, const QueryOptions& options = {});
        // Unity's Overlap*: the first of those (the lowest GameObject, then collider index);
        // false and an invalid ref when none
        static bool overlapCircle(Scene* scene, Vector2 point, float radius, ColliderRef2D& collider, const QueryOptions& options = {});
        static bool overlapBox(Scene* scene, Vector2 point, Vector2 size, float angle, ColliderRef2D& collider, const QueryOptions& options = {});
        static bool overlapCapsule(Scene* scene, Vector2 point, Vector2 size, CapsuleDirection2D capsuleDirection, float angle, ColliderRef2D& collider, const QueryOptions& options = {});
        static bool overlapPoint(Scene* scene, Vector2 point, ColliderRef2D& collider, const QueryOptions& options = {});
        static bool overlapArea(Scene* scene, Vector2 pointA, Vector2 pointB, ColliderRef2D& collider, const QueryOptions& options = {});

        // Unity Rigidbody2D.Cast: every shape of the body moved along direction, never hitting
        // the body itself; cast() the nearest hit, castAll() every collider's
        static RayReturn cast(Scene* scene, Entity body, Vector2 direction, float distance, const QueryOptions& options = {});
        static size_t castAll(Scene* scene, Entity body, Vector2 direction, float distance, std::vector<RayReturn>& hits, const QueryOptions& options = {});

        // Why the arguments cannot be used, or null (the Lua bindings raise it, C++ logs it)
        static const char* invalidDirection(const Vector2& direction, float distance);
        static const char* invalidRadius(float radius);
        static const char* invalidSize(const Vector2& size);
        static const char* invalidPoint(const Vector2& point);
    };
#endif

}

#endif //PHYSICSQUERIES2D_H
