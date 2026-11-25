// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WHEEL_COLLIDER_CONTROLLER_H
#define WHEEL_COLLIDER_CONTROLLER_H

// The controller of a vehicle made of WheelColliders (docs/specs/S96-physics-gaps.md REQ-3, REQ-5):
// Jolt's WheeledVehicleController driven either by each wheel's own motor torque, brake torque
// and steer angle (Unity's WheelCollider model), or by Jolt's engine, gear box and differentials
// (a VehicleDrivetrain). It reaches into WheeledVehicleController's protected members, which are
// those of Jolt 5.2: a Jolt upgrade is checked by engine/tests/core/physics/VehicleTest.cpp.

#ifdef JASPERENGINE_PHYSICS_3D

#include "Export.h"

#include "Jolt/Jolt.h"
#include "Jolt/Physics/Vehicle/WheeledVehicleController.h"

#include <vector>

namespace jasperengine{

    // a wheel's inputs for one step (Unity's model): N m, N m, radians (positive turns right)
    struct WheelDriveInput{
        float motorTorque = 0.0f;
        float brakeTorque = 0.0f;
        float steer = 0.0f;
    };

    class JASPERENGINE_API WheelColliderControllerSettings : public JPH::WheeledVehicleControllerSettings{
    public:
        // each wheel driven by its own inputs, not by the engine
        bool perWheelDrive = true;
        // the body's own allowSleeping: the vehicle never lets a body sleep that may not
        bool bodyAllowsSleep = true;

        JPH::VehicleController* ConstructController(JPH::VehicleConstraint& constraint) const override;
    };

    class JASPERENGINE_API WheelColliderController : public JPH::WheeledVehicleController{
    private:
        bool perWheelDrive;
        bool bodyAllowsSleep;
        std::vector<WheelDriveInput> inputs;
        // the engine's torque at each wheel in the last step (drivetrain), estimated at the clutch
        std::vector<float> driveTorques;

        void estimateDriveTorques();
        // the slip ratio and the tire's friction again, with Unity's (PhysX's) slip ratio
        void unitySlip(JPH::uint index, JPH::WheelWV& wheel, JPH::PhysicsSystem& physicsSystem);

    protected:
        bool AllowSleep() const override;
        void PreCollide(float deltaTime, JPH::PhysicsSystem& physicsSystem) override;
        void PostCollide(float deltaTime, JPH::PhysicsSystem& physicsSystem) override;

    public:
        WheelColliderController(const WheelColliderControllerSettings& settings, JPH::VehicleConstraint& constraint);

        bool isPerWheelDrive() const { return perWheelDrive; }
        // written between steps only (Jolt's job threads read them in the step)
        void setWheelInput(size_t wheel, const WheelDriveInput& input);
        float getDriveTorque(size_t wheel) const;
    };

}

#endif

#endif //WHEEL_COLLIDER_CONTROLLER_H
