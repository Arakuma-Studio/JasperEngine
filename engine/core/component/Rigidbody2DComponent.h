// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RIGIDBODY2D_COMPONENT_H
#define RIGIDBODY2D_COMPONENT_H

#include "Engine.h"
#include "ecs/Entity.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Quaternion.h"
#include "component/PhysicsCommon.h"
#ifdef JASPERENGINE_PHYSICS_2D
#include "box2d/box2d.h"
#endif

#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    class PhysicsMaterialRuntime;

    enum class Collider2DType{
        POLYGON,    // any simple polygon of 3+ points, concave too (split into convex pieces)
        CIRCLE,
        CAPSULE,
        SEGMENT,
        CHAIN       // 2+ points; one-sided Box2D chain from 4 points, segments below (Unity EdgeCollider2D)
    };

    // Unity collisionDetectionMode: CONTINUOUS is a Box2D bullet (swept against dynamic and
    // kinematic bodies too, not only static ones)
    enum class CollisionDetectionMode2D{
        DISCRETE,
        CONTINUOUS
    };

    // Unity sleepMode
    enum class RigidbodySleepMode2D{
        NEVER_SLEEP,
        START_AWAKE,
        START_ASLEEP
    };

    struct JASPERENGINE_API Collider2D{
#ifdef JASPERENGINE_PHYSICS_2D
        // runtime: the Box2D shapes of the collider, several for a polygon split in convex
        // pieces (every one reports this collider's index); none for a chain
        std::vector<b2ShapeId> shapes;
        b2ChainId chain = b2_nullChainId;
#endif

        Collider2DType type = Collider2DType::POLYGON;

        Vector2 pointA = Vector2::ZERO;
        Vector2 pointB = Vector2::ZERO;
        float radius = 0.0f;

        // polygon and chain points, GameObject-local (scaled by the transform), any count
        std::vector<Vector2> points;
        bool loop = false;

        // mass = area * density, in metres after pointsToMeterScale2D.
        // Collider3D::density is volume-based and defaults to 1000.
        float density = 1.0f;
        // the surface without a material (collider's or body's): combined as before materials
        // (geometric mean friction, maximum restitution)
        float friction = 0.6f;
        float restitution = 0.0f;
        // a .physicsmaterial asset path ("" = none): overrides friction and restitution, and
        // brings its combine modes. Takes precedence over the body's material.
        std::string material;

        // overlap only: always fires the sensor contact events, never collides
        bool sensor = false;

        bool enableHitEvents = false;
        // on for colliders created from now on (Unity); scenes keep their stored value
        bool contactEvents = true;
        bool preSolveEvents = false;
        bool sensorEvents = false;

        uint16_t categoryBits = 1;
        uint16_t maskBits = 0xFFFF;
        int16_t groupIndex = 0;

        // also gets this collider's contact callbacks (physics-contacts), none by default
        Entity receiver = NULL_ENTITY;

        // a slice of the GameObject's sheet sprite (docs/specs/S44-aseprite-import.md R17): the
        // collider is that slice's box on every frame (a 4-point polygon, rebuilt when the box
        // changes; type and points stay as saved) and has no shape on frames where the slice is
        // hidden. "" = its own shape.
        std::string slice;

        // runtime, not saved
        // rebuild this collider's shapes at the next sync
        bool dirty = true;
        // a material set from a script (PhysicsMaterial object), over `material`
        std::shared_ptr<PhysicsMaterialRuntime> materialInstance;
        // materialInstance is this collider's own copy (setColliderFriction on a material)
        bool ownsMaterialInstance = false;
        // the convex pieces of a POLYGON (metres, Box2D's units) and the key of what they were made of
        uint64_t decompositionKey = 0;
        std::vector<std::vector<Vector2>> pieces;
        int droppedPieces = 0;
        bool selfIntersecting = false;
        // why it has no piece (empty when it has some)
        std::string decompositionError;
        // the surface last applied to the shapes (friction, restitution, combine modes)
        float appliedFriction = -1.0f;
        float appliedRestitution = -1.0f;
        int appliedUserMaterial = -1;
        // the slice's box on the current frame (its shape's 4 points, never the saved `points`)
        // and whether the slice shows there
        bool sliceVisible = false;
        Rect sliceBounds = Rect(0.0f, 0.0f, 0.0f, 0.0f);
        std::vector<Vector2> slicePoints;
    };

    struct JASPERENGINE_API Rigidbody2DComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

#ifdef JASPERENGINE_PHYSICS_2D
        b2BodyId body = b2_nullBodyId;
#endif

        std::vector<Collider2D> colliders;

        bool needReloadBody = true;
        bool needUpdateColliders = true;

        BodyType type = BodyType::STATIC;

        // Unity Rigidbody2D fields (defaults = what bodies had before they existed)
        float gravityScale = 1.0f;
        float linearDamping = 0.0f;
        float angularDamping = 0.0f;
        bool fixedRotation = false;
        CollisionDetectionMode2D collisionDetection = CollisionDetectionMode2D::DISCRETE;
        RigidbodySleepMode2D sleepingMode = RigidbodySleepMode2D::START_AWAKE;
        // false: `mass` (kg) instead of the colliders' area x density
        bool useAutoMass = true;
        float mass = 1.0f;
        // false: centerOfMass, body-local in collider units (points, scaled by the transform)
        bool autoCenterOfMass = true;
        Vector2 centerOfMass = Vector2::ZERO;
        // false: inertia (kg m^2) about the centre of mass
        bool autoInertia = true;
        float inertia = 0.0f;
        // default .physicsmaterial of the colliders without one ("" = none)
        std::string material;
        // how it is drawn between fixed steps; SCENE: the scene's physics_interpolation
        RigidbodyInterpolation interpolation = RigidbodyInterpolation::SCENE;

        bool newBody = true;
        Vector2 loadedScale = Vector2::UNIT_SCALE;

        // Runtime state of the body sync (PhysicsSystem), not saved
        // a collider follows a sprite slice (checked once per step only then)
        bool hasSliceColliders = false;
        // a material set from a script for the whole body, over `material`
        std::shared_ptr<PhysicsMaterialRuntime> materialInstance;
        // the colliders' materials were resolved at this PhysicsMaterialRuntime::revision()
        uint64_t appliedMaterialRevision = 0;
        // the explicit mass data must be applied again (a Box2D call recomputed it)
        bool needApplyMass = false;
        // the body fields changed (gravity scale, damping...): applied before the next step
        bool needApplyParameters = false;
        // kinematic, moving to its Transform with a velocity over the frame's fixed steps (each
        // part on its own: a part not followed keeps its own velocity)
        bool followingPosition = false;
        bool followingRotation = false;
        // the last step of a frame put the followed parts at the Transform
        bool followReached = false;
#ifdef JASPERENGINE_PHYSICS_2D
        // the velocities the following set: a part stops with them unless a script changed them
        b2Vec2 followedLinearVelocity = b2Vec2_zero;
        float followedAngularVelocity = 0.0f;
        // pose before the last step, drawn towards the current one
        b2Transform previousTransform = b2Transform_identity;
        // linear velocity before the last step (m/s): Collision2D.relativeVelocity of an Enter
        b2Vec2 preStepLinearVelocity = b2Vec2_zero;
#endif
        // the contact messages its scripts (and its colliders' receivers') define
        // (PhysicsCallbacks bits): the shapes' Box2D events follow it
        uint16_t listenerMask = 0;
        // world pose physics and the Transform last agreed on (written to the Transform, or
        // the body put at it): a Transform still there was moved by nobody else since
        bool hasSyncedPose = false;
        Vector3 syncedPosition;
        Quaternion syncedRotation;
        // the state of the body it had in another scene (persistence moves a GameObject with
        // its body), in Box2D units: the next body made for it starts with these, once
        bool hasPendingVelocity = false;
        Vector2 pendingLinearVelocity = Vector2::ZERO;
        float pendingAngularVelocity = 0.0f;
        bool pendingAwake = true;
    };

}

#endif //RIGIDBODY2D_COMPONENT_H
