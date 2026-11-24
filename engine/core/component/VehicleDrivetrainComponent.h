// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VEHICLE_DRIVETRAIN_COMPONENT_H
#define VEHICLE_DRIVETRAIN_COMPONENT_H

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Curve.h"
#include "util/Angle.h"

#include <vector>

namespace jasperengine{

    enum class VehicleTransmissionMode{
        AUTO,       // shifts at shiftUpRPM / shiftDownRPM; throttle -1 reverses
        MANUAL      // the gear setGear chose
    };

    // Two wheels the engine drives through one ratio (a wheel may be none)
    struct JASPERENGINE_API VehicleDifferential{
        Entity leftWheel = NULL_ENTITY;
        Entity rightWheel = NULL_ENTITY;
        float ratio = 3.42f;                // gear box to wheels
        float leftRightSplit = 0.5f;        // 0 all to the left wheel, 1 all to the right
        float limitedSlipRatio = 1.4f;      // max / min wheel speed before the slower one gets it all
        float engineTorqueRatio = 1.0f;     // its share of the engine's torque

        bool operator==(const VehicleDifferential& o) const{
            return leftWheel == o.leftWheel && rightWheel == o.rightWheel && ratio == o.ratio && leftRightSplit == o.leftRightSplit &&
                   limitedSlipRatio == o.limitedSlipRatio && engineTorqueRatio == o.engineTorqueRatio;
        }
        bool operator!=(const VehicleDifferential& o) const{ return !(*this == o); }
    };

    // Keeps two wheels' suspensions at the same compression (N/m of difference)
    struct JASPERENGINE_API VehicleAntiRollBar{
        Entity leftWheel = NULL_ENTITY;
        Entity rightWheel = NULL_ENTITY;
        float stiffness = 1000.0f;

        bool operator==(const VehicleAntiRollBar& o) const{
            return leftWheel == o.leftWheel && rightWheel == o.rightWheel && stiffness == o.stiffness;
        }
        bool operator!=(const VehicleAntiRollBar& o) const{ return !(*this == o); }
    };

    // An engine, a gear box and differentials driving the WheelColliders of the body it is on
    // (beyond Unity, Jolt's WheeledVehicleController): throttle, steer, brake and handBrake drive
    // the wheels within their maxSteerAngle, maxBrakeTorque and maxHandBrakeTorque, and the wheels'
    // motorTorque, brakeTorque and steerAngle read what it applied. Without differentials every
    // axle (the wheels side by side) is driven, the torque shared evenly.
    struct JASPERENGINE_API VehicleDrivetrainComponent{
        bool enabled = true;

        // engine
        float maxTorque = 500.0f;           // N m
        float minRPM = 1000.0f;
        float maxRPM = 6000.0f;
        // the share of maxTorque (value) at a share of maxRPM (time, 0..1): Jolt's default
        Curve torqueCurve = Curve::polyline({{0.0f, 0.8f}, {0.66f, 1.0f}, {1.0f, 0.8f}});
        float engineInertia = 0.5f;         // kg m^2
        float engineDamping = 0.2f;

        // transmission
        VehicleTransmissionMode transmission = VehicleTransmissionMode::AUTO;
        std::vector<float> gearRatios = {2.66f, 1.78f, 1.3f, 1.0f, 0.74f};
        std::vector<float> reverseGearRatios = {-2.9f};
        float switchTime = 0.5f;            // s without torque while shifting (auto)
        float clutchReleaseTime = 0.3f;     // s
        float switchLatency = 0.5f;         // s before the next shift (auto)
        float shiftUpRPM = 4000.0f;
        float shiftDownRPM = 2000.0f;
        float clutchStrength = 10.0f;       // kg m^2 / s

        std::vector<VehicleDifferential> differentials;
        float differentialLimitedSlipRatio = 1.4f;
        std::vector<VehicleAntiRollBar> antiRollBars;
        // the body's up stays within this of the world's up (the engine's angle unit; 180: off)
        float maxPitchRollAngle = Angle::degToDefault(180.0f);

        // ---- runtime inputs (not saved)
        float throttle = 0.0f;              // -1..1 (auto: negative reverses; manual: 0..1)
        float steer = 0.0f;                 // -1..1, positive right
        float brake = 0.0f;                 // 0..1
        float handBrake = 0.0f;             // 0..1
        // setGear's gear for the manual transmission (-1 reverse, 0 neutral, 1.. forward)
        int requestedGear = 1;
        bool gearRequested = false;

        // ---- runtime read-outs of the last fixed step (not saved)
        float rpm = 0.0f;
        int gear = 0;
        float speed = 0.0f;                 // m/s along the body's forward
    };

}

#endif //VEHICLE_DRIVETRAIN_COMPONENT_H
