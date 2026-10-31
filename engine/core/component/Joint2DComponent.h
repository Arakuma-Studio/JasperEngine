// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JOINT2D_COMPONENT_H
#define JOINT2D_COMPONENT_H

#include "Engine.h"
#include "ecs/Entity.h"
#include "math/Vector2.h"
#include "component/PhysicsCommon.h"
#ifdef JASPERENGINE_PHYSICS_2D
#include "box2d/box2d.h"
#endif

#include <limits>

namespace jasperengine{

    // Unity: WheelJoint2D = WHEEL, HingeJoint2D = REVOLUTE, SliderJoint2D = PRISMATIC,
    // DistanceJoint2D = DISTANCE (maxDistanceOnly = rope), SpringJoint2D = DISTANCE with
    // enableSpring, FixedJoint2D = WELD, RelativeJoint2D = MOTOR, TargetJoint2D = MOUSE
    enum class Joint2DType{
        DISTANCE,
        REVOLUTE,
        PRISMATIC,
        //PULLEY,
        //GEAR,
        MOUSE,
        WHEEL,
        WELD,
        MOTOR
    };

    // Unity limitState: where the joint value is against its limits
    enum class JointLimitState2D{
        INACTIVE,
        LOWER,
        UPPER,
        EQUAL
    };

    // One component for every 2D joint type; the fields a type does not use are kept (hidden in
    // the Inspector). bodyA is Unity's "this" (the joint's Rigidbody 2D), bodyB the connected
    // body (none: the joint holds bodyA to the world).
    // Units: lengths, anchors, translations and limits in points, angles in the engine's angle
    // unit, angular speeds per second, linear speeds in points/s, forces N, torques N m,
    // frequencies Hz.
    struct JASPERENGINE_API Joint2DComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

#ifdef JASPERENGINE_PHYSICS_2D
        b2JointId joint = b2_nullJointId;
#endif
        Joint2DType type = Joint2DType::DISTANCE;

        Entity bodyA = NULL_ENTITY;
        Entity bodyB = NULL_ENTITY;

        // LOCAL (new joints): anchorA in bodyA's local space, anchorB in bodyB's, axis in bodyA's
        // frame, in GameObject-local units like collider points. WORLD (scenes saved before):
        // world points and a world axis, taken at creation.
        JointAnchorSpace anchorSpace = JointAnchorSpace::LOCAL;
        Vector2 anchorA = Vector2::ZERO;
        Vector2 anchorB = Vector2::ZERO;
        // LOCAL: anchorB is where anchorA is when the joint is made (Unity
        // autoConfigureConnectedAnchor); WORLD: the bodies' origins for DISTANCE
        bool autoAnchors = true;
        // WHEEL and PRISMATIC: the line of motion, (0, 1) = Unity's suspension angle 90
        Vector2 axis = Vector2(0.0f, 1.0f);
        // MOUSE: the world point the body is pulled to
        Vector2 target = Vector2::ZERO;

        bool enableCollision = false;

        // Unity breakForce / breakTorque: infinite never breaks
        float breakForce = std::numeric_limits<float>::infinity();
        float breakTorque = std::numeric_limits<float>::infinity();
        JointBreakAction breakAction = JointBreakAction::DESTROY;

        // REVOLUTE: angles; PRISMATIC, WHEEL: translations; DISTANCE: min/max length
        bool useLimits = false;
        float lowerAngle = -90.0f;
        float upperAngle = 90.0f;
        float lowerTranslation = 0.0f;
        float upperTranslation = 0.0f;

        // REVOLUTE, WHEEL (torque, angular speed); PRISMATIC, DISTANCE (force, linear speed)
        bool useMotor = false;
        float motorSpeed = 0.0f;
        float maxMotorTorque = 10000.0f;
        float maxMotorForce = 10000.0f;

        // WHEEL suspension, REVOLUTE / PRISMATIC / DISTANCE spring, MOUSE stiffness
        bool enableSpring = false;
        float frequency = 1.0f;
        float dampingRatio = 0.5f;

        // DISTANCE
        float length = 0.0f;
        bool autoLength = true;
        float minLength = 0.0f;
        // 0: the length
        float maxLength = 0.0f;
        // Unity maxDistanceOnly: the length is a maximum, the bodies can get closer
        bool rope = false;

        // WELD stiffness: 0 = rigid
        float linearFrequency = 0.0f;
        float angularFrequency = 0.0f;
        float linearDampingRatio = 0.0f;
        float angularDampingRatio = 0.0f;

        // MOTOR: bodyB's pose relative to bodyA's (linear in bodyA's frame), when !autoOffset
        Vector2 linearOffset = Vector2::ZERO;
        float angularOffset = 0.0f;
        bool autoOffset = true;
        // MOTOR; MOUSE (0: max(10, 1000 x bodyA's mass) N)
        float maxForce = 10000.0f;
        float maxTorque = 10000.0f;
        float correctionFactor = 0.3f;

        // Runtime, not saved
        bool needUpdateJoint = true;
        // the parameters changed: applied to the live joint before the next step
        bool needApplyParameters = false;
        // bodyB angle minus bodyA angle when the joint was made (radians)
        float referenceAngle = 0.0f;
        // WHEEL, PRISMATIC: the unit axis in bodyA's frame the joint was made with
        Vector2 localAxisA = Vector2(1.0f, 0.0f);
        // breaking reports once until the load goes back below the thresholds (CALLBACK_ONLY)
        bool breakArmed = true;
        // DISABLE / DESTROY broke it; cleared when the joint is made again
        bool broken = false;
    };

    // Fields whose default depends on the type: Unity's for new joints
    inline void setJoint2DTypeDefaults(Joint2DComponent& joint, Joint2DType type){
        joint.enableSpring = (type == Joint2DType::WHEEL);
        joint.frequency = (type == Joint2DType::WHEEL) ? 2.0f : (type == Joint2DType::MOUSE ? 5.0f : 1.0f);
        joint.dampingRatio = (type == Joint2DType::WHEEL) ? 0.7f : (type == Joint2DType::MOUSE ? 1.0f : 0.5f);
        joint.maxForce = (type == Joint2DType::MOUSE) ? 0.0f : 10000.0f;
    }

    // A new joint of the type with Unity's defaults
    inline Joint2DComponent makeJoint2D(Joint2DType type){
        Joint2DComponent joint;
        joint.type = type;
        setJoint2DTypeDefaults(joint, type);
        return joint;
    }

    // Changes the type keeping what was edited: a type-dependent field still at the old type's
    // default takes the new type's
    inline void changeJoint2DType(Joint2DComponent& joint, Joint2DType type){
        if (joint.type == type){
            return;
        }
        Joint2DComponent before = makeJoint2D(joint.type);
        Joint2DComponent after = makeJoint2D(type);
        if (joint.enableSpring == before.enableSpring) joint.enableSpring = after.enableSpring;
        if (joint.frequency == before.frequency) joint.frequency = after.frequency;
        if (joint.dampingRatio == before.dampingRatio) joint.dampingRatio = after.dampingRatio;
        if (joint.maxForce == before.maxForce) joint.maxForce = after.maxForce;
        joint.type = type;
    }

    // What a joint of the type had before these fields existed (Box2D's defaults and the old
    // engine code): the values of keys missing from scenes saved before them, and of the Lua
    // set*Joint helpers, so old scenes and scripts behave as they did
    inline void setJoint2DLegacyParameters(Joint2DComponent& joint, Joint2DType type){
        joint.type = type;
        joint.anchorSpace = JointAnchorSpace::WORLD;
        joint.enableCollision = false;
        joint.useLimits = false;
        joint.lowerAngle = 0.0f;
        joint.upperAngle = 0.0f;
        joint.lowerTranslation = 0.0f;
        joint.upperTranslation = 0.0f;
        joint.useMotor = false;
        joint.motorSpeed = 0.0f;
        joint.maxMotorTorque = 0.0f;
        joint.maxMotorForce = 0.0f;
        joint.enableSpring = (type == Joint2DType::WHEEL);
        joint.frequency = (type == Joint2DType::WHEEL) ? 1.0f : (type == Joint2DType::MOUSE ? 4.0f : 0.0f);
        joint.dampingRatio = (type == Joint2DType::WHEEL) ? 0.7f : (type == Joint2DType::MOUSE ? 1.0f : 0.0f);
        joint.length = 0.0f;
        joint.autoLength = true;
        joint.minLength = 0.0f;
        joint.maxLength = 0.0f;
        joint.linearFrequency = 0.0f;
        joint.angularFrequency = 0.0f;
        joint.linearDampingRatio = 0.0f;
        joint.angularDampingRatio = 0.0f;
        joint.linearOffset = Vector2::ZERO;
        joint.angularOffset = 0.0f;
        joint.autoOffset = false;
        joint.maxForce = (type == Joint2DType::MOTOR) ? 1.0f : 0.0f;
        joint.maxTorque = 1.0f;
        joint.correctionFactor = 0.3f;
    }

    inline Joint2DComponent makeLegacyJoint2D(Joint2DType type){
        Joint2DComponent joint;
        setJoint2DLegacyParameters(joint, type);
        // stored in every old file, the struct defaults of then
        joint.axis = Vector2::ZERO;
        joint.autoAnchors = true;
        joint.rope = false;
        return joint;
    }

}

#endif //JOINT2D_COMPONENT_H
