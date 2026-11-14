// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "PhysicsQueries2D.h"

#ifdef JASPERENGINE_PHYSICS_2D

#include "Log.h"
#include "Scene.h"
#include "component/Rigidbody2DComponent.h"
#include "registry/PhysicsLayers.h"
#include "subsystem/PhysicsSystem.h"
#include "util/Angle.h"
#include "util/Box2DAux.h"

#include <algorithm>
#include <cmath>
#include <memory>

using namespace jasperengine;

namespace jasperengine::physicsqueries2d {

    enum class Kind{ CIRCLE, POLYGON, CAPSULE };

    // A query shape in metres, posed by `transform` (Box2D's own types: no allocation)
    struct Shape2D{
        Kind kind = Kind::CIRCLE;
        b2Circle circle = {{0.0f, 0.0f}, 0.0f};
        b2Polygon polygon = {};
        b2Capsule capsule = {{0.0f, 0.0f}, {0.0f, 0.0f}, 0.0f};
        b2Transform transform = b2Transform_identity;
    };

    struct World2D{
        std::shared_ptr<PhysicsSystem> physics;
        b2WorldId world = b2_nullWorldId;
        float ptm = 1.0f;
    };

    // The scene's Box2D world, when one was made (it is created with the first 2D body)
    bool worldOf(Scene* scene, World2D& out){
        if (!scene){
            return false;
        }
        out.physics = scene->getSystem<PhysicsSystem>();
        if (!out.physics){
            return false;
        }
        out.world = out.physics->getWorld2D();
        out.ptm = out.physics->getPointsToMeterScale2D();
        return b2World_IsValid(out.world) && out.ptm > 0.0f;
    }

    bool finiteVector(const Vector2& v){
        return std::isfinite(v.x) && std::isfinite(v.y);
    }

    b2Vec2 metres(const Vector2& v, float ptm){
        return b2Vec2{v.x / ptm, v.y / ptm};
    }

    b2Transform poseAt(const Vector2& point, float angle, float ptm){
        return b2Transform{metres(point, ptm), b2MakeRot(Angle::defaultToRad(angle))};
    }

    Shape2D circleShape(const Vector2& point, float radius, float ptm){
        Shape2D shape;
        shape.kind = Kind::CIRCLE;
        shape.circle = b2Circle{{0.0f, 0.0f}, radius / ptm};
        shape.transform = poseAt(point, 0.0f, ptm);
        return shape;
    }

    Shape2D boxShape(const Vector2& point, const Vector2& size, float angle, float ptm){
        Shape2D shape;
        shape.kind = Kind::POLYGON;
        shape.polygon = b2MakeBox(0.5f * size.x / ptm, 0.5f * size.y / ptm);
        shape.transform = poseAt(point, angle, ptm);
        return shape;
    }

    // CapsuleCollider2D's shape: the round ends on the direction's axis, a circle when the
    // other side is not shorter
    Shape2D capsuleShape(const Vector2& point, const Vector2& size, CapsuleDirection2D direction, float angle, float ptm){
        const bool vertical = direction == CapsuleDirection2D::VERTICAL;
        const float radius = 0.5f * (vertical ? size.x : size.y);
        const float half = 0.5f * (vertical ? size.y : size.x) - radius;
        Shape2D shape;
        shape.transform = poseAt(point, angle, ptm);
        if (half <= 0.0f){
            shape.kind = Kind::CIRCLE;
            shape.circle = b2Circle{{0.0f, 0.0f}, radius / ptm};
            return shape;
        }
        shape.kind = Kind::CAPSULE;
        const b2Vec2 axis = vertical ? b2Vec2{0.0f, half / ptm} : b2Vec2{half / ptm, 0.0f};
        shape.capsule = b2Capsule{b2Neg(axis), axis, radius / ptm};
        return shape;
    }

    // The query filters of Physics2D.raycast (Box2DAux::CastCallback): the collider category
    // bits, the ignore list, the body a cast starts from, sensors (Unity triggers)
    struct Filter2D{
        const std::vector<Entity>* ignore = nullptr;
        Entity self = NULL_ENTITY;
        bool skipSensors = false;

        void set(const QueryOptions& options, Entity selfEntity){
            ignore = options.ignore;
            self = selfEntity;
            skipSensors = !Physics2D::hitsTriggers(options);
        }

        bool accepts(b2ShapeId shapeId, Entity& entity) const{
            if ((b2Shape_GetFilter(shapeId).categoryBits & Box2DLayers::USER_BITS) == 0){
                return false;
            }
            if (skipSensors && b2Shape_IsSensor(shapeId)){
                return false;
            }
            entity = static_cast<Entity>(reinterpret_cast<uintptr_t>(b2Body_GetUserData(b2Shape_GetBody(shapeId))));
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

    bool nearer2D(const RayReturn& a, const RayReturn& b){
        if (a.distance != b.distance) return a.distance < b.distance;
        if (a.body != b.body) return a.body < b.body;
        return a.colliderIndex < b.colliderIndex;
    }

    void keepHit2D(std::vector<RayReturn>& hits, const RayReturn& hit, size_t maxHits){
        for (RayReturn& kept : hits){
            if (kept.body == hit.body && kept.colliderIndex == hit.colliderIndex){
                if (nearer2D(hit, kept)){
                    kept = hit;
                }
                return;
            }
        }
        if (hits.size() < maxHits){
            hits.push_back(hit);
            return;
        }
        auto farthest = std::max_element(hits.begin(), hits.end(), nearer2D);
        if (farthest != hits.end() && nearer2D(hit, *farthest)){
            *farthest = hit;
        }
    }

    struct CastContext{
        Scene* scene = nullptr;
        float ptm = 1.0f;
        Vector2 origin;
        Vector2 direction;
        float distance = 0.0f;
        Filter2D filter;
        std::vector<RayReturn>* all = nullptr;
        size_t maxHits = 0;
        RayReturn closest = Ray::NO_HIT;
        float best = 1.0f;

        RayReturn makeHit(b2ShapeId shapeId, Entity entity, float fraction, Vector3 point, Vector3 normal) const{
            RayReturn hit = {true, fraction * distance, point, normal, entity, reinterpret_cast<size_t>(b2Shape_GetUserData(shapeId))};
            hit.scene = scene;
            hit.body2D = true;
            return hit;
        }

        void add(const RayReturn& hit, float fraction){
            if (all){
                keepHit2D(*all, hit, maxHits);
                return;
            }
            if (!closest.hit || nearer2D(hit, closest)){
                closest = hit;
                best = fraction;
            }
        }
    };

    // Box2D 3.1's shape cast stores whatever the callback returns as the next tree's clip
    // fraction, -1 included: a skipped shape returns the current clip instead of -1
    float castCallback(b2ShapeId shapeId, b2Vec2 point, b2Vec2 normal, float fraction, void* data){
        CastContext* context = static_cast<CastContext*>(data);
        Entity entity = NULL_ENTITY;
        if (!context->filter.accepts(shapeId, entity)){
            return context->all ? 1.0f : context->best;
        }
        context->add(context->makeHit(shapeId, entity, fraction, Vector3(point.x * context->ptm, point.y * context->ptm, 0.0f),
                                      Vector3(normal.x, normal.y, 0.0f)), fraction);
        return context->all ? 1.0f : context->best;
    }

    struct OverlapContext{
        Scene* scene = nullptr;
        Filter2D filter;
        // every collider, or (null) the first in GameObject / index order, or only whether any
        std::vector<ColliderRef2D>* out = nullptr;
        bool keepFirst = false;
        size_t maxHits = 0;
        bool any = false;
        ColliderRef2D first;
    };

    bool overlapCallback(b2ShapeId shapeId, void* data){
        OverlapContext* context = static_cast<OverlapContext*>(data);
        Entity entity = NULL_ENTITY;
        if (!context->filter.accepts(shapeId, entity)){
            return true;
        }
        const int index = (int)reinterpret_cast<size_t>(b2Shape_GetUserData(shapeId));
        context->any = true;
        if (context->out){
            // an attached child's collider resolves to the child's (S96 REQ-18)
            const ColliderRef2D collider(context->scene, entity, index);
            for (const ColliderRef2D& kept : *context->out){
                if (kept == collider){
                    return true;
                }
            }
            if (context->out->size() >= context->maxHits){
                return false;
            }
            context->out->push_back(collider);
            return true;
        }
        if (context->keepFirst){
            if (context->first.entity == NULL_ENTITY || entity < context->first.entity ||
                    (entity == context->first.entity && index < context->first.index)){
                context->first = ColliderRef2D(context->scene, entity, index);
            }
            return true;
        }
        return false;
    }

    void runOverlap(const World2D& world, const Shape2D& shape, b2QueryFilter filter, OverlapContext& context){
        switch (shape.kind){
            case Kind::CIRCLE:
                b2World_OverlapCircle(world.world, &shape.circle, shape.transform, filter, overlapCallback, &context);
                break;
            case Kind::POLYGON:
                b2World_OverlapPolygon(world.world, &shape.polygon, shape.transform, filter, overlapCallback, &context);
                break;
            case Kind::CAPSULE:
                b2World_OverlapCapsule(world.world, &shape.capsule, shape.transform, filter, overlapCallback, &context);
                break;
        }
    }

    void runCast(const World2D& world, const Shape2D& shape, b2Vec2 translation, b2QueryFilter filter, CastContext& context){
        switch (shape.kind){
            case Kind::CIRCLE:
                b2World_CastCircle(world.world, &shape.circle, shape.transform, translation, filter, castCallback, &context);
                break;
            case Kind::POLYGON:
                b2World_CastPolygon(world.world, &shape.polygon, shape.transform, translation, filter, castCallback, &context);
                break;
            case Kind::CAPSULE:
                b2World_CastCapsule(world.world, &shape.capsule, shape.transform, translation, filter, castCallback, &context);
                break;
        }
    }

    // `out` gets every collider; else `first` the first, else only whether any
    size_t overlap(Scene* scene, const World2D& world, const Shape2D& shape, const QueryOptions& options,
                   std::vector<ColliderRef2D>* out, ColliderRef2D* first){
        if (options.layerMask == LAYER_MASK_NOTHING || (out && options.maxHits == 0)){
            return 0;
        }
        OverlapContext context;
        context.scene = scene;
        context.filter.set(options, NULL_ENTITY);
        context.out = out;
        context.keepFirst = first != nullptr;
        context.maxHits = options.maxHits;
        runOverlap(world, shape, Box2DLayers::makeQueryFilter((uint16_t)~0u, options.layerMask), context);
        if (out){
            std::sort(out->begin(), out->end(), [](const ColliderRef2D& a, const ColliderRef2D& b){
                return a.entity != b.entity ? a.entity < b.entity : a.index < b.index;
            });
            return out->size();
        }
        if (first){
            *first = context.first;
        }
        return context.any ? 1 : 0;
    }

    // One query shape cast from where it is, for the world queries and each shape of a body;
    // the hits gather in `context`
    void castShape(Scene* scene, const World2D& world, const Shape2D& shape, const QueryOptions& options, CastContext& context){
        const b2Vec2 translation = metres(context.direction * context.distance, world.ptm);
        const b2QueryFilter filter = Box2DLayers::makeQueryFilter((uint16_t)~0u, options.layerMask);
        // Unity queriesStartInColliders: what the shape starts in is a hit at 0 (Box2D's cast
        // reports none there)
        if (Physics2D::startsInColliders(options)){
            struct StartContext{
                CastContext* cast;
                Vector3 point;
                Vector3 normal;
            } start{&context, Vector3(context.origin.x, context.origin.y, 0.0f), Vector3(-context.direction.x, -context.direction.y, 0.0f)};
            // every shape at the start, each kept as a hit by the callback below
            auto collect = [](b2ShapeId shapeId, void* data) -> bool {
                StartContext* start = static_cast<StartContext*>(data);
                Entity entity = NULL_ENTITY;
                if (start->cast->filter.accepts(shapeId, entity)){
                    start->cast->add(start->cast->makeHit(shapeId, entity, 0.0f, start->point, start->normal), 0.0f);
                }
                return true;
            };
            switch (shape.kind){
                case Kind::CIRCLE: b2World_OverlapCircle(world.world, &shape.circle, shape.transform, filter, collect, &start); break;
                case Kind::POLYGON: b2World_OverlapPolygon(world.world, &shape.polygon, shape.transform, filter, collect, &start); break;
                case Kind::CAPSULE: b2World_OverlapCapsule(world.world, &shape.capsule, shape.transform, filter, collect, &start); break;
            }
            if (!context.all && context.closest.hit){
                return;
            }
        }
        runCast(world, shape, translation, filter, context);
    }

    bool validCast(const char* function, const Vector2& origin, const Vector2& direction, float distance){
        const char* why = PhysicsQueries2D::invalidPoint(origin);
        if (!why) why = PhysicsQueries2D::invalidDirection(direction, distance);
        if (why){
            Log::error("%s: %s", function, why);
            return false;
        }
        return true;
    }

    bool validRadius(const char* function, float radius){
        if (const char* why = PhysicsQueries2D::invalidRadius(radius)){
            Log::error("%s: %s", function, why);
            return false;
        }
        return true;
    }

    bool validSize(const char* function, const Vector2& size){
        if (const char* why = PhysicsQueries2D::invalidSize(size)){
            Log::error("%s: %s", function, why);
            return false;
        }
        return true;
    }

    bool validPoint(const char* function, const Vector2& point){
        if (const char* why = PhysicsQueries2D::invalidPoint(point)){
            Log::error("%s: %s", function, why);
            return false;
        }
        return true;
    }

    // A world query cast: the nearest hit, or every one into `all`
    RayReturn worldCast(Scene* scene, const World2D& world, const Shape2D& shape, Vector2 origin, Vector2 direction, float distance,
                        const QueryOptions& options, std::vector<RayReturn>* all){
        if (options.layerMask == LAYER_MASK_NOTHING || (all && options.maxHits == 0)){
            return Ray::NO_HIT;
        }
        CastContext context;
        context.scene = scene;
        context.ptm = world.ptm;
        context.origin = origin;
        context.direction = direction.normalized();
        context.distance = distance;
        context.filter.set(options, NULL_ENTITY);
        context.all = all;
        context.maxHits = options.maxHits;
        castShape(scene, world, shape, options, context);
        if (all){
            std::sort(all->begin(), all->end(), nearer2D);
            return all->empty() ? Ray::NO_HIT : all->front();
        }
        return context.closest;
    }

    // Rigidbody2D.Cast: each of the body's Box2D shapes as a query shape where it is now
    RayReturn bodyCast(Scene* scene, const char* function, Entity body, Vector2 direction, float distance, const QueryOptions& options, std::vector<RayReturn>* all){
        if (all){
            all->clear();
        }
        if (const char* why = PhysicsQueries2D::invalidDirection(direction, distance)){
            Log::error("%s: %s", function, why);
            return Ray::NO_HIT;
        }
        World2D world;
        const Rigidbody2DComponent* component = scene ? scene->findComponent<Rigidbody2DComponent>(body) : nullptr;
        if (!worldOf(scene, world) || !component || !b2Body_IsValid(component->body) || options.layerMask == LAYER_MASK_NOTHING){
            return Ray::NO_HIT;
        }
        const b2Transform pose = b2Body_GetTransform(component->body);
        const int count = b2Body_GetShapeCount(component->body);
        std::vector<b2ShapeId> shapes((size_t)std::max(count, 0));
        const int found = count > 0 ? b2Body_GetShapes(component->body, shapes.data(), count) : 0;

        CastContext context;
        context.scene = scene;
        context.ptm = world.ptm;
        context.origin = Vector2(pose.p.x * world.ptm, pose.p.y * world.ptm);
        context.direction = direction.normalized();
        context.distance = distance;
        context.filter.set(options, body);
        context.all = all;
        context.maxHits = options.maxHits;
        for (int i = 0; i < found; i++){
            if (b2Shape_IsSensor(shapes[(size_t)i])){
                continue;
            }
            Shape2D shape;
            shape.transform = pose;
            switch (b2Shape_GetType(shapes[(size_t)i])){
                case b2_circleShape:
                    shape.kind = Kind::CIRCLE;
                    shape.circle = b2Shape_GetCircle(shapes[(size_t)i]);
                    break;
                case b2_polygonShape:
                    shape.kind = Kind::POLYGON;
                    shape.polygon = b2Shape_GetPolygon(shapes[(size_t)i]);
                    break;
                case b2_capsuleShape:
                    shape.kind = Kind::CAPSULE;
                    shape.capsule = b2Shape_GetCapsule(shapes[(size_t)i]);
                    break;
                case b2_segmentShape:{
                    const b2Segment segment = b2Shape_GetSegment(shapes[(size_t)i]);
                    shape.kind = Kind::CAPSULE;
                    shape.capsule = b2Capsule{segment.point1, segment.point2, 0.0f};
                    break;
                }
                case b2_chainSegmentShape:{
                    const b2Segment segment = b2Shape_GetChainSegment(shapes[(size_t)i]).segment;
                    shape.kind = Kind::CAPSULE;
                    shape.capsule = b2Capsule{segment.point1, segment.point2, 0.0f};
                    break;
                }
                default:
                    continue;
            }
            castShape(scene, world, shape, options, context);
        }
        if (all){
            std::sort(all->begin(), all->end(), nearer2D);
            return all->empty() ? Ray::NO_HIT : all->front();
        }
        return context.closest;
    }

}

using namespace jasperengine::physicsqueries2d;

const char* PhysicsQueries2D::invalidDirection(const Vector2& direction, float distance){
    const float length = direction.length();
    if (!(length > 0.0f) || !std::isfinite(length)){
        return "direction must be a non-zero finite vector";
    }
    if (!(distance > 0.0f) || !std::isfinite(distance)){
        return "distance must be a positive finite number";
    }
    return nullptr;
}

const char* PhysicsQueries2D::invalidRadius(float radius){
    return (radius > 0.0f && std::isfinite(radius)) ? nullptr : "radius must be a positive finite number";
}

const char* PhysicsQueries2D::invalidSize(const Vector2& size){
    return (finiteVector(size) && size.x > 0.0f && size.y > 0.0f) ? nullptr : "size must be positive and finite on both axes";
}

const char* PhysicsQueries2D::invalidPoint(const Vector2& point){
    return finiteVector(point) ? nullptr : "the position must be finite";
}

RayReturn PhysicsQueries2D::circleCast(Scene* scene, Vector2 origin, float radius, Vector2 direction, float distance, const QueryOptions& options){
    World2D world;
    if (!validCast("Physics2D.circleCast", origin, direction, distance) || !validRadius("Physics2D.circleCast", radius) || !worldOf(scene, world)){
        return Ray::NO_HIT;
    }
    return worldCast(scene, world, circleShape(origin, radius, world.ptm), origin, direction, distance, options, nullptr);
}

RayReturn PhysicsQueries2D::boxCast(Scene* scene, Vector2 origin, Vector2 size, float angle, Vector2 direction, float distance, const QueryOptions& options){
    World2D world;
    if (!validCast("Physics2D.boxCast", origin, direction, distance) || !validSize("Physics2D.boxCast", size) || !worldOf(scene, world)){
        return Ray::NO_HIT;
    }
    return worldCast(scene, world, boxShape(origin, size, angle, world.ptm), origin, direction, distance, options, nullptr);
}

RayReturn PhysicsQueries2D::capsuleCast(Scene* scene, Vector2 origin, Vector2 size, CapsuleDirection2D capsuleDirection, float angle, Vector2 direction, float distance, const QueryOptions& options){
    World2D world;
    if (!validCast("Physics2D.capsuleCast", origin, direction, distance) || !validSize("Physics2D.capsuleCast", size) || !worldOf(scene, world)){
        return Ray::NO_HIT;
    }
    return worldCast(scene, world, capsuleShape(origin, size, capsuleDirection, angle, world.ptm), origin, direction, distance, options, nullptr);
}

size_t PhysicsQueries2D::circleCastAll(Scene* scene, Vector2 origin, float radius, Vector2 direction, float distance, std::vector<RayReturn>& hits, const QueryOptions& options){
    hits.clear();
    World2D world;
    if (!validCast("Physics2D.circleCastAll", origin, direction, distance) || !validRadius("Physics2D.circleCastAll", radius) || !worldOf(scene, world)){
        return 0;
    }
    worldCast(scene, world, circleShape(origin, radius, world.ptm), origin, direction, distance, options, &hits);
    return hits.size();
}

size_t PhysicsQueries2D::boxCastAll(Scene* scene, Vector2 origin, Vector2 size, float angle, Vector2 direction, float distance, std::vector<RayReturn>& hits, const QueryOptions& options){
    hits.clear();
    World2D world;
    if (!validCast("Physics2D.boxCastAll", origin, direction, distance) || !validSize("Physics2D.boxCastAll", size) || !worldOf(scene, world)){
        return 0;
    }
    worldCast(scene, world, boxShape(origin, size, angle, world.ptm), origin, direction, distance, options, &hits);
    return hits.size();
}

size_t PhysicsQueries2D::capsuleCastAll(Scene* scene, Vector2 origin, Vector2 size, CapsuleDirection2D capsuleDirection, float angle, Vector2 direction, float distance, std::vector<RayReturn>& hits, const QueryOptions& options){
    hits.clear();
    World2D world;
    if (!validCast("Physics2D.capsuleCastAll", origin, direction, distance) || !validSize("Physics2D.capsuleCastAll", size) || !worldOf(scene, world)){
        return 0;
    }
    worldCast(scene, world, capsuleShape(origin, size, capsuleDirection, angle, world.ptm), origin, direction, distance, options, &hits);
    return hits.size();
}

size_t PhysicsQueries2D::overlapCircleAll(Scene* scene, Vector2 point, float radius, std::vector<ColliderRef2D>& colliders, const QueryOptions& options){
    colliders.clear();
    World2D world;
    if (!validPoint("Physics2D.overlapCircleAll", point) || !validRadius("Physics2D.overlapCircleAll", radius) || !worldOf(scene, world)){
        return 0;
    }
    return overlap(scene, world, circleShape(point, radius, world.ptm), options, &colliders, nullptr);
}

size_t PhysicsQueries2D::overlapBoxAll(Scene* scene, Vector2 point, Vector2 size, float angle, std::vector<ColliderRef2D>& colliders, const QueryOptions& options){
    colliders.clear();
    World2D world;
    if (!validPoint("Physics2D.overlapBoxAll", point) || !validSize("Physics2D.overlapBoxAll", size) || !worldOf(scene, world)){
        return 0;
    }
    return overlap(scene, world, boxShape(point, size, angle, world.ptm), options, &colliders, nullptr);
}

size_t PhysicsQueries2D::overlapCapsuleAll(Scene* scene, Vector2 point, Vector2 size, CapsuleDirection2D capsuleDirection, float angle, std::vector<ColliderRef2D>& colliders, const QueryOptions& options){
    colliders.clear();
    World2D world;
    if (!validPoint("Physics2D.overlapCapsuleAll", point) || !validSize("Physics2D.overlapCapsuleAll", size) || !worldOf(scene, world)){
        return 0;
    }
    return overlap(scene, world, capsuleShape(point, size, capsuleDirection, angle, world.ptm), options, &colliders, nullptr);
}

size_t PhysicsQueries2D::overlapPointAll(Scene* scene, Vector2 point, std::vector<ColliderRef2D>& colliders, const QueryOptions& options){
    colliders.clear();
    World2D world;
    if (!validPoint("Physics2D.overlapPointAll", point) || !worldOf(scene, world)){
        return 0;
    }
    // a circle of radius 0 is Box2D's own point overlap (b2World_OverlapPoint)
    return overlap(scene, world, circleShape(point, 0.0f, world.ptm), options, &colliders, nullptr);
}

size_t PhysicsQueries2D::overlapAreaAll(Scene* scene, Vector2 pointA, Vector2 pointB, std::vector<ColliderRef2D>& colliders, const QueryOptions& options){
    colliders.clear();
    World2D world;
    if (!validPoint("Physics2D.overlapAreaAll", pointA) || !validPoint("Physics2D.overlapAreaAll", pointB) || !worldOf(scene, world)){
        return 0;
    }
    const Vector2 size(std::fabs(pointB.x - pointA.x), std::fabs(pointB.y - pointA.y));
    if (!validSize("Physics2D.overlapAreaAll", size)){
        return 0;
    }
    return overlap(scene, world, boxShape((pointA + pointB) * 0.5f, size, 0.0f, world.ptm), options, &colliders, nullptr);
}

bool PhysicsQueries2D::overlapCircle(Scene* scene, Vector2 point, float radius, ColliderRef2D& collider, const QueryOptions& options){
    collider = ColliderRef2D();
    World2D world;
    if (!validPoint("Physics2D.overlapCircle", point) || !validRadius("Physics2D.overlapCircle", radius) || !worldOf(scene, world)){
        return false;
    }
    return overlap(scene, world, circleShape(point, radius, world.ptm), options, nullptr, &collider) > 0;
}

bool PhysicsQueries2D::overlapBox(Scene* scene, Vector2 point, Vector2 size, float angle, ColliderRef2D& collider, const QueryOptions& options){
    collider = ColliderRef2D();
    World2D world;
    if (!validPoint("Physics2D.overlapBox", point) || !validSize("Physics2D.overlapBox", size) || !worldOf(scene, world)){
        return false;
    }
    return overlap(scene, world, boxShape(point, size, angle, world.ptm), options, nullptr, &collider) > 0;
}

bool PhysicsQueries2D::overlapCapsule(Scene* scene, Vector2 point, Vector2 size, CapsuleDirection2D capsuleDirection, float angle, ColliderRef2D& collider, const QueryOptions& options){
    collider = ColliderRef2D();
    World2D world;
    if (!validPoint("Physics2D.overlapCapsule", point) || !validSize("Physics2D.overlapCapsule", size) || !worldOf(scene, world)){
        return false;
    }
    return overlap(scene, world, capsuleShape(point, size, capsuleDirection, angle, world.ptm), options, nullptr, &collider) > 0;
}

bool PhysicsQueries2D::overlapPoint(Scene* scene, Vector2 point, ColliderRef2D& collider, const QueryOptions& options){
    collider = ColliderRef2D();
    World2D world;
    if (!validPoint("Physics2D.overlapPoint", point) || !worldOf(scene, world)){
        return false;
    }
    return overlap(scene, world, circleShape(point, 0.0f, world.ptm), options, nullptr, &collider) > 0;
}

bool PhysicsQueries2D::overlapArea(Scene* scene, Vector2 pointA, Vector2 pointB, ColliderRef2D& collider, const QueryOptions& options){
    collider = ColliderRef2D();
    World2D world;
    if (!validPoint("Physics2D.overlapArea", pointA) || !validPoint("Physics2D.overlapArea", pointB) || !worldOf(scene, world)){
        return false;
    }
    const Vector2 size(std::fabs(pointB.x - pointA.x), std::fabs(pointB.y - pointA.y));
    if (!validSize("Physics2D.overlapArea", size)){
        return false;
    }
    return overlap(scene, world, boxShape((pointA + pointB) * 0.5f, size, 0.0f, world.ptm), options, nullptr, &collider) > 0;
}

RayReturn PhysicsQueries2D::cast(Scene* scene, Entity body, Vector2 direction, float distance, const QueryOptions& options){
    return bodyCast(scene, "Rigidbody2D.cast", body, direction, distance, options, nullptr);
}

size_t PhysicsQueries2D::castAll(Scene* scene, Entity body, Vector2 direction, float distance, std::vector<RayReturn>& hits, const QueryOptions& options){
    bodyCast(scene, "Rigidbody2D.castAll", body, direction, distance, options, &hits);
    return hits.size();
}

#endif // JASPERENGINE_PHYSICS_2D
