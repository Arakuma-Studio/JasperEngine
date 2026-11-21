// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The 3D spatial queries of Physics (spatial-queries R1-R8): overlaps, checks and shape casts on
// Jolt's broad phase. Query shapes are stack objects marked embedded (no reference count, no
// heap) and the collectors write into the caller's vector, so a query with a buffer that has
// room allocates nothing (D2). Body and collider come from the collector's OnBody and its
// TransformedShape context, read while Jolt holds the body, with no second lock.

#include "registry/PhysicsLayers.h"

#ifdef JASPERENGINE_PHYSICS_3D

#include "physics/PhysicsQueryReference.h"
#include "physics/PhysicsQueryDebug.h"
#include "physics/ContactTracker.h"
#include "subsystem/PhysicsSystem.h"
#include "util/JoltPhysicsAux.h"
#include "Scene.h"
#include "Log.h"

#include "Jolt/Physics/Collision/CollideShape.h"
#include "Jolt/Physics/Collision/ShapeCast.h"
#include "Jolt/Physics/Collision/Shape/SphereShape.h"
#include "Jolt/Physics/Collision/Shape/BoxShape.h"
#include "Jolt/Physics/Collision/Shape/CapsuleShape.h"
#include "Jolt/Physics/Body/BodyFilter.h"
#include "Jolt/Physics/Body/BodyLock.h"
#include "Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h"
#include "Jolt/Physics/Collision/Shape/ConvexShape.h"
#include "Jolt/Physics/Collision/TransformedShape.h"
#include "Jolt/Geometry/ConvexSupport.h"
#include "Jolt/Geometry/EPAPenetrationDepth.h"
#include "Jolt/Geometry/OrientedBox.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    bool finite3(const Vector3& v){
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    JPH::Vec3 toJolt3(const Vector3& v){
        return JPH::Vec3(v.x, v.y, v.z);
    }

    Vector3 fromJolt3(JPH::Vec3Arg v){
        return Vector3(v.GetX(), v.GetY(), v.GetZ());
    }

    // R7: one error naming the bad argument; false refuses the query
    bool checkRadius3D(const char* function, float radius){
        if (!(radius > 0.0f) || !std::isfinite(radius)){
            Log::error("%s: radius must be a positive finite number (got %g)", function, radius);
            return false;
        }
        return true;
    }

    bool checkPoint3D(const char* function, const char* name, const Vector3& point){
        if (!finite3(point)){
            Log::error("%s: %s must be a finite vector", function, name);
            return false;
        }
        return true;
    }

    bool checkExtents3D(const char* function, const Vector3& halfExtents){
        if (!finite3(halfExtents) || !(halfExtents.x > 0.0f) || !(halfExtents.y > 0.0f) || !(halfExtents.z > 0.0f)){
            Log::error("%s: halfExtents must be positive and finite", function);
            return false;
        }
        return true;
    }

    bool checkRotation3D(const char* function, const Quaternion& rotation, JPH::Quat& out){
        const float length = std::sqrt(rotation.w * rotation.w + rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z);
        if (!(length > 0.0f) || !std::isfinite(length)){
            Log::error("%s: rotation must be a non-zero finite quaternion", function);
            return false;
        }
        out = JPH::Quat(rotation.x / length, rotation.y / length, rotation.z / length, rotation.w / length);
        return true;
    }

    bool checkSweep3D(const char* function, const Vector3& direction, float maxDistance, JPH::Vec3& unit){
        const float length = direction.length();
        if (!(length > 0.0f) || !std::isfinite(length)){
            Log::error("%s: direction must be a non-zero vector", function);
            return false;
        }
        if (!(maxDistance > 0.0f) || !std::isfinite(maxDistance)){
            Log::error("%s: maxDistance must be a positive finite number", function);
            return false;
        }
        unit = toJolt3(direction / length);
        return true;
    }

    // The scene's physics system when its 3D world can be queried now: none without a world
    // (no creation, D6); inside a step one error per query function and none (R18)
    PhysicsSystem* queryWorld3D(Scene* scene, std::atomic<bool>& insideStepLogged, const char* function){
        if (!scene){
            return nullptr;
        }
        PhysicsSystem* system = scene->getSystem<PhysicsSystem>().get();
        if (!system->hasWorld3D()){
            return nullptr;
        }
        if (system->isSteppingWorld3D()){
            if (!insideStepLogged.exchange(true, std::memory_order_relaxed)){
                Log::error("%s: called inside the physics step (a contact callback); queries run outside the physics step: queue the query for onUpdate", function);
            }
            return nullptr;
        }
        return system;
    }

    // R5's body filter: triggers by the interaction, then the ignore list
    class QueryBodyFilter3D : public JPH::BodyFilter{
    private:
        std::span<const Entity> ignore;
        bool hitTriggers;

    public:
        QueryBodyFilter3D(std::span<const Entity> ignore, bool hitTriggers): ignore(ignore), hitTriggers(hitTriggers){ }

        bool passes(const JPH::Body& body) const{
            if (!hitTriggers && body.IsSensor()){
                return false;
            }
            if (!ignore.empty()){
                const Entity entity = PhysicsCallbacks::bodyEntity(body.GetUserData());
                for (Entity ignored : ignore){
                    if (ignored != NULL_ENTITY && ignored == entity){
                        return false;
                    }
                }
            }
            return true;
        }

        virtual bool ShouldCollideLocked(const JPH::Body& inBody) const override{
            return passes(inBody);
        }
    };

    // Every collider into the caller's vector (and the first contact points, for debug draw)
    class OverlapCollector3D : public JPH::CollideShapeCollector{
    public:
        std::vector<OverlapHit>& out;
        Vector3* points = nullptr;
        size_t pointCount = 0;
        Entity entity = NULL_ENTITY;
        bool sensor = false;

        explicit OverlapCollector3D(std::vector<OverlapHit>& out): out(out){ }

        virtual void OnBody(const JPH::Body& inBody) override{
            entity = PhysicsCallbacks::bodyEntity(inBody.GetUserData());
            sensor = inBody.IsSensor();
        }

        virtual void AddHit(const JPH::CollideShapeResult& inResult) override{
            OverlapHit hit;
            hit.body = entity;
            hit.colliderIndex = (uint32_t)GetContext()->GetSubShapeUserData(inResult.mSubShapeID2);
            hit.trigger = sensor;
            hit.hit = true;
            out.push_back(hit);
            if (points && pointCount < PhysicsQueryDebug::MAX_HIT_POINTS){
                points[pointCount++] = fromJolt3(inResult.mContactPointOn2);
            }
        }
    };

    class AnyHitCollector3D : public JPH::CollideShapeCollector{
    public:
        bool found = false;
        Vector3 point;

        virtual void AddHit(const JPH::CollideShapeResult& inResult) override{
            found = true;
            point = fromJolt3(inResult.mContactPointOn2);
            ForceEarlyOut();
        }
    };

    // The closest hit (closest set) or every hit into out; hits the shape starts in are dropped (R3)
    class CastCollector3D : public JPH::CastShapeCollector{
    public:
        std::vector<RayReturn>* out = nullptr;
        RayReturn closest = Ray::NO_HIT;
        float maxDistance = 0.0f;
        Entity entity = NULL_ENTITY;

        virtual void OnBody(const JPH::Body& inBody) override{
            entity = PhysicsCallbacks::bodyEntity(inBody.GetUserData());
        }

        virtual void AddHit(const JPH::ShapeCastResult& inResult) override{
            if (!(inResult.mFraction > 0.0f)){
                return;
            }
            if (!out && closest.hit && inResult.mFraction >= closest.distance / maxDistance){
                return;
            }
            RayReturn hit;
            hit.hit = true;
            hit.distance = inResult.mFraction * maxDistance;
            hit.point = fromJolt3(inResult.mContactPointOn2);
            const JPH::Vec3 axis = inResult.mPenetrationAxis;
            hit.normal = axis.LengthSq() > 0.0f ? fromJolt3(-axis.Normalized()) : Vector3::ZERO;
            hit.body = entity;
            hit.colliderIndex = (size_t)GetContext()->GetSubShapeUserData(inResult.mSubShapeID2);
            if (out){
                out->push_back(hit);
            }else{
                closest = hit;
                UpdateEarlyOutFraction(inResult.mFraction);
            }
        }
    };

    // Runs visit(shape, centerOfMassTransform) with the query shape built on the stack
    template<typename Visit>
    void withJoltShape(const physics::QueryShape& shape, const JPH::Quat& rotation, Visit&& visit){
        switch (shape.type){
            case physics::QueryShape::Type::Box: {
                // Jolt's default convex radius (its corners rounded by at most 5 cm, the faces where
                // they are): GJK answers the shallow pairs without the EPA a sharp box needs
                const float smallest = std::min(shape.halfExtents.x, std::min(shape.halfExtents.y, shape.halfExtents.z));
                JPH::BoxShape box(toJolt3(shape.halfExtents), std::min(JPH::cDefaultConvexRadius, 0.5f * smallest));
                box.SetEmbedded();
                visit(static_cast<const JPH::Shape&>(box), JPH::RMat44::sRotationTranslation(rotation, toJolt3(shape.a)));
                return;
            }
            case physics::QueryShape::Type::Capsule: {
                const JPH::Vec3 a = toJolt3(shape.a);
                const JPH::Vec3 b = toJolt3(shape.b);
                const JPH::Vec3 axis = b - a;
                const float length = axis.Length();
                const JPH::Vec3 centre = (a + b) * 0.5f;
                if (!(length > 1.0e-6f)){
                    JPH::SphereShape sphere(shape.radius);
                    sphere.SetEmbedded();
                    visit(static_cast<const JPH::Shape&>(sphere), JPH::RMat44::sTranslation(centre));
                    return;
                }
                JPH::CapsuleShape capsule(length * 0.5f, shape.radius);
                capsule.SetEmbedded();
                // Jolt's capsule lies along Y
                const JPH::Quat turn = JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), axis / length);
                visit(static_cast<const JPH::Shape&>(capsule), JPH::RMat44::sRotationTranslation(turn, centre));
                return;
            }
            default: {
                JPH::SphereShape sphere(shape.radius);
                sphere.SetEmbedded();
                visit(static_cast<const JPH::Shape&>(sphere), JPH::RMat44::sTranslation(toJolt3(shape.a)));
                return;
            }
        }
    }

    bool validShape3D(const char* function, const physics::QueryShape& shape, JPH::Quat& rotation){
        rotation = JPH::Quat::sIdentity();
        switch (shape.type){
            case physics::QueryShape::Type::Sphere:
                return checkPoint3D(function, "center", shape.a) && checkRadius3D(function, shape.radius);
            case physics::QueryShape::Type::Box:
                return checkPoint3D(function, "center", shape.a) && checkExtents3D(function, shape.halfExtents)
                    && checkRotation3D(function, shape.rotation, rotation);
            case physics::QueryShape::Type::Capsule:
                return checkPoint3D(function, "point0", shape.a) && checkPoint3D(function, "point1", shape.b)
                    && checkRadius3D(function, shape.radius);
            default:
                Log::error("%s: not a 3D shape", function);
                return false;
        }
    }

    JPH::CollideShapeSettings overlapSettings(){
        JPH::CollideShapeSettings settings;
        // touching counts: separation <= 0
        settings.mMaxSeparationDistance = 0.0f;
        settings.mBackFaceMode = JPH::EBackFaceMode::IgnoreBackFaces;
        return settings;
    }

    void recordOverlap3D(Scene* scene, const physics::QueryShape& shape, const Vector3* points, size_t pointCount, bool hit){
        physics::recordQuery(scene, shape, Vector3::ZERO, 0.0f, false, points, pointCount, hit);
    }

    // The overlap decision of Jolt's convex-vs-convex collide (its bounds test and its GJK step on
    // the shapes without their convex radius) without the penetration depth it then works out: where
    // the cores intersect (Jolt's EPA case) the shapes overlap, an overlap needs no depth
    bool convexOverlap3D(const JPH::ConvexShape& query, const JPH::ConvexShape::Support& querySupport, JPH::Mat44Arg queryInverse,
                         const JPH::ConvexShape& leaf, JPH::Vec3Arg leafScale, JPH::Mat44Arg leafTransform, float tolerance){
        const JPH::Mat44 leafToQuery = queryInverse * leafTransform;
        if (!JPH::OrientedBox(leafToQuery, leaf.GetLocalBounds().Scaled(leafScale)).Overlaps(query.GetLocalBounds())){
            return false;
        }
        JPH::Vec3 axis = leafToQuery.GetTranslation();
        if (axis.IsNearZero()){
            axis = JPH::Vec3::sAxisX();
        }
        JPH::ConvexShape::SupportBuffer leafBuffer;
        const JPH::ConvexShape::Support* leafSupport = leaf.GetSupportFunction(JPH::ConvexShape::ESupportMode::ExcludeConvexRadius, leafBuffer, leafScale);
        const JPH::TransformedConvexObject<JPH::ConvexShape::Support> leafInQuery(leafToQuery, *leafSupport);
        JPH::EPAPenetrationDepth depth;
        JPH::Vec3 pointA, pointB;
        return depth.GetPenetrationDepthStepGJK(querySupport, querySupport.GetConvexRadius(), leafInQuery, leafSupport->GetConvexRadius(),
                                                tolerance, axis, pointA, pointB) != JPH::EPAPenetrationDepth::EStatus::NotColliding;
    }

    // The same for a mesh or height field leaf, triangle by triangle as Jolt's convex-vs-triangles
    // collide decides (back faces skipped, the triangle's bounds, the GJK step): true at the first
    // triangle that overlaps
    bool triangleOverlap3D(const JPH::ConvexShape& query, const JPH::ConvexShape::Support& querySupport, JPH::Mat44Arg queryInverse,
                           const JPH::AABox& queryWorldBounds, const JPH::TransformedShape& leaf, float tolerance){
        const JPH::AABox queryBounds = query.GetLocalBounds();
        JPH::Shape::GetTrianglesContext context;
        leaf.mShape->GetTrianglesStart(context, queryWorldBounds, leaf.mShapePositionCOM, leaf.mShapeRotation, leaf.GetShapeScale());
        constexpr int BATCH = JPH::Shape::cGetTrianglesMinTrianglesRequested;
        JPH::Float3 vertices[3 * BATCH];
        for (;;){
            const int count = leaf.mShape->GetTrianglesNext(context, BATCH, vertices);
            if (count <= 0){
                return false;
            }
            for (int t = 0; t < count; t++){
                const JPH::Vec3 v0 = queryInverse * JPH::Vec3(vertices[3 * t]);
                const JPH::Vec3 v1 = queryInverse * JPH::Vec3(vertices[3 * t + 1]);
                const JPH::Vec3 v2 = queryInverse * JPH::Vec3(vertices[3 * t + 2]);
                const JPH::Vec3 normal = (v1 - v0).Cross(v2 - v0);
                if (normal.Dot(v0) > 0.0f){
                    continue;
                }
                JPH::AABox triangleBounds = JPH::AABox::sFromTwoPoints(v0, v1);
                triangleBounds.Encapsulate(v2);
                if (!triangleBounds.Overlaps(queryBounds)){
                    continue;
                }
                const JPH::TriangleConvexSupport triangle(v0, v1, v2);
                JPH::Vec3 axis = -normal;
                JPH::Vec3 pointA, pointB;
                JPH::EPAPenetrationDepth depth;
                if (depth.GetPenetrationDepthStepGJK(querySupport, querySupport.GetConvexRadius(), triangle, 0.0f, tolerance, axis, pointA, pointB)
                        != JPH::EPAPenetrationDepth::EStatus::NotColliding){
                    return true;
                }
            }
        }
    }

    // Every collider the query shape touches, by the broad phase, then a yes / no per leaf shape of each
    // body (convex: convexOverlap3D; a mesh or a height field: Jolt's collide); the same answer as
    // NarrowPhaseQuery::CollideShape with no separation, without its contact points and depths
    class BooleanOverlap3D{
    public:
        std::vector<OverlapHit>* out = nullptr;   // null: stop at the first hit (a check)
        bool found = false;
        Vector3* points = nullptr;
        size_t pointCount = 0;

        BooleanOverlap3D(PhysicsSystem* system, const JPH::ObjectLayerFilter& layerFilter, const QueryBodyFilter3D& bodyFilter,
                         const JPH::CollideShapeSettings& settings):
            system(system), layerFilter(layerFilter), bodyFilter(bodyFilter), settings(settings){ }

        // the query shapes are convex (sphere, box, capsule)
        void run(const JPH::ConvexShape& shape, JPH::Mat44Arg transform){
            query = &shape;
            queryTransform = transform;
            queryInverse = transform.InversedRotationTranslation();
            querySupport = shape.GetSupportFunction(JPH::ConvexShape::ESupportMode::ExcludeConvexRadius, querySupportBuffer, JPH::Vec3::sReplicate(1.0f));
            queryBounds = shape.GetWorldSpaceBounds(transform, JPH::Vec3::sReplicate(1.0f));
            Bodies bodies(*this);
            const JPH::BroadPhaseQuery& broadPhase = system->getWorld3D()->GetBroadPhaseQuery();
            if (shape.GetSubType() == JPH::EShapeSubType::Box){
                // a turned box covers far less than its bounds
                broadPhase.CollideOrientedBox(JPH::OrientedBox(transform, shape.GetLocalBounds().GetExtent()), bodies, JPH::BroadPhaseLayerFilter(), layerFilter);
            }else{
                broadPhase.CollideAABox(queryBounds, bodies, JPH::BroadPhaseLayerFilter(), layerFilter);
            }
        }

    private:
        PhysicsSystem* system;
        const JPH::ObjectLayerFilter& layerFilter;
        const QueryBodyFilter3D& bodyFilter;
        const JPH::CollideShapeSettings& settings;
        const JPH::ConvexShape* query = nullptr;
        JPH::Mat44 queryTransform = JPH::Mat44::sIdentity();
        JPH::Mat44 queryInverse = JPH::Mat44::sIdentity();
        JPH::ConvexShape::SupportBuffer querySupportBuffer;
        const JPH::ConvexShape::Support* querySupport = nullptr;
        JPH::AABox queryBounds;
        // the body being tested
        const JPH::Shape* bodyShape = nullptr;
        Entity entity = NULL_ENTITY;
        bool sensor = false;
        size_t bodyFirstHit = 0;

        bool hasCollider(uint32_t collider) const{
            for (size_t i = bodyFirstHit; out && i < out->size(); i++){
                if ((*out)[i].colliderIndex == collider) return true;
            }
            return false;
        }

        // returns false to stop
        bool testLeaf(const JPH::TransformedShape& leaf){
            // a check is answered (a compound does not stop between its leaves on its own)
            if (!out && found){
                return false;
            }
            const uint32_t collider = (uint32_t)bodyShape->GetSubShapeUserData(leaf.mSubShapeIDCreator.GetID());
            if (hasCollider(collider)){
                return true;
            }
            bool hit;
            const JPH::EShapeType type = leaf.mShape->GetType();
            if (type == JPH::EShapeType::Convex){
                hit = convexOverlap3D(*query, *querySupport, queryInverse, static_cast<const JPH::ConvexShape&>(*leaf.mShape),
                                      leaf.GetShapeScale(), leaf.GetCenterOfMassTransform(), settings.mCollisionTolerance);
            }else if (type == JPH::EShapeType::Mesh || type == JPH::EShapeType::HeightField){
                hit = triangleOverlap3D(*query, *querySupport, queryInverse, queryBounds, leaf, settings.mCollisionTolerance);
            }else{
                AnyHitCollector3D any;
                leaf.CollideShape(query, JPH::Vec3::sReplicate(1.0f), queryTransform, settings, JPH::RVec3::sZero(), any);
                hit = any.found;
            }
            if (!hit){
                return true;
            }
            found = true;
            if (points && pointCount < PhysicsQueryDebug::MAX_HIT_POINTS){
                points[pointCount++] = fromJolt3(leaf.GetCenterOfMassTransform().GetTranslation());
            }
            if (!out){
                return false;
            }
            OverlapHit result;
            result.body = entity;
            result.colliderIndex = collider;
            result.trigger = sensor;
            result.hit = true;
            out->push_back(result);
            return true;
        }

        class Leaves : public JPH::TransformedShapeCollector{
        public:
            explicit Leaves(BooleanOverlap3D& owner): owner(owner){ }
            virtual void AddHit(const JPH::TransformedShape& inResult) override{
                if (!owner.testLeaf(inResult)){
                    ForceEarlyOut();
                }
            }
        private:
            BooleanOverlap3D& owner;
        };

        class Bodies : public JPH::CollideShapeBodyCollector{
        public:
            explicit Bodies(BooleanOverlap3D& owner): owner(owner){ }
            virtual void AddHit(const JPH::BodyID& inResult) override{
                if (!owner.bodyFilter.ShouldCollide(inResult)){
                    return;
                }
                JPH::BodyLockRead lock(owner.system->getBodyLockInterface3D(), inResult);
                if (!lock.SucceededAndIsInBroadPhase()){
                    return;
                }
                const JPH::Body& body = lock.GetBody();
                if (!owner.bodyFilter.ShouldCollideLocked(body)){
                    return;
                }
                const JPH::TransformedShape transformed = body.GetTransformedShape();
                owner.entity = PhysicsCallbacks::bodyEntity(body.GetUserData());
                owner.sensor = body.IsSensor();
                lock.ReleaseLock();
                owner.bodyShape = transformed.mShape;
                owner.bodyFirstHit = owner.out ? owner.out->size() : 0;
                Leaves leaves(owner);
                transformed.CollectTransformedShapes(owner.queryBounds, leaves);
                if (leaves.ShouldEarlyOut()){
                    ForceEarlyOut();
                }
            }
        private:
            BooleanOverlap3D& owner;
        };
    };

    size_t overlap3D(const char* function, std::atomic<bool>& logged, Scene* scene, const physics::QueryShape& shape,
                     std::vector<OverlapHit>& out, const PhysicsQueryOptions& options){
        out.clear();
        JPH::Quat rotation;
        if (!validShape3D(function, shape, rotation)){
            return 0;
        }
        PhysicsSystem* system = queryWorld3D(scene, logged, function);
        if (system && options.layerMask != LAYER_MASK_NOTHING){
            const JoltQueryObjectLayerFilter layerFilter(system->getObjectLayerTable3D(), options.layerMask, (uint16_t)~0u, (uint16_t)~0u);
            const QueryBodyFilter3D bodyFilter(options.ignore, Physics::hitsTriggers(options.triggers));
            const JPH::CollideShapeSettings settings = overlapSettings();
            Vector3 points[PhysicsQueryDebug::MAX_HIT_POINTS];
            const bool recording = physics::recordingQueries();
            BooleanOverlap3D overlap(system, layerFilter, bodyFilter, settings);
            overlap.out = &out;
            overlap.points = recording ? points : nullptr;
            withJoltShape(shape, rotation, [&](const JPH::Shape& joltShape, JPH::RMat44Arg transform){
                overlap.run(static_cast<const JPH::ConvexShape&>(joltShape), transform);
            });
            out.resize(physics::sortOverlapHits(out.data(), out.size()));
            if (recording){
                recordOverlap3D(scene, shape, points, overlap.pointCount, !out.empty());
            }
        }else if (physics::recordingQueries()){
            recordOverlap3D(scene, shape, nullptr, 0, false);
        }
        Physics::countQuery(out.size());
        return out.size();
    }

    bool check3D(const char* function, std::atomic<bool>& logged, Scene* scene, const physics::QueryShape& shape, const PhysicsQueryOptions& options){
        JPH::Quat rotation;
        if (!validShape3D(function, shape, rotation)){
            return false;
        }
        bool found = false;
        Vector3 points[PhysicsQueryDebug::MAX_HIT_POINTS];
        size_t pointCount = 0;
        PhysicsSystem* system = queryWorld3D(scene, logged, function);
        if (system && options.layerMask != LAYER_MASK_NOTHING){
            const JoltQueryObjectLayerFilter layerFilter(system->getObjectLayerTable3D(), options.layerMask, (uint16_t)~0u, (uint16_t)~0u);
            const QueryBodyFilter3D bodyFilter(options.ignore, Physics::hitsTriggers(options.triggers));
            const JPH::CollideShapeSettings settings = overlapSettings();
            BooleanOverlap3D overlap(system, layerFilter, bodyFilter, settings);
            overlap.points = points;
            withJoltShape(shape, rotation, [&](const JPH::Shape& joltShape, JPH::RMat44Arg transform){
                overlap.run(static_cast<const JPH::ConvexShape&>(joltShape), transform);
            });
            found = overlap.found;
            pointCount = overlap.pointCount;
        }
        Physics::countQuery(found ? 1 : 0);
        if (physics::recordingQueries()){
            recordOverlap3D(scene, shape, points, pointCount, found);
        }
        return found;
    }

    // closest (out null) or every hit (out), distances in world units
    RayReturn cast3D(const char* function, std::atomic<bool>& logged, Scene* scene, const physics::QueryShape& shape,
                     const Vector3& direction, float maxDistance, std::vector<RayReturn>* out, const PhysicsQueryOptions& options){
        if (out){
            out->clear();
        }
        JPH::Quat rotation;
        JPH::Vec3 unit;
        if (!validShape3D(function, shape, rotation) || !checkSweep3D(function, direction, maxDistance, unit)){
            return Ray::NO_HIT;
        }
        CastCollector3D collector;
        collector.out = out;
        collector.maxDistance = maxDistance;
        PhysicsSystem* system = queryWorld3D(scene, logged, function);
        if (system && options.layerMask != LAYER_MASK_NOTHING){
            const JoltQueryObjectLayerFilter layerFilter(system->getObjectLayerTable3D(), options.layerMask, (uint16_t)~0u, (uint16_t)~0u);
            const QueryBodyFilter3D bodyFilter(options.ignore, Physics::hitsTriggers(options.triggers));
            JPH::ShapeCastSettings settings;
            settings.mBackFaceModeTriangles = JPH::EBackFaceMode::IgnoreBackFaces;
            settings.mBackFaceModeConvex = JPH::EBackFaceMode::IgnoreBackFaces;
            settings.mReturnDeepestPoint = false;
            const JPH::NarrowPhaseQuery& query = system->getNarrowPhaseQuery3D();
            withJoltShape(shape, rotation, [&](const JPH::Shape& joltShape, JPH::RMat44Arg transform){
                const JPH::RShapeCast cast(&joltShape, JPH::Vec3::sReplicate(1.0f), transform, unit * maxDistance);
                query.CastShape(cast, settings, JPH::RVec3::sZero(), collector, JPH::BroadPhaseLayerFilter(), layerFilter, bodyFilter);
            });
        }
        RayReturn result = collector.closest;
        size_t hits = result.hit ? 1 : 0;
        if (out){
            out->resize(physics::sortCastHits(out->data(), out->size()));
            result = out->empty() ? Ray::NO_HIT : out->front();
            hits = out->size();
        }
        Physics::countQuery(hits);
        if (physics::recordingQueries()){
            Vector3 points[PhysicsQueryDebug::MAX_HIT_POINTS];
            size_t pointCount = 0;
            if (out){
                for (size_t i = 0; i < out->size() && pointCount < PhysicsQueryDebug::MAX_HIT_POINTS; i++){
                    points[pointCount++] = (*out)[i].point;
                }
            }else if (result.hit){
                points[pointCount++] = result.point;
            }
            const float reach = out ? (out->empty() ? maxDistance : out->back().distance) : (result.hit ? result.distance : maxDistance);
            physics::recordQuery(scene, shape, direction.normalized(), reach, true, points, pointCount, hits > 0);
        }
        return result;
    }

    physics::QueryShape querySphereShape3D(const Vector3& center, float radius){
        physics::QueryShape shape;
        shape.type = physics::QueryShape::Type::Sphere;
        shape.a = center;
        shape.radius = radius;
        return shape;
    }

    physics::QueryShape queryBoxShape3D(const Vector3& center, const Vector3& halfExtents, const Quaternion& rotation){
        physics::QueryShape shape;
        shape.type = physics::QueryShape::Type::Box;
        shape.a = center;
        shape.halfExtents = halfExtents;
        shape.rotation = rotation;
        return shape;
    }

    physics::QueryShape queryCapsuleShape3D(const Vector3& point0, const Vector3& point1, float radius){
        physics::QueryShape shape;
        shape.type = physics::QueryShape::Type::Capsule;
        shape.a = point0;
        shape.b = point1;
        shape.radius = radius;
        return shape;
    }

    // one "inside the step" error per query function (R18)
    std::atomic<bool> stepLoggedOverlapSphere{false}, stepLoggedOverlapBox{false}, stepLoggedOverlapCapsule{false};
    std::atomic<bool> stepLoggedCheckSphere{false}, stepLoggedCheckBox{false}, stepLoggedCheckCapsule{false};
    std::atomic<bool> stepLoggedSphereCast{false}, stepLoggedBoxCast{false}, stepLoggedCapsuleCast{false};

}

size_t Physics::overlapSphere(Scene* scene, const Vector3& center, float radius, std::vector<OverlapHit>& out, const PhysicsQueryOptions& options){
    return overlap3D("Physics.overlapSphere", stepLoggedOverlapSphere, scene, querySphereShape3D(center, radius), out, options);
}

size_t Physics::overlapBox(Scene* scene, const Vector3& center, const Vector3& halfExtents, const Quaternion& rotation, std::vector<OverlapHit>& out, const PhysicsQueryOptions& options){
    return overlap3D("Physics.overlapBox", stepLoggedOverlapBox, scene, queryBoxShape3D(center, halfExtents, rotation), out, options);
}

size_t Physics::overlapCapsule(Scene* scene, const Vector3& point0, const Vector3& point1, float radius, std::vector<OverlapHit>& out, const PhysicsQueryOptions& options){
    return overlap3D("Physics.overlapCapsule", stepLoggedOverlapCapsule, scene, queryCapsuleShape3D(point0, point1, radius), out, options);
}

bool Physics::checkSphere(Scene* scene, const Vector3& center, float radius, const PhysicsQueryOptions& options){
    return check3D("Physics.checkSphere", stepLoggedCheckSphere, scene, querySphereShape3D(center, radius), options);
}

bool Physics::checkBox(Scene* scene, const Vector3& center, const Vector3& halfExtents, const Quaternion& rotation, const PhysicsQueryOptions& options){
    return check3D("Physics.checkBox", stepLoggedCheckBox, scene, queryBoxShape3D(center, halfExtents, rotation), options);
}

bool Physics::checkCapsule(Scene* scene, const Vector3& point0, const Vector3& point1, float radius, const PhysicsQueryOptions& options){
    return check3D("Physics.checkCapsule", stepLoggedCheckCapsule, scene, queryCapsuleShape3D(point0, point1, radius), options);
}

RayReturn Physics::sphereCast(Scene* scene, const Vector3& origin, float radius, const Vector3& direction, float maxDistance, const PhysicsQueryOptions& options){
    return cast3D("Physics.sphereCast", stepLoggedSphereCast, scene, querySphereShape3D(origin, radius), direction, maxDistance, nullptr, options);
}

RayReturn Physics::boxCast(Scene* scene, const Vector3& center, const Vector3& halfExtents, const Vector3& direction, float maxDistance, const Quaternion& rotation, const PhysicsQueryOptions& options){
    return cast3D("Physics.boxCast", stepLoggedBoxCast, scene, queryBoxShape3D(center, halfExtents, rotation), direction, maxDistance, nullptr, options);
}

RayReturn Physics::capsuleCast(Scene* scene, const Vector3& point0, const Vector3& point1, float radius, const Vector3& direction, float maxDistance, const PhysicsQueryOptions& options){
    return cast3D("Physics.capsuleCast", stepLoggedCapsuleCast, scene, queryCapsuleShape3D(point0, point1, radius), direction, maxDistance, nullptr, options);
}

size_t Physics::sphereCastAll(Scene* scene, const Vector3& origin, float radius, const Vector3& direction, float maxDistance, std::vector<RayReturn>& out, const PhysicsQueryOptions& options){
    cast3D("Physics.sphereCastAll", stepLoggedSphereCast, scene, querySphereShape3D(origin, radius), direction, maxDistance, &out, options);
    return out.size();
}

size_t Physics::boxCastAll(Scene* scene, const Vector3& center, const Vector3& halfExtents, const Vector3& direction, float maxDistance, const Quaternion& rotation, std::vector<RayReturn>& out, const PhysicsQueryOptions& options){
    cast3D("Physics.boxCastAll", stepLoggedBoxCast, scene, queryBoxShape3D(center, halfExtents, rotation), direction, maxDistance, &out, options);
    return out.size();
}

size_t Physics::capsuleCastAll(Scene* scene, const Vector3& point0, const Vector3& point1, float radius, const Vector3& direction, float maxDistance, std::vector<RayReturn>& out, const PhysicsQueryOptions& options){
    cast3D("Physics.capsuleCastAll", stepLoggedCapsuleCast, scene, queryCapsuleShape3D(point0, point1, radius), direction, maxDistance, &out, options);
    return out.size();
}

// The reference sweep (R16): every body of the world, each with its own CollideShape
void physics::referenceOverlap3D(Scene* scene, const QueryShape& shape, const PhysicsQueryOptions& options, std::vector<OverlapHit>& out){
    out.clear();
    JPH::Quat rotation;
    if (!scene || !validShape3D("referenceOverlap", shape, rotation)){
        return;
    }
    std::shared_ptr<PhysicsSystem> system = scene->getSystem<PhysicsSystem>();
    if (!system->hasWorld3D() || system->isSteppingWorld3D() || options.layerMask == LAYER_MASK_NOTHING){
        return;
    }
    const JoltQueryObjectLayerFilter layerFilter(system->getObjectLayerTable3D(), options.layerMask, (uint16_t)~0u, (uint16_t)~0u);
    const QueryBodyFilter3D bodyFilter(options.ignore, Physics::hitsTriggers(options.triggers));
    const JPH::CollideShapeSettings settings = overlapSettings();
    JPH::BodyIDVector bodies;
    system->getWorld3D()->GetBodies(bodies);
    OverlapCollector3D collector(out);
    for (const JPH::BodyID& id : bodies){
        JPH::BodyLockRead lock(system->getBodyLockInterface3D(), id);
        if (!lock.Succeeded()){
            continue;
        }
        const JPH::Body& body = lock.GetBody();
        if (!body.IsInBroadPhase() || !layerFilter.ShouldCollide(body.GetObjectLayer()) || !bodyFilter.passes(body)){
            continue;
        }
        collector.OnBody(body);
        const JPH::TransformedShape transformed = body.GetTransformedShape();
        withJoltShape(shape, rotation, [&](const JPH::Shape& joltShape, JPH::RMat44Arg transform){
            transformed.CollideShape(&joltShape, JPH::Vec3::sReplicate(1.0f), transform, settings, JPH::RVec3::sZero(), collector);
        });
    }
    out.resize(sortOverlapHits(out.data(), out.size()));
}

#endif
