
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef Box2DAux_h
#define Box2DAux_h

#include "box2d/box2d.h"
#include "subsystem/PhysicsSystem.h"
#include "registry/PhysicsLayers.h"
#include "physics/PhysicsMaterial.h"

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
        // S96 queries: sensor shapes (Unity triggers) are not hits
        bool skipSensors = false;
    };


    class Box2DAux{
    public:

        // Box2D friction / restitution callbacks: the shapes' user material ids carry the
        // combine modes of their PhysicsMaterial (0 = none, today's rule)
        static float CombineFriction(float frictionA, int userMaterialIdA, float frictionB, int userMaterialIdB){
            return combineSurface(frictionA, frictionModeOf2D(userMaterialIdA), frictionB, frictionModeOf2D(userMaterialIdB), PhysicsMaterialCombine::GEOMETRIC_MEAN);
        }

        static float CombineRestitution(float restitutionA, int userMaterialIdA, float restitutionB, int userMaterialIdB){
            return combineSurface(restitutionA, bounceModeOf2D(userMaterialIdA), restitutionB, bounceModeOf2D(userMaterialIdB), PhysicsMaterialCombine::MAXIMUM);
        }

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

            // Physics2D.ignoreCollision
            std::shared_ptr<PhysicsSystem> physics = scene->getSystem<PhysicsSystem>();
            if (physics->isIgnoredPair2D(entityA, entityB)){
                return false;
            }

            return physics->shouldCollide2D.callRet(Rigidbody2D(scene, entityA), shapeIndexA, Rigidbody2D(scene, entityB), shapeIndexB, true);
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

        // -1 skips the shape, the fraction clips the ray to find the closest hit,
        // 1 keeps the whole ray to store every hit
        // https://www.iforce2d.net/b2dtut/world-querying
        static float CastCallback(b2ShapeId shapeId, b2Vec2 point, b2Vec2 normal, float fraction, void* context){
            Box2DWorldRayCastContext* ctx = (Box2DWorldRayCastContext*)context;

            if ((b2Shape_GetFilter(shapeId).categoryBits & (uint64_t)ctx->maskBits & Box2DLayers::USER_BITS) == 0){
                return -1.0f;
            }

            if (ctx->skipSensors && b2Shape_IsSensor(shapeId)){
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


}

#endif //Box2DAux_h
