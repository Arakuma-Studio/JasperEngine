// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RIGIDBODY_COMPONENT_H
#define RIGIDBODY_COMPONENT_H

#include "Engine.h"
#include "ecs/Entity.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "component/PhysicsCommon.h"
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#ifdef JASPERENGINE_PHYSICS_3D
#include "Jolt/Jolt.h"
#include "Jolt/Physics/Body/Body.h"
#include "physics/JoltSurfaceMaterial.h"
#endif

namespace jasperengine{

    class PhysicsMaterialRuntime;

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
        // runtime: the leaf shape (world-scaled, in the collider's own frame) and its surface
        JPH::ShapeRefC shape = NULL;
        JPH::Ref<JoltSurfaceMaterial> surface;
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

        // RAW_VERTICES (hull points) and RAW_MESH (triangles) geometry, GameObject-local (scaled
        // by the transform), any size
        std::vector<Vector3> vertices;
        std::vector<uint32_t> indices;

        // Unity isTrigger: overlaps are reported (sensor contacts), nothing is pushed; a trigger
        // does not count in the body's mass unless every collider is one
        bool isTrigger = false;
        // a .physicsmaterial asset path ("" = none): friction, bounciness and their combine
        // modes, over the body's material and its legacy friction / restitution
        std::string material;
        // also gets this collider's contact callbacks (physics-contacts), none by default
        Entity receiver = NULL_ENTITY;

        // runtime, not saved
        // rebuild this collider's shape at the next sync
        bool dirty = true;
        // a material set from a script (PhysicsMaterial object), over `material`
        std::shared_ptr<PhysicsMaterialRuntime> materialInstance;
        // materialInstance is this collider's own copy (setColliderFriction on a material)
        bool ownsMaterialInstance = false;
    };

    // S96 WP-C: a collider of a child GameObject attached to this body (attachToParentBody,
    // physics/AttachedColliders.h): a copy of the child's collider posed in the body's frame
    // (world units, the child's scale in its leaf), its receiver the child. Runtime only.
    struct AttachedCollider3D{
        Entity child = NULL_ENTITY;
        // in the child's colliders
        size_t index = 0;
        Collider3D collider;
    };

    struct RigidbodyComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

#ifdef JASPERENGINE_PHYSICS_3D
        JPH::BodyID body;
        // runtime: a sensor body with the trigger colliders of a body that has solid ones too,
        // following it (invalid when the body has no such mix)
        JPH::BodyID triggerBody;
#endif

        std::vector<Collider3D> colliders;

        bool needReloadBody = true;
        bool needUpdateColliders = true;

        bool overrideMassProperties = false;
        Vector3 solidBoxSize;
        // kg/m3, the default of Collider3D::density
        float solidBoxDensity = 1000.0f;

        BodyType type = BodyType::STATIC;
        RigidbodyMotionQuality motionQuality = RigidbodyMotionQuality::DISCRETE;
        float gravityFactor = 1.0f;
        // The surface of the colliders without a material (theirs or the body's), Jolt's
        // defaults: two of them combine by geometric mean friction and the larger restitution
        float friction = 0.2f;
        float restitution = 0.0f;
        // default .physicsmaterial of the colliders without one ("" = none)
        std::string material;

        // Unity Rigidbody fields; the defaults are what Jolt used before they existed
        float linearDamping = 0.05f;
        float angularDamping = 0.05f;
        // m/s, rad/s
        float maxLinearVelocity = 500.0f;
        float maxAngularVelocity = 0.25f * 3.14159265358979323846f * 60.0f;
        // false: `mass` (kg) instead of the colliders' volume x density
        bool automaticMass = true;
        float mass = 1.0f;
        // false: centerOfMass, body-local in world units (not scaled by the transform)
        bool automaticCenterOfMass = true;
        Vector3 centerOfMass = Vector3::ZERO;
        // false: the principal moments (kg m^2) about the centre of mass, in the frame rotated
        // by inertiaTensorRotation
        bool automaticInertiaTensor = true;
        Vector3 inertiaTensor = Vector3(1.0f, 1.0f, 1.0f);
        Quaternion inertiaTensorRotation = Quaternion::IDENTITY;
        bool allowSleeping = true;
        // a kinematic body also touches static and kinematic bodies (a kinematic body with a
        // trigger collider does anyway, Unity's trigger matrix)
        bool collideKinematicVsNonDynamic = false;
        // SCENE: the scene's physics_interpolation
        RigidbodyInterpolation interpolation = RigidbodyInterpolation::SCENE;
        // In a water (a mesh drawn with library:water, Water.h), Jolt's buoyancy at the centre
        // of mass each fixed step: 1 neutral, more floats, less sinks, 0 ignores the water
        // (the default). The drags slow the body moving and turning in it (Jolt's linear drag
        // about 0.5, angular about 0.01 times the body's size).
        float buoyancy = 0.0f;
        float waterDrag = 0.5f;
        float waterAngularDrag = 0.01f;
        uint8_t allowedDOFs = RigidbodyAllowedDOF_All;
        bool sensor = false;
        bool newBody = true;
        Vector3 loadedScale = Vector3::UNIT_SCALE;

        // Runtime state of the body sync (PhysicsSystem), not saved
        // kinematic, moving to its Transform with MoveKinematic over the frame's fixed steps (each
        // part on its own: a part not followed keeps its own velocity)
        bool followingPosition = false;
        bool followingRotation = false;
        // the last step of a frame put the followed parts at the Transform
        bool followReached = false;
        // the velocities the following set: a part stops with them unless a script changed them
        Vector3 followedLinearVelocity;
        Vector3 followedAngularVelocity;
        // pose before the last step, drawn towards the current one
        Vector3 previousPosition;
        Quaternion previousRotation;
        // world pose physics and the Transform last agreed on (written to the Transform, or
        // the body put at it): a Transform still there was moved by nobody else since
        bool hasSyncedPose = false;
        Vector3 syncedPosition;
        Quaternion syncedRotation;
        // the state of the body it had in another scene (persistence moves a GameObject with
        // its body): the next body made for it starts with these, once
        bool hasPendingVelocity = false;
        Vector3 pendingLinearVelocity;
        Vector3 pendingAngularVelocity;
        bool pendingAwake = true;

        // a material set from a script for the whole body, over `material`
        std::shared_ptr<PhysicsMaterialRuntime> materialInstance;
        // the colliders' materials were resolved at this PhysicsMaterialRuntime::revision()
        uint64_t appliedMaterialRevision = 0;
        // the mass fields changed: the mass properties (and centre of mass) are applied again
        bool massDirty = false;
        // the body fields changed (damping, max velocities, sleeping...): applied before the next step
        bool needApplyParameters = false;
        // Rigidbody::setCollisionGroupID (same valid group: no collision), kept across reloads
        uint32_t collisionGroupID = std::numeric_limits<uint32_t>::max();
        // a type change asked for inside a step, applied right after it
        bool hasPendingType = false;
        BodyType pendingType = BodyType::STATIC;

        // Collision filter bits (Rigidbody::setBitsFilter), kept across body reloads. Two
        // bodies collide when each one's category meets the other's mask and the Layer
        // Collision Matrix lets their GameObject layers collide.
        uint16_t categoryBits = 1;
        uint16_t maskBits = 0xFFFF;
        // the contact messages its scripts (and its colliders' receivers') define
        // (PhysicsCallbacks bits), packed in the Jolt body's user data; runtime
        uint16_t listenerMask = 0;

        // S96 WP-C
        // Unity's colliders below a Rigidbody: this GameObject's colliders join the body of its
        // nearest ancestor with an enabled Rigidbody (physics/AttachedColliders.h) and this
        // GameObject has no body of its own (its body fields are ignored). No such ancestor: it
        // is its own body.
        bool attachToParentBody = false;
        // runtime: the attached children's colliders, after `colliders` in the combined collider
        // index the body's sub-shapes, contacts and queries report
        std::vector<AttachedCollider3D> attachedColliders;
        // runtime, on an attached child: the body its colliders joined
        Entity attachedTo = NULL_ENTITY;
        // runtime, on a body with attached children: consecutive steps it was composed again for
        // a moved child, and whether that was reported
        int attachedRecomposeSteps = 0;
        bool warnedAttachedRecompose = false;
        // runtime: Rigidbody.movePosition / moveRotation (D8), the pose reached by the end of
        // the next fixed step (a kinematic body moved from an update: of the frame's steps)
        bool hasMovePosition = false;
        bool hasMoveRotation = false;
        Vector3 movePosition;
        Quaternion moveRotation;
        // the target was set inside a fixed step (onFixedUpdate): reached by the end of this step
        bool moveFromFixedStep = false;
        // kinematic: the steps left to reach the target, and the velocities that move set (they
        // stop once it is reached unless a script changed them)
        int moveStepsLeft = 0;
        bool movingToTarget = false;
        // the step that ends a kinematic move: the body is put exactly at the target after it
        // (its velocity integrates rotations only approximately)
        bool moveSnap = false;
        Vector3 movedLinearVelocity;
        Vector3 movedAngularVelocity;
    };

}

#endif //RIGIDBODY_COMPONENT_H
