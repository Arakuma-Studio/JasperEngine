// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef CHARACTER_CONTROLLER_SYSTEM_H
#define CHARACTER_CONTROLLER_SYSTEM_H

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Vector3.h"
#include "component/CharacterControllerComponent.h"
#include "object/physics/ControllerColliderHit.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "Jolt/Jolt.h"
#include "Jolt/Physics/Character/CharacterVirtual.h"
#endif

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jasperengine{

    class Scene;
    class PhysicsSystem;

#ifdef JASPERENGINE_PHYSICS_3D

    // The CharacterControllers of a scene's 3D world (docs/specs/S22-gameplay.md R19-R27),
    // owned by its PhysicsSystem: a Jolt CharacterVirtual for each enabled component of an
    // active GameObject, moved by move/simpleMove at once with the world's layer filters,
    // pushing dynamic bodies through a CharacterContactListener, blocked by one another, and
    // with an optional kinematic inner body the rest of the world sees. After every world
    // step the platforms carry their passengers, the ground state is refreshed and root motion
    // is applied.
    class JASPERENGINE_API CharacterControllerSystem{
    private:
        struct Runtime{
            Entity entity = NULL_ENTITY;
            JPH::Ref<JPH::CharacterVirtual> character;
            JPH::ObjectLayer objectLayer = 0;
            // what the character was made with: a change of one of these remakes or reshapes it
            float height = 0.0f;
            float radius = 0.0f;
            Vector3 center;
            Vector3 scale = Vector3(1.0f, 1.0f, 1.0f);
            float skinWidth = 0.0f;
            bool detectCollisions = false;
            bool characterCollisions = false;
            int layer = 0;
            uint16_t categoryBits = 1;
            uint16_t maskBits = 0xFFFF;
            // the world position it last put its Transform at (a different one is a teleport)
            Vector3 syncedPosition;
            // simpleMove's own vertical speed (gravity), reset on the ground
            float verticalSpeed = 0.0f;
            // Jolt said supported at the end of the last move (moveBy's stick-to-floor)
            bool supportedByMove = false;
            // root motion (setRootMotion), applied in every fixed step until the next publish
            bool hasRootMotion = false;
            Vector3 rootVelocity;
            float rootYawRate = 0.0f;
            bool rootMotionWarned = false;
        };

        class ContactListener;
        class InnerBodiesFilter;

        Scene* scene;
        PhysicsSystem* physics;
        std::unordered_map<uint32_t, Runtime> runtimes;
        uint32_t nextHandle = 1;
        std::unique_ptr<JPH::CharacterVsCharacterCollisionSimple> characterVsCharacter;
        std::unique_ptr<ContactListener> listener;
        // the inner bodies, read from Jolt's threads during a step (only changed between steps)
        std::unordered_set<uint32_t> innerBodies;
        std::unordered_set<Entity> refusedWithRigidbody;
        std::vector<ControllerColliderHit> hitScratch;

        Runtime* find(Entity entity);
        Runtime* ensure(Entity entity, CharacterControllerComponent& controller);
        bool create(Entity entity, CharacterControllerComponent& controller);
        void destroy(CharacterControllerComponent& controller);
        // the settings that need no remaking (slope, mass, strength, edges, layer and bits)
        void applySettings(Runtime& runtime, CharacterControllerComponent& controller);
        // a Transform moved by something else: the character goes there
        void followTransform(Runtime& runtime);
        // the capsule of a size at a scale; null when the size is not valid
        JPH::RefConst<JPH::Shape> makeShape(float height, float radius, const Vector3& scale) const;
        Vector3 worldScale(Entity entity) const;
        // a move of a displacement over dt, with the stairs and the floor; the move's flags
        int moveBy(Runtime& runtime, CharacterControllerComponent& controller, const Vector3& motion, float dt, bool reportHits);
        void readGroundState(Runtime& runtime, CharacterControllerComponent& controller);
        void writeTransform(Runtime& runtime, const Vector3& worldPosition, float yawDelta);
        void deliverHits(Entity entity, const Vector3& motion);
        float moveDelta() const;

    public:
        CharacterControllerSystem(Scene* scene, PhysicsSystem* physics);
        ~CharacterControllerSystem();

        CharacterControllerSystem(const CharacterControllerSystem&) = delete;
        CharacterControllerSystem& operator=(const CharacterControllerSystem&) = delete;

        // Before each world step: characters made and removed as components come and go, are
        // enabled or disabled and their GameObjects (in)activated, a sharing Rigidbody refused
        void preStep();
        // After each world step: moving platforms carry, the ground state follows what moved,
        // root motion moves (dt: the fixed step)
        void postStep(float dt);
        size_t count() const { return runtimes.size(); }

        int move(Entity entity, const Vector3& motion);
        bool simpleMove(Entity entity, const Vector3& speed);
        bool trySetSize(Entity entity, float height, float radius, const Vector3& center);
        void setRootMotion(Entity entity, const Vector3& worldVelocity, float yawRate);

        // the component went (PhysicsSystem::onComponentRemoved)
        void remove(Entity entity);
        // its GameObject's layer changed
        void refreshLayer(Entity entity);
        // every character goes (the world is torn down)
        void clear();

        bool isInnerBody(const JPH::BodyID& body) const;
        // the controller entity of a character (the CharacterVsCharacter contacts)
        static Entity entityOf(const JPH::CharacterVirtual* character);
    };

#endif

}

#endif //CHARACTER_CONTROLLER_SYSTEM_H
