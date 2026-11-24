// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Contract (spatial-queries R15, R27, D8)
// - The spatial queries a scene's main thread ran lately, for Physics.debugDrawQueries: a ring of
//   the newest CAPACITY entries (shape, cast sweep, hit points), each alive for its lifetime in
//   unscaled seconds. PhysicsSystem owns one per scene, made by the first recorded query, and
//   draws the live entries through its physics debug lines.
// - physics::noteOverlap / noteCast are what S96's queries (PhysicsQueries3D, PhysicsQueries2D)
//   and the raycasts call: they count the query for the "Physics queries" monitors and, with
//   debug draw on, record it; off, that costs one relaxed atomic add and load. Queries off the
//   main thread are counted, never recorded.
// - Main thread only.

#ifndef PHYSICS_QUERY_DEBUG_H
#define PHYSICS_QUERY_DEBUG_H

#include "Export.h"
#include "physics/PhysicsQueryReference.h"
#include "registry/PhysicsLayers.h"
#include "math/Ray.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>

namespace jasperengine{

    class Scene;

    class JASPERENGINE_API PhysicsQueryDebug{
    public:
        static constexpr size_t CAPACITY = 256;
        static constexpr size_t MAX_HIT_POINTS = 4;
        static constexpr float LIFETIME = 1.0f;
        // a ring unused this long with debug draw off is freed
        static constexpr double RELEASE_AFTER = 10.0;

        struct Entry{
            physics::QueryShape shape;
            // casts: the sweep (unit direction) and how far the shape went (to its hit, or the whole distance)
            Vector3 direction;
            float distance = 0.0f;
            bool cast = false;
            bool hit = false;
            uint8_t hitPoints = 0;
            Vector3 points[MAX_HIT_POINTS];
            double expires = 0.0;
        };

        void record(const Entry& entry, double now, float lifetime);
        // the live entries, oldest first
        void forEachLive(double now, const std::function<void(const Entry&)>& visit) const;
        size_t liveCount(double now) const;
        bool allocated() const { return entries != nullptr; }
        // frees the ring once nothing is live and debug draw has been off RELEASE_AFTER seconds
        void releaseIfIdle(double now, bool debugDrawOn);

        // the lines of one entry (green with hits, red without; hits as crosses)
        static void emitLines(const Entry& entry, const std::function<void(const Vector3&, const Vector3&, const Vector4&)>& emit);

    private:
        std::unique_ptr<Entry[]> entries;
        size_t next = 0;
        size_t count = 0;
        double offSince = -1.0;
    };

    namespace physics{

        // Records a query for the scene's debug draw when Physics.debugDrawQueries is on and the
        // caller is the main thread. hitPoints: where to mark hits (cast hit points, overlapped
        // GameObjects).
        JASPERENGINE_API void recordQuery(Scene* scene, const QueryShape& shape, const Vector3& direction, float distance, bool cast,
                                          const Vector3* hitPoints, size_t hitCount, bool hit);
        // true when a query should be recorded now (debug draw on, main thread)
        JASPERENGINE_API bool recordingQueries();
        // recordQuery for an overlap or a check, its hits marked at these GameObjects' positions
        JASPERENGINE_API void recordOverlap(Scene* scene, const QueryShape& shape, bool hit, const Entity* entities, size_t entityCount);

        // An overlap or check ran: `hits` colliders found (a check: 0 or 1), `colliders` the first
        // of them (ColliderRef / ColliderRef2D), or null
        template<typename Ref>
        void noteOverlap(Scene* scene, const QueryShape& shape, size_t hits, const Ref* colliders, size_t colliderCount){
            Physics::countQuery(hits);
            if (!recordingQueries()){
                return;
            }
            Entity entities[PhysicsQueryDebug::MAX_HIT_POINTS];
            const size_t count = colliders ? std::min(colliderCount, PhysicsQueryDebug::MAX_HIT_POINTS) : 0;
            for (size_t i = 0; i < count; i++){
                entities[i] = colliders[i].entity;
            }
            recordOverlap(scene, shape, hits > 0, entities, count);
        }
        // A cast ran along `direction` for `distance`: its hits, nearest first
        JASPERENGINE_API void noteCast(Scene* scene, const QueryShape& shape, const Vector3& direction, float distance,
                                       const RayReturn* hits, size_t hitCount);

    }

}

#endif // PHYSICS_QUERY_DEBUG_H
