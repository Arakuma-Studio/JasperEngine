// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The contact half of PhysicsSystem (physics-contacts spec 6.1): listener masks, the per-step
// collider pairs of both worlds, the Unity-style per-object callbacks (onCollisionEnter...,
// onTriggerEnter2D...) and the global events, all on the main thread after the step that made
// them; Physics.ignoreCollision.

#include "PhysicsSystem.h"
#include "component/CharacterControllerComponent.h"
#include "Scene.h"
#include "Log.h"
#include "script/ScriptMessages.h"
#include "script/LuaBinding.h"

#include "lua.hpp"
#include "LuaBridge.h"

#ifdef JASPERENGINE_PHYSICS_2D
#include "util/Box2DAux.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "util/JoltPhysicsAux.h"
#include "Jolt/Physics/Collision/CollisionCollectorImpl.h"
#include "Jolt/Physics/Collision/CollideShape.h"
#include "Jolt/Physics/Body/BodyFilter.h"
#endif

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>

using namespace jasperengine;

#if defined(JASPERENGINE_PHYSICS_2D) || defined(JASPERENGINE_PHYSICS_3D)
namespace {

    // Enter / Stay / Exit of a collision or a trigger, 3D or 2D (ScriptMessage order)
    ScriptMessage contactMessage(bool trigger, ContactPhase phase, bool is2D){
        return static_cast<ScriptMessage>((is2D ? 6 : 0) + (trigger ? 3 : 0) + static_cast<int>(phase));
    }

    // A message to one GameObject: active in hierarchy, and a script there defines it. Every
    // script that defines it gets it, enabled or not (Unity).
    void deliver(Scene* scene, Entity target, ScriptMessage message,
                 const std::function<int(lua_State*)>& pushArgs, const std::function<void(ScriptBase&)>& callCpp){
        if (target == NULL_ENTITY || !scene->isEntityCreated(target) || !scene->isEntityActiveInHierarchy(target)){
            return;
        }
        if (!(ScriptMessages::receivers(scene, target) & scriptMessageBit(message))){
            return;
        }
        ScriptMessages::send(scene, target, message, pushArgs, callCpp, DeliverRequireActive);
    }

    template<typename T>
    std::function<int(lua_State*)> pushValue(const T& value){
        return [&value](lua_State* L){
            if (!luabridge::push(L, value)){
                lua_pushnil(L);
            }
            return 1;
        };
    }

}
#endif

void PhysicsSystem::onScriptReceiversChanged(Scene* scene, Entity entity){
    (void)entity;
    if (!scene || scene->isTearingDown()){
        return;
    }
    if (std::shared_ptr<PhysicsSystem> physics = scene->getSystem<PhysicsSystem>()){
        physics->markListenerMasksDirty();
    }
}

uint16_t PhysicsSystem::computeListenerMask(Entity entity, const std::vector<Entity>& receivers) const{
    uint32_t mask = ScriptMessages::receivers(scene, entity);
    for (Entity receiver : receivers){
        if (receiver != NULL_ENTITY && receiver != entity && scene->isEntityCreated(receiver)){
            mask |= ScriptMessages::receivers(scene, receiver);
        }
    }
    return PhysicsCallbacks::fromReceivers(mask);
}

void PhysicsSystem::refreshListenerMasks(){
    listenerMasksDirty = false;
#ifdef JASPERENGINE_PHYSICS_2D
    if (!steppingWorld2D){
        auto bodies2d = scene->getComponentArray<Rigidbody2DComponent>();
        for (size_t i = 0; i < bodies2d->size(); i++){
            refreshListenerMask2D(bodies2d->getEntity(i), bodies2d->getComponentFromIndex(i));
        }
    }else{
        listenerMasksDirty = true;
    }
#endif
#ifdef JASPERENGINE_PHYSICS_3D
    if (!steppingWorld3D){
        auto bodies3d = scene->getComponentArray<RigidbodyComponent>();
        for (size_t i = 0; i < bodies3d->size(); i++){
            refreshListenerMask3D(bodies3d->getEntity(i), bodies3d->getComponentFromIndex(i));
        }
    }else{
        listenerMasksDirty = true;
    }
#endif
}

// ============================================================================================ 2D

#ifdef JASPERENGINE_PHYSICS_2D

namespace {

    uint64_t shapeKey(b2ShapeId id){
        return ((uint64_t)(uint32_t)id.index1 << 32) | ((uint64_t)id.world0 << 16) | (uint64_t)id.revision;
    }

    struct ShapePairKey2D{
        uint64_t first = 0, second = 0;

        static ShapePairKey2D make(b2ShapeId a, b2ShapeId b){
            const uint64_t ka = shapeKey(a);
            const uint64_t kb = shapeKey(b);
            return ka < kb ? ShapePairKey2D{ka, kb} : ShapePairKey2D{kb, ka};
        }
        bool operator==(const ShapePairKey2D& other) const{ return first == other.first && second == other.second; }
        bool operator<(const ShapePairKey2D& other) const{ return first != other.first ? first < other.first : second < other.second; }
    };

    struct ShapePairHash2D{
        size_t operator()(const ShapePairKey2D& key) const{
            return std::hash<uint64_t>()(key.first * 0x9E3779B97F4A7C15ull ^ key.second);
        }
    };

    // a touching shape pair: shapeA / keyA one side, shapeB / keyB the other (a sensor pair:
    // A is the sensor)
    struct ShapeContact2D{
        b2ShapeId shapeA = b2_nullShapeId, shapeB = b2_nullShapeId;
        ColliderKey keyA, keyB;
        bool sensor = false;
        // the pair has the authored event flag (the global events see it)
        bool authored = false;
        uint32_t sinceStep = 0;
    };

    Entity bodyEntity2D(b2BodyId body){
        return (Entity)reinterpret_cast<uintptr_t>(b2Body_GetUserData(body));
    }

    ColliderKey shapeColliderKey(b2ShapeId shape){
        return ColliderKey{bodyEntity2D(b2Shape_GetBody(shape)), (int)reinterpret_cast<size_t>(b2Shape_GetUserData(shape))};
    }

    bool isChainSegment(b2ShapeId shape){
        return b2Chain_IsValid(b2Shape_GetParentChain(shape));
    }

    const Collider2D* collider2DOf(Scene* scene, const ColliderKey& key){
        if (!scene->isEntityCreated(key.entity)) return nullptr;
        const Rigidbody2DComponent* body = scene->findComponent<Rigidbody2DComponent>(key.entity);
        if (!body || key.collider < 0 || (size_t)key.collider >= body->colliders.size()) return nullptr;
        return &body->colliders[(size_t)key.collider];
    }

    uint16_t listenerMask2D(Scene* scene, Entity entity){
        if (!scene->isEntityCreated(entity)) return 0;
        const Rigidbody2DComponent* body = scene->findComponent<Rigidbody2DComponent>(entity);
        return body ? body->listenerMask : 0;
    }

    // the flags the collider asks for itself (chain segments never had events: Box2D makes
    // them without, whatever the collider says)
    bool authoredContactEvents(const Collider2D* collider, b2ShapeId shape){
        return collider && collider->contactEvents && !isChainSegment(shape);
    }

    bool authoredSensorEvents(const Collider2D& collider, b2ShapeId shape){
        return (collider.sensorEvents || collider.sensor) && !isChainSegment(shape);
    }

    Entity receiver2D(Scene* scene, const ColliderKey& key){
        const Collider2D* collider = collider2DOf(scene, key);
        if (!collider || collider->receiver == NULL_ENTITY || collider->receiver == key.entity || !scene->isEntityCreated(collider->receiver)){
            return NULL_ENTITY;
        }
        return collider->receiver;
    }

    bool bodyAwake2D(b2ShapeId shape){
        return b2Shape_IsValid(shape) && b2Body_IsAwake(b2Shape_GetBody(shape));
    }

}

namespace jasperengine {
    struct ContactState2D{
        std::unordered_map<ShapePairKey2D, ShapeContact2D, ShapePairHash2D> shapes;
        std::vector<b2ContactData> scratch;
        uint32_t step = 0;
    };
}

std::shared_ptr<ContactState2D> jasperengine::makeContactState2D(){
    return std::make_shared<ContactState2D>();
}

bool PhysicsSystem::applyShapeEventFlags2D(const Collider2D& collider, uint16_t mask, b2ShapeId shape) const{
    if (!b2Shape_IsValid(shape)){
        return false;
    }
    const bool contact = authoredContactEvents(&collider, shape) || (mask & PhysicsCallbacks::COLLISION_2D) != 0;
    const bool sensor = authoredSensorEvents(collider, shape) || (mask & PhysicsCallbacks::TRIGGER_2D) != 0;
    bool changed = false;
    if (b2Shape_AreContactEventsEnabled(shape) != contact){
        b2Shape_EnableContactEvents(shape, contact);
        changed = true;
    }
    if (b2Shape_AreSensorEventsEnabled(shape) != sensor){
        b2Shape_EnableSensorEvents(shape, sensor);
        changed = true;
    }
    return changed;
}

void PhysicsSystem::refreshListenerMask2D(Entity entity, Rigidbody2DComponent& body){
    std::vector<Entity> receivers;
    for (const Collider2D& collider : body.colliders){
        if (collider.receiver != NULL_ENTITY) receivers.push_back(collider.receiver);
    }
    body.listenerMask = computeListenerMask(entity, receivers);
    if (!b2Body_IsValid(body.body) || steppingWorld2D){
        return;
    }

    bool changed = false;
    for (const Collider2D& collider : body.colliders){
        for (const b2ShapeId& shape : collider.shapes){
            changed |= applyShapeEventFlags2D(collider, body.listenerMask, shape);
        }
        if (b2Chain_IsValid(collider.chain)){
            const int count = b2Chain_GetSegmentCount(collider.chain);
            std::vector<b2ShapeId> segments((size_t)std::max(0, count));
            const int returned = count > 0 ? b2Chain_GetSegments(collider.chain, segments.data(), count) : 0;
            for (int i = 0; i < returned; i++){
                changed |= applyShapeEventFlags2D(collider, body.listenerMask, segments[(size_t)i]);
            }
        }
    }
    // Box2D fixes a contact's flags when it is made: pair the shapes again
    if (changed){
        refilterShapes2D(body.body, true, -1);
    }
}

void PhysicsSystem::processContacts2D(){
    if (!b2World_IsValid(world2D)){
        return;
    }
    ContactState2D& state = *contacts2D;
    state.step++;

    // copies: a callback may create and destroy bodies
    const b2ContactEvents contactEvents = b2World_GetContactEvents(world2D);
    const b2SensorEvents sensorEvents = b2World_GetSensorEvents(world2D);
    const std::vector<b2ContactBeginTouchEvent> begins(contactEvents.beginEvents, contactEvents.beginEvents + contactEvents.beginCount);
    const std::vector<b2ContactEndTouchEvent> ends(contactEvents.endEvents, contactEvents.endEvents + contactEvents.endCount);
    const std::vector<b2ContactHitEvent> hits(contactEvents.hitEvents, contactEvents.hitEvents + contactEvents.hitCount);
    const std::vector<b2SensorBeginTouchEvent> sensorBegins(sensorEvents.beginEvents, sensorEvents.beginEvents + sensorEvents.beginCount);
    const std::vector<b2SensorEndTouchEvent> sensorEnds(sensorEvents.endEvents, sensorEvents.endEvents + sensorEvents.endCount);

    // 1. the touching shape pairs (end events of destroyed shapes are found by their ids)
    for (const b2ContactEndTouchEvent& event : ends){
        state.shapes.erase(ShapePairKey2D::make(event.shapeIdA, event.shapeIdB));
    }
    for (const b2SensorEndTouchEvent& event : sensorEnds){
        state.shapes.erase(ShapePairKey2D::make(event.sensorShapeId, event.visitorShapeId));
    }
    for (const b2ContactBeginTouchEvent& event : begins){
        if (!b2Shape_IsValid(event.shapeIdA) || !b2Shape_IsValid(event.shapeIdB)){
            continue;
        }
        ShapeContact2D contact;
        contact.shapeA = event.shapeIdA;
        contact.shapeB = event.shapeIdB;
        contact.keyA = shapeColliderKey(event.shapeIdA);
        contact.keyB = shapeColliderKey(event.shapeIdB);
        contact.authored = authoredContactEvents(collider2DOf(scene, contact.keyA), event.shapeIdA) ||
                           authoredContactEvents(collider2DOf(scene, contact.keyB), event.shapeIdB);
        contact.sinceStep = state.step;
        state.shapes[ShapePairKey2D::make(event.shapeIdA, event.shapeIdB)] = contact;
    }
    for (const b2SensorBeginTouchEvent& event : sensorBegins){
        if (!b2Shape_IsValid(event.sensorShapeId) || !b2Shape_IsValid(event.visitorShapeId)){
            continue;
        }
        ShapeContact2D contact;
        contact.shapeA = event.sensorShapeId;
        contact.shapeB = event.visitorShapeId;
        contact.keyA = shapeColliderKey(event.sensorShapeId);
        contact.keyB = shapeColliderKey(event.visitorShapeId);
        contact.sensor = true;
        contact.authored = true;
        contact.sinceStep = state.step;
        state.shapes[ShapePairKey2D::make(event.sensorShapeId, event.visitorShapeId)] = contact;
    }

    // 2. the global events, in their old order, for the pairs with the authored flags (D16):
    // the per-object listeners turned more events on. A shape destroyed by an earlier
    // callback is skipped.
    auto shapeIndex = [](b2ShapeId shape){ return (unsigned long)reinterpret_cast<size_t>(b2Shape_GetUserData(shape)); };
    auto shapeBody = [this](b2ShapeId shape){ return Rigidbody2D(scene, bodyEntity2D(b2Shape_GetBody(shape))); };
    auto authoredPair = [this](b2ShapeId a, b2ShapeId b){
        return authoredContactEvents(collider2DOf(scene, shapeColliderKey(a)), a) || authoredContactEvents(collider2DOf(scene, shapeColliderKey(b)), b);
    };
    if (beginContact2D.hasSubscribers()){
        for (const b2ContactBeginTouchEvent& event : begins){
            if (b2Shape_IsValid(event.shapeIdA) && b2Shape_IsValid(event.shapeIdB) && authoredPair(event.shapeIdA, event.shapeIdB)){
                beginContact2D.call(shapeBody(event.shapeIdA), shapeIndex(event.shapeIdA), shapeBody(event.shapeIdB), shapeIndex(event.shapeIdB));
            }
        }
    }
    if (endContact2D.hasSubscribers()){
        for (const b2ContactEndTouchEvent& event : ends){
            if (b2Shape_IsValid(event.shapeIdA) && b2Shape_IsValid(event.shapeIdB) && authoredPair(event.shapeIdA, event.shapeIdB)){
                endContact2D.call(shapeBody(event.shapeIdA), shapeIndex(event.shapeIdA), shapeBody(event.shapeIdB), shapeIndex(event.shapeIdB));
            }
        }
    }
    if (hitContact2D.hasSubscribers()){
        for (const b2ContactHitEvent& event : hits){
            if (b2Shape_IsValid(event.shapeIdA) && b2Shape_IsValid(event.shapeIdB)){
                hitContact2D.call(shapeBody(event.shapeIdA), shapeIndex(event.shapeIdA), shapeBody(event.shapeIdB), shapeIndex(event.shapeIdB),
                                  Vector2(event.point.x, event.point.y), Vector2(event.normal.x, event.normal.y), event.approachSpeed);
            }
        }
    }
    // sensor shapes always had their events on
    if (beginSensorContact2D.hasSubscribers()){
        for (const b2SensorBeginTouchEvent& event : sensorBegins){
            if (b2Shape_IsValid(event.sensorShapeId) && b2Shape_IsValid(event.visitorShapeId)){
                beginSensorContact2D.call(shapeBody(event.sensorShapeId), shapeIndex(event.sensorShapeId), shapeBody(event.visitorShapeId), shapeIndex(event.visitorShapeId));
            }
        }
    }
    if (endSensorContact2D.hasSubscribers()){
        for (const b2SensorEndTouchEvent& event : sensorEnds){
            if (b2Shape_IsValid(event.sensorShapeId) && b2Shape_IsValid(event.visitorShapeId)){
                endSensorContact2D.call(shapeBody(event.sensorShapeId), shapeIndex(event.sensorShapeId), shapeBody(event.visitorShapeId), shapeIndex(event.visitorShapeId));
            }
        }
    }
    // the stays (D17): touching pairs with the authored flag that began before this step,
    // while either body is awake, in shape order
    if (stayContact2D.hasSubscribers() || staySensorContact2D.hasSubscribers()){
        std::vector<std::pair<ShapePairKey2D, ShapeContact2D>> stays;
        for (const auto& [key, contact] : state.shapes){
            if (contact.authored && contact.sinceStep != state.step && b2Shape_IsValid(contact.shapeA) && b2Shape_IsValid(contact.shapeB) &&
                    (bodyAwake2D(contact.shapeA) || bodyAwake2D(contact.shapeB))){
                stays.push_back({key, contact});
            }
        }
        std::sort(stays.begin(), stays.end(), [](const auto& x, const auto& y){ return x.first < y.first; });
        for (const auto& [key, contact] : stays){
            if (!b2Shape_IsValid(contact.shapeA) || !b2Shape_IsValid(contact.shapeB)){
                continue;
            }
            FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>& event = contact.sensor ? staySensorContact2D : stayContact2D;
            event.call(shapeBody(contact.shapeA), shapeIndex(contact.shapeA), shapeBody(contact.shapeB), shapeIndex(contact.shapeB));
        }
    }

    // 3. the collider pairs a script listens to
    struct PairData2D{
        // the shapes of the pair, the a side first
        std::vector<std::pair<b2ShapeId, b2ShapeId>> shapes;
    };
    std::vector<PairData2D> data;
    ContactTracker::CurrentMap now;
    for (const auto& [key, contact] : state.shapes){
        const uint16_t masks = listenerMask2D(scene, contact.keyA.entity) | listenerMask2D(scene, contact.keyB.entity);
        // a trigger pair for trigger listeners, a solid one for collision listeners
        const uint16_t wanted = contact.sensor ? PhysicsCallbacks::TRIGGER_2D : PhysicsCallbacks::COLLISION_2D;
        if (!(masks & wanted) || !b2Shape_IsValid(contact.shapeA) || !b2Shape_IsValid(contact.shapeB)){
            continue;
        }
        const ColliderPairKey pairKey = ColliderPairKey::make(contact.keyA, contact.keyB);
        const bool aFirst = pairKey.a == contact.keyA;
        const bool asleep = !bodyAwake2D(contact.shapeA) && !bodyAwake2D(contact.shapeB);
        auto found = now.find(pairKey);
        if (found == now.end()){
            data.emplace_back();
            found = now.emplace(pairKey, ContactTracker::Current{contact.sensor, asleep, (int)data.size() - 1}).first;
        }else{
            found->second.trigger = found->second.trigger || contact.sensor;
            found->second.asleep = found->second.asleep && asleep;
        }
        data[(size_t)found->second.data].shapes.push_back(aFirst ? std::make_pair(contact.shapeA, contact.shapeB) : std::make_pair(contact.shapeB, contact.shapeA));
    }
    // shape order inside a pair does not depend on the hash map
    for (PairData2D& pair : data){
        std::sort(pair.shapes.begin(), pair.shapes.end(), [](const auto& x, const auto& y){
            return ShapePairKey2D{shapeKey(x.first), shapeKey(x.second)} < ShapePairKey2D{shapeKey(y.first), shapeKey(y.second)};
        });
    }

    const std::vector<ContactTracker::Event> events = contactTracker2D.update(now);
    contactTracker2D.setRecordedCount(now.size());

    // the contact data of a pair seen from one side
    const float scale = pointsToMeterScale2D;
    auto buildCollision = [&](const ContactTracker::Event& event, bool selfIsA) -> Collision2D{
        const ColliderKey self = selfIsA ? event.a : event.b;
        const ColliderKey other = selfIsA ? event.b : event.a;
        Collision2D collision;
        collision.thisCollider = ColliderRef2D(scene, self.entity, self.collider);
        collision.otherCollider = ColliderRef2D(scene, other.entity, other.collider);
        if (event.phase == ContactPhase::Exit || event.data < 0){
            return collision;
        }

        b2Vec2 velocitySelf = b2Vec2_zero, velocityOther = b2Vec2_zero;
        for (Entity entity : {self.entity, other.entity}){
            const Rigidbody2DComponent* body = scene->findComponent<Rigidbody2DComponent>(entity);
            b2Vec2 velocity = b2Vec2_zero;
            if (body && b2Body_IsValid(body->body)){
                velocity = (event.phase == ContactPhase::Enter) ? body->preStepLinearVelocity : b2Body_GetLinearVelocity(body->body);
            }
            (entity == self.entity ? velocitySelf : velocityOther) = velocity;
        }
        collision.relativeVelocity = Vector2((velocityOther.x - velocitySelf.x) * scale, (velocityOther.y - velocitySelf.y) * scale);

        for (const auto& [shapeOnA, shapeOnB] : data[(size_t)event.data].shapes){
            const b2ShapeId shapeSelf = selfIsA ? shapeOnA : shapeOnB;
            const b2ShapeId shapeOther = selfIsA ? shapeOnB : shapeOnA;
            if (!b2Shape_IsValid(shapeSelf) || !b2Shape_IsValid(shapeOther)){
                continue;
            }
            const int capacity = b2Shape_GetContactCapacity(shapeSelf);
            if (capacity <= 0){
                continue;
            }
            state.scratch.resize((size_t)capacity);
            const int count = b2Shape_GetContactData(shapeSelf, state.scratch.data(), capacity);
            for (int i = 0; i < count; i++){
                const b2ContactData& contactData = state.scratch[(size_t)i];
                const bool selfIsManifoldA = B2_ID_EQUALS(contactData.shapeIdA, shapeSelf) && B2_ID_EQUALS(contactData.shapeIdB, shapeOther);
                const bool selfIsManifoldB = B2_ID_EQUALS(contactData.shapeIdB, shapeSelf) && B2_ID_EQUALS(contactData.shapeIdA, shapeOther);
                if (!selfIsManifoldA && !selfIsManifoldB){
                    continue;
                }
                // Box2D's normal points from shape A to shape B: toward this side when this is B
                const b2Vec2 normal = selfIsManifoldB ? contactData.manifold.normal : b2Neg(contactData.manifold.normal);
                const b2BodyId bodySelf = b2Shape_GetBody(shapeSelf);
                const b2BodyId bodyOther = b2Shape_GetBody(shapeOther);
                for (int p = 0; p < contactData.manifold.pointCount; p++){
                    const b2ManifoldPoint& point = contactData.manifold.points[p];
                    ContactPoint2D contact;
                    contact.point = Vector2(point.point.x * scale, point.point.y * scale);
                    contact.normal = Vector2(normal.x, normal.y);
                    contact.separation = point.separation * scale;
                    contact.normalImpulse = point.normalImpulse;
                    contact.tangentImpulse = point.tangentImpulse;
                    auto pointVelocity = [&](b2BodyId bodyId){
                        const b2Vec2 linear = b2Body_GetLinearVelocity(bodyId);
                        const float angular = b2Body_GetAngularVelocity(bodyId);
                        const b2Vec2 r = b2Sub(point.point, b2Body_GetWorldCenterOfMass(bodyId));
                        return b2Add(linear, b2CrossSV(angular, r));
                    };
                    const b2Vec2 relative = b2Sub(pointVelocity(bodyOther), pointVelocity(bodySelf));
                    contact.relativeVelocity = Vector2(relative.x * scale, relative.y * scale);
                    contact.thisCollider = collision.thisCollider;
                    contact.otherCollider = collision.otherCollider;
                    collision.contacts.push_back(contact);
                }
            }
        }
        // the deepest 8 of a collider made of several shapes
        if (collision.contacts.size() > 8){
            std::stable_sort(collision.contacts.begin(), collision.contacts.end(), [](const ContactPoint2D& x, const ContactPoint2D& y){ return x.separation < y.separation; });
            collision.contacts.resize(8);
        }
        return collision;
    };

    for (const ContactTracker::Event& event : events){
        // a GameObject an earlier callback destroyed: nothing more for its pairs this step
        if (event.phase != ContactPhase::Exit && (!scene->isEntityCreated(event.a.entity) || !scene->isEntityCreated(event.b.entity))){
            continue;
        }
        const ScriptMessage message = contactMessage(event.trigger, event.phase, true);
        for (int side = 0; side < 2; side++){
            const bool selfIsA = (side == 0);
            const ColliderKey self = selfIsA ? event.a : event.b;
            const ColliderKey other = selfIsA ? event.b : event.a;
            const Entity targets[2] = {self.entity, receiver2D(scene, self)};
            if (event.trigger){
                const ColliderRef2D otherRef(scene, other.entity, other.collider);
                auto callCpp = [&](ScriptBase& script){
                    if (event.phase == ContactPhase::Enter) script.onTriggerEnter2D(otherRef);
                    else if (event.phase == ContactPhase::Stay) script.onTriggerStay2D(otherRef);
                    else script.onTriggerExit2D(otherRef);
                };
                for (Entity target : targets){
                    deliver(scene, target, message, pushValue(otherRef), callCpp);
                }
            }else{
                bool built = false;
                Collision2D collision;
                for (Entity target : targets){
                    if (target == NULL_ENTITY || !scene->isEntityCreated(target) || !(ScriptMessages::receivers(scene, target) & scriptMessageBit(message))){
                        continue;
                    }
                    if (!built){
                        collision = buildCollision(event, selfIsA);
                        built = true;
                    }
                    auto callCpp = [&](ScriptBase& script){
                        if (event.phase == ContactPhase::Enter) script.onCollisionEnter2D(collision);
                        else if (event.phase == ContactPhase::Stay) script.onCollisionStay2D(collision);
                        else script.onCollisionExit2D(collision);
                    };
                    deliver(scene, target, message, pushValue(collision), callCpp);
                }
            }
        }
    }
}

void PhysicsSystem::contactBodyRemoved2D(Entity entity){
    if (contacts2D){
        for (auto it = contacts2D->shapes.begin(); it != contacts2D->shapes.end();){
            it = (it->second.keyA.entity == entity || it->second.keyB.entity == entity) ? contacts2D->shapes.erase(it) : std::next(it);
        }
    }
    for (auto it = ignoredPairs2D.begin(); it != ignoredPairs2D.end();){
        it = (it->first == entity || it->second == entity) ? ignoredPairs2D.erase(it) : std::next(it);
    }

    const std::vector<ContactTracker::Pair> pairs = contactTracker2D.removeEntity(entity);
    // scene unload, Play stop: no callbacks
    if (scene->isTearingDown() || pairs.empty()){
        return;
    }
    for (const ContactTracker::Pair& pair : pairs){
        const ColliderKey self = (pair.a.entity == entity) ? pair.b : pair.a;
        const ColliderKey departing = (pair.a.entity == entity) ? pair.a : pair.b;
        const ScriptMessage message = contactMessage(pair.trigger, ContactPhase::Exit, true);
        const Entity targets[2] = {self.entity, receiver2D(scene, self)};
        if (pair.trigger){
            const ColliderRef2D otherRef(scene, departing.entity, departing.collider);
            for (Entity target : targets){
                deliver(scene, target, message, pushValue(otherRef), [&](ScriptBase& script){ script.onTriggerExit2D(otherRef); });
            }
        }else{
            Collision2D collision;
            collision.thisCollider = ColliderRef2D(scene, self.entity, self.collider);
            collision.otherCollider = ColliderRef2D(scene, departing.entity, departing.collider);
            for (Entity target : targets){
                deliver(scene, target, message, pushValue(collision), [&](ScriptBase& script){ script.onCollisionExit2D(collision); });
            }
        }
    }
}

void PhysicsSystem::contactColliderRemoved2D(Entity entity, size_t index){
    const int removed = (int)index;
    auto renumber = [&](ColliderKey& key){
        if (key.entity != entity || key.collider < 0) return;
        if (key.collider == removed) key.collider = -1;
        else if (key.collider > removed) key.collider--;
    };
    if (contacts2D){
        for (auto& [key, contact] : contacts2D->shapes){
            renumber(contact.keyA);
            renumber(contact.keyB);
        }
    }
    contactTracker2D.removeCollider(entity, removed);
}

bool PhysicsSystem::setIgnoreCollision2D(Entity a, Entity b, bool ignore){
    if (a == b || !scene->findComponent<Rigidbody2DComponent>(a) || !scene->findComponent<Rigidbody2DComponent>(b)){
        return false;
    }
    const std::pair<Entity, Entity> key = (a < b) ? std::make_pair(a, b) : std::make_pair(b, a);
    const bool changed = ignore ? ignoredPairs2D.insert(key).second : ignoredPairs2D.erase(key) > 0;
    if (changed){
        // Box2D filters a pair when it is made: drop both bodies' contacts and pair them again
        // (touching ones end, with their Exit; overlapping ones are found again when un-ignored)
        repairBody2D(a);
        repairBody2D(b);
    }
    return true;
}

bool PhysicsSystem::getIgnoreCollision2D(Entity a, Entity b) const{
    return isIgnoredPair2D(a, b);
}

void PhysicsSystem::repairBody2D(Entity entity){
    const Rigidbody2DComponent* body = scene->findComponent<Rigidbody2DComponent>(entity);
    if (!body || !b2Body_IsValid(body->body)){
        return;
    }
    if (steppingWorld2D){
        pendingRefilter2D.push_back(entity);
        return;
    }
    refilterShapes2D(body->body, true, -1);
}

std::vector<ContactSummary> PhysicsSystem::getContactSummary2D(Entity entity){
    std::vector<ContactSummary> result;
    auto addOnce = [&](const ContactSummary& summary){
        for (const ContactSummary& existing : result){
            if (existing.other == summary.other && existing.collider == summary.collider && existing.otherCollider == summary.otherCollider){
                return;
            }
        }
        result.push_back(summary);
    };
    for (const ContactTracker::Pair& pair : contactTracker2D.getPairsOf(entity)){
        const bool selfIsA = pair.a.entity == entity;
        const ColliderKey self = selfIsA ? pair.a : pair.b;
        const ColliderKey other = selfIsA ? pair.b : pair.a;
        addOnce({other.entity, self.collider, other.collider, pair.trigger, true, pair.sinceStep, pair.asleep});
    }
    const Rigidbody2DComponent* body = scene->findComponent<Rigidbody2DComponent>(entity);
    if (!body || !b2Body_IsValid(body->body)){
        return result;
    }
    const bool asleep = !b2Body_IsAwake(body->body);
    // touching solid contacts
    const int capacity = b2Body_GetContactCapacity(body->body);
    if (capacity > 0){
        std::vector<b2ContactData> contacts((size_t)capacity);
        const int count = b2Body_GetContactData(body->body, contacts.data(), capacity);
        for (int i = 0; i < count; i++){
            if (contacts[(size_t)i].manifold.pointCount <= 0) continue;
            b2ShapeId mine = contacts[(size_t)i].shapeIdA, theirs = contacts[(size_t)i].shapeIdB;
            if (bodyEntity2D(b2Shape_GetBody(mine)) != entity) std::swap(mine, theirs);
            const ColliderKey self = shapeColliderKey(mine), other = shapeColliderKey(theirs);
            addOnce({other.entity, self.collider, other.collider, false, false, 0, asleep});
        }
    }
    // overlaps of its sensors, and the sensors it is in that Box2D told about
    for (const Collider2D& collider : body->colliders){
        for (const b2ShapeId& shape : collider.shapes){
            if (!b2Shape_IsValid(shape) || !b2Shape_IsSensor(shape)) continue;
            const int sensorCapacity = b2Shape_GetSensorCapacity(shape);
            if (sensorCapacity <= 0) continue;
            std::vector<b2ShapeId> overlaps((size_t)sensorCapacity);
            const int count = b2Shape_GetSensorOverlaps(shape, overlaps.data(), sensorCapacity);
            for (int i = 0; i < count; i++){
                if (!b2Shape_IsValid(overlaps[(size_t)i])) continue;
                const ColliderKey self = shapeColliderKey(shape), other = shapeColliderKey(overlaps[(size_t)i]);
                addOnce({other.entity, self.collider, other.collider, true, false, 0, asleep});
            }
        }
    }
    if (contacts2D){
        for (const auto& [key, contact] : contacts2D->shapes){
            if (contact.keyA.entity != entity && contact.keyB.entity != entity) continue;
            const bool selfIsA = contact.keyA.entity == entity;
            const ColliderKey self = selfIsA ? contact.keyA : contact.keyB;
            const ColliderKey other = selfIsA ? contact.keyB : contact.keyA;
            addOnce({other.entity, self.collider, other.collider, contact.sensor, false, contact.sinceStep, asleep});
        }
    }
    return result;
}

#endif

// ============================================================================================ 3D

#ifdef JASPERENGINE_PHYSICS_3D

namespace {

    const RigidbodyComponent* rigidbodyOf(Scene* scene, Entity entity){
        return scene->isEntityCreated(entity) ? scene->findComponent<RigidbodyComponent>(entity) : nullptr;
    }

    Entity receiver3D(Scene* scene, const ColliderKey& key){
        const RigidbodyComponent* body = rigidbodyOf(scene, key.entity);
        if (!body || key.collider < 0 || (size_t)key.collider >= body->colliders.size()){
            return NULL_ENTITY;
        }
        const Entity receiver = body->colliders[(size_t)key.collider].receiver;
        if (receiver == NULL_ENTITY || receiver == key.entity || !scene->isEntityCreated(receiver)){
            return NULL_ENTITY;
        }
        return receiver;
    }

    // a collider pair's manifolds of the step, merged; normal and impulse in the a -> b sense
    struct PairData3D{
        Vector3 normalAB;
        float deepest = -1e30f;
        Vector3 impulseOnB;
        Vector3 velocityA, velocityB;
        struct Point{
            Vector3 point;
            float separation;
            Vector3 impulseOnB;
        };
        std::vector<Point> points;
    };

    void addManifold(PairData3D& pair, const RawContact3D& raw, bool body1IsA){
        const Vector3 normal = body1IsA ? raw.normal : raw.normal * -1.0f;
        if (raw.penetration > pair.deepest){
            pair.deepest = raw.penetration;
            pair.normalAB = normal;
            pair.velocityA = body1IsA ? raw.velocity1 : raw.velocity2;
            pair.velocityB = body1IsA ? raw.velocity2 : raw.velocity1;
        }
        const uint8_t count = std::min(raw.numPoints1, raw.numPoints2);
        for (uint8_t i = 0; i < count; i++){
            const Vector3 on1 = raw.baseOffset + raw.pointsOn1[i];
            const Vector3 on2 = raw.baseOffset + raw.pointsOn2[i];
            PairData3D::Point point;
            point.point = (on1 + on2) * 0.5f;
            // Jolt's normal moves body 2 out of body 1: < 0 when they overlap
            point.separation = (on2 - on1).dotProduct(raw.normal);
            Vector3 impulseOn2;
            if (raw.hasImpulses){
                impulseOn2 = raw.normal * raw.contactImpulses[i] + raw.tangent1 * raw.frictionImpulses1[i] + raw.tangent2 * raw.frictionImpulses2[i];
            }
            point.impulseOnB = body1IsA ? impulseOn2 : impulseOn2 * -1.0f;
            pair.impulseOnB = pair.impulseOnB + point.impulseOnB;
            pair.points.push_back(point);
        }
    }

}

std::vector<ContactSummary> PhysicsSystem::getContactSummary3D(Entity entity){
    std::vector<ContactSummary> result;
    for (const ContactTracker::Pair& pair : contactTracker3D.getPairsOf(entity)){
        const bool selfIsA = pair.a.entity == entity;
        const ColliderKey self = selfIsA ? pair.a : pair.b;
        const ColliderKey other = selfIsA ? pair.b : pair.a;
        result.push_back({other.entity, self.collider, other.collider, pair.trigger, true, pair.sinceStep, pair.asleep});
    }
    const RigidbodyComponent* body = rigidbodyOf(scene, entity);
    // what the tracker does not follow (no listener for that kind of pair) is asked of the
    // narrow phase
    if (!body || body->body.IsInvalid() || steppingWorld3D){
        return result;
    }
    JPH::BodyInterface& bodyInterface = getBodyInterface3D();
    if (!bodyInterface.IsAdded(body->body)){
        return result;
    }
    const JPH::TransformedShape transformed = bodyInterface.GetTransformedShape(body->body);
    const bool asleep = !bodyInterface.IsActive(body->body);
    const bool sensor = body->sensor;
    JPH::CollideShapeSettings settings;
    settings.mMaxSeparationDistance = 0.02f;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    JPH::IgnoreSingleBodyFilter self(body->body);
    getNarrowPhaseQuery3D().CollideShape(transformed.mShape, transformed.GetShapeScale(), transformed.GetCenterOfMassTransform(), settings,
                                         transformed.GetCenterOfMassTransform().GetTranslation(), collector, {}, {}, self);
    for (const JPH::CollideShapeResult& hit : collector.mHits){
        JPH::BodyLockRead lock(getBodyLockInterface3D(), hit.mBodyID2);
        if (!lock.Succeeded()) continue;
        const JPH::Body& other = lock.GetBody();
        ContactSummary summary;
        summary.other = PhysicsCallbacks::bodyEntity(other.GetUserData());
        summary.collider = (int)transformed.mShape->GetSubShapeUserData(hit.mSubShapeID1);
        summary.otherCollider = (int)other.GetShape()->GetSubShapeUserData(hit.mSubShapeID2);
        summary.trigger = sensor || other.IsSensor();
        summary.asleep = asleep;
        bool seen = false;
        for (const ContactSummary& existing : result){
            seen = seen || (existing.other == summary.other && existing.collider == summary.collider && existing.otherCollider == summary.otherCollider);
        }
        if (!seen) result.push_back(summary);
    }
    return result;
}

void PhysicsSystem::refreshListenerMask3D(Entity entity, RigidbodyComponent& body){
    std::vector<Entity> receivers;
    for (const Collider3D& collider : body.colliders){
        if (collider.receiver != NULL_ENTITY) receivers.push_back(collider.receiver);
    }
    const uint16_t mask = computeListenerMask(entity, receivers);
    if (mask == body.listenerMask){
        return;
    }
    body.listenerMask = mask;
    if (body.body.IsInvalid() || steppingWorld3D){
        return;
    }
    const uint64_t userData = PhysicsCallbacks::packBodyUserData(entity, mask);
    {
        JPH::BodyLockWrite lock(getBodyLockInterface3D(), body.body);
        if (lock.Succeeded()){
            lock.GetBody().SetUserData(userData);
            lock.GetBody().SetUseManifoldReduction(usesManifoldReduction3D(body));
        }
    }
    // its trigger colliders' sensor body reports for it
    if (!body.triggerBody.IsInvalid()){
        JPH::BodyLockWrite lock(getBodyLockInterface3D(), body.triggerBody);
        if (lock.Succeeded()){
            lock.GetBody().SetUserData(userData);
        }
    }
}

void PhysicsSystem::setSynchronousContactEvents3D(bool synchronous){
    synchronousContactEvents3D = synchronous;
}

bool PhysicsSystem::isSynchronousContactEvents3D() const{
    return synchronousContactEvents3D;
}

bool PhysicsSystem::isStepSynchronous3D() const{
    return stepSynchronous3D;
}

bool PhysicsSystem::setIgnoreCollision3D(Entity a, Entity b, bool ignore){
    if (a == b || !rigidbodyOf(scene, a) || !rigidbodyOf(scene, b)){
        return false;
    }
    // queued while the world steps, both woken (physics-3d's pair filter)
    setBodyPairIgnored3D(a, b, JoltPairFilter3D::SCRIPT, ignore);
    return true;
}

bool PhysicsSystem::getIgnoreCollision3D(Entity a, Entity b) const{
    return pairFilter3D && (pairFilter3D->getReasons(a, b) & JoltPairFilter3D::SCRIPT) != 0;
}

void PhysicsSystem::dispatchContactRemoved3D(const JPH::BodyID& body1, const JPH::BodyID& body2, uint32_t subShape1, uint32_t subShape2){
    if (!onContactRemoved3D.hasSubscribers()){
        return;
    }
    JPH::BodyInterface& bodyInterface = getBodyInterface3D();
    if (!bodyInterface.IsAdded(body1) || !bodyInterface.IsAdded(body2)){
        return;
    }
    JPH::RefConst<JPH::Shape> shape1 = bodyInterface.GetShape(body1);
    JPH::RefConst<JPH::Shape> shape2 = bodyInterface.GetShape(body2);
    if (!shape1 || !shape2){
        return;
    }
    JPH::SubShapeID id1, id2;
    id1.SetValue(subShape1);
    id2.SetValue(subShape2);
    const Entity entity1 = PhysicsCallbacks::bodyEntity(bodyInterface.GetUserData(body1));
    const Entity entity2 = PhysicsCallbacks::bodyEntity(bodyInterface.GetUserData(body2));
    const unsigned long index1 = (unsigned long)shape1->GetSubShapeUserData(id1);
    const unsigned long index2 = (unsigned long)shape2->GetSubShapeUserData(id2);

    std::lock_guard<std::recursive_mutex> lock(getCallbackMutex3D());
    onContactRemoved3D(Rigidbody(scene, entity1), Rigidbody(scene, entity2), index1, index2);
}

void PhysicsSystem::processContacts3D(){
    std::vector<RawContact3D>& raws = *rawContacts3D;
    contactRecorder3D->collect(raws);
    contactTracker3D.setRecordedCount(raws.size());

    // 1. the global events of the step (legacy names and arguments, now here; a synchronous
    // step called the contact ones inside it). Activations first, then contacts added,
    // persisted, removed, each in body id order.
    std::vector<RawActivation3D> activations;
    contactRecorder3D->takeActivations(activations);
    // the job threads pushed them in any order
    std::stable_sort(activations.begin(), activations.end(), [](const RawActivation3D& x, const RawActivation3D& y){
        return x.body < y.body;
    });
    for (const RawActivation3D& activation : activations){
        if (rigidbodyOf(scene, activation.entity)){
            (activation.activated ? onBodyActivated3D : onBodyDeactivated3D)(Rigidbody(scene, activation.entity));
        }
    }
    if (!stepSynchronous3D){
        for (const RawContact3D& raw : raws){
            if (raw.kind == RawContact3D::Removed){
                dispatchContactRemoved3D(raw.body1, raw.body2, raw.subShape1, raw.subShape2);
                continue;
            }
            FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D)>& event = (raw.kind == RawContact3D::Added) ? onContactAdded3D : onContactPersisted3D;
            if (!event.hasSubscribers()){
                continue;
            }
            // two bodies; or a trigger and a Character Controller's inner body (no Rigidbody: its
            // wrapper is invalid), whose contacts with solid bodies stay unreported (oe-port's rule)
            const bool bodies = rigidbodyOf(scene, raw.entity1) && rigidbodyOf(scene, raw.entity2);
            auto bodyOrController = [this](Entity entity){
                return rigidbodyOf(scene, entity) || scene->findComponent<CharacterControllerComponent>(entity);
            };
            if (!bodies && !(raw.sensor && bodyOrController(raw.entity1) && bodyOrController(raw.entity2))){
                continue;
            }
            event(Rigidbody(scene, raw.entity1), Rigidbody(scene, raw.entity2), Contact3D(scene, raw.snapshot()));
        }
    }

    // 2. the collider pairs a script listens to
    std::vector<PairData3D> data;
    ContactTracker::CurrentMap now;
    for (const RawContact3D& raw : raws){
        const uint16_t wanted = raw.sensor ? PhysicsCallbacks::TRIGGER_3D : PhysicsCallbacks::COLLISION_3D;
        if (raw.kind == RawContact3D::Removed || !((raw.mask1 | raw.mask2) & wanted)){
            continue;
        }
        // a body a global handler above destroyed or made again (its exits were sent then);
        // a trigger collider's sensor body counts as its body
        const RigidbodyComponent* body1 = rigidbodyOf(scene, raw.entity1);
        const RigidbodyComponent* body2 = rigidbodyOf(scene, raw.entity2);
        if (!body1 || !body2 || (body1->body != raw.body1 && body1->triggerBody != raw.body1) ||
                (body2->body != raw.body2 && body2->triggerBody != raw.body2)){
            continue;
        }
        const ColliderKey key1{raw.entity1, raw.collider1};
        const ColliderKey key2{raw.entity2, raw.collider2};
        const ColliderPairKey pairKey = ColliderPairKey::make(key1, key2);
        auto found = now.find(pairKey);
        if (found == now.end()){
            data.emplace_back();
            found = now.emplace(pairKey, ContactTracker::Current{raw.sensor, false, (int)data.size() - 1}).first;
        }else{
            found->second.trigger = found->second.trigger || raw.sensor;
        }
        addManifold(data[(size_t)found->second.data], raw, pairKey.a == key1);
    }
    // pairs whose bodies fell asleep: Jolt removed their contacts, they still touch (D7). Kept
    // while no body of the pair is active and both are in the world with the colliders.
    JPH::BodyInterface& bodyInterface = getBodyInterface3D();
    for (const auto& [key, pair] : contactTracker3D.getPairs()){
        if (now.count(key)){
            continue;
        }
        bool keep = true;
        for (const ColliderKey& side : {key.a, key.b}){
            // a trigger collider on a sensor body of its own: that body never sleeps, its
            // overlaps are always reported
            const RigidbodyComponent* owner = rigidbodyOf(scene, side.entity);
            if (pair.trigger && owner && !owner->triggerBody.IsInvalid()){
                keep = false;
                break;
            }
            const RigidbodyComponent* body = rigidbodyOf(scene, side.entity);
            if (!body || body->body.IsInvalid() || !bodyInterface.IsAdded(body->body) || bodyInterface.IsActive(body->body) ||
                    side.collider < 0 || (size_t)side.collider >= body->colliders.size()){
                keep = false;
                break;
            }
        }
        if (keep){
            now.emplace(key, ContactTracker::Current{pair.trigger, true, -1});
        }
    }

    const std::vector<ContactTracker::Event> events = contactTracker3D.update(now);

    auto buildCollision = [&](const ContactTracker::Event& event, bool selfIsA) -> Collision{
        const ColliderKey self = selfIsA ? event.a : event.b;
        const ColliderKey other = selfIsA ? event.b : event.a;
        Collision collision;
        collision.thisCollider = ColliderRef(scene, self.entity, self.collider);
        collision.otherCollider = ColliderRef(scene, other.entity, other.collider);
        if (event.phase == ContactPhase::Exit || event.data < 0){
            return collision;
        }
        const PairData3D& pair = data[(size_t)event.data];
        // toward this side; the impulse on this body
        const float sign = selfIsA ? -1.0f : 1.0f;
        const Vector3 normal = pair.normalAB * sign;
        collision.impulse = pair.impulseOnB * sign;
        collision.relativeVelocity = selfIsA ? pair.velocityB - pair.velocityA : pair.velocityA - pair.velocityB;
        std::vector<PairData3D::Point> points = pair.points;
        if (points.size() > 8){
            std::stable_sort(points.begin(), points.end(), [](const PairData3D::Point& x, const PairData3D::Point& y){ return x.separation < y.separation; });
            points.resize(8);
        }
        for (const PairData3D::Point& point : points){
            ContactPoint contact;
            contact.point = point.point;
            contact.normal = normal;
            contact.separation = point.separation;
            contact.impulse = point.impulseOnB * sign;
            contact.thisCollider = collision.thisCollider;
            contact.otherCollider = collision.otherCollider;
            collision.contacts.push_back(contact);
        }
        return collision;
    };

    // 3. the per-object callbacks: triggers then collisions; Enter, Stay, Exit; pairs sorted;
    // the lower side's GameObject, then its collider's receiver, then the other side
    for (const ContactTracker::Event& event : events){
        if (event.phase != ContactPhase::Exit && (!scene->isEntityCreated(event.a.entity) || !scene->isEntityCreated(event.b.entity))){
            continue;
        }
        const ScriptMessage message = contactMessage(event.trigger, event.phase, false);
        for (int side = 0; side < 2; side++){
            const bool selfIsA = (side == 0);
            const ColliderKey self = selfIsA ? event.a : event.b;
            const ColliderKey other = selfIsA ? event.b : event.a;
            const Entity targets[2] = {self.entity, receiver3D(scene, self)};
            if (event.trigger){
                const ColliderRef otherRef(scene, other.entity, other.collider);
                auto callCpp = [&](ScriptBase& script){
                    if (event.phase == ContactPhase::Enter) script.onTriggerEnter(otherRef);
                    else if (event.phase == ContactPhase::Stay) script.onTriggerStay(otherRef);
                    else script.onTriggerExit(otherRef);
                };
                for (Entity target : targets){
                    deliver(scene, target, message, pushValue(otherRef), callCpp);
                }
            }else{
                bool built = false;
                Collision collision;
                for (Entity target : targets){
                    if (target == NULL_ENTITY || !scene->isEntityCreated(target) || !(ScriptMessages::receivers(scene, target) & scriptMessageBit(message))){
                        continue;
                    }
                    if (!built){
                        collision = buildCollision(event, selfIsA);
                        built = true;
                    }
                    auto callCpp = [&](ScriptBase& script){
                        if (event.phase == ContactPhase::Enter) script.onCollisionEnter(collision);
                        else if (event.phase == ContactPhase::Stay) script.onCollisionStay(collision);
                        else script.onCollisionExit(collision);
                    };
                    deliver(scene, target, message, pushValue(collision), callCpp);
                }
            }
        }
    }
}

void PhysicsSystem::contactBodyRemoved3D(Entity entity){
    // its ignored pairs go with it (the entity id may be reused)
    if (pairFilter3D){
        changePairFilter3D([entity](JoltPairFilter3D& filter){ filter.forgetEntity(entity); });
    }

    const std::vector<ContactTracker::Pair> pairs = contactTracker3D.removeEntity(entity);
    if (scene->isTearingDown() || pairs.empty()){
        return;
    }
    for (const ContactTracker::Pair& pair : pairs){
        const ColliderKey self = (pair.a.entity == entity) ? pair.b : pair.a;
        const ColliderKey departing = (pair.a.entity == entity) ? pair.a : pair.b;
        const ScriptMessage message = contactMessage(pair.trigger, ContactPhase::Exit, false);
        const Entity targets[2] = {self.entity, receiver3D(scene, self)};
        if (pair.trigger){
            const ColliderRef otherRef(scene, departing.entity, departing.collider);
            for (Entity target : targets){
                deliver(scene, target, message, pushValue(otherRef), [&](ScriptBase& script){ script.onTriggerExit(otherRef); });
            }
        }else{
            Collision collision;
            collision.thisCollider = ColliderRef(scene, self.entity, self.collider);
            collision.otherCollider = ColliderRef(scene, departing.entity, departing.collider);
            for (Entity target : targets){
                deliver(scene, target, message, pushValue(collision), [&](ScriptBase& script){ script.onCollisionExit(collision); });
            }
        }
    }
}

#endif
