// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RIGIDBODY_COMPONENT_H
#define RIGIDBODY_COMPONENT_H

#include "Engine.h"
#include "ecs/Entity.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "util/HybridArray.h"
#include <cstdint>

#ifdef JASPERENGINE_PHYSICS_3D
#include "Jolt/Jolt.h"
#include "Jolt/Physics/Body/Body.h"
#endif

namespace jasperengine{

    enum class Collider3DType{
        SPHERE,
        BOX,
        CAPSULE,
        TAPERED_CAPSULE,
        CYLINDER,
        CONVEX_HULL,
        MESH,
        HEIGHTFIELD
    };

    enum class Collider3DSource{
        NONE,
        RAW_VERTICES,
        RAW_MESH,
        ENTITY_MESH,
        ENTITY_HEIGHTFIELD
    };

    enum class RigidbodyMotionQuality{
        DISCRETE,
        LINEAR_CAST
    };

    // Same bits as JPH::EAllowedDOFs, so the backend takes them with a plain cast.
    enum RigidbodyAllowedDOFFlags : uint8_t {
        RigidbodyAllowedDOF_TranslationX = 1 << 0,
        RigidbodyAllowedDOF_TranslationY = 1 << 1,
        RigidbodyAllowedDOF_TranslationZ = 1 << 2,
        RigidbodyAllowedDOF_RotationX    = 1 << 3,
        RigidbodyAllowedDOF_RotationY    = 1 << 4,
        RigidbodyAllowedDOF_RotationZ    = 1 << 5,
        RigidbodyAllowedDOF_All          = 0x3F
    };

    struct JASPERENGINE_API Collider3D{
#ifdef JASPERENGINE_PHYSICS_3D
        JPH::ShapeRefC shape = NULL;
#endif
        Vector3 position = Vector3::ZERO;
        Quaternion rotation = Quaternion::IDENTITY;

        Collider3DType type = Collider3DType::SPHERE;

        float width = 1.0f;
        float height = 1.0f;
        float depth = 1.0f;

        float radius = 1.0f;
        float halfHeight = 0.5f;
        float topRadius = 0.5f;
        float bottomRadius = 0.5f;
        // kg/m3: mass = scaled volume * density (radius-1 sphere is ~4189 kg).
        // Collider2D::density is area-based and defaults to 1.
        float density = 1000.0f;

        Collider3DSource source = Collider3DSource::NONE;
        Entity sourceEntity = NULL_ENTITY;
        unsigned int samplesSize = 0;

        HybridArray<Vector3, MAX_COLLIDER_VERTICES_3D> vertices;
        uint16_t numVertices = 0;
        HybridArray<uint16_t, MAX_COLLIDER_INDICES_3D> indices;
        uint16_t numIndices = 0;
    };

    struct RigidbodyComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

#ifdef JASPERENGINE_PHYSICS_3D
        JPH::BodyID body;
#endif

        HybridArray<Collider3D, MAX_COLLIDERS_3D> colliders;
        size_t numColliders = 0;

        bool needReloadBody = true;
        bool needUpdateColliders = true;

        bool overrideMassProperties = false;
        Vector3 solidBoxSize;
        float solidBoxDensity;
        
        BodyType type = BodyType::STATIC;
        RigidbodyMotionQuality motionQuality = RigidbodyMotionQuality::DISCRETE;
        float gravityFactor = 1.0f;
        // Surface of the whole body (Jolt keeps them per body, not per collider); the
        // defaults are Jolt's. A contact combines two frictions by geometric mean and takes
        // the larger restitution.
        float friction = 0.2f;
        float restitution = 0.0f;
        uint8_t allowedDOFs = RigidbodyAllowedDOF_All;
        bool sensor = false;
        bool newBody = true;
        Vector3 loadedScale = Vector3::UNIT_SCALE;

        // Collision filter bits (Rigidbody::setBitsFilter), kept across body reloads. Two
        // bodies collide when each one's category meets the other's mask and the Layer
        // Collision Matrix lets their GameObject layers collide.
        uint16_t categoryBits = 1;
        uint16_t maskBits = 0xFFFF;
    };

}

#endif //RIGIDBODY_COMPONENT_H
