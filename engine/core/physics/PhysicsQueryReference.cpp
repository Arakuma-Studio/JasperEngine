// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "PhysicsQueryReference.h"
#include "Log.h"
#include "Scene.h"
#include "registry/PhysicsLayers.h"
#include "util/Angle.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "object/physics/Collision.h"
#include "physics/ContactTracker.h"
#include "physics/PhysicsQueries3D.h"
#include "subsystem/PhysicsSystem.h"
#include "util/JoltPhysicsAux.h"

#include "Jolt/Physics/Body/BodyLock.h"
#include "Jolt/Physics/Collision/CollidePointResult.h"
#include "Jolt/Physics/Collision/CollideShape.h"
#include "Jolt/Physics/Collision/Shape/BoxShape.h"
#include "Jolt/Physics/Collision/Shape/CapsuleShape.h"
#include "Jolt/Physics/Collision/Shape/SphereShape.h"
#include "Jolt/Physics/Collision/TransformedShape.h"
#endif

#ifdef JASPERENGINE_PHYSICS_2D
#include "object/physics/Collision2D.h"
#include "physics/PhysicsQueries2D.h"
#include "subsystem/PhysicsSystem.h"
#include "util/Box2DAux.h"
#endif

#include <algorithm>
#include <cfloat>
#include <cmath>

using namespace jasperengine;

namespace {

    bool referenceLess(const physics::QueryCollider& a, const physics::QueryCollider& b){
        return a.entity != b.entity ? a.entity < b.entity : a.index < b.index;
    }

    // S96's overlap order, each collider once
    void sortReference(std::vector<physics::QueryCollider>& out){
        std::sort(out.begin(), out.end(), referenceLess);
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

#ifdef JASPERENGINE_PHYSICS_3D
    JPH::Vec3 referenceToJolt(const Vector3& v){
        return JPH::Vec3(v.x, v.y, v.z);
    }

    // every collider one body's shape meets, as S96's OverlapCollector resolves it
    template<typename Base>
    class ReferenceCollector3D : public Base{
    public:
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        std::vector<physics::QueryCollider>* out = nullptr;

        void AddHit(const typename Base::ResultType& result) override{
            const JPH::TransformedShape* context = this->GetContext();
            const int index = context ? (int)context->mShape->GetSubShapeUserData(result.mSubShapeID2) : 0;
            const ColliderRef collider(scene, entity, index);
            out->push_back({collider.entity, collider.index});
        }
    };

    // The body-by-body sweep of S96's 3D overlaps: the same shapes (a sharp box, a capsule that
    // is a sphere when its points meet), filters and back-face rule, each body's shape tested alone
    void referenceOverlap3D(Scene* scene, const physics::QueryShape& shape, const QueryOptions& options, std::vector<physics::QueryCollider>& out){
        std::shared_ptr<PhysicsSystem> system = scene->getSystem<PhysicsSystem>();
        if (!system || !system->hasWorld3D() || options.layerMask == LAYER_MASK_NOTHING){
            return;
        }
        const JoltQueryObjectLayerFilter layers(system->getObjectLayerTable3D(), options.layerMask, (uint16_t)~0u, (uint16_t)~0u);
        const bool skipSensors = !Physics::hitsTriggers(options);

        JPH::SphereShape sphere(std::max(shape.radius, 1.0e-6f));
        sphere.SetEmbedded();
        JPH::BoxShape box(referenceToJolt(shape.type == physics::QueryShape::Type::Box ? shape.halfExtents : Vector3(1, 1, 1)), 0.0f);
        box.SetEmbedded();
        const float halfLength = 0.5f * (shape.b - shape.a).length();
        JPH::CapsuleShape capsule(std::max(halfLength, 1.0e-6f), std::max(shape.radius, 1.0e-6f));
        capsule.SetEmbedded();
        const JPH::Shape* query = &sphere;
        JPH::RMat44 pose = JPH::RMat44::sTranslation(JPH::RVec3(referenceToJolt(shape.a)));
        if (shape.type == physics::QueryShape::Type::Box){
            query = &box;
            const Quaternion& q = shape.rotation;
            pose = JPH::RMat44::sRotationTranslation(JPH::Quat(q.x, q.y, q.z, q.w).Normalized(), JPH::RVec3(referenceToJolt(shape.a)));
        }else if (shape.type == physics::QueryShape::Type::Capsule){
            const Vector3 axis = shape.b - shape.a;
            const float length = axis.length();
            const JPH::RVec3 center(referenceToJolt((shape.a + shape.b) * 0.5f));
            pose = JPH::RMat44::sTranslation(center);
            if (length > 1.0e-6f){
                query = &capsule;
                pose = JPH::RMat44::sRotationTranslation(JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), referenceToJolt(axis / length)), center);
            }
        }
        JPH::CollideShapeSettings settings;
        settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;

        JPH::BodyIDVector bodies;
        system->getWorld3D()->GetBodies(bodies);
        ReferenceCollector3D<JPH::CollideShapeCollector> shapes;
        ReferenceCollector3D<JPH::CollidePointCollector> points;
        shapes.scene = points.scene = scene;
        shapes.out = points.out = &out;
        for (const JPH::BodyID& id : bodies){
            JPH::BodyLockRead lock(system->getBodyLockInterface3D(), id);
            if (!lock.Succeeded()){
                continue;
            }
            const JPH::Body& body = lock.GetBody();
            if (!body.IsInBroadPhase() || !layers.ShouldCollide(body.GetObjectLayer()) || (skipSensors && body.IsSensor())){
                continue;
            }
            const Entity entity = PhysicsCallbacks::bodyEntity(body.GetUserData());
            bool ignored = false;
            if (options.ignore){
                for (Entity each : *options.ignore){
                    ignored = ignored || (each != NULL_ENTITY && each == entity);
                }
            }
            if (ignored){
                continue;
            }
            const JPH::TransformedShape transformed = body.GetTransformedShape();
            if (shape.type == physics::QueryShape::Type::Point){
                points.entity = entity;
                transformed.CollidePoint(JPH::RVec3(referenceToJolt(shape.a)), points);
            }else{
                shapes.entity = entity;
                transformed.CollideShape(query, JPH::Vec3::sReplicate(1.0f), pose, settings, pose.GetTranslation(), shapes);
            }
        }
    }
#endif

#ifdef JASPERENGINE_PHYSICS_2D
    // Box2D's distance proxy of a live shape, as its world overlap builds it
    b2ShapeProxy referenceProxy2D(b2ShapeId shapeId, b2Vec2* scratch){
        switch (b2Shape_GetType(shapeId)){
            case b2_circleShape: {
                const b2Circle circle = b2Shape_GetCircle(shapeId);
                scratch[0] = circle.center;
                return b2MakeProxy(scratch, 1, circle.radius);
            }
            case b2_capsuleShape: {
                const b2Capsule capsule = b2Shape_GetCapsule(shapeId);
                scratch[0] = capsule.center1;
                scratch[1] = capsule.center2;
                return b2MakeProxy(scratch, 2, capsule.radius);
            }
            case b2_segmentShape: {
                const b2Segment segment = b2Shape_GetSegment(shapeId);
                scratch[0] = segment.point1;
                scratch[1] = segment.point2;
                return b2MakeProxy(scratch, 2, 0.0f);
            }
            case b2_chainSegmentShape: {
                const b2ChainSegment segment = b2Shape_GetChainSegment(shapeId);
                scratch[0] = segment.segment.point1;
                scratch[1] = segment.segment.point2;
                return b2MakeProxy(scratch, 2, 0.0f);
            }
            default: {
                const b2Polygon polygon = b2Shape_GetPolygon(shapeId);
                for (int i = 0; i < polygon.count; i++){
                    scratch[i] = polygon.vertices[i];
                }
                return b2MakeProxy(scratch, polygon.count, polygon.radius);
            }
        }
    }

    bool collectReferenceShape2D(b2ShapeId shapeId, void* context){
        static_cast<std::vector<b2ShapeId>*>(context)->push_back(shapeId);
        return true;
    }

    // The shape-by-shape sweep of S96's 2D overlaps: the query shape built as theirs (a circle,
    // box or capsule about the origin, posed by its transform), Box2D's shape distance per shape
    void referenceOverlap2D(Scene* scene, const physics::QueryShape& shape, const QueryOptions& options, std::vector<physics::QueryCollider>& out){
        std::shared_ptr<PhysicsSystem> system = scene->getSystem<PhysicsSystem>();
        if (!system || options.layerMask == LAYER_MASK_NOTHING){
            return;
        }
        const b2WorldId world = system->getWorld2D();
        const float ptm = system->getPointsToMeterScale2D();
        if (!b2World_IsValid(world) || !(ptm > 0.0f)){
            return;
        }
        Vector2 center(shape.a.x, shape.a.y);
        Vector2 size(shape.halfExtents.x * 2.0f, shape.halfExtents.y * 2.0f);
        float radians = Angle::degToRad(shape.angle);
        if (shape.type == physics::QueryShape::Type::Area2D){
            center = Vector2((shape.a.x + shape.b.x) * 0.5f, (shape.a.y + shape.b.y) * 0.5f);
            size = Vector2(std::fabs(shape.b.x - shape.a.x), std::fabs(shape.b.y - shape.a.y));
            radians = 0.0f;
        }
        const b2Transform transform = {{center.x / ptm, center.y / ptm}, b2MakeRot(radians)};
        b2Vec2 queryPoints[B2_MAX_POLYGON_VERTICES];
        b2ShapeProxy proxy;
        switch (shape.type){
            case physics::QueryShape::Type::Point2D:
                queryPoints[0] = {0.0f, 0.0f};
                proxy = b2MakeProxy(queryPoints, 1, 0.0f);
                break;
            case physics::QueryShape::Type::Circle2D:
                queryPoints[0] = {0.0f, 0.0f};
                proxy = b2MakeProxy(queryPoints, 1, shape.radius / ptm);
                break;
            case physics::QueryShape::Type::Capsule2D: {
                const bool vertical = shape.direction2D == CapsuleDirection2D::VERTICAL;
                const float radius = 0.5f * (vertical ? size.x : size.y);
                const float half = 0.5f * (vertical ? size.y : size.x) - radius;
                queryPoints[0] = {0.0f, 0.0f};
                proxy = b2MakeProxy(queryPoints, 1, radius / ptm);
                if (half > 0.0f){
                    const b2Vec2 axis = vertical ? b2Vec2{0.0f, half / ptm} : b2Vec2{half / ptm, 0.0f};
                    queryPoints[0] = b2Neg(axis);
                    queryPoints[1] = axis;
                    proxy = b2MakeProxy(queryPoints, 2, radius / ptm);
                }
                break;
            }
            default: {
                const b2Polygon polygon = b2MakeBox(0.5f * size.x / ptm, 0.5f * size.y / ptm);
                for (int i = 0; i < polygon.count; i++){
                    queryPoints[i] = polygon.vertices[i];
                }
                proxy = b2MakeProxy(queryPoints, polygon.count, polygon.radius);
                break;
            }
        }

        // every shape (the tree walk of an all-covering box), then the overlaps' tests on each
        std::vector<b2ShapeId> shapes;
        b2QueryFilter everything;
        everything.categoryBits = ~0ull;
        everything.maskBits = ~0ull;
        const b2AABB all = {{-FLT_MAX * 0.25f, -FLT_MAX * 0.25f}, {FLT_MAX * 0.25f, FLT_MAX * 0.25f}};
        b2World_OverlapAABB(world, all, everything, collectReferenceShape2D, &shapes);

        const b2QueryFilter filter = Box2DLayers::makeQueryFilter((uint16_t)~0u, options.layerMask);
        const bool skipSensors = !Physics2D::hitsTriggers(options);
        for (b2ShapeId shapeId : shapes){
            const b2Filter bits = b2Shape_GetFilter(shapeId);
            if ((bits.categoryBits & filter.maskBits) == 0 || (bits.maskBits & filter.categoryBits) == 0 ||
                    (bits.categoryBits & Box2DLayers::USER_BITS) == 0 || (skipSensors && b2Shape_IsSensor(shapeId))){
                continue;
            }
            const Entity entity = static_cast<Entity>(reinterpret_cast<uintptr_t>(b2Body_GetUserData(b2Shape_GetBody(shapeId))));
            bool ignored = false;
            if (options.ignore){
                for (Entity each : *options.ignore){
                    ignored = ignored || (each != NULL_ENTITY && each == entity);
                }
            }
            if (ignored){
                continue;
            }
            b2Vec2 scratch[B2_MAX_POLYGON_VERTICES];
            b2DistanceInput input;
            input.proxyA = proxy;
            input.proxyB = referenceProxy2D(shapeId, scratch);
            input.transformA = transform;
            input.transformB = b2Body_GetTransform(b2Shape_GetBody(shapeId));
            input.useRadii = true;
            b2SimplexCache cache = {};
            if (b2ShapeDistance(&cache, &input, nullptr, 0).distance > 0.0f){
                continue;
            }
            const ColliderRef2D collider(scene, entity, (int)reinterpret_cast<size_t>(b2Shape_GetUserData(shapeId)));
            out.push_back({collider.entity, collider.index});
        }
    }
#endif

}

physics::QueryShape physics::QueryShape::sphere(const Vector3& center, float radius){
    QueryShape shape;
    shape.type = Type::Sphere;
    shape.a = center;
    shape.radius = radius;
    return shape;
}

physics::QueryShape physics::QueryShape::box(const Vector3& center, const Vector3& halfExtents, const Quaternion& rotation){
    QueryShape shape;
    shape.type = Type::Box;
    shape.a = center;
    shape.halfExtents = halfExtents;
    shape.rotation = rotation;
    return shape;
}

physics::QueryShape physics::QueryShape::capsule(const Vector3& point1, const Vector3& point2, float radius){
    QueryShape shape;
    shape.type = Type::Capsule;
    shape.a = point1;
    shape.b = point2;
    shape.radius = radius;
    return shape;
}

physics::QueryShape physics::QueryShape::point(const Vector3& point){
    QueryShape shape;
    shape.type = Type::Point;
    shape.a = point;
    return shape;
}

physics::QueryShape physics::QueryShape::point2D(const Vector2& point){
    QueryShape shape;
    shape.type = Type::Point2D;
    shape.a = Vector3(point.x, point.y, 0.0f);
    return shape;
}

physics::QueryShape physics::QueryShape::circle2D(const Vector2& center, float radius){
    QueryShape shape = point2D(center);
    shape.type = Type::Circle2D;
    shape.radius = radius;
    return shape;
}

physics::QueryShape physics::QueryShape::box2D(const Vector2& center, const Vector2& size, float angle){
    QueryShape shape = point2D(center);
    shape.type = Type::Box2D;
    shape.halfExtents = Vector3(size.x * 0.5f, size.y * 0.5f, 0.0f);
    shape.angle = Angle::defaultToDeg(angle);
    return shape;
}

physics::QueryShape physics::QueryShape::capsule2D(const Vector2& center, const Vector2& size, CapsuleDirection2D direction, float angle){
    QueryShape shape = box2D(center, size, angle);
    shape.type = Type::Capsule2D;
    shape.direction2D = direction;
    return shape;
}

physics::QueryShape physics::QueryShape::area2D(const Vector2& pointA, const Vector2& pointB){
    QueryShape shape = point2D(pointA);
    shape.type = Type::Area2D;
    shape.b = Vector3(pointB.x, pointB.y, 0.0f);
    return shape;
}

void physics::capsule2DSegment(const Vector2& center, const Vector2& size, CapsuleDirection2D direction, float angleDegrees,
                               Vector2& point1, Vector2& point2, float& radius){
    // Unity: the round ends lie along direction; the size is the capsule's bounding box, so
    // the radius is half the other side and the segment the rest of this one
    const bool vertical = direction == CapsuleDirection2D::VERTICAL;
    const float along = std::fabs(vertical ? size.y : size.x);
    const float across = std::fabs(vertical ? size.x : size.y);
    radius = across * 0.5f;
    const float half = std::max(0.0f, along * 0.5f - radius);
    const float angle = Angle::degToRad(angleDegrees);
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const Vector2 localAxis = vertical ? Vector2(0.0f, half) : Vector2(half, 0.0f);
    const Vector2 axis(localAxis.x * c - localAxis.y * s, localAxis.x * s + localAxis.y * c);
    point1 = Vector2(center.x - axis.x, center.y - axis.y);
    point2 = Vector2(center.x + axis.x, center.y + axis.y);
}

void physics::referenceOverlap(Scene* scene, const QueryShape& shape, const QueryOptions& options, std::vector<QueryCollider>& out){
    out.clear();
    if (!scene){
        return;
    }
#ifdef JASPERENGINE_PHYSICS_3D
    if (!shape.is2D()){
        referenceOverlap3D(scene, shape, options, out);
    }
#endif
#ifdef JASPERENGINE_PHYSICS_2D
    if (shape.is2D()){
        referenceOverlap2D(scene, shape, options, out);
    }
#endif
    (void)options;
    sortReference(out);
}

size_t physics::queryOverlap(Scene* scene, const QueryShape& shape, const QueryOptions& options, std::vector<QueryCollider>& out){
    out.clear();
#ifdef JASPERENGINE_PHYSICS_3D
    if (!shape.is2D()){
        std::vector<ColliderRef> colliders;
        switch (shape.type){
            case QueryShape::Type::Box: PhysicsQueries3D::overlapBox(scene, shape.a, shape.halfExtents, shape.rotation, colliders, options); break;
            case QueryShape::Type::Capsule: PhysicsQueries3D::overlapCapsule(scene, shape.a, shape.b, shape.radius, colliders, options); break;
            case QueryShape::Type::Point: PhysicsQueries3D::overlapPoint(scene, shape.a, colliders, options); break;
            default: PhysicsQueries3D::overlapSphere(scene, shape.a, shape.radius, colliders, options); break;
        }
        for (const ColliderRef& collider : colliders) out.push_back({collider.entity, collider.index});
        return out.size();
    }
#endif
#ifdef JASPERENGINE_PHYSICS_2D
    if (shape.is2D()){
        std::vector<ColliderRef2D> colliders;
        const Vector2 a(shape.a.x, shape.a.y);
        const Vector2 size(shape.halfExtents.x * 2.0f, shape.halfExtents.y * 2.0f);
        const float angle = Angle::degToDefault(shape.angle);
        switch (shape.type){
            case QueryShape::Type::Circle2D: PhysicsQueries2D::overlapCircleAll(scene, a, shape.radius, colliders, options); break;
            case QueryShape::Type::Box2D: PhysicsQueries2D::overlapBoxAll(scene, a, size, angle, colliders, options); break;
            case QueryShape::Type::Capsule2D: PhysicsQueries2D::overlapCapsuleAll(scene, a, size, shape.direction2D, angle, colliders, options); break;
            case QueryShape::Type::Area2D: PhysicsQueries2D::overlapAreaAll(scene, a, Vector2(shape.b.x, shape.b.y), colliders, options); break;
            default: PhysicsQueries2D::overlapPointAll(scene, a, colliders, options); break;
        }
        for (const ColliderRef2D& collider : colliders) out.push_back({collider.entity, collider.index});
        return out.size();
    }
#endif
    (void)scene;
    (void)options;
    Log::error("PhysicsQueryReference: this build has no physics backend for the shape");
    return 0;
}

physics::ReferenceDiff physics::compare(std::span<const QueryCollider> query, std::span<const QueryCollider> reference){
    // both are in the overlaps' order: a merge walk
    ReferenceDiff diff;
    diff.referenceCount = reference.size();
    size_t i = 0;
    size_t j = 0;
    while (i < query.size() || j < reference.size()){
        if (j >= reference.size() || (i < query.size() && referenceLess(query[i], reference[j]))){
            diff.extra.push_back(query[i++]);
        }else if (i >= query.size() || referenceLess(reference[j], query[i])){
            diff.missing.push_back(reference[j++]);
        }else{
            i++;
            j++;
        }
    }
    return diff;
}
