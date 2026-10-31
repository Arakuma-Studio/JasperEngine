// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JOINT3D_COMPONENT_H
#define JOINT3D_COMPONENT_H

#include "Engine.h"
#include "ecs/Entity.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "component/PhysicsCommon.h"
#include "util/Angle.h"
#include <limits>
#include <vector>
#ifdef JASPERENGINE_PHYSICS_3D
#include "Jolt/Jolt.h"
#include "Jolt/Physics/Constraints/TwoBodyConstraint.h"
#endif

namespace jasperengine{

    // Unity: FixedJoint = FIXED, HingeJoint = HINGE, CharacterJoint = SWINGTWIST, SpringJoint =
    // DISTANCE with useSpring, ConfigurableJoint = SIXDOF
    enum class Joint3DType{
        FIXED,
        DISTANCE,
        POINT,
        HINGE,
        CONE,
        PRISMATIC, // slider in Jolt
        SWINGTWIST,
        SIXDOF,
        PATH,
        GEAR,
        RACKANDPINON,
        PULLEY
    };

    // A SIXDOF axis (Unity ConfigurableJoint xMotion...)
    enum class JointMotion{
        LOCKED,
        LIMITED,
        FREE
    };

    // How a SWINGTWIST joint limits its swing: an elliptic cone or a pyramid
    enum class JointSwingType{
        CONE,
        PYRAMID
    };

    // One component for every 3D joint type; the fields a type does not use are kept (hidden in
    // the Inspector). bodyA is Unity's "this" (the joint's Rigidbody), bodyB the connected body.
    // Units: metres, the engine's angle unit (per second for angular speeds), N, N m, kg.
    struct JASPERENGINE_API Joint3DComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

#ifdef JASPERENGINE_PHYSICS_3D
        JPH::TwoBodyConstraint* joint = NULL;
#endif
        Joint3DType type = Joint3DType::FIXED;

        Entity bodyA = NULL_ENTITY;
        Entity bodyB = NULL_ENTITY;

        // LOCAL (new joints, Unity): `anchor` (and anchorA) in bodyA's local space, anchorB in
        // bodyB's (the connected anchor), the axes in bodyA's frame, in GameObject-local units
        // like collider positions. WORLD (scenes saved before): world points and axes, taken
        // when the joint is made.
        JointAnchorSpace anchorSpace = JointAnchorSpace::LOCAL;

        Vector3 anchorA = Vector3::ZERO;
        Vector3 anchorB = Vector3::ZERO;
        Vector3 anchor = Vector3::ZERO;
        Vector3 axis = Vector3::ZERO;
        Vector3 normal = Vector3::ZERO;
        Vector3 twistAxis = Vector3::ZERO;
        Vector3 planeAxis = Vector3::ZERO;
        Vector3 axisX = Vector3::ZERO;
        Vector3 axisY = Vector3::ZERO;

        // HINGE (angles) and PRISMATIC (metres) limits, with useLimits
        float limitsMin = 0.0f;
        float limitsMax = 0.0f;
        float normalHalfConeAngle = 45.0f;
        float planeHalfConeAngle = 45.0f;
        float twistMinAngle = -45.0f;
        float twistMaxAngle = 45.0f;

        Vector3 fixedPointA = Vector3::ZERO;
        Vector3 fixedPointB = Vector3::ZERO;

        Entity hingeA = NULL_ENTITY;
        Entity hingeB = NULL_ENTITY;
        Entity hinge = NULL_ENTITY;
        Entity slider = NULL_ENTITY;

        int numTeethGearA = 20;
        int numTeethGearB = 20;
        int numTeethRack = 20;
        int numTeethGear = 20;
        int rackLength = 100;

        std::vector<Vector3> pathPoints;
        Vector3 pathPosition = Vector3::ZERO;
        bool isLooping = false;
        // LOCAL: anchorB is where `anchor` is when the joint is made (Unity
        // autoConfigureConnectedAnchor); WORLD: the bodies' origins for DISTANCE
        bool autoAnchors = true;

        // the connected bodies collide with each other (Unity enableCollision)
        bool enableCollision = false;

        // Unity breakForce / breakTorque: infinite never breaks
        float breakForce = std::numeric_limits<float>::infinity();
        float breakTorque = std::numeric_limits<float>::infinity();
        JointBreakAction breakAction = JointBreakAction::DESTROY;

        // HINGE, PRISMATIC
        bool useLimits = false;
        // soft limits (HINGE, PRISMATIC, DISTANCE): 0 = hard
        float limitsSpringFrequency = 0.0f;
        float limitsSpringDamping = 0.0f;

        // HINGE (angular speed, torque), PRISMATIC (m/s, force), SWINGTWIST (to motorTargetRotation)
        bool useMotor = false;
        float motorTargetVelocity = 0.0f;
        float motorMaxForce = 1000.0f;

        // HINGE, PRISMATIC: a spring to springTargetPosition (Unity HingeJoint.spring);
        // DISTANCE: a soft rope between minDistance and maxDistance (Unity SpringJoint)
        bool useSpring = false;
        float springStiffness = 0.0f;
        float springDamper = 0.0f;
        float springTargetPosition = 0.0f;

        // HINGE, SWINGTWIST (N m), PRISMATIC (N): friction when no motor drives it
        float maxFriction = 0.0f;

        // DISTANCE: -1 = the distance when the joint is made
        float minDistance = -1.0f;
        float maxDistance = -1.0f;

        // CONE
        float coneHalfAngle = Angle::degToDefault(45.0f);

        // SWINGTWIST
        JointSwingType swingType = JointSwingType::CONE;
        // the orientation the motor drives to, in the joint's frame
        Quaternion motorTargetRotation = Quaternion::IDENTITY;
        float motorSpringFrequency = 20.0f;
        float motorSpringDamping = 1.0f;

        // SIXDOF (Unity ConfigurableJoint): per axis, in the joint's frame (axisX, axisY)
        JointMotion xMotion = JointMotion::FREE;
        JointMotion yMotion = JointMotion::FREE;
        JointMotion zMotion = JointMotion::FREE;
        JointMotion angularXMotion = JointMotion::FREE;
        JointMotion angularYMotion = JointMotion::FREE;
        JointMotion angularZMotion = JointMotion::FREE;
        // the LIMITED axes' ranges (metres, angles)
        Vector3 linearLimitMin = Vector3::ZERO;
        Vector3 linearLimitMax = Vector3::ZERO;
        Vector3 angularLimitMin = Vector3::ZERO;
        Vector3 angularLimitMax = Vector3::ZERO;
        // drives of the axes that are not locked, to the targets (Unity's drives)
        bool useDrive = false;
        float driveStiffness = 0.0f;
        float driveDamping = 0.0f;
        float angularDriveStiffness = 0.0f;
        float angularDriveDamping = 0.0f;
        float driveMaxForce = std::numeric_limits<float>::infinity();
        float driveMaxTorque = std::numeric_limits<float>::infinity();
        Vector3 targetPosition = Vector3::ZERO;
        Quaternion targetRotation = Quaternion::IDENTITY;
        Vector3 targetVelocity = Vector3::ZERO;
        Vector3 targetAngularVelocity = Vector3::ZERO;

        // Runtime, not saved
        bool needUpdateJoint = true;
        // the last try to make it failed (logged once, until it is made)
        bool loadFailed = false;
        // the parameters changed: applied to the live joint before the next step
        bool needApplyParameters = false;
        // HINGE (radians) / PRISMATIC (metres): the centre of the limits the frame was turned or
        // shifted by, so a range not containing 0 works; read-outs add it back
        float limitsCenter = 0.0f;
        // the pair the joint keeps from colliding (enableCollision false), released with it
        Entity ignoredA = NULL_ENTITY;
        Entity ignoredB = NULL_ENTITY;
        // the force and torque of the last step (N, N m)
        Vector3 currentForce = Vector3::ZERO;
        Vector3 currentTorque = Vector3::ZERO;
        // breaking reports once until the load goes back below the thresholds (CALLBACK_ONLY)
        bool breakArmed = true;
        // DISABLE / DESTROY broke it; cleared when the joint is made again
        bool broken = false;
    };

    // Fields whose default depends on the type: Unity's for new joints
    inline void setJoint3DTypeDefaults(Joint3DComponent& joint, Joint3DType type){
        if (type == Joint3DType::HINGE){
            joint.limitsMin = Angle::degToDefault(-45.0f);
            joint.limitsMax = Angle::degToDefault(45.0f);
        }else{
            joint.limitsMin = 0.0f;
            joint.limitsMax = 0.0f;
        }
    }

    // A new joint of the type with Unity's defaults
    inline Joint3DComponent makeJoint3D(Joint3DType type){
        Joint3DComponent joint;
        joint.type = type;
        setJoint3DTypeDefaults(joint, type);
        return joint;
    }

    // Changes the type keeping what was edited: a type-dependent field still at the old type's
    // default takes the new type's
    inline void changeJoint3DType(Joint3DComponent& joint, Joint3DType type){
        if (joint.type == type){
            return;
        }
        Joint3DComponent before = makeJoint3D(joint.type);
        Joint3DComponent after = makeJoint3D(type);
        if (joint.limitsMin == before.limitsMin && joint.limitsMax == before.limitsMax){
            joint.limitsMin = after.limitsMin;
            joint.limitsMax = after.limitsMax;
        }
        joint.type = type;
    }

    // What a joint had before these fields existed (Jolt's defaults and the old engine code):
    // the values of keys missing from scenes saved before them, and of the Lua set*Joint helpers
    inline void setJoint3DLegacyParameters(Joint3DComponent& joint, Joint3DType type){
        joint.type = type;
        joint.anchorSpace = JointAnchorSpace::WORLD;
        // Jolt never stopped the connected bodies from colliding
        joint.enableCollision = true;
        joint.breakForce = std::numeric_limits<float>::infinity();
        joint.breakTorque = std::numeric_limits<float>::infinity();
        joint.breakAction = JointBreakAction::DESTROY;
        joint.useLimits = (type == Joint3DType::PRISMATIC);
        joint.limitsSpringFrequency = 0.0f;
        joint.limitsSpringDamping = 0.0f;
        joint.useMotor = false;
        joint.motorTargetVelocity = 0.0f;
        joint.useSpring = false;
        joint.maxFriction = 0.0f;
        joint.minDistance = -1.0f;
        joint.maxDistance = -1.0f;
        // the cone joint's swing limit was left 0 before (a bug): Unity's 45 degrees
        joint.coneHalfAngle = Angle::degToDefault(45.0f);
        joint.swingType = JointSwingType::CONE;
        joint.xMotion = joint.yMotion = joint.zMotion = JointMotion::FREE;
        joint.angularXMotion = joint.angularYMotion = joint.angularZMotion = JointMotion::FREE;
        joint.useDrive = false;
    }

}

#endif //JOINT3D_COMPONENT_H
