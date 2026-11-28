// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VehicleDrivetrain.h"

using namespace jasperengine;

VehicleDrivetrain::VehicleDrivetrain(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

VehicleDrivetrain::~VehicleDrivetrain() = default;
VehicleDrivetrain::VehicleDrivetrain(const VehicleDrivetrain& rhs) = default;
VehicleDrivetrain& VehicleDrivetrain::operator=(const VehicleDrivetrain& rhs) = default;
VehicleDrivetrain::VehicleDrivetrain(VehicleDrivetrain&& rhs) noexcept = default;
VehicleDrivetrain& VehicleDrivetrain::operator=(VehicleDrivetrain&& rhs) noexcept = default;

VehicleDrivetrainComponent& VehicleDrivetrain::component() const{
    return getComponent<VehicleDrivetrainComponent>();
}

bool VehicleDrivetrain::isValid() const{
    return scene && scene->findComponent<VehicleDrivetrainComponent>(entity) != nullptr;
}

void VehicleDrivetrain::setEnabled(bool enabled){ component().enabled = enabled; }
bool VehicleDrivetrain::isEnabled() const{ return component().enabled; }

void VehicleDrivetrain::setThrottle(float throttle){ component().throttle = throttle; }
float VehicleDrivetrain::getThrottle() const{ return component().throttle; }
void VehicleDrivetrain::setSteer(float steer){ component().steer = steer; }
float VehicleDrivetrain::getSteer() const{ return component().steer; }
void VehicleDrivetrain::setBrake(float brake){ component().brake = brake; }
float VehicleDrivetrain::getBrake() const{ return component().brake; }
void VehicleDrivetrain::setHandBrake(float handBrake){ component().handBrake = handBrake; }
float VehicleDrivetrain::getHandBrake() const{ return component().handBrake; }

void VehicleDrivetrain::setGear(int gear){
    VehicleDrivetrainComponent& drivetrain = component();
    drivetrain.requestedGear = gear;
    drivetrain.gearRequested = true;
}

float VehicleDrivetrain::getRpm() const{ return component().rpm; }
int VehicleDrivetrain::getGear() const{ return component().gear; }
float VehicleDrivetrain::getSpeed() const{ return component().speed; }

// a changed setting builds the vehicle again at the next step (VehicleSystem compares them)
void VehicleDrivetrain::setMaxTorque(float torque){ component().maxTorque = torque; }
float VehicleDrivetrain::getMaxTorque() const{ return component().maxTorque; }
void VehicleDrivetrain::setMinRPM(float rpm){ component().minRPM = rpm; }
float VehicleDrivetrain::getMinRPM() const{ return component().minRPM; }
void VehicleDrivetrain::setMaxRPM(float rpm){ component().maxRPM = rpm; }
float VehicleDrivetrain::getMaxRPM() const{ return component().maxRPM; }
void VehicleDrivetrain::setTransmission(VehicleTransmissionMode mode){ component().transmission = mode; }
VehicleTransmissionMode VehicleDrivetrain::getTransmission() const{ return component().transmission; }
void VehicleDrivetrain::setShiftUpRPM(float rpm){ component().shiftUpRPM = rpm; }
float VehicleDrivetrain::getShiftUpRPM() const{ return component().shiftUpRPM; }
void VehicleDrivetrain::setShiftDownRPM(float rpm){ component().shiftDownRPM = rpm; }
float VehicleDrivetrain::getShiftDownRPM() const{ return component().shiftDownRPM; }
void VehicleDrivetrain::setClutchStrength(float strength){ component().clutchStrength = strength; }
float VehicleDrivetrain::getClutchStrength() const{ return component().clutchStrength; }
void VehicleDrivetrain::setMaxPitchRollAngle(float angle){ component().maxPitchRollAngle = angle; }
float VehicleDrivetrain::getMaxPitchRollAngle() const{ return component().maxPitchRollAngle; }
