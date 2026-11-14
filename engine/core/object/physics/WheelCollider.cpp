// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "WheelCollider.h"

#include "component/Transform.h"
#include "subsystem/PhysicsSystem.h"

using namespace jasperengine;

WheelCollider::WheelCollider(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

WheelCollider::~WheelCollider() = default;
WheelCollider::WheelCollider(const WheelCollider& rhs) = default;
WheelCollider& WheelCollider::operator=(const WheelCollider& rhs) = default;
WheelCollider::WheelCollider(WheelCollider&& rhs) noexcept = default;
WheelCollider& WheelCollider::operator=(WheelCollider&& rhs) noexcept = default;

WheelColliderComponent& WheelCollider::component() const{
    return getComponent<WheelColliderComponent>();
}

bool WheelCollider::isValid() const{
    return scene && scene->findComponent<WheelColliderComponent>(entity) != nullptr;
}

void WheelCollider::setEnabled(bool enabled){ component().enabled = enabled; }
bool WheelCollider::isEnabled() const{ return component().enabled; }

// a changed field builds the vehicle again at the next step (VehicleSystem compares them)
void WheelCollider::setCenter(const Vector3& center){ component().center = center; }
Vector3 WheelCollider::getCenter() const{ return component().center; }
void WheelCollider::setRadius(float radius){ component().radius = radius; }
float WheelCollider::getRadius() const{ return component().radius; }
void WheelCollider::setMass(float mass){ component().mass = mass; }
float WheelCollider::getMass() const{ return component().mass; }
void WheelCollider::setWheelDampingRate(float rate){ component().wheelDampingRate = rate; }
float WheelCollider::getWheelDampingRate() const{ return component().wheelDampingRate; }
void WheelCollider::setSuspensionDistance(float distance){ component().suspensionDistance = distance; }
float WheelCollider::getSuspensionDistance() const{ return component().suspensionDistance; }
void WheelCollider::setForceAppPointDistance(float distance){ component().forceAppPointDistance = distance; }
float WheelCollider::getForceAppPointDistance() const{ return component().forceAppPointDistance; }
void WheelCollider::setSuspensionSpring(const JointSpring3D& spring){ component().suspensionSpring = spring; }
JointSpring3D WheelCollider::getSuspensionSpring() const{ return component().suspensionSpring; }
void WheelCollider::setForwardFriction(const WheelFrictionCurve& curve){ component().forwardFriction = curve; }
WheelFrictionCurve WheelCollider::getForwardFriction() const{ return component().forwardFriction; }
void WheelCollider::setSidewaysFriction(const WheelFrictionCurve& curve){ component().sidewaysFriction = curve; }
WheelFrictionCurve WheelCollider::getSidewaysFriction() const{ return component().sidewaysFriction; }
void WheelCollider::setMaxSteerAngle(float angle){ component().maxSteerAngle = angle; }
float WheelCollider::getMaxSteerAngle() const{ return component().maxSteerAngle; }
void WheelCollider::setMaxBrakeTorque(float torque){ component().maxBrakeTorque = torque; }
float WheelCollider::getMaxBrakeTorque() const{ return component().maxBrakeTorque; }
void WheelCollider::setMaxHandBrakeTorque(float torque){ component().maxHandBrakeTorque = torque; }
float WheelCollider::getMaxHandBrakeTorque() const{ return component().maxHandBrakeTorque; }
void WheelCollider::setDriveTransform(bool drive){ component().driveTransform = drive; }
bool WheelCollider::isDriveTransform() const{ return component().driveTransform; }

void WheelCollider::setMotorTorque(float torque){ component().motorTorque = torque; }
float WheelCollider::getMotorTorque() const{ return component().motorTorque; }
void WheelCollider::setBrakeTorque(float torque){ component().brakeTorque = torque; }
float WheelCollider::getBrakeTorque() const{ return component().brakeTorque; }
void WheelCollider::setSteerAngle(float angle){ component().steerAngle = angle; }
float WheelCollider::getSteerAngle() const{ return component().steerAngle; }

bool WheelCollider::isGrounded() const{ return component().isGrounded; }
float WheelCollider::getRpm() const{ return component().rpm; }
float WheelCollider::getSprungMass() const{ return component().sprungMass; }
float WheelCollider::getSuspensionCompression() const{ return component().suspensionCompression; }
Entity WheelCollider::getVehicleBody() const{ return component().vehicleBody; }

bool WheelCollider::getGroundHit(WheelGroundHit& hit) const{
    const WheelColliderComponent& wheel = component();
    hit = WheelGroundHit();
    if (!wheel.isGrounded){
        return false;
    }
    hit.point = wheel.hit.point;
    hit.normal = wheel.hit.normal;
    hit.forwardDir = wheel.hit.forwardDir;
    hit.sidewaysDir = wheel.hit.sidewaysDir;
    hit.force = wheel.hit.force;
    hit.forwardSlip = wheel.hit.forwardSlip;
    hit.sidewaysSlip = wheel.hit.sidewaysSlip;
    hit.collider = ColliderRef(scene, wheel.hit.colliderEntity, wheel.hit.colliderIndex);
    return true;
}

void WheelCollider::getWorldPose(Vector3& position, Quaternion& rotation) const{
    const WheelColliderComponent& wheel = component();
    if (wheel.wheelIndex >= 0){
        position = wheel.worldPosePosition;
        rotation = wheel.worldPoseRotation;
        return;
    }
    position = Vector3::ZERO;
    rotation = Quaternion::IDENTITY;
    if (Transform* transform = scene->findComponent<Transform>(entity)){
        if (std::shared_ptr<PhysicsSystem> physics = scene->findSystem<PhysicsSystem>()){
            physics->resolveTransformWorldPose(entity);
        }
        position = transform->modelMatrix * wheel.center;
        rotation = transform->worldRotation;
    }
}
