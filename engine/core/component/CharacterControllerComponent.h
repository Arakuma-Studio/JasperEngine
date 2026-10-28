// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef CHARACTER_CONTROLLER_COMPONENT_H
#define CHARACTER_CONTROLLER_COMPONENT_H

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Vector3.h"
#include "component/CharacterCollision.h"
#include <cstdint>

namespace jasperengine{

    // A kinematic capsule moved by script (Unity's CharacterController) on Jolt's
    // CharacterVirtual: move/simpleMove sweep it through the world at once, climbing steps up to
    // stepOffset, sliding on walls, refusing slopes steeper than slopeLimit and pushing dynamic
    // bodies; after each world step the physics carries it on a moving platform. The capsule is
    // centred at the Transform's position plus center, sizes in the GameObject's scaled units.
    // It cannot share its GameObject with a Rigidbody (docs/mcp/guide/physics.md).
    struct CharacterControllerComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

        Vector3 center = Vector3(0.0f, 0.0f, 0.0f);
        float height = 2.0f;
        float radius = 0.5f;
        float slopeLimit = 45.0f;               // degrees: steeper surfaces are not walkable
        float stepOffset = 0.3f;                // steps up to this high are climbed (0: none)
        float skinWidth = 0.02f;                // how far it stays off other colliders
        float minMoveDistance = 0.001f;         // a shorter move does nothing
        float mass = 70.0f;                     // kg, its weight on what it stands on
        float pushStrength = 100.0f;            // N, the most it pushes a dynamic body with
        bool pushBodies = true;
        bool movingPlatforms = true;            // carried by what it stands on
        bool inheritPlatformYaw = true;         // and turned with it
        bool detectCollisions = true;           // an inner body: ray casts, triggers and bodies see it
        bool characterCollisions = true;        // other controllers block it
        bool enhancedEdgeRemoval = false;       // smoother over the inner edges of meshes (costs more)
        // Collision filter bits, as a Rigidbody's: a pair meets when each one's category meets
        // the other's mask and the Layer Collision Matrix lets their layers collide
        uint16_t categoryBits = 1;
        uint16_t maskBits = 0xFFFF;

        // ---- runtime state (not saved): the last move and the last step
        bool isGrounded = false;                // standing on walkable ground (Jolt OnGround)
        bool isOnSteepSlope = false;            // touching ground too steep to stand on
        int collisionFlags = CharacterCollision_None;
        Vector3 velocity;                       // the last move's actual displacement / its dt
        Vector3 groundNormal;
        Entity groundEntity = NULL_ENTITY;
        Vector3 groundVelocity;

        // its GameObject has a Rigidbody as well: it does nothing (logged once)
        bool refusedWithRigidbody = false;
        // the character the physics made for it (CharacterControllerSystem), 0 for none
        uint32_t handle = 0;
    };

}

#endif //CHARACTER_CONTROLLER_COMPONENT_H
