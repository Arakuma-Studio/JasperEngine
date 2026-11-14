// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "PhysicsQueries3D.h"

#ifdef JASPERENGINE_PHYSICS_3D

#include "Log.h"
#include "Scene.h"
#include "component/RigidbodyComponent.h"
#include "physics/AttachedColliders.h"
#include "physics/ContactTracker.h"
#include "registry/PhysicsLayers.h"
#include "subsystem/PhysicsSystem.h"
#include "util/JoltPhysicsAux.h"

#include "Jolt/Geometry/ConvexSupport.h"
#include "Jolt/Geometry/GJKClosestPoint.h"
#include "Jolt/Physics/Body/BodyLock.h"
#include "Jolt/Physics/Collision/CollidePointResult.h"
#include "Jolt/Physics/Collision/CollideShape.h"
#include "Jolt/Physics/Collision/NarrowPhaseQuery.h"
#include "Jolt/Physics/Collision/ShapeCast.h"
#include "Jolt/Physics/Collision/Shape/BoxShape.h"
#include "Jolt/Physics/Collision/Shape/CapsuleShape.h"
#include "Jolt/Physics/Collision/Shape/ConvexShape.h"
#include "Jolt/Physics/Collision/Shape/SphereShape.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <memory>

using namespace jasperengine;

namespace jasperengine::physicsqueries3d {

    bool finiteVector(const Vector3& v){
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    JPH::Vec3 toJolt(const Vector3& v){
        return JPH::Vec3(v.x, v.y, v.z);
    }

    Vector3 fromJolt(JPH::Vec3Arg v){
        return Vector3(v.GetX(), v.GetY(), v.GetZ());
    }

    JPH::Quat toJolt(const Quaternion& q){
        return JPH::Quat(q.x, q.y, q.z, q.w).Normalized();
    }

    // The world of a scene, when one was made (bodies exist only once Play built them)
    std::shared_ptr<PhysicsSystem> worldOf(Scene* scene){
        if (!scene){
            return nullptr;
        }
        std::shared_ptr<PhysicsSystem> physics = scene->getSystem<PhysicsSystem>();
        return (physics && physics->hasWorld3D()) ? physics : nullptr;
    }

    // ignore list, the body a sweep starts from, and triggers (their colliders live on sensor
    // bodies: the body itself or its trigger companion)
    class QueryBodyFilter : public JPH::BodyFilter{
    public:
        const std::vector<Entity>* ignore = nullptr;
        Entity self = NULL_ENTITY;
        bool skipSensors = false;

        bool ShouldCollideLocked(const JPH::Body& body) const override{
            if (skipSensors && body.IsSensor()){
                return false;
            }
            const Entity entity = PhysicsCallbacks::bodyEntity(body.GetUserData());
            if (self != NULL_ENTITY && entity == self){
                return false;
            }
            if (ignore){
                for (Entity ignored : *ignore){
                    if (ignored != NULL_ENTITY && ignored == entity){
                        return false;
                    }
                }
            }
            return true;
        }
    };

    struct Query{
        std::shared_ptr<PhysicsSystem> physics;
        JoltQueryObjectLayerFilter layers;
        QueryBodyFilter bodies;
        JPH::BroadPhaseLayerFilter broadPhase;

        Query(std::shared_ptr<PhysicsSystem> system, const QueryOptions& options, Entity self):
            physics(std::move(system)),
            layers(physics->getObjectLayerTable3D(), options.layerMask, (uint16_t)~0u, (uint16_t)~0u){
            bodies.ignore = options.ignore;
            bodies.self = self;
            bodies.skipSensors = !Physics::hitsTriggers(options);
        }
    };

    bool sameCollider(const RayReturn& a, const RayReturn& b){
        return a.body == b.body && a.colliderIndex == b.colliderIndex;
    }

    bool nearer(const RayReturn& a, const RayReturn& b){
        if (a.distance != b.distance) return a.distance < b.distance;
        if (a.body != b.body) return a.body < b.body;
        return a.colliderIndex < b.colliderIndex;
    }

    // Keeps one hit per collider (its nearest) and, past `maxHits`, the nearest ones
    void keepHit(std::vector<RayReturn>& hits, const RayReturn& hit, size_t maxHits){
        for (RayReturn& kept : hits){
            if (sameCollider(kept, hit)){
                if (nearer(hit, kept)){
                    kept = hit;
                }
                return;
            }
        }
        if (hits.size() < maxHits){
            hits.push_back(hit);
            return;
        }
        auto farthest = std::max_element(hits.begin(), hits.end(), nearer);
        if (farthest != hits.end() && nearer(hit, *farthest)){
            *farthest = hit;
        }
    }

    // A shape cast's hits as RayReturns: the nearest only (early outs prune the rest), or every
    // collider's nearest into `all`
    class CastCollector : public JPH::CastShapeCollector{
    public:
        Scene* scene = nullptr;
        Vector3 start;
        Vector3 direction;
        float maxDistance = 0.0f;
        JPH::RVec3 baseOffset;
        bool hitFromInside = false;
        std::vector<RayReturn>* all = nullptr;
        size_t maxHits = 0;

        RayReturn closest = Ray::NO_HIT;

        void OnBody(const JPH::Body& body) override{
            entity = PhysicsCallbacks::bodyEntity(body.GetUserData());
        }

        void AddHit(const JPH::ShapeCastResult& result) override{
            const bool atStart = result.mFraction <= 0.0f;
            // Unity: a collider the shape starts in is not a hit of a cast
            if (atStart && !hitFromInside){
                return;
            }
            RayReturn hit = Ray::NO_HIT;
            hit.hit = true;
            hit.body = entity;
            hit.scene = scene;
            const JPH::TransformedShape* context = GetContext();
            hit.colliderIndex = context ? (size_t)context->mShape->GetSubShapeUserData(result.mSubShapeID2) : 0;
            if (atStart){
                hit.distance = 0.0f;
                hit.point = start;
                hit.normal = -direction;
            }else{
                hit.distance = result.mFraction * maxDistance;
                hit.point = fromJolt(JPH::Vec3(baseOffset + result.mContactPointOn2));
                const JPH::Vec3 axis = result.mPenetrationAxis;
                hit.normal = axis.LengthSq() > 1.0e-12f ? fromJolt(-axis.Normalized()) : -direction;
            }
            if (all){
                keepHit(*all, hit, maxHits);
                return;
            }
            if (!closest.hit || nearer(hit, closest)){
                closest = hit;
            }
            UpdateEarlyOutFraction(result.GetEarlyOutFraction());
        }

    private:
        Entity entity = NULL_ENTITY;
    };

    // The colliders an overlap touches, each once; `out` null: stop at the first (a check)
    template<typename Base>
    class OverlapCollector : public Base{
    public:
        Scene* scene = nullptr;
        std::vector<ColliderRef>* out = nullptr;
        size_t maxHits = 0;
        bool any = false;

        void OnBody(const JPH::Body& body) override{
            entity = PhysicsCallbacks::bodyEntity(body.GetUserData());
        }

        void AddHit(const typename Base::ResultType& result) override{
            any = true;
            if (!out){
                this->ForceEarlyOut();
                return;
            }
            const JPH::TransformedShape* context = this->GetContext();
            const int index = context ? (int)context->mShape->GetSubShapeUserData(result.mSubShapeID2) : 0;
            // an attached child's collider resolves to the child's (S96 REQ-18)
            const ColliderRef collider(scene, entity, index);
            for (const ColliderRef& kept : *out){
                if (kept == collider){
                    return;
                }
            }
            if (out->size() >= maxHits){
                this->ForceEarlyOut();
                return;
            }
            out->push_back(collider);
        }

    private:
        Entity entity = NULL_ENTITY;
    };

    void sortColliders(std::vector<ColliderRef>& colliders){
        std::sort(colliders.begin(), colliders.end(), [](const ColliderRef& a, const ColliderRef& b){
            return a.entity != b.entity ? a.entity < b.entity : a.index < b.index;
        });
    }

    // The cast of `shape` (centre of mass at `start`); `all` null: the nearest hit
    RayReturn cast(Scene* scene, const char* function, const JPH::Shape& shape, const JPH::RMat44& start, Vector3 direction, float maxDistance,
                   const QueryOptions& options, Entity self, std::vector<RayReturn>* all){
        if (all){
            all->clear();
        }
        if (const char* why = PhysicsQueries3D::invalidDirection(direction, maxDistance)){
            Log::error("%s: %s", function, why);
            return Ray::NO_HIT;
        }
        std::shared_ptr<PhysicsSystem> physics = worldOf(scene);
        if (!physics || options.layerMask == LAYER_MASK_NOTHING || (all && options.maxHits == 0)){
            return Ray::NO_HIT;
        }
        direction = direction.normalized();
        Query query(physics, options, self);

        JPH::ShapeCastSettings settings;
        settings.mBackFaceModeTriangles = Physics::hitsBackfaces(options) ? JPH::EBackFaceMode::CollideWithBackFaces : JPH::EBackFaceMode::IgnoreBackFaces;
        const JPH::RVec3 baseOffset = start.GetTranslation();
        const JPH::RShapeCast shapeCast(&shape, JPH::Vec3::sReplicate(1.0f), start, toJolt(direction * maxDistance));

        CastCollector collector;
        collector.scene = scene;
        collector.start = fromJolt(JPH::Vec3(start.GetTranslation()));
        collector.direction = direction;
        collector.maxDistance = maxDistance;
        collector.baseOffset = baseOffset;
        collector.hitFromInside = options.hitFromInside;
        collector.all = all;
        collector.maxHits = options.maxHits;
        physics->getNarrowPhaseQuery3D().CastShape(shapeCast, settings, baseOffset, collector, query.broadPhase, query.layers, query.bodies);
        if (all){
            std::sort(all->begin(), all->end(), nearer);
            return all->empty() ? Ray::NO_HIT : all->front();
        }
        return collector.closest;
    }

    // The overlap of `shape` (centre of mass at `transform`): the count, or 1 for a check
    size_t overlap(Scene* scene, const JPH::Shape& shape, const JPH::RMat44& transform, const QueryOptions& options, std::vector<ColliderRef>* out){
        if (out){
            out->clear();
        }
        std::shared_ptr<PhysicsSystem> physics = worldOf(scene);
        if (!physics || options.layerMask == LAYER_MASK_NOTHING || (out && options.maxHits == 0)){
            return 0;
        }
        Query query(physics, options, NULL_ENTITY);
        JPH::CollideShapeSettings settings;
        // what is inside a mesh's closed surface touches its back faces (OpenEngine's overlap rule)
        settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
        OverlapCollector<JPH::CollideShapeCollector> collector;
        collector.scene = scene;
        collector.out = out;
        collector.maxHits = options.maxHits;
        physics->getNarrowPhaseQuery3D().CollideShape(&shape, JPH::Vec3::sReplicate(1.0f), transform, settings, transform.GetTranslation(), collector,
                                                      query.broadPhase, query.layers, query.bodies);
        if (!out){
            return collector.any ? 1 : 0;
        }
        sortColliders(*out);
        return out->size();
    }

    size_t overlapAtPoint(Scene* scene, Vector3 point, const QueryOptions& options, std::vector<ColliderRef>* out){
        if (out){
            out->clear();
        }
        std::shared_ptr<PhysicsSystem> physics = worldOf(scene);
        if (!physics || options.layerMask == LAYER_MASK_NOTHING || (out && options.maxHits == 0)){
            return 0;
        }
        Query query(physics, options, NULL_ENTITY);
        OverlapCollector<JPH::CollidePointCollector> collector;
        collector.scene = scene;
        collector.out = out;
        collector.maxHits = options.maxHits;
        physics->getNarrowPhaseQuery3D().CollidePoint(JPH::RVec3(toJolt(point)), collector, query.broadPhase, query.layers, query.bodies);
        if (!out){
            return collector.any ? 1 : 0;
        }
        sortColliders(*out);
        return out->size();
    }

    JPH::RMat44 poseAt(const Vector3& position, const Quaternion& rotation){
        return JPH::RMat44::sRotationTranslation(toJolt(rotation), JPH::RVec3(toJolt(position)));
    }

    // A capsule between two points as Jolt's Y-aligned capsule and its pose; a sphere when the
    // points meet. The shapes live on the caller's stack (SetEmbedded: no reference counting).
    struct CapsuleQuery{
        JPH::CapsuleShape capsule;
        JPH::SphereShape sphere;
        const JPH::Shape* shape = nullptr;
        JPH::RMat44 pose;

        CapsuleQuery(const Vector3& point1, const Vector3& point2, float radius):
            capsule(std::max(0.5f * (point2 - point1).length(), 1.0e-6f), radius), sphere(radius){
            capsule.SetEmbedded();
            sphere.SetEmbedded();
            const Vector3 axis = point2 - point1;
            const float length = axis.length();
            const Vector3 center = (point1 + point2) * 0.5f;
            if (length <= 1.0e-6f){
                shape = &sphere;
                pose = poseAt(center, Quaternion::IDENTITY);
                return;
            }
            shape = &capsule;
            const JPH::Quat rotation = JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), toJolt(axis / length));
            pose = JPH::RMat44::sRotationTranslation(rotation, JPH::RVec3(toJolt(center)));
        }
    };

    bool validSphere(const char* function, const Vector3& center, float radius){
        const char* why = PhysicsQueries3D::invalidPoint(center);
        if (!why) why = PhysicsQueries3D::invalidRadius(radius);
        if (why){
            Log::error("%s: %s", function, why);
            return false;
        }
        return true;
    }

    bool validBox(const char* function, const Vector3& center, const Vector3& halfExtents){
        const char* why = PhysicsQueries3D::invalidPoint(center);
        if (!why) why = PhysicsQueries3D::invalidHalfExtents(halfExtents);
        if (why){
            Log::error("%s: %s", function, why);
            return false;
        }
        return true;
    }

    bool validCapsule(const char* function, const Vector3& point1, const Vector3& point2, float radius){
        const char* why = PhysicsQueries3D::invalidPoint(point1);
        if (!why) why = PhysicsQueries3D::invalidPoint(point2);
        if (!why) why = PhysicsQueries3D::invalidRadius(radius);
        if (why){
            Log::error("%s: %s", function, why);
            return false;
        }
        return true;
    }

    // a TransformedShapeCollector keeping the leaf that holds one collider (its sub-shape user data)
    class ColliderLeafCollector : public JPH::TransformedShapeCollector{
    public:
        const JPH::Shape* root = nullptr;
        JPH::uint64 index = 0;
        bool found = false;
        JPH::TransformedShape leaf;

        void AddHit(const JPH::TransformedShape& shape) override{
            if (found || root->GetSubShapeUserData(shape.mSubShapeIDCreator.GetID()) != index){
                return;
            }
            leaf = shape;
            found = true;
            ForceEarlyOut();
        }
    };

}

using namespace jasperengine::physicsqueries3d;

const char* PhysicsQueries3D::invalidDirection(const Vector3& direction, float maxDistance){
    const float length = direction.length();
    if (!(length > 0.0f) || !std::isfinite(length)){
        return "direction must be a non-zero finite vector";
    }
    if (!(maxDistance > 0.0f) || !std::isfinite(maxDistance)){
        return "maxDistance must be a positive finite number";
    }
    return nullptr;
}

const char* PhysicsQueries3D::invalidRadius(float radius){
    return (radius > 0.0f && std::isfinite(radius)) ? nullptr : "radius must be a positive finite number";
}

const char* PhysicsQueries3D::invalidHalfExtents(const Vector3& halfExtents){
    return (finiteVector(halfExtents) && halfExtents.x > 0.0f && halfExtents.y > 0.0f && halfExtents.z > 0.0f)
        ? nullptr : "halfExtents must be positive and finite on every axis";
}

const char* PhysicsQueries3D::invalidPoint(const Vector3& point){
    return finiteVector(point) ? nullptr : "the position must be finite";
}

RayReturn PhysicsQueries3D::sphereCast(Scene* scene, Vector3 origin, float radius, Vector3 direction, float maxDistance, const QueryOptions& options){
    if (!validSphere("Physics.sphereCast", origin, radius)){
        return Ray::NO_HIT;
    }
    JPH::SphereShape sphere(radius);
    sphere.SetEmbedded();
    return cast(scene, "Physics.sphereCast", sphere, poseAt(origin, Quaternion::IDENTITY), direction, maxDistance, options, NULL_ENTITY, nullptr);
}

RayReturn PhysicsQueries3D::boxCast(Scene* scene, Vector3 center, Vector3 halfExtents, Vector3 direction, Quaternion orientation, float maxDistance, const QueryOptions& options){
    if (!validBox("Physics.boxCast", center, halfExtents)){
        return Ray::NO_HIT;
    }
    // no convex radius: the cast box has sharp corners, as Unity's
    JPH::BoxShape box(toJolt(halfExtents), 0.0f);
    box.SetEmbedded();
    return cast(scene, "Physics.boxCast", box, poseAt(center, orientation), direction, maxDistance, options, NULL_ENTITY, nullptr);
}

RayReturn PhysicsQueries3D::capsuleCast(Scene* scene, Vector3 point1, Vector3 point2, float radius, Vector3 direction, float maxDistance, const QueryOptions& options){
    if (!validCapsule("Physics.capsuleCast", point1, point2, radius)){
        return Ray::NO_HIT;
    }
    CapsuleQuery capsule(point1, point2, radius);
    return cast(scene, "Physics.capsuleCast", *capsule.shape, capsule.pose, direction, maxDistance, options, NULL_ENTITY, nullptr);
}

size_t PhysicsQueries3D::sphereCastAll(Scene* scene, Vector3 origin, float radius, Vector3 direction, float maxDistance, std::vector<RayReturn>& hits, const QueryOptions& options){
    hits.clear();
    if (!validSphere("Physics.sphereCastAll", origin, radius)){
        return 0;
    }
    JPH::SphereShape sphere(radius);
    sphere.SetEmbedded();
    cast(scene, "Physics.sphereCastAll", sphere, poseAt(origin, Quaternion::IDENTITY), direction, maxDistance, options, NULL_ENTITY, &hits);
    return hits.size();
}

size_t PhysicsQueries3D::boxCastAll(Scene* scene, Vector3 center, Vector3 halfExtents, Vector3 direction, Quaternion orientation, float maxDistance, std::vector<RayReturn>& hits, const QueryOptions& options){
    hits.clear();
    if (!validBox("Physics.boxCastAll", center, halfExtents)){
        return 0;
    }
    JPH::BoxShape box(toJolt(halfExtents), 0.0f);
    box.SetEmbedded();
    cast(scene, "Physics.boxCastAll", box, poseAt(center, orientation), direction, maxDistance, options, NULL_ENTITY, &hits);
    return hits.size();
}

size_t PhysicsQueries3D::capsuleCastAll(Scene* scene, Vector3 point1, Vector3 point2, float radius, Vector3 direction, float maxDistance, std::vector<RayReturn>& hits, const QueryOptions& options){
    hits.clear();
    if (!validCapsule("Physics.capsuleCastAll", point1, point2, radius)){
        return 0;
    }
    CapsuleQuery capsule(point1, point2, radius);
    cast(scene, "Physics.capsuleCastAll", *capsule.shape, capsule.pose, direction, maxDistance, options, NULL_ENTITY, &hits);
    return hits.size();
}

size_t PhysicsQueries3D::overlapSphere(Scene* scene, Vector3 center, float radius, std::vector<ColliderRef>& colliders, const QueryOptions& options){
    colliders.clear();
    if (!validSphere("Physics.overlapSphere", center, radius)){
        return 0;
    }
    JPH::SphereShape sphere(radius);
    sphere.SetEmbedded();
    return overlap(scene, sphere, poseAt(center, Quaternion::IDENTITY), options, &colliders);
}

size_t PhysicsQueries3D::overlapBox(Scene* scene, Vector3 center, Vector3 halfExtents, Quaternion orientation, std::vector<ColliderRef>& colliders, const QueryOptions& options){
    colliders.clear();
    if (!validBox("Physics.overlapBox", center, halfExtents)){
        return 0;
    }
    JPH::BoxShape box(toJolt(halfExtents), 0.0f);
    box.SetEmbedded();
    return overlap(scene, box, poseAt(center, orientation), options, &colliders);
}

size_t PhysicsQueries3D::overlapCapsule(Scene* scene, Vector3 point1, Vector3 point2, float radius, std::vector<ColliderRef>& colliders, const QueryOptions& options){
    colliders.clear();
    if (!validCapsule("Physics.overlapCapsule", point1, point2, radius)){
        return 0;
    }
    CapsuleQuery capsule(point1, point2, radius);
    return overlap(scene, *capsule.shape, capsule.pose, options, &colliders);
}

size_t PhysicsQueries3D::overlapPoint(Scene* scene, Vector3 point, std::vector<ColliderRef>& colliders, const QueryOptions& options){
    colliders.clear();
    if (const char* why = invalidPoint(point)){
        Log::error("Physics.overlapPoint: %s", why);
        return 0;
    }
    return overlapAtPoint(scene, point, options, &colliders);
}

bool PhysicsQueries3D::checkSphere(Scene* scene, Vector3 center, float radius, const QueryOptions& options){
    if (!validSphere("Physics.checkSphere", center, radius)){
        return false;
    }
    JPH::SphereShape sphere(radius);
    sphere.SetEmbedded();
    return overlap(scene, sphere, poseAt(center, Quaternion::IDENTITY), options, nullptr) > 0;
}

bool PhysicsQueries3D::checkBox(Scene* scene, Vector3 center, Vector3 halfExtents, Quaternion orientation, const QueryOptions& options){
    if (!validBox("Physics.checkBox", center, halfExtents)){
        return false;
    }
    JPH::BoxShape box(toJolt(halfExtents), 0.0f);
    box.SetEmbedded();
    return overlap(scene, box, poseAt(center, orientation), options, nullptr) > 0;
}

bool PhysicsQueries3D::checkCapsule(Scene* scene, Vector3 point1, Vector3 point2, float radius, const QueryOptions& options){
    if (!validCapsule("Physics.checkCapsule", point1, point2, radius)){
        return false;
    }
    CapsuleQuery capsule(point1, point2, radius);
    return overlap(scene, *capsule.shape, capsule.pose, options, nullptr) > 0;
}

bool PhysicsQueries3D::checkPoint(Scene* scene, Vector3 point, const QueryOptions& options){
    if (const char* why = invalidPoint(point)){
        Log::error("Physics.checkPoint: %s", why);
        return false;
    }
    return overlapAtPoint(scene, point, options, nullptr) > 0;
}

bool PhysicsQueries3D::closestPoint(Scene* scene, Vector3 point, const ColliderRef& collider, Vector3& closest){
    closest = point;
    if (!finiteVector(point) || !scene || collider.index < 0){
        return false;
    }
    std::shared_ptr<PhysicsSystem> physics = worldOf(scene);
    // an attached child's collider is a leaf of the body it joined (S96 REQ-18)
    Entity bodyEntity = collider.entity;
    int index = collider.index;
    AttachedColliders::bodyIndexOf3D(scene, collider.entity, collider.index, bodyEntity, index);
    const RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(bodyEntity);
    if (!physics || !body || (size_t)index >= AttachedColliders::colliderCount(*body)){
        return false;
    }
    // a trigger collider of a body with solid ones is on its companion sensor body
    for (const JPH::BodyID& id : {body->body, body->triggerBody}){
        if (id.IsInvalid()){
            continue;
        }
        JPH::BodyLockRead lock(physics->getBodyLockInterface3D(), id);
        if (!lock.Succeeded()){
            continue;
        }
        const JPH::TransformedShape shape = lock.GetBody().GetTransformedShape();
        lock.ReleaseLock();
        ColliderLeafCollector leaves;
        leaves.root = shape.mShape;
        leaves.index = (JPH::uint64)index;
        shape.CollectTransformedShapes(JPH::AABox::sBiggest(), leaves);
        if (!leaves.found){
            continue;
        }
        if (leaves.leaf.mShape->GetType() != JPH::EShapeType::Convex){
            return false;
        }
        const JPH::ConvexShape* convex = static_cast<const JPH::ConvexShape*>(leaves.leaf.mShape.GetPtr());
        const JPH::RMat44 transform = leaves.leaf.GetCenterOfMassTransform();
        const JPH::Vec3 local = JPH::Vec3(transform.InversedRotationTranslation() * JPH::RVec3(toJolt(point)));
        JPH::ConvexShape::SupportBuffer buffer;
        const JPH::ConvexShape::Support* support = convex->GetSupportFunction(JPH::ConvexShape::ESupportMode::IncludeConvexRadius, buffer, leaves.leaf.GetShapeScale());
        JPH::PointConvexSupport target{local};
        JPH::GJKClosestPoint gjk;
        JPH::Vec3 v = local.IsNearZero() ? JPH::Vec3::sAxisX() : local;
        JPH::Vec3 onShape, onPoint;
        const float distanceSq = gjk.GetClosestPoints(*support, target, 1.0e-5f, FLT_MAX, v, onShape, onPoint);
        // 0: the point is inside (Unity returns it unchanged)
        if (distanceSq > 0.0f){
            closest = fromJolt(JPH::Vec3(transform * JPH::RVec3(onShape)));
        }
        return true;
    }
    return false;
}

RayReturn PhysicsQueries3D::sweepTest(Scene* scene, Entity body, Vector3 direction, float maxDistance, const QueryOptions& options){
    std::shared_ptr<PhysicsSystem> physics = worldOf(scene);
    const RigidbodyComponent* component = scene ? scene->findComponent<RigidbodyComponent>(body) : nullptr;
    if (!physics || !component || component->body.IsInvalid()){
        return Ray::NO_HIT;
    }
    JPH::RefConst<JPH::Shape> shape;
    JPH::RMat44 start;
    {
        JPH::BodyLockRead lock(physics->getBodyLockInterface3D(), component->body);
        if (!lock.Succeeded()){
            return Ray::NO_HIT;
        }
        shape = lock.GetBody().GetShape();
        start = lock.GetBody().GetCenterOfMassTransform();
    }
    return cast(scene, "Rigidbody.sweepTest", *shape, start, direction, maxDistance, options, body, nullptr);
}

size_t PhysicsQueries3D::sweepTestAll(Scene* scene, Entity body, Vector3 direction, float maxDistance, std::vector<RayReturn>& hits, const QueryOptions& options){
    hits.clear();
    std::shared_ptr<PhysicsSystem> physics = worldOf(scene);
    const RigidbodyComponent* component = scene ? scene->findComponent<RigidbodyComponent>(body) : nullptr;
    if (!physics || !component || component->body.IsInvalid()){
        return 0;
    }
    JPH::RefConst<JPH::Shape> shape;
    JPH::RMat44 start;
    {
        JPH::BodyLockRead lock(physics->getBodyLockInterface3D(), component->body);
        if (!lock.Succeeded()){
            return 0;
        }
        shape = lock.GetBody().GetShape();
        start = lock.GetBody().GetCenterOfMassTransform();
    }
    cast(scene, "Rigidbody.sweepTestAll", *shape, start, direction, maxDistance, options, body, &hits);
    return hits.size();
}

#endif // JASPERENGINE_PHYSICS_3D
