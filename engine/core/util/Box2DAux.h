
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef Box2DAux_h
#define Box2DAux_h

#include "box2d/box2d.h"
#include "subsystem/PhysicsSystem.h"
#include "registry/PhysicsLayers.h"

namespace jasperengine{

    // GameObject layers in Box2D's 64-bit filters. The low 16 bits are the collider's own
    // category and mask bits; the high 32 bits of the category hold the GameObject layer,
    // one bit. Masks keep the high bits clear, so Box2D's own pair test
    // (catA & maskB && catB & maskA) is still the collider bits alone. The layer collision
    // matrix gates pairs in Box2DAux::CollisionFilter, and a layer-masked query puts the
    // mask in the high bits of its query mask, which Box2D tests in its tree walk.
    namespace Box2DLayers{
        constexpr uint64_t USER_BITS = 0xFFFFull;
        // Flipped on a shape's category to make Box2D drop and re-pair its contacts after
        // the layer collision matrix changed; no mask has it, so it never matches
        constexpr uint64_t REFILTER_BIT = 1ull << 31;
        constexpr int LAYER_SHIFT = 32;

        inline uint64_t layerBit(int layer){
            return 1ull << (LAYER_SHIFT + (layer & 31));
        }

        inline b2Filter makeFilter(int layer, uint16_t categoryBits, uint16_t maskBits, int16_t groupIndex){
            b2Filter filter = b2DefaultFilter();
            filter.categoryBits = (uint64_t)categoryBits | layerBit(layer);
            filter.maskBits = (uint64_t)maskBits;
            filter.groupIndex = groupIndex;
            return filter;
        }

        // keeps the layer bit (and the refilter bit) of a live shape's category
        inline uint64_t withUserCategory(uint64_t shapeCategoryBits, uint16_t categoryBits){
            return (shapeCategoryBits & ~USER_BITS) | (uint64_t)categoryBits;
        }

        inline int layerOf(uint64_t shapeCategoryBits){
            uint32_t bits = (uint32_t)(shapeCategoryBits >> LAYER_SHIFT);
            if (bits == 0){
                return 0;
            }
            // de Bruijn index of the single set bit
            static const int index[32] = {
                0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
                31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9
            };
            bits &= (~bits + 1u);
            return index[(uint32_t)(bits * 0x077CB531u) >> 27];
        }

        inline b2QueryFilter makeQueryFilter(uint16_t categoryBits, uint32_t layerMask){
            b2QueryFilter filter;
            filter.categoryBits = (uint64_t)categoryBits;
            filter.maskBits = (uint64_t)layerMask << LAYER_SHIFT;
            return filter;
        }
    }

    struct Box2DWorldRayCastOutput{
        b2BodyId bodyId;
        b2ShapeId shapeId;
        b2Vec2 point;
        b2Vec2 normal;
        float fraction;
    };

    struct Box2DWorldRayCastContext{
        // every hit when set, else only the closest is kept (and the ray clipped to it)
        std::vector<Box2DWorldRayCastOutput>* outputs = nullptr;
        Box2DWorldRayCastOutput closest = {};
        bool hit = false;
        bool onlyStatic = false;
        const std::vector<Entity>* ignoreEntities = nullptr;
        // query mask tested against the collider category bits
        uint16_t maskBits = 0xFFFF;
    };


    class Box2DAux{
    public:

        static bool CollisionFilter(b2ShapeId shapeIdA, b2ShapeId shapeIdB, void* context){
            Scene* scene = (Scene*)context;

            // Layer Collision Matrix first: a disabled pair never gets a contact
            const LayerCollisionMatrix& matrix = Physics2D::getLayerCollisionMatrix();
            if (!matrix.isAllEnabled()){
                const int layerA = Box2DLayers::layerOf(b2Shape_GetFilter(shapeIdA).categoryBits);
                const int layerB = Box2DLayers::layerOf(b2Shape_GetFilter(shapeIdB).categoryBits);
                if (!matrix.collides(layerA, layerB)){
                    return false;
                }
            }

            size_t shapeIndexA = reinterpret_cast<size_t>(b2Shape_GetUserData(shapeIdA));
            size_t shapeIndexB = reinterpret_cast<size_t>(b2Shape_GetUserData(shapeIdB));

            b2BodyId bodyIdA = b2Shape_GetBody(shapeIdA);
            b2BodyId bodyIdB = b2Shape_GetBody(shapeIdB);

            Entity entityA = reinterpret_cast<uintptr_t>(b2Body_GetUserData(bodyIdA));
            Entity entityB = reinterpret_cast<uintptr_t>(b2Body_GetUserData(bodyIdB));

            return scene->getSystem<PhysicsSystem>()->shouldCollide2D.callRet(Rigidbody2D(scene, entityA), shapeIndexA, Rigidbody2D(scene, entityB), shapeIndexB, true);
        }

        static bool PreSolve(b2ShapeId shapeIdA, b2ShapeId shapeIdB, b2Manifold* manifold, void* context){
            Scene* scene = (Scene*)context;

            size_t shapeIndexA = reinterpret_cast<size_t>(b2Shape_GetUserData(shapeIdA));
            size_t shapeIndexB = reinterpret_cast<size_t>(b2Shape_GetUserData(shapeIdB));

            b2BodyId bodyIdA = b2Shape_GetBody(shapeIdA);
            b2BodyId bodyIdB = b2Shape_GetBody(shapeIdB);

            Entity entityA = reinterpret_cast<uintptr_t>(b2Body_GetUserData(bodyIdA));
            Entity entityB = reinterpret_cast<uintptr_t>(b2Body_GetUserData(bodyIdB));

            return scene->getSystem<PhysicsSystem>()->preSolve2D.callRet(Rigidbody2D(scene, entityA), shapeIndexA, Rigidbody2D(scene, entityB), shapeIndexB, Manifold2D(scene, manifold), true);
        }

        // A shape destroyed since the step (a body removed or recreated, or by an earlier
        // callback) still has its events, so they are skipped
        static void manageEvents(Scene* scene, b2WorldId world){
            b2ContactEvents contactEvents = b2World_GetContactEvents(world);
            b2SensorEvents sensorEvents = b2World_GetSensorEvents(world);

            for (int i = 0; i < contactEvents.beginCount; ++i){
                b2ContactBeginTouchEvent* beginEvent = contactEvents.beginEvents + i;
                if (!b2Shape_IsValid(beginEvent->shapeIdA) || !b2Shape_IsValid(beginEvent->shapeIdB))
                    continue;

                size_t shapeIndexA = reinterpret_cast<size_t>(b2Shape_GetUserData(beginEvent->shapeIdA));
                size_t shapeIndexB = reinterpret_cast<size_t>(b2Shape_GetUserData(beginEvent->shapeIdB));

                b2BodyId bodyIdA = b2Shape_GetBody(beginEvent->shapeIdA);
                b2BodyId bodyIdB = b2Shape_GetBody(beginEvent->shapeIdB);

                Entity entityA = reinterpret_cast<uintptr_t>(b2Body_GetUserData(bodyIdA));
                Entity entityB = reinterpret_cast<uintptr_t>(b2Body_GetUserData(bodyIdB));

                scene->getSystem<PhysicsSystem>()->beginContact2D.call(Rigidbody2D(scene, entityA), shapeIndexA, Rigidbody2D(scene, entityB), shapeIndexB);
            }

            for (int i = 0; i < contactEvents.endCount; ++i){
                b2ContactEndTouchEvent* endEvent = contactEvents.endEvents + i;
                if (!b2Shape_IsValid(endEvent->shapeIdA) || !b2Shape_IsValid(endEvent->shapeIdB))
                    continue;

                size_t shapeIndexA = reinterpret_cast<size_t>(b2Shape_GetUserData(endEvent->shapeIdA));
                size_t shapeIndexB = reinterpret_cast<size_t>(b2Shape_GetUserData(endEvent->shapeIdB));

                b2BodyId bodyIdA = b2Shape_GetBody(endEvent->shapeIdA);
                b2BodyId bodyIdB = b2Shape_GetBody(endEvent->shapeIdB);

                Entity entityA = reinterpret_cast<uintptr_t>(b2Body_GetUserData(bodyIdA));
                Entity entityB = reinterpret_cast<uintptr_t>(b2Body_GetUserData(bodyIdB));

                scene->getSystem<PhysicsSystem>()->endContact2D.call(Rigidbody2D(scene, entityA), shapeIndexA, Rigidbody2D(scene, entityB), shapeIndexB);
            }

            for (int i = 0; i < contactEvents.hitCount; ++i){
                b2ContactHitEvent* hitEvent = contactEvents.hitEvents + i;
                if (!b2Shape_IsValid(hitEvent->shapeIdA) || !b2Shape_IsValid(hitEvent->shapeIdB))
                    continue;

                size_t shapeIndexA = reinterpret_cast<size_t>(b2Shape_GetUserData(hitEvent->shapeIdA));
                size_t shapeIndexB = reinterpret_cast<size_t>(b2Shape_GetUserData(hitEvent->shapeIdB));

                b2BodyId bodyIdA = b2Shape_GetBody(hitEvent->shapeIdA);
                b2BodyId bodyIdB = b2Shape_GetBody(hitEvent->shapeIdB);

                Entity entityA = reinterpret_cast<uintptr_t>(b2Body_GetUserData(bodyIdA));
                Entity entityB = reinterpret_cast<uintptr_t>(b2Body_GetUserData(bodyIdB));

                Vector2 point(hitEvent->point.x, hitEvent->point.y);
                Vector2 normal(hitEvent->normal.x, hitEvent->normal.y);

                scene->getSystem<PhysicsSystem>()->hitContact2D.call(Rigidbody2D(scene, entityA), shapeIndexA, Rigidbody2D(scene, entityB), shapeIndexB, point, normal, hitEvent->approachSpeed);
            }

            for (int i = 0; i < sensorEvents.beginCount; ++i){
                b2SensorBeginTouchEvent* beginTouch = sensorEvents.beginEvents + i;
                if (!b2Shape_IsValid(beginTouch->sensorShapeId) || !b2Shape_IsValid(beginTouch->visitorShapeId))
                    continue;

                size_t shapeIndexSensor = reinterpret_cast<size_t>(b2Shape_GetUserData(beginTouch->sensorShapeId));
                size_t shapeIndexVisitor = reinterpret_cast<size_t>(b2Shape_GetUserData(beginTouch->visitorShapeId));

                b2BodyId bodyIdSensor = b2Shape_GetBody(beginTouch->sensorShapeId);
                b2BodyId bodyIdVisitor = b2Shape_GetBody(beginTouch->visitorShapeId);

                Entity entitySensor = reinterpret_cast<uintptr_t>(b2Body_GetUserData(bodyIdSensor));
                Entity entityVisitor = reinterpret_cast<uintptr_t>(b2Body_GetUserData(bodyIdVisitor));

                scene->getSystem<PhysicsSystem>()->beginSensorContact2D.call(Rigidbody2D(scene, entitySensor), shapeIndexSensor, Rigidbody2D(scene, entityVisitor), shapeIndexVisitor);
            }

            for (int i = 0; i < sensorEvents.endCount; ++i){
                b2SensorEndTouchEvent* endTouch = sensorEvents.endEvents + i;
                if (!b2Shape_IsValid(endTouch->sensorShapeId) || !b2Shape_IsValid(endTouch->visitorShapeId))
                    continue;

                size_t shapeIndexSensor = reinterpret_cast<size_t>(b2Shape_GetUserData(endTouch->sensorShapeId));
                size_t shapeIndexVisitor = reinterpret_cast<size_t>(b2Shape_GetUserData(endTouch->visitorShapeId));

                b2BodyId bodyIdSensor = b2Shape_GetBody(endTouch->sensorShapeId);
                b2BodyId bodyIdVisitor = b2Shape_GetBody(endTouch->visitorShapeId);

                Entity entitySensor = reinterpret_cast<uintptr_t>(b2Body_GetUserData(bodyIdSensor));
                Entity entityVisitor = reinterpret_cast<uintptr_t>(b2Body_GetUserData(bodyIdVisitor));

                scene->getSystem<PhysicsSystem>()->endSensorContact2D.call(Rigidbody2D(scene, entitySensor), shapeIndexSensor, Rigidbody2D(scene, entityVisitor), shapeIndexVisitor);
            }
        }

        // -1 skips the shape, the fraction clips the ray to find the closest hit,
        // 1 keeps the whole ray to store every hit
        // https://www.iforce2d.net/b2dtut/world-querying
        static float CastCallback(b2ShapeId shapeId, b2Vec2 point, b2Vec2 normal, float fraction, void* context){
            Box2DWorldRayCastContext* ctx = (Box2DWorldRayCastContext*)context;

            if ((b2Shape_GetFilter(shapeId).categoryBits & (uint64_t)ctx->maskBits & Box2DLayers::USER_BITS) == 0){
                return -1.0f;
            }

            b2BodyId bodyId = b2Shape_GetBody(shapeId);
            if (ctx->onlyStatic && (b2Body_GetType(bodyId) != b2_staticBody)){
                return -1.0f;
            }

            Entity entity = static_cast<Entity>(reinterpret_cast<uintptr_t>(b2Body_GetUserData(bodyId)));
            if (ctx->ignoreEntities){
                for (Entity ignored : *ctx->ignoreEntities){
                    if (ignored != NULL_ENTITY && entity == ignored){
                        return -1.0f;
                    }
                }
            }

            if (ctx->outputs){
                ctx->outputs->push_back({bodyId, shapeId, point, normal, fraction});
                return 1.0f;
            }

            ctx->closest = {bodyId, shapeId, point, normal, fraction};
            ctx->hit = true;
            return fraction;
        }

    };

/*
    class Box2DContactFilter : public b2ContactFilter{
    private:
        Scene* scene;
        PhysicsSystem* physicsSystem;

    public:

        Box2DContactFilter(Scene* scene, PhysicsSystem* physicsSystem){
            this->scene = scene;
            this->physicsSystem = physicsSystem;
        }
        
        bool ShouldCollide(b2Fixture* fixtureA, b2Fixture* fixtureB){
            unsigned long shapeIndexA = fixtureA->GetUserData().pointer;
            Entity entityA = fixtureA->GetBody()->GetUserData().pointer;
            Rigidbody2D bodyA(scene, entityA);

            unsigned long shapeIndexB = fixtureB->GetUserData().pointer;
            Entity entityB = fixtureB->GetBody()->GetUserData().pointer;
            Rigidbody2D bodyB(scene, entityB);

            return physicsSystem->shouldCollide2D.callRet(bodyA, shapeIndexA, bodyB, shapeIndexB, true);
        }
    };


    class Box2DContactListener : public b2ContactListener{
    private:
        Scene* scene;
        PhysicsSystem* physicsSystem;

    public:

        Box2DContactListener(Scene* scene, PhysicsSystem* physicsSystem){
            this->scene = scene;
            this->physicsSystem = physicsSystem;
        }
        
        void BeginContact(b2Contact* contact){
            physicsSystem->beginContact2D.call(Contact2D(scene, contact));
        }
        
        void EndContact(b2Contact* contact){
            physicsSystem->endContact2D.call(Contact2D(scene, contact));
        }
        
        void PreSolve(b2Contact* contact, const b2Manifold* oldManifold){
            physicsSystem->preSolve2D.call(Contact2D(scene, contact), Manifold2D(scene, oldManifold));
        }
        
        void PostSolve(b2Contact* contact, const b2ContactImpulse* impulse){
            physicsSystem->postSolve2D.call(Contact2D(scene, contact), ContactImpulse2D(impulse));
        }
    };


    struct B2_API Box2DWorldRayCastOutput{
        b2Fixture* fixture;
        b2Vec2 point;
	    b2Vec2 normal;
	    float fraction;
    };


    class Box2DRayCastCallback : public b2RayCastCallback{
    private:

        std::vector<Box2DWorldRayCastOutput>* outputs;
        bool returnAllResults;
        bool onlyStatic;
        uint16_t collisionGroup;
        uint16_t collisionMask;

	public:

        Box2DRayCastCallback(std::vector<Box2DWorldRayCastOutput>* outputs){
            this->outputs = outputs;
            this->returnAllResults = true;
            this->onlyStatic = false;
            this->collisionGroup = (uint16_t)~0u;
            this->collisionMask = (uint16_t)~0u;
        }

        Box2DRayCastCallback(std::vector<Box2DWorldRayCastOutput>* outputs, bool returnAllResults){
            this->outputs = outputs;
            this->returnAllResults = returnAllResults;
            this->onlyStatic = false;
            this->collisionGroup = (uint16_t)~0u;
            this->collisionMask = (uint16_t)~0u;
        }

        Box2DRayCastCallback(std::vector<Box2DWorldRayCastOutput>* outputs, bool returnAllResults, bool onlyStatic){
            this->outputs = outputs;
            this->returnAllResults = returnAllResults;
            this->onlyStatic = onlyStatic;
            this->collisionGroup = (uint16_t)~0u;
            this->collisionMask = (uint16_t)~0u;
        }

        Box2DRayCastCallback(std::vector<Box2DWorldRayCastOutput>* outputs, bool returnAllResults, bool onlyStatic, uint16_t collisionGroup, uint16_t collisionMask){
            this->outputs = outputs;
            this->returnAllResults = returnAllResults;
            this->onlyStatic = onlyStatic;
            this->collisionGroup = collisionGroup;
            this->collisionMask = collisionMask;
        }

		float ReportFixture(b2Fixture* fixture, const b2Vec2& point, const b2Vec2& normal, float fraction){
            if (fixture->IsSensor())
                return -1.f;

            if (onlyStatic && fixture->GetBody()->GetType() != b2BodyType::b2_staticBody)
                return -1.f;

            if ((fixture->GetFilterData().categoryBits & collisionMask) && (fixture->GetFilterData().maskBits & collisionGroup)){
                // fraction return to fint the closest intersection
                // return 1 to store all intersections
                // https://www.iforce2d.net/b2dtut/world-querying
                if (returnAllResults){
                    outputs->push_back({fixture, point, normal, fraction});
                    return 1.0f;
                }else{
                    outputs->clear();
                    outputs->push_back({fixture, point, normal, fraction});
                    return fraction;
                }

            }

            return -1.f;
        }
    };
*/
}

#endif //Box2DAux_h
