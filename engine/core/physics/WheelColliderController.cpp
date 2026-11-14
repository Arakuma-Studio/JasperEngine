// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "WheelColliderController.h"

#ifdef JASPERENGINE_PHYSICS_3D

#include "Jolt/Physics/Body/BodyLockInterface.h"
#include "Jolt/Physics/PhysicsSystem.h"
#include "Jolt/Physics/Vehicle/VehicleConstraint.h"
#include "Jolt/Physics/Vehicle/VehicleEngine.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace{

    // PhysX's (Unity's) least slip ratio denominator: below this ground speed a spin counts against
    // it, so a wheel pulling away from rest is in the rising part of its curve, not sliding at its
    // asymptote (Jolt divides by the ground speed alone, any spin at rest is an endless slip)
    constexpr float WHEEL_MIN_LONGITUDINAL_SLIP_SPEED = 4.0f;

}

JPH::VehicleController* WheelColliderControllerSettings::ConstructController(JPH::VehicleConstraint& constraint) const{
    return new WheelColliderController(*this, constraint);
}

WheelColliderController::WheelColliderController(const WheelColliderControllerSettings& settings, JPH::VehicleConstraint& constraint):
    JPH::WheeledVehicleController(settings, constraint),
    perWheelDrive(settings.perWheelDrive),
    bodyAllowsSleep(settings.bodyAllowsSleep){
}

void WheelColliderController::setWheelInput(size_t wheel, const WheelDriveInput& input){
    if (wheel >= inputs.size()){
        inputs.resize(wheel + 1);
    }
    inputs[wheel] = input;
}

float WheelColliderController::getDriveTorque(size_t wheel) const{
    return wheel < driveTorques.size() ? driveTorques[wheel] : 0.0f;
}

bool WheelColliderController::AllowSleep() const{
    if (!bodyAllowsSleep){
        return false;
    }
    // a wheel in the air keeps it awake (it may be about to land)
    for (const JPH::Wheel* wheel : mConstraint.GetWheels()){
        if (!wheel->HasContact()){
            return false;
        }
    }
    if (!perWheelDrive){
        return JPH::WheeledVehicleController::AllowSleep();
    }
    for (const WheelDriveInput& input : inputs){
        if (input.motorTorque != 0.0f){
            return false;
        }
    }
    return true;
}

void WheelColliderController::PreCollide(float deltaTime, JPH::PhysicsSystem& physicsSystem){
    if (!perWheelDrive){
        JPH::WheeledVehicleController::PreCollide(deltaTime, physicsSystem);
        return;
    }
    // Jolt turns a wheel about the body's up by a positive angle towards +X, the engine's right
    constexpr float maxSteer = 0.4999f * JPH::JPH_PI;
    JPH::Wheels& wheels = mConstraint.GetWheels();
    for (size_t i = 0; i < wheels.size(); i++){
        const float steer = i < inputs.size() ? inputs[i].steer : 0.0f;
        wheels[i]->SetSteerAngle(std::clamp(steer, -maxSteer, maxSteer));
    }
}

void WheelColliderController::PostCollide(float deltaTime, JPH::PhysicsSystem& physicsSystem){
    if (!perWheelDrive){
        JPH::WheeledVehicleController::PostCollide(deltaTime, physicsSystem);
        // the tire forces are solved after this: they take Unity's slip too
        JPH::Wheels& driven = mConstraint.GetWheels();
        for (JPH::uint i = 0; i < (JPH::uint)driven.size(); i++){
            unitySlip(i, *static_cast<JPH::WheelWV*>(driven[i]), physicsSystem);
        }
        estimateDriveTorques();
        return;
    }

    // Unity's model: each wheel spun by its motor torque against its inertia, then braked (as
    // Jolt brakes the engine-driven wheels); the tires' friction curves and the ground then
    // bring it to the ground's speed in SolveLongitudinalAndLateralConstraints
    JPH::Wheels& wheels = mConstraint.GetWheels();
    for (JPH::uint i = 0; i < (JPH::uint)wheels.size(); i++){
        JPH::WheelWV* wheel = static_cast<JPH::WheelWV*>(wheels[i]);
        // the spin's damping, the slips and the friction combined with the ground
        wheel->Update(i, deltaTime, mConstraint);
        unitySlip(i, *wheel, physicsSystem);

        const WheelDriveInput input = i < inputs.size() ? inputs[i] : WheelDriveInput();
        if (input.motorTorque != 0.0f && std::isfinite(input.motorTorque)){
            wheel->ApplyTorque(input.motorTorque, deltaTime);
        }

        const JPH::WheelSettingsWV* settings = wheel->GetSettings();
        const float brake = std::isfinite(input.brakeTorque) ? std::max(0.0f, input.brakeTorque) : 0.0f;
        if (brake > 0.0f){
            // the torque that stops the wheel in this step: more locks it and the rest pushes
            // on the ground through the tire
            const float lockTorque = std::fabs(wheel->GetAngularVelocity()) * settings->mInertia / deltaTime;
            if (brake > lockTorque){
                wheel->SetAngularVelocity(0.0f);
                wheel->mBrakeImpulse = (brake - lockTorque) * deltaTime / settings->mRadius;
            }else{
                wheel->ApplyTorque(-JPH::Sign(wheel->GetAngularVelocity()) * brake, deltaTime);
                wheel->mBrakeImpulse = 0.0f;
            }
        }else{
            wheel->mBrakeImpulse = 0.0f;
        }
    }
    mPreviousDeltaTime = deltaTime;
}

void WheelColliderController::unitySlip(JPH::uint index, JPH::WheelWV& wheel, JPH::PhysicsSystem& physicsSystem){
    if (!wheel.HasContact()){
        return;
    }
    const JPH::Body* ground = physicsSystem.GetBodyLockInterfaceNoLock().TryGetBody(wheel.GetContactBodyID());
    if (!ground){
        return;
    }
    const JPH::WheelSettingsWV* settings = wheel.GetSettings();
    JPH::Vec3 relative = mConstraint.GetVehicleBody()->GetPointVelocity(wheel.GetContactPosition()) - wheel.GetContactPointVelocity();
    relative -= wheel.GetContactNormal().Dot(relative) * wheel.GetContactNormal();
    const float forwardSpeed = relative.Dot(wheel.GetContactLongitudinal());
    const float denominator = std::max(std::fabs(forwardSpeed), WHEEL_MIN_LONGITUDINAL_SLIP_SPEED);
    wheel.mLongitudinalSlip = std::fabs(wheel.GetAngularVelocity() * settings->mRadius - forwardSpeed) / denominator;
    float longitudinal = settings->mLongitudinalFriction.GetValue(wheel.mLongitudinalSlip);
    float lateral = settings->mLateralFriction.GetValue(JPH::RadiansToDegrees(wheel.mLateralSlip));
    mConstraint.GetCombineFriction()(index, longitudinal, lateral, *ground, wheel.GetContactSubShapeID());
    wheel.mCombinedLongitudinalFriction = longitudinal;
    wheel.mCombinedLateralFriction = lateral;
}

// The torque the clutch passes to each driven wheel after the step's engine solve:
// S (we - avg(R ww)) shared by the differentials' torque ratios (Jolt's own model, without the
// limited slip between differentials). A read-out, not fed back.
void WheelColliderController::estimateDriveTorques(){
    const JPH::Wheels& wheels = mConstraint.GetWheels();
    driveTorques.assign(wheels.size(), 0.0f);

    const float transmissionRatio = mTransmission.GetCurrentRatio();
    if (transmissionRatio == 0.0f){
        return;
    }
    float sum = 0.0f;
    int driven = 0;
    for (const JPH::VehicleDifferentialSettings& d : mDifferentials){
        for (int index : {d.mLeftWheel, d.mRightWheel}){
            if (index >= 0 && index < (int)wheels.size()){
                sum += transmissionRatio * d.mDifferentialRatio * wheels[index]->GetAngularVelocity();
                driven++;
            }
        }
    }
    if (driven == 0){
        return;
    }
    const float clutchStrength = mTransmission.GetClutchFriction() * mTransmission.mClutchStrength;
    const float clutchTorque = clutchStrength * (mEngine.GetAngularVelocity() - sum / (float)driven);
    for (const JPH::VehicleDifferentialSettings& d : mDifferentials){
        const bool hasLeft = d.mLeftWheel >= 0 && d.mLeftWheel < (int)wheels.size();
        const bool hasRight = d.mRightWheel >= 0 && d.mRightWheel < (int)wheels.size();
        const float ratio = transmissionRatio * d.mDifferentialRatio;
        if (hasLeft && hasRight){
            float left = 0.5f, right = 0.5f;
            d.CalculateTorqueRatio(wheels[d.mLeftWheel]->GetAngularVelocity(), wheels[d.mRightWheel]->GetAngularVelocity(), left, right);
            driveTorques[d.mLeftWheel] = ratio * d.mEngineTorqueRatio * left * clutchTorque;
            driveTorques[d.mRightWheel] = ratio * d.mEngineTorqueRatio * right * clutchTorque;
        }else if (hasLeft){
            driveTorques[d.mLeftWheel] = ratio * d.mEngineTorqueRatio * clutchTorque;
        }else if (hasRight){
            driveTorques[d.mRightWheel] = ratio * d.mEngineTorqueRatio * clutchTorque;
        }
    }
}

#endif
