// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RIGIDBODY2D_COMPONENT_H
#define RIGIDBODY2D_COMPONENT_H

#include "Engine.h"
#include "math/Vector2.h"
#include "util/HybridArray.h"
#ifdef JASPERENGINE_PHYSICS_2D
#include "box2d/box2d.h"
#endif

namespace jasperengine{

    enum class Collider2DType{
        POLYGON,
        CIRCLE,
        CAPSULE,
        SEGMENT,
        CHAIN // chain is a different type in Box2D 3.x
    };

    struct JASPERENGINE_API Collider2D{
#ifdef JASPERENGINE_PHYSICS_2D
        b2ShapeId shape = b2_nullShapeId;
        b2ChainId chain = b2_nullChainId;
#endif

        Collider2DType type = Collider2DType::POLYGON;

        Vector2 pointA = Vector2::ZERO;
        Vector2 pointB = Vector2::ZERO;
        float radius = 0.0f;

        HybridArray<Vector2, MAX_COLLIDER_POINTS_2D> vertices;
        uint8_t numVertices = 0;
        bool loop = false;

        // mass = area * density, in metres after pointsToMeterScale2D.
        // Collider3D::density is volume-based and defaults to 1000.
        float density = 1.0f;
        float friction = 0.6f;
        float restitution = 0.0f;

        // overlap only: always fires the sensor contact events, never collides
        bool sensor = false;

        bool enableHitEvents = false;
        bool contactEvents = false;
        bool preSolveEvents = false;
        bool sensorEvents = false;

        uint16_t categoryBits = 1;
        uint16_t maskBits = 0xFFFF;
        int16_t groupIndex = 0;
    };

    struct Rigidbody2DComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

#ifdef JASPERENGINE_PHYSICS_2D
        b2BodyId body = b2_nullBodyId;
#endif

        HybridArray<Collider2D, MAX_COLLIDERS_2D> colliders;
        size_t numColliders = 0;

        bool needReloadBody = true;
        bool needUpdateColliders = true;

        BodyType type = BodyType::STATIC;
        bool newBody = true;
        Vector2 loadedScale = Vector2::UNIT_SCALE;
    };

}

#endif //RIGIDBODY2D_COMPONENT_H
