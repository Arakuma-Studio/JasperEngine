// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef CHARACTER_CONTROLLER_2D_COMPONENT_H
#define CHARACTER_CONTROLLER_2D_COMPONENT_H

#include "component/CharacterCollision.h"
#include "ecs/Entity.h"
#include "math/Vector2.h"
#include <cstdint>

namespace jasperengine{

    // The 2D CharacterController (docs/specs/S22-gameplay.md D2): a kinematic capsule in the Box2D
    // world, standing upright, moved by move/simpleMove with a collide-and-slide of its own on
    // Box2D's capsule casts and overlaps (at most 4 iterations): it climbs steps up to stepOffset,
    // refuses slopes steeper than slopeLimit, keeps skinWidth off colliders, sticks to the floor
    // going down, rides kinematic platforms and pushes dynamic bodies. Sizes in points (the
    // canvas units 2D bodies use; the world has pointsToMeterScale2D points a metre), centred at the
    // Transform's position plus center. It cannot share its GameObject with a Rigidbody 2D.
    struct CharacterController2DComponent{
        bool enabled = true;

        Vector2 center = Vector2(0.0f, 0.0f);
        float height = 128.0f;              // points (2 m at 64 points a metre)
        float radius = 32.0f;
        float slopeLimit = 45.0f;           // degrees
        float stepOffset = 19.2f;           // points (0.3 m)
        float skinWidth = 1.28f;            // points (0.02 m)
        float minMoveDistance = 0.064f;     // points (0.001 m)
        float pushStrength = 100.0f;        // N: impulse pushStrength x dt along the move
        bool pushBodies = true;
        bool movingPlatforms = true;
        uint16_t categoryBits = 1;
        uint16_t maskBits = 0xFFFF;

        // ---- runtime state (not saved): the last move and the last step
        bool isGrounded = false;
        bool isOnSteepSlope = false;
        int collisionFlags = CharacterCollision_None;
        Vector2 velocity;                   // points / s
        Vector2 groundNormal;
        Entity groundEntity = NULL_ENTITY;
        Vector2 groundVelocity;             // points / s
        // its GameObject has a Rigidbody 2D as well: it does nothing (logged once)
        bool refusedWithRigidbody = false;
    };

}

#endif //CHARACTER_CONTROLLER_2D_COMPONENT_H
