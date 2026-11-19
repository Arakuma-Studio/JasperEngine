// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The 2D spatial queries of Physics2D (spatial-queries R9-R11): Box2D's world overlaps and shape
// casts, in scene units (points) at the API and metres inside. The callbacks write into the
// caller's vector, or keep only the first hit, so nothing is allocated per query (D2).

#include "registry/PhysicsLayers.h"

#ifdef JASPERENGINE_PHYSICS_2D

#include "physics/PhysicsQueryReference.h"
#include "physics/PhysicsQueryDebug.h"
#include "subsystem/PhysicsSystem.h"
#include "util/Box2DAux.h"
#include "Scene.h"
#include "Log.h"

#include <cfloat>
#include <cmath>

using namespace jasperengine;

namespace {

    bool finite2D(const Vector2& v){
        return std::isfinite(v.x) && std::isfinite(v.y);
    }

    // The scene's world when it can be queried now (D6, R18)
    struct QueryWorld2D{
        PhysicsSystem* system = nullptr;
        b2WorldId world = b2_nullWorldId;
        float pointsToMeter = 1.0f;
    };

    bool queryWorld2D(Scene* scene, std::atomic<bool>& insideStepLogged, const char* function, QueryWorld2D& out){
        if (!scene){
            return false;
        }
        PhysicsSystem* system = scene->getSystem<PhysicsSystem>().get();
        const b2WorldId world = system->getWorld2D();
        if (!b2World_IsValid(world)){
            return false;
        }
        if (system->isSteppingWorld2D()){
            if (!insideStepLogged.exchange(true, std::memory_order_relaxed)){
                Log::error("%s: called inside the physics step (a contact callback); queries run outside the physics step: queue the query for onUpdate", function);
            }
            return false;
        }
        out.system = system;
        out.world = world;
        out.pointsToMeter = system->getPointsToMeterScale2D();
        return true;
    }

    // R11 after Box2D's own layer and mask tests: the collider's category, triggers, ignore list
    struct ShapeFilter2D{
        std::span<const Entity> ignore;
        bool hitTriggers = true;

        bool passes(b2ShapeId shapeId, Entity& entity) const{
            if ((b2Shape_GetFilter(shapeId).categoryBits & Box2DLayers::USER_BITS) == 0){
                return false;
            }
            if (!hitTriggers && b2Shape_IsSensor(shapeId)){
                return false;
            }
            entity = static_cast<Entity>(reinterpret_cast<uintptr_t>(b2Body_GetUserData(b2Shape_GetBody(shapeId))));
            for (Entity ignored : ignore){
                if (ignored != NULL_ENTITY && ignored == entity){
                    return false;
                }
            }
            return true;
        }
    };

    OverlapHit overlapHitOf2D(b2ShapeId shapeId, Entity entity){
        OverlapHit hit;
        hit.body = entity;
        hit.colliderIndex = (uint32_t)reinterpret_cast<uintptr_t>(b2Shape_GetUserData(shapeId));
        hit.trigger = b2Shape_IsSensor(shapeId);
        hit.hit = true;
        return hit;
    }

    bool overlapFirstLess(const OverlapHit& a, const OverlapHit& b){
        return a.body != b.body ? a.body < b.body : a.colliderIndex < b.colliderIndex;
    }

    // every hit into out, or (out null) the first in R6's order
    struct OverlapContext2D{
        ShapeFilter2D filter;
        std::vector<OverlapHit>* out = nullptr;
        OverlapHit first;
        size_t count = 0;
        Vector3* points = nullptr;
        size_t pointCount = 0;
        float pointsToMeter = 1.0f;
    };

    bool overlapCallback2D(b2ShapeId shapeId, void* context){
        OverlapContext2D* ctx = static_cast<OverlapContext2D*>(context);
        Entity entity = NULL_ENTITY;
        if (!ctx->filter.passes(shapeId, entity)){
            return true;
        }
        const OverlapHit hit = overlapHitOf2D(shapeId, entity);
        ctx->count++;
        if (ctx->out){
            ctx->out->push_back(hit);
        }else if (!ctx->first.hit || overlapFirstLess(hit, ctx->first)){
            ctx->first = hit;
        }
        if (ctx->points && ctx->pointCount < PhysicsQueryDebug::MAX_HIT_POINTS){
            const b2Vec2 p = b2Body_GetPosition(b2Shape_GetBody(shapeId));
            ctx->points[ctx->pointCount++] = Vector3(p.x * ctx->pointsToMeter, p.y * ctx->pointsToMeter, 0.0f);
        }
        return true;
    }

    struct CastContext2D{
        ShapeFilter2D filter;
        std::vector<RayReturn>* out = nullptr;
        RayReturn closest = Ray::NO_HIT;
        float closestFraction = 1.0f;
        float distance = 0.0f;
        float pointsToMeter = 1.0f;
    };

    float castCallback2D(b2ShapeId shapeId, b2Vec2 point, b2Vec2 normal, float fraction, void* context){
        CastContext2D* ctx = static_cast<CastContext2D*>(context);
        // never -1 (Box2D carries a callback's value into its next tree as the max fraction)
        // nor 0 (it stops the cast): a skipped shape keeps the current clip
        const float keep = ctx->out ? 1.0f : ctx->closestFraction;
        Entity entity = NULL_ENTITY;
        // shapes the cast starts in are not reported (R3)
        if (!(fraction > 0.0f) || !ctx->filter.passes(shapeId, entity)){
            return keep;
        }
        RayReturn hit;
        hit.hit = true;
        hit.distance = fraction * ctx->distance;
        hit.point = Vector3(point.x * ctx->pointsToMeter, point.y * ctx->pointsToMeter, 0.0f);
        hit.normal = Vector3(normal.x, normal.y, 0.0f);
        hit.body = entity;
        hit.colliderIndex = (size_t)reinterpret_cast<uintptr_t>(b2Shape_GetUserData(shapeId));
        if (ctx->out){
            ctx->out->push_back(hit);
            return 1.0f;
        }
        if (!ctx->closest.hit || fraction < ctx->closestFraction){
            ctx->closest = hit;
            ctx->closestFraction = fraction;
        }
        return ctx->closestFraction;
    }

    // A query shape in metres: a proxy-able circle, capsule or polygon
    struct Shape2D{
        enum Kind { CIRCLE, CAPSULE, POLYGON } kind = CIRCLE;
        b2Circle circle{};
        b2Capsule capsule{};
        b2Polygon polygon{};
        b2Transform transform = b2Transform_identity;
    };

    bool checkPositive2D(const char* function, const char* name, float value){
        if (!(value > 0.0f) || !std::isfinite(value)){
            Log::error("%s: %s must be a positive finite number (got %g)", function, name, value);
            return false;
        }
        return true;
    }

    bool checkPoint2D(const char* function, const char* name, const Vector3& point){
        if (!std::isfinite(point.x) || !std::isfinite(point.y)){
            Log::error("%s: %s must be a finite vector", function, name);
            return false;
        }
        return true;
    }

    // the QueryShape (scene units) as a Box2D shape (metres); false and one error when invalid (R7)
    bool makeShape2D(const char* function, const physics::QueryShape& shape, float pointsToMeter, Shape2D& out){
        const float scale = 1.0f / pointsToMeter;
        const b2Vec2 center = {shape.a.x * scale, shape.a.y * scale};
        switch (shape.type){
            case physics::QueryShape::Type::Point2D:
                if (!checkPoint2D(function, "point", shape.a)) return false;
                out.kind = Shape2D::CIRCLE;
                out.circle = {center, 0.0f};
                return true;
            case physics::QueryShape::Type::Circle2D:
                if (!checkPoint2D(function, "center", shape.a) || !checkPositive2D(function, "radius", shape.radius)) return false;
                out.kind = Shape2D::CIRCLE;
                out.circle = {center, shape.radius * scale};
                return true;
            case physics::QueryShape::Type::Box2D:
            case physics::QueryShape::Type::Area2D: {
                Vector3 middle = shape.a;
                Vector3 half = shape.halfExtents;
                float angle = shape.angle;
                if (shape.type == physics::QueryShape::Type::Area2D){
                    if (!checkPoint2D(function, "pointA", shape.a) || !checkPoint2D(function, "pointB", shape.b)) return false;
                    middle = (shape.a + shape.b) * 0.5f;
                    half = Vector3(std::fabs(shape.b.x - shape.a.x) * 0.5f, std::fabs(shape.b.y - shape.a.y) * 0.5f, 0.0f);
                    angle = 0.0f;
                }else if (!checkPoint2D(function, "center", shape.a)){
                    return false;
                }
                if (!std::isfinite(angle)){
                    Log::error("%s: angle must be a finite number", function);
                    return false;
                }
                if (!checkPositive2D(function, "size.x", half.x * 2.0f) || !checkPositive2D(function, "size.y", half.y * 2.0f)) return false;
                out.kind = Shape2D::POLYGON;
                out.polygon = b2MakeBox(half.x * scale, half.y * scale);
                out.transform = {{middle.x * scale, middle.y * scale}, b2MakeRot(angle * (float)M_PI / 180.0f)};
                return true;
            }
            case physics::QueryShape::Type::Capsule2D: {
                if (!checkPoint2D(function, "center", shape.a) || !std::isfinite(shape.angle)) return false;
                if (!checkPositive2D(function, "size.x", shape.halfExtents.x * 2.0f) || !checkPositive2D(function, "size.y", shape.halfExtents.y * 2.0f)) return false;
                Vector2 p1, p2;
                float radius;
                physics::capsule2DSegment(Vector2(shape.a.x, shape.a.y), Vector2(shape.halfExtents.x * 2.0f, shape.halfExtents.y * 2.0f),
                                          shape.direction2D, shape.angle, p1, p2, radius);
                out.kind = Shape2D::CAPSULE;
                out.capsule = {{p1.x * scale, p1.y * scale}, {p2.x * scale, p2.y * scale}, radius * scale};
                return true;
            }
            default:
                Log::error("%s: not a 2D shape", function);
                return false;
        }
    }

    b2ShapeProxy proxyOf2D(const Shape2D& shape){
        switch (shape.kind){
            case Shape2D::CAPSULE: return b2MakeProxy(&shape.capsule.center1, 2, shape.capsule.radius);
            case Shape2D::POLYGON: return b2MakeProxy(shape.polygon.vertices, shape.polygon.count, shape.polygon.radius);
            default: return b2MakeProxy(&shape.circle.center, 1, shape.circle.radius);
        }
    }

    void recordQuery2D(Scene* scene, const physics::QueryShape& shape, const Vector3& direction, float distance, bool cast,
                       const Vector3* points, size_t pointCount, bool hit){
        physics::recordQuery(scene, shape, direction, distance, cast, points, pointCount, hit);
    }

    // All (out) or first (out null)
    OverlapHit overlap2D(const char* function, std::atomic<bool>& logged, Scene* scene, const physics::QueryShape& shape,
                         std::vector<OverlapHit>* out, const PhysicsQueryOptions& options){
        if (out){
            out->clear();
        }
        QueryWorld2D world;
        const bool queryable = queryWorld2D(scene, logged, function, world);
        Shape2D query;
        // the arguments are checked with or without a world, so a bad call always says so
        if (!makeShape2D(function, shape, queryable ? world.pointsToMeter : 1.0f, query)){
            return OverlapHit();
        }
        OverlapContext2D context;
        context.filter.ignore = options.ignore;
        context.filter.hitTriggers = Physics2D::hitsTriggers(options.triggers);
        context.out = out;
        Vector3 points[PhysicsQueryDebug::MAX_HIT_POINTS];
        const bool recording = physics::recordingQueries();
        if (queryable && options.layerMask != LAYER_MASK_NOTHING){
            context.points = recording ? points : nullptr;
            context.pointsToMeter = world.pointsToMeter;
            const b2QueryFilter filter = Box2DLayers::makeQueryFilter((uint16_t)~0u, options.layerMask);
            switch (query.kind){
                case Shape2D::CIRCLE: b2World_OverlapCircle(world.world, &query.circle, query.transform, filter, overlapCallback2D, &context); break;
                case Shape2D::CAPSULE: b2World_OverlapCapsule(world.world, &query.capsule, query.transform, filter, overlapCallback2D, &context); break;
                case Shape2D::POLYGON: b2World_OverlapPolygon(world.world, &query.polygon, query.transform, filter, overlapCallback2D, &context); break;
            }
        }
        size_t hits = context.first.hit ? 1 : 0;
        if (out){
            out->resize(physics::sortOverlapHits(out->data(), out->size()));
            hits = out->size();
            if (!out->empty()){
                context.first = out->front();
            }
        }
        Physics::countQuery(hits);
        if (recording){
            recordQuery2D(scene, shape, Vector3::ZERO, 0.0f, false, points, context.pointCount, hits > 0);
        }
        return context.first;
    }

    RayReturn cast2D(const char* function, std::atomic<bool>& logged, Scene* scene, const physics::QueryShape& shape,
                     const Vector2& direction, float distance, std::vector<RayReturn>* out, const PhysicsQueryOptions& options){
        if (out){
            out->clear();
        }
        const float length = direction.length();
        if (!(length > 0.0f) || !std::isfinite(length)){
            Log::error("%s: direction must be a non-zero vector", function);
            return Ray::NO_HIT;
        }
        if (!(distance > 0.0f) || !std::isfinite(distance)){
            Log::error("%s: distance must be a positive finite number", function);
            return Ray::NO_HIT;
        }
        QueryWorld2D world;
        const bool queryable = queryWorld2D(scene, logged, function, world);
        Shape2D query;
        if (!makeShape2D(function, shape, queryable ? world.pointsToMeter : 1.0f, query)){
            return Ray::NO_HIT;
        }
        const Vector2 unit(direction.x / length, direction.y / length);
        CastContext2D context;
        context.filter.ignore = options.ignore;
        context.filter.hitTriggers = Physics2D::hitsTriggers(options.triggers);
        context.out = out;
        context.distance = distance;
        if (queryable && options.layerMask != LAYER_MASK_NOTHING){
            context.pointsToMeter = world.pointsToMeter;
            const float meters = distance / world.pointsToMeter;
            const b2Vec2 translation = {unit.x * meters, unit.y * meters};
            const b2QueryFilter filter = Box2DLayers::makeQueryFilter((uint16_t)~0u, options.layerMask);
            switch (query.kind){
                case Shape2D::CIRCLE: b2World_CastCircle(world.world, &query.circle, query.transform, translation, filter, castCallback2D, &context); break;
                case Shape2D::CAPSULE: b2World_CastCapsule(world.world, &query.capsule, query.transform, translation, filter, castCallback2D, &context); break;
                case Shape2D::POLYGON: b2World_CastPolygon(world.world, &query.polygon, query.transform, translation, filter, castCallback2D, &context); break;
            }
        }
        RayReturn result = context.closest;
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
            const float reach = out ? (out->empty() ? distance : out->back().distance) : (result.hit ? result.distance : distance);
            recordQuery2D(scene, shape, Vector3(unit.x, unit.y, 0.0f), reach, true, points, pointCount, hits > 0);
        }
        return result;
    }

    physics::QueryShape shape2D(physics::QueryShape::Type type, const Vector2& a){
        physics::QueryShape shape;
        shape.type = type;
        shape.a = Vector3(a.x, a.y, 0.0f);
        return shape;
    }

    physics::QueryShape circleShape2D(const Vector2& center, float radius){
        physics::QueryShape shape = shape2D(physics::QueryShape::Type::Circle2D, center);
        shape.radius = radius;
        return shape;
    }

    physics::QueryShape boxShape2D(const Vector2& center, const Vector2& size, float angle){
        physics::QueryShape shape = shape2D(physics::QueryShape::Type::Box2D, center);
        shape.halfExtents = Vector3(size.x * 0.5f, size.y * 0.5f, 0.0f);
        shape.angle = angle;
        return shape;
    }

    physics::QueryShape capsuleShape2D(const Vector2& center, const Vector2& size, CapsuleDirection2D direction, float angle){
        physics::QueryShape shape = shape2D(physics::QueryShape::Type::Capsule2D, center);
        shape.halfExtents = Vector3(size.x * 0.5f, size.y * 0.5f, 0.0f);
        shape.direction2D = direction;
        shape.angle = angle;
        return shape;
    }

    physics::QueryShape areaShape2D(const Vector2& pointA, const Vector2& pointB){
        physics::QueryShape shape = shape2D(physics::QueryShape::Type::Area2D, pointA);
        shape.b = Vector3(pointB.x, pointB.y, 0.0f);
        return shape;
    }

    std::atomic<bool> stepLoggedOverlapPoint2D{false}, stepLoggedOverlapCircle2D{false}, stepLoggedOverlapBox2D{false};
    std::atomic<bool> stepLoggedOverlapCapsule2D{false}, stepLoggedOverlapArea2D{false};
    std::atomic<bool> stepLoggedCircleCast2D{false}, stepLoggedBoxCast2D{false}, stepLoggedCapsuleCast2D{false};

    // Box2D's distance proxy of a live shape (as its world overlap builds it)
    b2ShapeProxy shapeProxy2D(b2ShapeId shapeId, b2Vec2* scratch){
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

    struct ShapeList2D{
        std::vector<b2ShapeId> shapes;
    };

    bool collectShape2D(b2ShapeId shapeId, void* context){
        static_cast<ShapeList2D*>(context)->shapes.push_back(shapeId);
        return true;
    }

}

OverlapHit Physics2D::overlapPoint(Scene* scene, const Vector2& point, const PhysicsQueryOptions& options){
    return overlap2D("Physics2D.overlapPoint", stepLoggedOverlapPoint2D, scene, shape2D(physics::QueryShape::Type::Point2D, point), nullptr, options);
}

size_t Physics2D::overlapPointAll(Scene* scene, const Vector2& point, std::vector<OverlapHit>& out, const PhysicsQueryOptions& options){
    overlap2D("Physics2D.overlapPointAll", stepLoggedOverlapPoint2D, scene, shape2D(physics::QueryShape::Type::Point2D, point), &out, options);
    return out.size();
}

OverlapHit Physics2D::overlapCircle(Scene* scene, const Vector2& center, float radius, const PhysicsQueryOptions& options){
    return overlap2D("Physics2D.overlapCircle", stepLoggedOverlapCircle2D, scene, circleShape2D(center, radius), nullptr, options);
}

size_t Physics2D::overlapCircleAll(Scene* scene, const Vector2& center, float radius, std::vector<OverlapHit>& out, const PhysicsQueryOptions& options){
    overlap2D("Physics2D.overlapCircleAll", stepLoggedOverlapCircle2D, scene, circleShape2D(center, radius), &out, options);
    return out.size();
}

OverlapHit Physics2D::overlapBox(Scene* scene, const Vector2& center, const Vector2& size, float angleDegrees, const PhysicsQueryOptions& options){
    return overlap2D("Physics2D.overlapBox", stepLoggedOverlapBox2D, scene, boxShape2D(center, size, angleDegrees), nullptr, options);
}

size_t Physics2D::overlapBoxAll(Scene* scene, const Vector2& center, const Vector2& size, float angleDegrees, std::vector<OverlapHit>& out, const PhysicsQueryOptions& options){
    overlap2D("Physics2D.overlapBoxAll", stepLoggedOverlapBox2D, scene, boxShape2D(center, size, angleDegrees), &out, options);
    return out.size();
}

OverlapHit Physics2D::overlapCapsule(Scene* scene, const Vector2& center, const Vector2& size, CapsuleDirection2D direction, float angleDegrees, const PhysicsQueryOptions& options){
    return overlap2D("Physics2D.overlapCapsule", stepLoggedOverlapCapsule2D, scene, capsuleShape2D(center, size, direction, angleDegrees), nullptr, options);
}

size_t Physics2D::overlapCapsuleAll(Scene* scene, const Vector2& center, const Vector2& size, CapsuleDirection2D direction, float angleDegrees, std::vector<OverlapHit>& out, const PhysicsQueryOptions& options){
    overlap2D("Physics2D.overlapCapsuleAll", stepLoggedOverlapCapsule2D, scene, capsuleShape2D(center, size, direction, angleDegrees), &out, options);
    return out.size();
}

OverlapHit Physics2D::overlapArea(Scene* scene, const Vector2& pointA, const Vector2& pointB, const PhysicsQueryOptions& options){
    return overlap2D("Physics2D.overlapArea", stepLoggedOverlapArea2D, scene, areaShape2D(pointA, pointB), nullptr, options);
}

size_t Physics2D::overlapAreaAll(Scene* scene, const Vector2& pointA, const Vector2& pointB, std::vector<OverlapHit>& out, const PhysicsQueryOptions& options){
    overlap2D("Physics2D.overlapAreaAll", stepLoggedOverlapArea2D, scene, areaShape2D(pointA, pointB), &out, options);
    return out.size();
}

RayReturn Physics2D::circleCast(Scene* scene, const Vector2& origin, float radius, const Vector2& direction, float distance, const PhysicsQueryOptions& options){
    return cast2D("Physics2D.circleCast", stepLoggedCircleCast2D, scene, circleShape2D(origin, radius), direction, distance, nullptr, options);
}

RayReturn Physics2D::boxCast(Scene* scene, const Vector2& origin, const Vector2& size, float angleDegrees, const Vector2& direction, float distance, const PhysicsQueryOptions& options){
    return cast2D("Physics2D.boxCast", stepLoggedBoxCast2D, scene, boxShape2D(origin, size, angleDegrees), direction, distance, nullptr, options);
}

RayReturn Physics2D::capsuleCast(Scene* scene, const Vector2& origin, const Vector2& size, CapsuleDirection2D capsuleDirection, float angleDegrees, const Vector2& direction, float distance, const PhysicsQueryOptions& options){
    return cast2D("Physics2D.capsuleCast", stepLoggedCapsuleCast2D, scene, capsuleShape2D(origin, size, capsuleDirection, angleDegrees), direction, distance, nullptr, options);
}

size_t Physics2D::circleCastAll(Scene* scene, const Vector2& origin, float radius, const Vector2& direction, float distance, std::vector<RayReturn>& out, const PhysicsQueryOptions& options){
    cast2D("Physics2D.circleCastAll", stepLoggedCircleCast2D, scene, circleShape2D(origin, radius), direction, distance, &out, options);
    return out.size();
}

size_t Physics2D::boxCastAll(Scene* scene, const Vector2& origin, const Vector2& size, float angleDegrees, const Vector2& direction, float distance, std::vector<RayReturn>& out, const PhysicsQueryOptions& options){
    cast2D("Physics2D.boxCastAll", stepLoggedBoxCast2D, scene, boxShape2D(origin, size, angleDegrees), direction, distance, &out, options);
    return out.size();
}

size_t Physics2D::capsuleCastAll(Scene* scene, const Vector2& origin, const Vector2& size, CapsuleDirection2D capsuleDirection, float angleDegrees, const Vector2& direction, float distance, std::vector<RayReturn>& out, const PhysicsQueryOptions& options){
    cast2D("Physics2D.capsuleCastAll", stepLoggedCapsuleCast2D, scene, capsuleShape2D(origin, size, capsuleDirection, angleDegrees), direction, distance, &out, options);
    return out.size();
}

// The reference sweep (R16): every shape of the world, each tested with Box2D's shape distance
void physics::referenceOverlap2D(Scene* scene, const QueryShape& shape, const PhysicsQueryOptions& options, std::vector<OverlapHit>& out){
    out.clear();
    if (!scene){
        return;
    }
    std::shared_ptr<PhysicsSystem> system = scene->getSystem<PhysicsSystem>();
    const b2WorldId world = system->getWorld2D();
    if (!b2World_IsValid(world) || system->isSteppingWorld2D() || options.layerMask == LAYER_MASK_NOTHING){
        return;
    }
    Shape2D query;
    if (!makeShape2D("referenceOverlap", shape, system->getPointsToMeterScale2D(), query)){
        return;
    }
    // every shape (the tree walk of an all-covering box) and our own tests on each
    ShapeList2D list;
    b2QueryFilter everything;
    everything.categoryBits = ~0ull;
    everything.maskBits = ~0ull;
    const b2AABB all = {{-FLT_MAX * 0.25f, -FLT_MAX * 0.25f}, {FLT_MAX * 0.25f, FLT_MAX * 0.25f}};
    b2World_OverlapAABB(world, all, everything, collectShape2D, &list);

    const b2QueryFilter filter = Box2DLayers::makeQueryFilter((uint16_t)~0u, options.layerMask);
    ShapeFilter2D shapeFilter;
    shapeFilter.ignore = options.ignore;
    shapeFilter.hitTriggers = Physics2D::hitsTriggers(options.triggers);
    const b2ShapeProxy queryProxy = proxyOf2D(query);
    for (b2ShapeId shapeId : list.shapes){
        const b2Filter shapeFilterBits = b2Shape_GetFilter(shapeId);
        if ((shapeFilterBits.categoryBits & filter.maskBits) == 0 || (shapeFilterBits.maskBits & filter.categoryBits) == 0){
            continue;
        }
        Entity entity = NULL_ENTITY;
        if (!shapeFilter.passes(shapeId, entity)){
            continue;
        }
        b2Vec2 scratch[B2_MAX_POLYGON_VERTICES];
        b2DistanceInput input;
        input.proxyA = queryProxy;
        input.proxyB = shapeProxy2D(shapeId, scratch);
        input.transformA = query.transform;
        input.transformB = b2Body_GetTransform(b2Shape_GetBody(shapeId));
        input.useRadii = true;
        b2SimplexCache cache = {};
        const b2DistanceOutput output = b2ShapeDistance(&cache, &input, nullptr, 0);
        if (output.distance > 0.0f){
            continue;
        }
        out.push_back(overlapHitOf2D(shapeId, entity));
    }
    out.resize(sortOverlapHits(out.data(), out.size()));
}

#endif
