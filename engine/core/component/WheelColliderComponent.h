// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WHEEL_COLLIDER_COMPONENT_H
#define WHEEL_COLLIDER_COMPONENT_H

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Vector3.h"
#include "math/Quaternion.h"
#include "util/Angle.h"

namespace jasperengine{

    // Unity's WheelFrictionCurve: the tire force (times the wheel's load) at a slip, rising from
    // (0, 0) to the extremum, falling to the asymptote and constant after it, all times stiffness.
    // Forward slip is the slip ratio, sideways slip the slip angle in radians (docs/mcp/guide/physics.md).
    struct JASPERENGINE_API WheelFrictionCurve{
        float extremumSlip = 0.4f;
        float extremumValue = 1.0f;
        float asymptoteSlip = 0.8f;
        float asymptoteValue = 0.5f;
        float stiffness = 1.0f;

        bool operator==(const WheelFrictionCurve& o) const{
            return extremumSlip == o.extremumSlip && extremumValue == o.extremumValue && asymptoteSlip == o.asymptoteSlip &&
                   asymptoteValue == o.asymptoteValue && stiffness == o.stiffness;
        }
        bool operator!=(const WheelFrictionCurve& o) const{ return !(*this == o); }
    };

    // Unity's JointSpring as the WheelCollider's suspension: N/m, N s/m, and the rest point as a
    // fraction of the travel from full droop (0) to full compression (1)
    struct JASPERENGINE_API JointSpring3D{
        float spring = 35000.0f;
        float damper = 4500.0f;
        float targetPosition = 0.5f;

        bool operator==(const JointSpring3D& o) const{
            return spring == o.spring && damper == o.damper && targetPosition == o.targetPosition;
        }
        bool operator!=(const JointSpring3D& o) const{ return !(*this == o); }
    };

    // Unity's WheelHit: what a grounded wheel touched in the last fixed step (world space)
    struct JASPERENGINE_API WheelHit{
        Vector3 point;
        Vector3 normal;
        Vector3 forwardDir;
        Vector3 sidewaysDir;
        float force = 0.0f;             // N along the normal (the suspension's push)
        float forwardSlip = 0.0f;       // slip ratio, positive when the wheel spins faster than the ground
        float sidewaysSlip = 0.0f;      // slip angle (radians), positive when sliding to the right
        Entity colliderEntity = NULL_ENTITY;
        int colliderIndex = -1;
    };

    // A wheel of the vehicle made of the dynamic Rigidbody above it (Unity's WheelCollider), on
    // Jolt's vehicle constraint (VehicleSystem). The suspension hangs from the GameObject's
    // position + center (in its local space) down the body's down axis by suspensionDistance;
    // the wheel rolls along the GameObject's forward. It is not a collider: a cylinder cast finds
    // the ground. Sizes in metres. driveTransform puts the GameObject at getWorldPose() after
    // each step, so a wheel mesh below it follows (interpolated with the body).
    struct JASPERENGINE_API WheelColliderComponent{
        bool enabled = true;

        Vector3 center = Vector3(0.0f, 0.0f, 0.0f);
        float radius = 0.5f;
        float mass = 20.0f;                     // kg: its inertia is 0.5 mass radius^2
        float wheelDampingRate = 0.25f;         // N m s: the spin's own drag
        float suspensionDistance = 0.3f;        // the travel (m)
        float forceAppPointDistance = 0.0f;     // m above the wheel's lowest point where the forces push (0: the contact)
        JointSpring3D suspensionSpring;
        WheelFrictionCurve forwardFriction;
        WheelFrictionCurve sidewaysFriction{0.2f, 1.0f, 0.5f, 0.75f, 1.0f};
        // the limits a VehicleDrivetrain drives the wheel within (unused without one)
        float maxSteerAngle = Angle::degToDefault(70.0f);
        float maxBrakeTorque = 1500.0f;
        float maxHandBrakeTorque = 4000.0f;
        bool driveTransform = true;

        // ---- runtime inputs (not saved): N m, N m, the engine's angle unit (positive: right).
        // With a VehicleDrivetrain they read what it applied.
        float motorTorque = 0.0f;
        float brakeTorque = 0.0f;
        float steerAngle = 0.0f;

        // ---- runtime read-outs of the last fixed step (not saved)
        bool isGrounded = false;
        float rpm = 0.0f;
        float sprungMass = 0.0f;                // kg of the body this wheel holds up
        float suspensionCompression = 0.0f;     // 0 full droop .. 1 full compression
        WheelHit hit;
        Vector3 worldPosePosition;
        Quaternion worldPoseRotation;
        // the body it is a wheel of, and its index in that vehicle (-1: none)
        Entity vehicleBody = NULL_ENTITY;
        int wheelIndex = -1;

        // ---- VehicleSystem's own state
        // the GameObject's pose relative to the body when it was not yet driven, and the local
        // pose last written to its Transform (a different one: something else moved it)
        bool hasRestPose = false;
        Vector3 restPosition;
        Quaternion restRotation;
        Vector3 restScale = Vector3(1.0f, 1.0f, 1.0f);
        bool hasWrittenPose = false;
        Vector3 writtenPosition;
        Quaternion writtenRotation;
    };

}

#endif //WHEEL_COLLIDER_COMPONENT_H
