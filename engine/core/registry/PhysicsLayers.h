// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef PHYSICSLAYERS_H
#define PHYSICSLAYERS_H

#include "Export.h"
#include "TagManager.h"
#include "math/Quaternion.h"
#include "math/Ray.h"
#include "math/Vector2.h"
#include "math/Vector3.h"

#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine{

    class Scene;

    // Unity's Layer Collision Matrix, one for 3D (Physics, Jolt) and one for 2D
    // (Physics2D, Box2D): process-wide like the layer names, set by the editor from
    // project.yaml and by an exported game from its generated init(). Row i has bit j set
    // when GameObjects on layers i and j collide; it is kept symmetric. All layers
    // collide by default.
    //
    // Bodies on a disabled pair neither collide nor report contact or trigger events. The
    // matrix gates first: a pair it enables still goes through the per-collider category
    // and mask bits (and the 2D group index) and the shouldCollide callbacks.
    // Changes reach live bodies before the next physics step.
    class JASPERENGINE_API LayerCollisionMatrix{
    private:
        std::atomic<uint32_t> rows[MAX_LAYERS];
        std::atomic<uint32_t> version;
        std::atomic<bool> allEnabled;

        void refreshAllEnabled();

    public:
        LayerCollisionMatrix();

        LayerCollisionMatrix(const LayerCollisionMatrix&) = delete;
        LayerCollisionMatrix& operator=(const LayerCollisionMatrix&) = delete;

        // false and an error for a layer outside 0..31
        bool setIgnore(int layer1, int layer2, bool ignore);
        bool isIgnored(int layer1, int layer2) const;
        // layers that 'layer' collides with, Everything when out of range
        uint32_t getMask(int layer) const;
        // sets the row and mirrors it into the column
        bool setMask(int layer, uint32_t layerMask);
        void reset();
        // Every row at once (index = layer); a pair collides only when both rows say so
        void setRows(const uint32_t newRows[MAX_LAYERS]);

        // Lock-free reads for the physics backends: row 'layer' of the matrix
        inline bool collides(unsigned int layer1, unsigned int layer2) const{
            return (rows[layer1 & 31u].load(std::memory_order_relaxed) >> (layer2 & 31u)) & 1u;
        }
        inline bool isAllEnabled() const{
            return allEnabled.load(std::memory_order_relaxed);
        }
        // Bumped by every change, so a physics world can tell it has rows to re-apply
        inline uint32_t getVersion() const{
            return version.load(std::memory_order_acquire);
        }
        // Every row, index = layer
        void getRows(uint32_t outRows[MAX_LAYERS]) const;
    };

    // Unity's QueryTriggerInteraction (same values): whether a query reports trigger colliders.
    // UseGlobal follows Physics.queriesHitTriggers (Physics2D's for 2D queries).
    enum class QueryTriggerInteraction : uint8_t { UseGlobal = 0, Ignore = 1, Collide = 2 };
    // Unity's CapsuleDirection2D: the axis of a 2D capsule's round ends
    enum class CapsuleDirection2D : uint8_t { Vertical = 0, Horizontal = 1 };

    // One collider an overlap found (docs/scripting/spatial-queries.md). hit is false for a 2D
    // first-form query that found nothing.
    struct OverlapHit{
        Entity body = NULL_ENTITY;
        uint32_t colliderIndex = 0;
        bool trigger = false;
        bool hit = false;
    };

    // The trailing arguments of every spatial query, for C++ callers
    struct PhysicsQueryOptions{
        uint32_t layerMask = LAYER_MASK_DEFAULT_RAYCAST;
        QueryTriggerInteraction triggers = QueryTriggerInteraction::UseGlobal;
        // bodies the query passes through
        std::span<const Entity> ignore;
    };

    // Unity's Physics: layer collision matrix and layer-masked queries of the 3D world
    //
    // Spatial queries (overlap, check, cast) run on the physics engine's broad phase and see the
    // world of the last step. Overlap results are sorted by (body, colliderIndex), cast All results
    // by distance then (body, colliderIndex): the same world gives the same output. The forms
    // with a caller vector allocate nothing once it has room and are safe from jobs outside the
    // step; inside a step (contact callbacks) every query logs one error and finds nothing.
    class JASPERENGINE_API Physics{
    public:
        // Unity's Physics.IgnoreRaycastLayer, DefaultRaycastLayers and AllLayers: masks
        static constexpr uint32_t IGNORE_RAYCAST_LAYER = 1u << IGNORE_RAYCAST_LAYER_INDEX;
        static constexpr uint32_t DEFAULT_RAYCAST_LAYERS = LAYER_MASK_DEFAULT_RAYCAST;
        static constexpr uint32_t ALL_LAYERS = LAYER_MASK_EVERYTHING;

        static LayerCollisionMatrix& getLayerCollisionMatrix();

        // Unity Physics.IgnoreLayerCollision / GetIgnoreLayerCollision
        static void ignoreLayerCollision(int layer1, int layer2, bool ignore = true);
        static bool getIgnoreLayerCollision(int layer1, int layer2);
        // Unity Physics.GetLayerCollisionMask: the layers 'layer' collides with
        static uint32_t getLayerCollisionMask(int layer);
        static void setLayerCollisionMask(int layer, uint32_t layerMask);
        // every pair collides again
        static void resetLayerCollisionMatrix();

        // The runtime collider and joint wireframes of every scene, 3D and 2D (Game view
        // "Physics" toggle, Lua Physics.debugDraw, MCP configure_game_view physics_debug), in Play
        // and exported builds; debugDrawContacts adds the contact points and normals. Off: nothing
        // is drawn or computed.
        static void setDebugDraw(bool debugDraw);
        static bool isDebugDraw();
        static void setDebugDrawContacts(bool debugDrawContacts);
        static bool isDebugDrawContacts();
        // The spatial queries of the main thread, drawn for 1 s through the same lines (Game view
        // Physics > Queries, Lua Physics.debugDrawQueries, configure_game_view
        // physics_debug_queries). Off: nothing is recorded.
        static void setDebugDrawQueries(bool debugDrawQueries);
        static bool isDebugDrawQueries();

        // Unity's Physics.queriesHitTriggers: what QueryTriggerInteraction::UseGlobal means for
        // every 3D query, raycast included. Process-wide, default true.
        static void setQueriesHitTriggers(bool hitTriggers);
        static bool getQueriesHitTriggers();
        // whether a query with this interaction reports triggers
        static bool hitsTriggers(QueryTriggerInteraction triggers);

        // Every spatial query and the hits it returned since start, both worlds (the
        // "Physics queries" monitors take per-frame differences)
        static uint64_t getQueryCount();
        static uint64_t getQueryHitCount();
        static void countQuery(size_t hits);

#ifdef JASPERENGINE_PHYSICS_3D
        // Closest hit of the 3D bodies on a layer in layerMask. With a Ray, distance is
        // the fraction of ray.direction (as Ray::intersects); with origin, direction and
        // maxDistance it is in world units along the normalized direction (Unity).
        static RayReturn raycast(Scene* scene, const Ray& ray, uint32_t layerMask = DEFAULT_RAYCAST_LAYERS);
        static RayReturn raycast(Scene* scene, Vector3 origin, Vector3 direction, float maxDistance, uint32_t layerMask = DEFAULT_RAYCAST_LAYERS);
        // Every collider hit, nearest first, one hit per collider
        static std::vector<RayReturn> raycastAll(Scene* scene, const Ray& ray, uint32_t layerMask = DEFAULT_RAYCAST_LAYERS);
        static std::vector<RayReturn> raycastAll(Scene* scene, Vector3 origin, Vector3 direction, float maxDistance, uint32_t layerMask = DEFAULT_RAYCAST_LAYERS);
        // the same with trigger filtering (the overloads above are UseGlobal)
        static RayReturn raycast(Scene* scene, const Ray& ray, uint32_t layerMask, QueryTriggerInteraction triggers);
        static RayReturn raycast(Scene* scene, Vector3 origin, Vector3 direction, float maxDistance, uint32_t layerMask, QueryTriggerInteraction triggers);
        static std::vector<RayReturn> raycastAll(Scene* scene, const Ray& ray, uint32_t layerMask, QueryTriggerInteraction triggers);
        static std::vector<RayReturn> raycastAll(Scene* scene, Vector3 origin, Vector3 direction, float maxDistance, uint32_t layerMask, QueryTriggerInteraction triggers);

        // Every collider whose shape touches the query shape (separation <= 0), one hit per
        // collider, into out (cleared first). Returns the count. Mesh colliders are surfaces:
        // a shape wholly inside a closed mesh does not overlap it.
        static size_t overlapSphere(Scene* scene, const Vector3& center, float radius, std::vector<OverlapHit>& out, const PhysicsQueryOptions& options = {});
        static size_t overlapBox(Scene* scene, const Vector3& center, const Vector3& halfExtents, const Quaternion& rotation, std::vector<OverlapHit>& out, const PhysicsQueryOptions& options = {});
        // a capsule between two points (they may coincide: a sphere)
        static size_t overlapCapsule(Scene* scene, const Vector3& point0, const Vector3& point1, float radius, std::vector<OverlapHit>& out, const PhysicsQueryOptions& options = {});
        // true when the overlap would find a collider; stops at the first
        static bool checkSphere(Scene* scene, const Vector3& center, float radius, const PhysicsQueryOptions& options = {});
        static bool checkBox(Scene* scene, const Vector3& center, const Vector3& halfExtents, const Quaternion& rotation, const PhysicsQueryOptions& options = {});
        static bool checkCapsule(Scene* scene, const Vector3& point0, const Vector3& point1, float radius, const PhysicsQueryOptions& options = {});
        // The closest collider the shape meets moving along direction for maxDistance (world
        // units). Colliders it overlaps at the start are not reported (Unity).
        static RayReturn sphereCast(Scene* scene, const Vector3& origin, float radius, const Vector3& direction, float maxDistance, const PhysicsQueryOptions& options = {});
        static RayReturn boxCast(Scene* scene, const Vector3& center, const Vector3& halfExtents, const Vector3& direction, float maxDistance, const Quaternion& rotation, const PhysicsQueryOptions& options = {});
        static RayReturn capsuleCast(Scene* scene, const Vector3& point0, const Vector3& point1, float radius, const Vector3& direction, float maxDistance, const PhysicsQueryOptions& options = {});
        // every collider met, nearest first, one hit per collider, into out (cleared first)
        static size_t sphereCastAll(Scene* scene, const Vector3& origin, float radius, const Vector3& direction, float maxDistance, std::vector<RayReturn>& out, const PhysicsQueryOptions& options = {});
        static size_t boxCastAll(Scene* scene, const Vector3& center, const Vector3& halfExtents, const Vector3& direction, float maxDistance, const Quaternion& rotation, std::vector<RayReturn>& out, const PhysicsQueryOptions& options = {});
        static size_t capsuleCastAll(Scene* scene, const Vector3& point0, const Vector3& point1, float radius, const Vector3& direction, float maxDistance, std::vector<RayReturn>& out, const PhysicsQueryOptions& options = {});
#endif
    };

    // Unity's Physics2D: the same for the 2D world. 2D distances are in scene units (points).
    class JASPERENGINE_API Physics2D{
    public:
        static constexpr uint32_t IGNORE_RAYCAST_LAYER = Physics::IGNORE_RAYCAST_LAYER;
        static constexpr uint32_t DEFAULT_RAYCAST_LAYERS = Physics::DEFAULT_RAYCAST_LAYERS;
        static constexpr uint32_t ALL_LAYERS = Physics::ALL_LAYERS;

        static LayerCollisionMatrix& getLayerCollisionMatrix();

        static void ignoreLayerCollision(int layer1, int layer2, bool ignore = true);
        static bool getIgnoreLayerCollision(int layer1, int layer2);
        static uint32_t getLayerCollisionMask(int layer);
        static void setLayerCollisionMask(int layer, uint32_t layerMask);
        static void resetLayerCollisionMatrix();

        // Unity's Physics2D.queriesHitTriggers: what UseGlobal means for every 2D query
        static void setQueriesHitTriggers(bool hitTriggers);
        static bool getQueriesHitTriggers();
        static bool hitsTriggers(QueryTriggerInteraction triggers);

#ifdef JASPERENGINE_PHYSICS_2D
        static RayReturn raycast(Scene* scene, const Ray& ray, uint32_t layerMask = DEFAULT_RAYCAST_LAYERS);
        static RayReturn raycast(Scene* scene, Vector2 origin, Vector2 direction, float distance, uint32_t layerMask = DEFAULT_RAYCAST_LAYERS);
        static std::vector<RayReturn> raycastAll(Scene* scene, const Ray& ray, uint32_t layerMask = DEFAULT_RAYCAST_LAYERS);
        static std::vector<RayReturn> raycastAll(Scene* scene, Vector2 origin, Vector2 direction, float distance, uint32_t layerMask = DEFAULT_RAYCAST_LAYERS);
        static RayReturn raycast(Scene* scene, const Ray& ray, uint32_t layerMask, QueryTriggerInteraction triggers);
        static RayReturn raycast(Scene* scene, Vector2 origin, Vector2 direction, float distance, uint32_t layerMask, QueryTriggerInteraction triggers);
        static std::vector<RayReturn> raycastAll(Scene* scene, const Ray& ray, uint32_t layerMask, QueryTriggerInteraction triggers);
        static std::vector<RayReturn> raycastAll(Scene* scene, Vector2 origin, Vector2 direction, float distance, uint32_t layerMask, QueryTriggerInteraction triggers);

        // Overlaps in three forms (Unity): the first collider in (body, colliderIndex) order
        // (hit false when none), All into out (cleared first, returns the count). Positions and
        // sizes in scene units (points); angles in degrees.
        static OverlapHit overlapPoint(Scene* scene, const Vector2& point, const PhysicsQueryOptions& options = {});
        static size_t overlapPointAll(Scene* scene, const Vector2& point, std::vector<OverlapHit>& out, const PhysicsQueryOptions& options = {});
        static OverlapHit overlapCircle(Scene* scene, const Vector2& center, float radius, const PhysicsQueryOptions& options = {});
        static size_t overlapCircleAll(Scene* scene, const Vector2& center, float radius, std::vector<OverlapHit>& out, const PhysicsQueryOptions& options = {});
        static OverlapHit overlapBox(Scene* scene, const Vector2& center, const Vector2& size, float angleDegrees, const PhysicsQueryOptions& options = {});
        static size_t overlapBoxAll(Scene* scene, const Vector2& center, const Vector2& size, float angleDegrees, std::vector<OverlapHit>& out, const PhysicsQueryOptions& options = {});
        // size is the capsule's bounding box; the round ends lie along direction (Unity)
        static OverlapHit overlapCapsule(Scene* scene, const Vector2& center, const Vector2& size, CapsuleDirection2D direction, float angleDegrees, const PhysicsQueryOptions& options = {});
        static size_t overlapCapsuleAll(Scene* scene, const Vector2& center, const Vector2& size, CapsuleDirection2D direction, float angleDegrees, std::vector<OverlapHit>& out, const PhysicsQueryOptions& options = {});
        // the axis-aligned rectangle between two corners (any order)
        static OverlapHit overlapArea(Scene* scene, const Vector2& pointA, const Vector2& pointB, const PhysicsQueryOptions& options = {});
        static size_t overlapAreaAll(Scene* scene, const Vector2& pointA, const Vector2& pointB, std::vector<OverlapHit>& out, const PhysicsQueryOptions& options = {});
        // Casts: the closest collider met (distance in scene units, z = 0); shapes overlapped at
        // the start are not reported
        static RayReturn circleCast(Scene* scene, const Vector2& origin, float radius, const Vector2& direction, float distance, const PhysicsQueryOptions& options = {});
        static RayReturn boxCast(Scene* scene, const Vector2& origin, const Vector2& size, float angleDegrees, const Vector2& direction, float distance, const PhysicsQueryOptions& options = {});
        static RayReturn capsuleCast(Scene* scene, const Vector2& origin, const Vector2& size, CapsuleDirection2D capsuleDirection, float angleDegrees, const Vector2& direction, float distance, const PhysicsQueryOptions& options = {});
        static size_t circleCastAll(Scene* scene, const Vector2& origin, float radius, const Vector2& direction, float distance, std::vector<RayReturn>& out, const PhysicsQueryOptions& options = {});
        static size_t boxCastAll(Scene* scene, const Vector2& origin, const Vector2& size, float angleDegrees, const Vector2& direction, float distance, std::vector<RayReturn>& out, const PhysicsQueryOptions& options = {});
        static size_t capsuleCastAll(Scene* scene, const Vector2& origin, const Vector2& size, CapsuleDirection2D capsuleDirection, float angleDegrees, const Vector2& direction, float distance, std::vector<RayReturn>& out, const PhysicsQueryOptions& options = {});
#endif
    };

}

#endif //PHYSICSLAYERS_H
