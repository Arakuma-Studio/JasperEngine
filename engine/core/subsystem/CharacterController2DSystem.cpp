// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CharacterController2DSystem.h"

#ifdef JASPERENGINE_PHYSICS_2D

#include "Engine.h"
#include "Input.h"
#include "Log.h"
#include "Scene.h"
#include "component/Rigidbody2DComponent.h"
#include "component/Transform.h"
#include "registry/PhysicsLayers.h"
#include "subsystem/PhysicsSystem.h"
#include "util/Box2DAux.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine;

namespace {

    constexpr int MAX_ITERATIONS = 4;
    constexpr int SOLVER_ITERATIONS = 20;
    // metres: Box2D's linear slop, a cast stops this far inside what it hits
    constexpr float LINEAR_SLOP = 0.005f;
    // metres: how far past the skin the planes are gathered (Box2D's speculative distance)
    constexpr float PROBE_MARGIN = 4.0f * LINEAR_SLOP;
    // metres: the least a step forward goes, as Jolt's mWalkStairsMinStepForward
    constexpr float MIN_STEP_FORWARD = 0.02f;
    // metres: a move shorter than this ends the iterations
    constexpr float MOVE_TOLERANCE = 1.0e-4f;
    // metres: the shortest capsule segment (Box2D needs one), and the solver's convergence
    constexpr float MIN_HALF_SEGMENT = 0.001f;
    constexpr float SOLVER_TOLERANCE = 1.0e-6f;

    float dot(b2Vec2 a, b2Vec2 b){ return a.x * b.x + a.y * b.y; }
    b2Vec2 add(b2Vec2 a, b2Vec2 b){ return {a.x + b.x, a.y + b.y}; }
    b2Vec2 sub(b2Vec2 a, b2Vec2 b){ return {a.x - b.x, a.y - b.y}; }
    b2Vec2 mul(float s, b2Vec2 a){ return {s * a.x, s * a.y}; }
    float length(b2Vec2 a){ return std::sqrt(a.x * a.x + a.y * a.y); }

    bool finite(const Vector2& v){
        return std::isfinite(v.x) && std::isfinite(v.y);
    }

    Entity entityOf(b2BodyId body){
        return static_cast<Entity>(reinterpret_cast<uintptr_t>(b2Body_GetUserData(body)));
    }

    // what a controller's queries see: not sensors, not its own bodies, its mask on the collider bits
    bool accepts(b2ShapeId shape, Entity self, uint16_t maskBits){
        if (b2Shape_IsSensor(shape))
            return false;
        if ((b2Shape_GetFilter(shape).categoryBits & (uint64_t)maskBits & Box2DLayers::USER_BITS) == 0)
            return false;
        return entityOf(b2Shape_GetBody(shape)) != self;
    }

    struct OverlapContext{
        std::vector<b2ShapeId>* shapes;
        Entity self;
        uint16_t maskBits;
    };

    bool overlapCallback(b2ShapeId shape, void* context){
        OverlapContext* ctx = static_cast<OverlapContext*>(context);
        if (accepts(shape, ctx->self, ctx->maskBits))
            ctx->shapes->push_back(shape);
        return true;
    }

    struct CastContext{
        Entity self;
        uint16_t maskBits;
        bool hit = false;
        float fraction = 1.0f;
        b2ShapeId shape = b2_nullShapeId;
        b2Vec2 point = {0.0f, 0.0f};
        b2Vec2 normal = {0.0f, 0.0f};
    };

    float castCallback(b2ShapeId shape, b2Vec2 point, b2Vec2 normal, float fraction, void* context){
        CastContext* ctx = static_cast<CastContext*>(context);
        if (!accepts(shape, ctx->self, ctx->maskBits))
            return -1.0f;
        if (!ctx->hit || fraction < ctx->fraction){
            ctx->hit = true;
            ctx->fraction = fraction;
            ctx->shape = shape;
            ctx->point = point;
            ctx->normal = normal;
        }
        return fraction;
    }

    // Box2D 3.1's b2SolvePlanes: the displacement nearest `delta` that keeps the capsule on the
    // outer side of every plane (separation + normal . delta >= 0), Gauss-Seidel on the
    // accumulated pushes
    b2Vec2 solvePlanes(b2Vec2 delta, std::vector<CharacterController2DSystem::Plane>& planes){
        for (CharacterController2DSystem::Plane& plane : planes)
            plane.push = 0.0f;
        for (int iteration = 0; iteration < SOLVER_ITERATIONS; iteration++){
            float total = 0.0f;
            for (CharacterController2DSystem::Plane& plane : planes){
                const float separation = plane.separation + dot(plane.normal, delta);
                const float accumulated = plane.push;
                plane.push = std::max(0.0f, accumulated - separation);
                const float push = plane.push - accumulated;
                delta = add(delta, mul(push, plane.normal));
                total += std::fabs(push);
            }
            if (total < SOLVER_TOLERANCE)
                break;
        }
        return delta;
    }

    // a point's velocity on a body
    b2Vec2 pointVelocity(b2BodyId body, b2Vec2 point){
        const b2Vec2 v = b2Body_GetLinearVelocity(body);
        const float w = b2Body_GetAngularVelocity(body);
        const b2Vec2 r = sub(point, b2Body_GetWorldCenterOfMass(body));
        return {v.x - w * r.y, v.y + w * r.x};
    }

}

CharacterController2DSystem::CharacterController2DSystem(Scene* scene, PhysicsSystem* physics): scene(scene), physics(physics){
}

bool CharacterController2DSystem::makeCapsule(Entity entity, const CharacterController2DComponent& controller, Capsule& capsule){
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform)
        return false;
    physics->resolveTransformWorldPose(entity);
    const float s = physics->pointsToMeterScale2D;
    const Vector2 scale(std::fabs(transform->worldScale.x), std::fabs(transform->worldScale.y));
    const float radius = std::max(0.0f, controller.radius) * scale.x / s;
    const float height = std::max(0.0f, controller.height) * scale.y / s;
    if (!(radius > 0.0f) || !std::isfinite(radius) || !std::isfinite(height))
        return false;
    const Vector2 center = Vector2(transform->worldPosition.x, transform->worldPosition.y) + controller.center * scale;
    capsule.center = {center.x / s, center.y / s};
    capsule.radius = radius;
    capsule.halfSegment = std::max(MIN_HALF_SEGMENT, height * 0.5f - radius);
    capsule.skin = std::max(LINEAR_SLOP, controller.skinWidth / s);
    const float slope = std::clamp(controller.slopeLimit, 0.0f, 90.0f) * 0.017453292519943295f;
    capsule.cosSlope = std::cos(slope);
    capsule.stepOffset = std::max(0.0f, controller.stepOffset) / s;
    capsule.minMoveDistance = std::max(0.0f, controller.minMoveDistance) / s;
    capsule.layer = scene->getEntityLayer(entity);
    capsule.filter = Box2DLayers::makeQueryFilter(controller.categoryBits, Physics2D::getLayerCollisionMatrix().getMask(capsule.layer));
    capsule.maskBits = controller.maskBits;
    capsule.entity = entity;
    return true;
}

void CharacterController2DSystem::collectPlanes(const Capsule& capsule, b2Vec2 center, std::vector<Plane>& planes){
    planes.clear();
    const b2WorldId world = physics->world2D;
    if (!b2World_IsValid(world))
        return;

    const b2Transform xf = {center, b2Rot_identity};
    b2Capsule probe = {{0.0f, -capsule.halfSegment}, {0.0f, capsule.halfSegment}, capsule.radius + capsule.skin + PROBE_MARGIN};
    shapeScratch.clear();
    OverlapContext context = {&shapeScratch, capsule.entity, capsule.maskBits};
    b2World_OverlapCapsule(world, &probe, xf, capsule.filter, overlapCallback, &context);

    // the skin-inflated capsule: a separation of 0 is the skin's width off the surface
    const b2Capsule skin = {{0.0f, -capsule.halfSegment}, {0.0f, capsule.halfSegment}, capsule.radius + capsule.skin};
    for (b2ShapeId shape : shapeScratch){
        const b2BodyId body = b2Shape_GetBody(shape);
        const b2Transform bodyXf = b2Body_GetTransform(body);
        b2Manifold manifold = {};
        bool towardsCapsule = true;
        switch (b2Shape_GetType(shape)){
            case b2_circleShape:{
                const b2Circle circle = b2Shape_GetCircle(shape);
                manifold = b2CollideCapsuleAndCircle(&skin, xf, &circle, bodyXf);
                towardsCapsule = false;
                break;
            }
            case b2_capsuleShape:{
                const b2Capsule other = b2Shape_GetCapsule(shape);
                manifold = b2CollideCapsules(&other, bodyXf, &skin, xf);
                break;
            }
            case b2_segmentShape:{
                const b2Segment segment = b2Shape_GetSegment(shape);
                manifold = b2CollideSegmentAndCapsule(&segment, bodyXf, &skin, xf);
                break;
            }
            case b2_polygonShape:{
                const b2Polygon polygon = b2Shape_GetPolygon(shape);
                manifold = b2CollidePolygonAndCapsule(&polygon, bodyXf, &skin, xf);
                break;
            }
            case b2_chainSegmentShape:{
                const b2ChainSegment segment = b2Shape_GetChainSegment(shape);
                b2SimplexCache cache = {};
                manifold = b2CollideChainSegmentAndCapsule(&segment, bodyXf, &skin, xf, &cache);
                break;
            }
            default:
                break;
        }
        if (manifold.pointCount <= 0)
            continue;
        Plane plane;
        plane.normal = towardsCapsule ? manifold.normal : b2Vec2{-manifold.normal.x, -manifold.normal.y};
        plane.separation = manifold.points[0].separation;
        plane.point = manifold.points[0].point;
        for (int i = 1; i < manifold.pointCount; i++){
            if (manifold.points[i].separation < plane.separation){
                plane.separation = manifold.points[i].separation;
                plane.point = manifold.points[i].point;
            }
        }
        plane.shape = shape;
        plane.body = body;
        plane.push = 0.0f;
        planes.push_back(plane);
    }
}

float CharacterController2DSystem::cast(const Capsule& capsule, b2Vec2 center, b2Vec2 translation, Plane* hit){
    const b2WorldId world = physics->world2D;
    if (!b2World_IsValid(world) || (translation.x == 0.0f && translation.y == 0.0f))
        return 1.0f;
    const b2Capsule skin = {{0.0f, -capsule.halfSegment}, {0.0f, capsule.halfSegment}, capsule.radius + capsule.skin};
    CastContext context;
    context.self = capsule.entity;
    context.maskBits = capsule.maskBits;
    b2World_CastCapsule(world, &skin, {center, b2Rot_identity}, translation, capsule.filter, castCallback, &context);
    if (!context.hit)
        return 1.0f;
    if (hit){
        hit->normal = context.normal;
        hit->separation = 0.0f;
        hit->point = context.point;
        hit->shape = context.shape;
        hit->body = b2Shape_GetBody(context.shape);
        hit->push = 1.0f;
    }
    return std::clamp(context.fraction, 0.0f, 1.0f);
}

b2Vec2 CharacterController2DSystem::slide(const Capsule& capsule, b2Vec2 center, b2Vec2 delta, std::vector<Plane>* touched){
    const b2Vec2 target = add(center, delta);
    b2Vec2 position = center;
    for (int iteration = 0; iteration < MAX_ITERATIONS; iteration++){
        collectPlanes(capsule, position, planeScratch);
        // a surface too steep to walk on is a wall as well: it is not climbed sideways
        const size_t surfaces = planeScratch.size();
        for (size_t i = 0; i < surfaces; i++){
            const Plane plane = planeScratch[i];
            if (plane.normal.y > 0.0f && plane.normal.y < capsule.cosSlope && plane.normal.x != 0.0f){
                Plane wall = plane;
                wall.normal = {plane.normal.x > 0.0f ? 1.0f : -1.0f, 0.0f};
                planeScratch.push_back(wall);
            }
        }
        const b2Vec2 wanted = solvePlanes(sub(target, position), planeScratch);
        if (touched){
            for (const Plane& plane : planeScratch){
                if (plane.push > 0.0f)
                    touched->push_back(plane);
            }
        }
        Plane hit;
        const float fraction = cast(capsule, position, wanted, &hit);
        const b2Vec2 moved = mul(fraction, wanted);
        position = add(position, moved);
        if (fraction < 1.0f && touched)
            touched->push_back(hit);
        if (length(moved) < MOVE_TOLERANCE)
            break;
    }
    return position;
}

bool CharacterController2DSystem::tryStep(const Capsule& capsule, b2Vec2& center, float horizontal, float blockingTop){
    if (capsule.stepOffset <= 0.0f || horizontal == 0.0f)
        return false;
    // what stopped it is no higher than a step over the ground it stands on
    const float ground = center.y - capsule.halfSegment - capsule.radius - capsule.skin;
    if (blockingTop > ground + capsule.stepOffset + 0.5f * capsule.skin + LINEAR_SLOP)
        return false;
    const float direction = horizontal > 0.0f ? 1.0f : -1.0f;

    // up, as far as the step or a ceiling lets it
    const float up = cast(capsule, center, {0.0f, capsule.stepOffset}, nullptr) * capsule.stepOffset;
    if (up <= LINEAR_SLOP)
        return false;
    const b2Vec2 raised = {center.x, center.y + up};

    // forward (a cast: not sliding up what it meets, which would climb more than the step), then
    // down onto what is there
    auto land = [&](float forward, b2Vec2& landing, bool& walkable){
        const b2Vec2 step = {direction * forward, 0.0f};
        const b2Vec2 ahead = add(raised, mul(cast(capsule, raised, step, nullptr), step));
        if (std::fabs(ahead.x - raised.x) < MOVE_TOLERANCE)
            return false;
        Plane floor;
        const float fraction = cast(capsule, ahead, {0.0f, -up}, &floor);
        if (fraction >= 1.0f)
            return false;
        landing = {ahead.x, ahead.y - fraction * up};
        walkable = floor.normal.y >= capsule.cosSlope;
        return true;
    };

    // Over what stopped it: a riser taller than stepOffset is not climbed by perching the round
    // bottom on its edge (a walkable contact normal once raised). Contact points lie on the
    // surfaces, as does the bottom of the skin-inflated capsule.
    auto clears = [&](const b2Vec2& landing){
        const float bottom = landing.y - capsule.halfSegment - capsule.radius - capsule.skin;
        return landing.y > center.y + LINEAR_SLOP && blockingTop <= bottom + 0.5f * capsule.skin + LINEAR_SLOP;
    };

    b2Vec2 landing;
    bool walkable = false;
    const float forward = std::max(std::fabs(horizontal), MIN_STEP_FORWARD);
    if (!land(forward, landing, walkable) || !walkable || !clears(landing)){
        // on the step's edge: far enough forward to stand on its top, as Jolt's
        // mWalkStairsStepForwardTest (the capsule's radius past where it stopped)
        const float clear = std::max(forward, capsule.radius + capsule.skin);
        if (!land(clear, landing, walkable) || !walkable || !clears(landing))
            return false;
    }
    // out of what the casts that started touching did not see
    center = slide(capsule, landing, {0.0f, 0.0f}, nullptr);
    return true;
}

bool CharacterController2DSystem::supported(const Capsule& capsule, b2Vec2 center){
    collectPlanes(capsule, center, planeScratch);
    const float tolerance = 0.5f * capsule.skin + LINEAR_SLOP;
    for (const Plane& plane : planeScratch){
        if (plane.separation <= tolerance && plane.normal.y >= capsule.cosSlope)
            return true;
    }
    return false;
}

bool CharacterController2DSystem::stickToFloor(const Capsule& capsule, b2Vec2& center){
    if (capsule.stepOffset <= 0.0f)
        return false;
    // Down a step's height onto walkable ground. An edge too steep to stand on (the round bottom
    // past a stair's nosing) is slid down, away from it, to what is under it. A cast that starts
    // touching a surface does not see it, so the ground is looked for with the planes as well.
    b2Vec2 position = center;
    float remaining = capsule.stepOffset;
    for (int iteration = 0; iteration < MAX_ITERATIONS && remaining > LINEAR_SLOP; iteration++){
        Plane hit;
        const float fraction = cast(capsule, position, {0.0f, -remaining}, &hit);
        if (fraction >= 1.0f)
            break;
        position.y -= fraction * remaining;
        remaining -= fraction * remaining;
        if (hit.normal.y >= capsule.cosSlope || supported(capsule, position)){
            // out of what a cast that started touching did not see
            center = slide(capsule, position, {0.0f, 0.0f}, nullptr);
            return true;
        }
        if (hit.normal.y <= 0.0f)
            return false;
        // along the steep surface, downwards
        b2Vec2 tangent = {hit.normal.y, -hit.normal.x};
        if (tangent.y > 0.0f)
            tangent = {-tangent.x, -tangent.y};
        if (tangent.y >= -1.0e-4f)
            return false;
        const b2Vec2 along = mul(remaining / -tangent.y, tangent);
        const float slid = cast(capsule, position, along, nullptr);
        if (slid <= 0.0f)
            return false;
        position = add(position, mul(slid, along));
        remaining -= slid * remaining;
        if (supported(capsule, position)){
            center = slide(capsule, position, {0.0f, 0.0f}, nullptr);
            return true;
        }
    }
    return false;
}

void CharacterController2DSystem::readGround(const Capsule& capsule, b2Vec2 center, CharacterController2DComponent& controller, Runtime& runtime){
    collectPlanes(capsule, center, planeScratch);
    const float tolerance = 0.5f * capsule.skin + LINEAR_SLOP;
    const Plane* ground = nullptr;
    bool steep = false;
    for (const Plane& plane : planeScratch){
        if (plane.separation > tolerance || plane.normal.y <= 0.0f)
            continue;
        if (plane.normal.y >= capsule.cosSlope){
            if (!ground || plane.normal.y > ground->normal.y)
                ground = &plane;
        }else{
            steep = true;
        }
    }
    const float s = physics->pointsToMeterScale2D;
    controller.isGrounded = ground != nullptr;
    controller.isOnSteepSlope = !ground && steep;
    if (ground){
        controller.groundNormal = Vector2(ground->normal.x, ground->normal.y);
        controller.groundEntity = entityOf(ground->body);
        const b2Vec2 v = b2Body_GetType(ground->body) == b2_staticBody ? b2Vec2{0.0f, 0.0f} : pointVelocity(ground->body, ground->point);
        controller.groundVelocity = Vector2(v.x * s, v.y * s);
        runtime.ground = ground->body;
        runtime.groundPoint = ground->point;
    }else{
        controller.groundNormal = Vector2(0.0f, 0.0f);
        controller.groundEntity = NULL_ENTITY;
        controller.groundVelocity = Vector2(0.0f, 0.0f);
        runtime.ground = b2_nullBodyId;
    }
}

int CharacterController2DSystem::classify(const Capsule& capsule, b2Vec2 center, const std::vector<Plane>& touched) const{
    // a contact on the bottom half-circle has a normal pointing up at its centre, one on the
    // top half-circle down, one on the straight sides sideways (Unity's below / above / sides)
    constexpr float SIDE = 1.0e-3f;
    int flags = CharacterCollision_None;
    for (const Plane& plane : touched){
        if (plane.normal.y > SIDE) flags |= CharacterCollision_Below;
        else if (plane.normal.y < -SIDE) flags |= CharacterCollision_Above;
        else flags |= CharacterCollision_Sides;
    }
    (void)capsule;
    (void)center;
    return flags;
}

void CharacterController2DSystem::push(const Capsule& capsule, const CharacterController2DComponent& controller, b2Vec2 direction, float dt,
                                       const std::vector<Plane>& touched){
    // the impulse pushStrength x dt along the move, once per body it went into, not what it stands on
    pushedScratch.clear();
    const float impulse = std::max(0.0f, controller.pushStrength) * dt;
    if (impulse <= 0.0f)
        return;
    for (const Plane& plane : touched){
        if (!b2Body_IsValid(plane.body) || b2Body_GetType(plane.body) != b2_dynamicBody)
            continue;
        if (plane.normal.y >= capsule.cosSlope || dot(direction, plane.normal) >= 0.0f)
            continue;
        bool done = false;
        for (b2BodyId body : pushedScratch)
            done = done || B2_ID_EQUALS(body, plane.body);
        if (done)
            continue;
        pushedScratch.push_back(plane.body);
        b2Body_ApplyLinearImpulse(plane.body, mul(impulse, direction), plane.point, true);
    }
}

void CharacterController2DSystem::writeTransform(Entity entity, const CharacterController2DComponent& controller, b2Vec2 center){
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform)
        return;
    const float s = physics->pointsToMeterScale2D;
    const Vector2 scale(std::fabs(transform->worldScale.x), std::fabs(transform->worldScale.y));
    const Vector2 position = Vector2(center.x * s, center.y * s) - controller.center * scale;
    physics->setTransformWorldPose(entity, Vector3(position.x, position.y, transform->worldPosition.z), transform->worldRotation);
}

bool CharacterController2DSystem::refused(Entity entity, CharacterController2DComponent& controller){
    // a Rigidbody 2D on the same GameObject would fight it (docs/mcp/guide/physics.md)
    if (!scene->findComponent<Rigidbody2DComponent>(entity)){
        controller.refusedWithRigidbody = false;
        return false;
    }
    if (!controller.refusedWithRigidbody){
        controller.refusedWithRigidbody = true;
        Log::error("CharacterController2D of entity %u is ignored: its GameObject has a Rigidbody 2D (remove one of them)", entity);
    }
    return true;
}

bool CharacterController2DSystem::usable(Entity entity, CharacterController2DComponent& controller){
    if (!controller.enabled || !scene->isEntityActiveInHierarchy(entity))
        return false;
    return !refused(entity, controller);
}

float CharacterController2DSystem::moveDelta() const{
    // the fixed step inside the fixed phase, the frame's delta otherwise
    const float dt = Input::isFixedPhase() ? Engine::getUpdateTime() : Engine::getDeltatime();
    return dt > 0.0f ? dt : 1.0f / 60.0f;
}

int CharacterController2DSystem::moveBy(Entity entity, CharacterController2DComponent& controller, Runtime& runtime, b2Vec2 motion, float dt, bool scripted){
    Capsule capsule;
    if (!makeCapsule(entity, controller, capsule))
        return CharacterCollision_None;
    const bool wasSupported = controller.isGrounded || controller.isOnSteepSlope;
    const b2Vec2 start = capsule.center;

    touchedScratch.clear();
    b2Vec2 position = slide(capsule, start, motion, scripted ? &touchedScratch : nullptr);

    if (scripted && capsule.stepOffset > 0.0f && wasSupported && motion.x != 0.0f){
        // a wall stopped it on the ground: a step it can climb?
        bool blocked = false;
        float blockingTop = -std::numeric_limits<float>::max();
        for (const Plane& plane : touchedScratch){
            if (plane.normal.y < capsule.cosSlope && plane.normal.x * motion.x < 0.0f){
                blocked = true;
                blockingTop = std::max(blockingTop, plane.point.y);
            }
        }
        const float remaining = motion.x - (position.x - start.x);
        if (blocked && remaining * motion.x > 0.0f && std::fabs(remaining) > MOVE_TOLERANCE){
            b2Vec2 stepped = position;
            if (tryStep(capsule, stepped, remaining, blockingTop))
                position = stepped;
        }
    }

    readGround(capsule, position, controller, runtime);
    if (scripted && wasSupported && !controller.isGrounded && motion.y <= 0.0f && stickToFloor(capsule, position))
        readGround(capsule, position, controller, runtime);

    const float s = physics->pointsToMeterScale2D;
    if (scripted){
        int flags = classify(capsule, position, touchedScratch);
        if (controller.isGrounded)
            flags |= CharacterCollision_Below;
        controller.collisionFlags = flags;
        controller.velocity = Vector2((position.x - start.x) * s / dt, (position.y - start.y) * s / dt);
        const float distance = length(motion);
        if (controller.pushBodies && distance > 0.0f)
            push(capsule, controller, mul(1.0f / distance, motion), dt, touchedScratch);
    }
    writeTransform(entity, controller, position);
    return controller.collisionFlags;
}

int CharacterController2DSystem::move(Entity entity, const Vector2& motion){
    CharacterController2DComponent* controller = scene->findComponent<CharacterController2DComponent>(entity);
    if (!controller)
        return CharacterCollision_None;
    if (physics->steppingWorld2D){
        Log::error("CharacterController2D.move of entity %u from a physics callback is ignored (move in onUpdate or onFixedUpdate)", entity);
        return controller->collisionFlags;
    }
    if (!usable(entity, *controller))
        return CharacterCollision_None;
    if (!finite(motion)){
        Log::warn("CharacterController2D.move of entity %u: the motion is not finite, ignored", entity);
        return controller->collisionFlags;
    }
    const float s = physics->pointsToMeterScale2D;
    if (motion.length() < controller->minMoveDistance){
        controller->velocity = Vector2(0.0f, 0.0f);
        return controller->collisionFlags;
    }
    Runtime& runtime = runtimes[entity];
    return moveBy(entity, *controller, runtime, {motion.x / s, motion.y / s}, moveDelta(), true);
}

bool CharacterController2DSystem::simpleMove(Entity entity, const Vector2& speed){
    CharacterController2DComponent* controller = scene->findComponent<CharacterController2DComponent>(entity);
    if (!controller)
        return false;
    if (physics->steppingWorld2D){
        Log::error("CharacterController2D.simpleMove of entity %u from a physics callback is ignored (move in onUpdate or onFixedUpdate)", entity);
        return controller->isGrounded;
    }
    if (!usable(entity, *controller) || !finite(speed))
        return controller->isGrounded;
    const float dt = moveDelta();
    const float s = physics->pointsToMeterScale2D;
    Runtime& runtime = runtimes[entity];
    // its own gravity, from rest on the ground; speed.y is ignored (Unity)
    if (controller->isGrounded)
        runtime.verticalSpeed = 0.0f;
    runtime.verticalSpeed += physics->getGravity2D().y * dt;
    const int flags = move(entity, Vector2(speed.x * dt, runtime.verticalSpeed * s * dt));
    // a script callback may have removed it
    controller = scene->findComponent<CharacterController2DComponent>(entity);
    auto it = runtimes.find(entity);
    if (!controller || it == runtimes.end())
        return false;
    if (controller->isGrounded || ((flags & CharacterCollision_Above) && it->second.verticalSpeed > 0.0f))
        it->second.verticalSpeed = 0.0f;
    return controller->isGrounded;
}

void CharacterController2DSystem::postStep(float dt){
    auto controllers = scene->getComponentArray<CharacterController2DComponent>();
    if (controllers->size() == 0)
        return;
    const float s = physics->pointsToMeterScale2D;
    for (size_t i = 0; i < controllers->size(); i++){
        CharacterController2DComponent& controller = controllers->getComponentFromIndex(i);
        const Entity entity = controllers->getEntity(i);
        if (!usable(entity, controller)){
            controller.isGrounded = false;
            controller.isOnSteepSlope = false;
            continue;
        }
        Runtime& runtime = runtimes[entity];
        Capsule capsule;
        if (!makeCapsule(entity, controller, capsule))
            continue;

        // the platform it stood on moved in the step: its velocity where it stood carries it
        b2Vec2 carry = {0.0f, 0.0f};
        if (controller.movingPlatforms && controller.isGrounded && b2Body_IsValid(runtime.ground) &&
                b2Body_GetType(runtime.ground) != b2_staticBody){
            carry = mul(dt, pointVelocity(runtime.ground, runtime.groundPoint));
        }
        if (carry.x != 0.0f || carry.y != 0.0f){
            // only carries: the move's own velocity and flags are not in it
            moveBy(entity, controller, runtime, carry, dt, false);
        }else{
            // the ground under it may have gone (a platform dropping away)
            readGround(capsule, capsule.center, controller, runtime);
        }
    }
    (void)s;
}

void CharacterController2DSystem::remove(Entity entity){
    runtimes.erase(entity);
}

void CharacterController2DSystem::clear(){
    runtimes.clear();
}

#endif



