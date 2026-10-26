// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef CHARACTER_CONTROLLER_2D_SYSTEM_H
#define CHARACTER_CONTROLLER_2D_SYSTEM_H

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Vector2.h"
#include "component/CharacterController2DComponent.h"

#ifdef JASPERENGINE_PHYSICS_2D
#include "box2d/box2d.h"
#endif

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace jasperengine{

    class Scene;
    class PhysicsSystem;

#ifdef JASPERENGINE_PHYSICS_2D

    // The 2D CharacterControllers of a scene (docs/specs/S22-gameplay.md D2, P5), owned by its
    // PhysicsSystem: a collide-and-slide of our own on Box2D v3.0's capsule casts, overlaps and
    // manifolds (Box2D 3.1's mover is not in 3.0). A move solves the contact planes around the
    // capsule for its target, casts towards the solution and repeats, at most 4 times; then it
    // climbs a step (up, forward, down casts) when a wall stopped it on the ground, and sticks to
    // the floor going down. After each world step the platforms carry their passengers and the
    // ground state is refreshed. The controller has no Box2D body: rays and sensors do not see it.
    // Internally in Box2D metres; components, moves and results in points.
    class JASPERENGINE_API CharacterController2DSystem{
    public:
        // a surface near the capsule: what the plane solver keeps the capsule out of
        struct Plane{
            b2Vec2 normal;          // towards the capsule
            float separation;       // of the skin-inflated capsule (negative: inside the skin)
            b2Vec2 point;           // world contact point (metres)
            b2ShapeId shape;
            b2BodyId body;
            float push;             // the solver's accumulated push along the normal
        };

    private:
        struct Runtime{
            // simpleMove's own vertical speed (metres / s), reset on the ground
            float verticalSpeed = 0.0f;
            // what it stands on, for the platform carry after the next step
            b2BodyId ground = b2_nullBodyId;
            b2Vec2 groundPoint = {0.0f, 0.0f};
        };

        // the capsule of a controller in the world, metres
        struct Capsule{
            b2Vec2 center;          // the capsule's centre (Transform position + center)
            float halfSegment;      // half the distance between the circles' centres
            float radius;
            float skin;
            float cosSlope;         // walkable: normal.y at least this
            float stepOffset;
            float minMoveDistance;
            b2QueryFilter filter;
            uint16_t maskBits;
            int layer;
            Entity entity;
        };

        Scene* scene;
        PhysicsSystem* physics;
        std::unordered_map<Entity, Runtime> runtimes;
        std::vector<b2ShapeId> shapeScratch;
        std::vector<Plane> planeScratch;
        std::vector<Plane> touchedScratch;
        std::vector<b2BodyId> pushedScratch;

        bool makeCapsule(Entity entity, const CharacterController2DComponent& controller, Capsule& capsule);
        // the planes of the surfaces within reach of the skin around the capsule at `center`
        void collectPlanes(const Capsule& capsule, b2Vec2 center, std::vector<Plane>& planes);
        // the free part (0..1) of a cast of the skin-inflated capsule by `translation`, and what stopped it
        float cast(const Capsule& capsule, b2Vec2 center, b2Vec2 translation, Plane* hit);
        // a collide-and-slide from `center` to `center + delta`, the contacts that stopped it in `touched`
        b2Vec2 slide(const Capsule& capsule, b2Vec2 center, b2Vec2 delta, std::vector<Plane>* touched);
        // a step up over what stopped it (the top of its contacts: blockingTop), up, forward and down
        bool tryStep(const Capsule& capsule, b2Vec2& center, float horizontal, float blockingTop);
        bool stickToFloor(const Capsule& capsule, b2Vec2& center);
        // standing on walkable ground (within the skin) at `center`
        bool supported(const Capsule& capsule, b2Vec2 center);
        // the ground state at `center`: grounded on a walkable surface in reach of the skin
        void readGround(const Capsule& capsule, b2Vec2 center, CharacterController2DComponent& controller, Runtime& runtime);
        int classify(const Capsule& capsule, b2Vec2 center, const std::vector<Plane>& touched) const;
        void push(const Capsule& capsule, const CharacterController2DComponent& controller, b2Vec2 direction, float dt,
                  const std::vector<Plane>& touched);
        void writeTransform(Entity entity, const CharacterController2DComponent& controller, b2Vec2 center);
        bool refused(Entity entity, CharacterController2DComponent& controller);
        bool usable(Entity entity, CharacterController2DComponent& controller);
        // a move by a displacement in metres over dt, `withSteps` for a script's move
        int moveBy(Entity entity, CharacterController2DComponent& controller, Runtime& runtime, b2Vec2 motion, float dt, bool scripted);
        float moveDelta() const;

    public:
        CharacterController2DSystem(Scene* scene, PhysicsSystem* physics);

        CharacterController2DSystem(const CharacterController2DSystem&) = delete;
        CharacterController2DSystem& operator=(const CharacterController2DSystem&) = delete;

        // After each world step: the platforms carry, the ground state follows what moved
        void postStep(float dt);

        // motion and speed in points (per second for speed)
        int move(Entity entity, const Vector2& motion);
        bool simpleMove(Entity entity, const Vector2& speed);

        // the component went
        void remove(Entity entity);
        void clear();
        size_t count() const { return runtimes.size(); }
    };

#endif

}

#endif //CHARACTER_CONTROLLER_2D_SYSTEM_H
