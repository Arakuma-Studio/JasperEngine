// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S96 WP-A (docs/specs/S96-physics-gaps.md REQ-1..REQ-9): Lua's WheelCollider, VehicleDrivetrain,
// the value types JointSpring, WheelFrictionCurve and WheelHit, and GameObject's
// getWheelCollider / getVehicleDrivetrain. JointSpring and WheelFrictionCurve are values, as in
// Unity: change a copy and assign it back (wheel.forwardFriction = f).

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Scene.h"
#include "component/VehicleDrivetrainComponent.h"
#include "component/WheelColliderComponent.h"
#include "object/GameObject.h"
#include "object/physics/VehicleDrivetrain.h"
#include "object/physics/WheelCollider.h"

#include <optional>

#ifdef JASPERENGINE_PHYSICS_3D

namespace luabridge
{
    template<> struct Stack<VehicleTransmissionMode> : EnumWrapper<VehicleTransmissionMode>{};
}

using namespace jasperengine;

namespace {

    // (position, rotation): two results, pushed by hand (LuaBridge returns a std::tuple as one table)
    int luaWheelWorldPose(lua_State* L){
        auto self = luabridge::get<WheelCollider*>(L, 1);
        if (!self || !*self || !(*self)->isValid()){
            return luaL_argerror(L, 1, "a WheelCollider (call it with ':')");
        }
        Vector3 position;
        Quaternion rotation;
        (*self)->getWorldPose(position, rotation);
        (void)luabridge::push(L, position);
        (void)luabridge::push(L, rotation);
        return 2;
    }

}

void LuaBinding::registerVehicleClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    luabridge::getGlobalNamespace(L)
        .beginNamespace("VehicleTransmissionMode")
        .addVariable("AUTO", VehicleTransmissionMode::AUTO)
        .addVariable("MANUAL", VehicleTransmissionMode::MANUAL)
        .endNamespace();

    // Unity's JointSpring: the WheelCollider's suspension (N/m, N s/m, rest point 0..1 of the travel)
    luabridge::getGlobalNamespace(L)
        .beginClass<JointSpring3D>("JointSpring")
        .addConstructor(
            [](void* ptr) { return new (ptr) JointSpring3D(); },
            [](void* ptr, float spring, float damper, float targetPosition) {
                return new (ptr) JointSpring3D{spring, damper, targetPosition}; })
        .addProperty("spring", &JointSpring3D::spring, &JointSpring3D::spring)
        .addProperty("damper", &JointSpring3D::damper, &JointSpring3D::damper)
        .addProperty("targetPosition", &JointSpring3D::targetPosition, &JointSpring3D::targetPosition)
        .endClass();

    // Unity's WheelFrictionCurve (slips: forward the slip ratio, sideways the slip angle in radians)
    luabridge::getGlobalNamespace(L)
        .beginClass<WheelFrictionCurve>("WheelFrictionCurve")
        .addConstructor(
            [](void* ptr) { return new (ptr) WheelFrictionCurve(); },
            [](void* ptr, float extremumSlip, float extremumValue, float asymptoteSlip, float asymptoteValue, float stiffness) {
                return new (ptr) WheelFrictionCurve{extremumSlip, extremumValue, asymptoteSlip, asymptoteValue, stiffness}; })
        .addProperty("extremumSlip", &WheelFrictionCurve::extremumSlip, &WheelFrictionCurve::extremumSlip)
        .addProperty("extremumValue", &WheelFrictionCurve::extremumValue, &WheelFrictionCurve::extremumValue)
        .addProperty("asymptoteSlip", &WheelFrictionCurve::asymptoteSlip, &WheelFrictionCurve::asymptoteSlip)
        .addProperty("asymptoteValue", &WheelFrictionCurve::asymptoteValue, &WheelFrictionCurve::asymptoteValue)
        .addProperty("stiffness", &WheelFrictionCurve::stiffness, &WheelFrictionCurve::stiffness)
        .endClass();

    // Unity's WheelHit: what a grounded wheel touched in the last fixed step
    luabridge::getGlobalNamespace(L)
        .beginClass<WheelGroundHit>("WheelHit")
        .addProperty("point", &WheelGroundHit::point, &WheelGroundHit::point)
        .addProperty("normal", &WheelGroundHit::normal, &WheelGroundHit::normal)
        .addProperty("forwardDir", &WheelGroundHit::forwardDir, &WheelGroundHit::forwardDir)
        .addProperty("sidewaysDir", &WheelGroundHit::sidewaysDir, &WheelGroundHit::sidewaysDir)
        .addProperty("force", &WheelGroundHit::force, &WheelGroundHit::force)
        .addProperty("forwardSlip", &WheelGroundHit::forwardSlip, &WheelGroundHit::forwardSlip)
        .addProperty("sidewaysSlip", &WheelGroundHit::sidewaysSlip, &WheelGroundHit::sidewaysSlip)
        // the touched collider of a Rigidbody, nil for ground without one (a terrain world's)
        .addProperty("collider", [] (const WheelGroundHit* self) -> std::optional<ColliderRef> {
            return self->collider.isValid() ? std::optional<ColliderRef>(self->collider) : std::nullopt; })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<WheelCollider, EntityHandle>("WheelCollider")
        .addConstructor <void (Scene*, Entity)> ()
        .addProperty("valid", &WheelCollider::isValid)
        .addProperty("enabled", &WheelCollider::isEnabled, &WheelCollider::setEnabled)
        .addProperty("center", &WheelCollider::getCenter, &WheelCollider::setCenter)
        .addProperty("radius", &WheelCollider::getRadius, &WheelCollider::setRadius)
        .addProperty("mass", &WheelCollider::getMass, &WheelCollider::setMass)
        .addProperty("wheelDampingRate", &WheelCollider::getWheelDampingRate, &WheelCollider::setWheelDampingRate)
        .addProperty("suspensionDistance", &WheelCollider::getSuspensionDistance, &WheelCollider::setSuspensionDistance)
        .addProperty("forceAppPointDistance", &WheelCollider::getForceAppPointDistance, &WheelCollider::setForceAppPointDistance)
        .addProperty("suspensionSpring", &WheelCollider::getSuspensionSpring, &WheelCollider::setSuspensionSpring)
        .addProperty("forwardFriction", &WheelCollider::getForwardFriction, &WheelCollider::setForwardFriction)
        .addProperty("sidewaysFriction", &WheelCollider::getSidewaysFriction, &WheelCollider::setSidewaysFriction)
        .addProperty("maxSteerAngle", &WheelCollider::getMaxSteerAngle, &WheelCollider::setMaxSteerAngle)
        .addProperty("maxBrakeTorque", &WheelCollider::getMaxBrakeTorque, &WheelCollider::setMaxBrakeTorque)
        .addProperty("maxHandBrakeTorque", &WheelCollider::getMaxHandBrakeTorque, &WheelCollider::setMaxHandBrakeTorque)
        .addProperty("driveTransform", &WheelCollider::isDriveTransform, &WheelCollider::setDriveTransform)
        .addProperty("motorTorque", &WheelCollider::getMotorTorque, &WheelCollider::setMotorTorque)
        .addProperty("brakeTorque", &WheelCollider::getBrakeTorque, &WheelCollider::setBrakeTorque)
        .addProperty("steerAngle", &WheelCollider::getSteerAngle, &WheelCollider::setSteerAngle)
        .addProperty("isGrounded", &WheelCollider::isGrounded)
        .addProperty("rpm", &WheelCollider::getRpm)
        .addProperty("sprungMass", &WheelCollider::getSprungMass)
        .addProperty("suspensionCompression", &WheelCollider::getSuspensionCompression)
        // the vehicle's body GameObject, nil while no vehicle has the wheel
        .addProperty("attachedRigidbody", [] (const WheelCollider* self) -> std::optional<GameObject> {
            const Entity body = self->isValid() ? self->getVehicleBody() : NULL_ENTITY;
            return body != NULL_ENTITY ? std::optional<GameObject>(GameObject(self->getScene(), body)) : std::nullopt; })
        // the ground under the wheel in the last step, nil when it touched nothing
        .addFunction("getGroundHit", [] (const WheelCollider* self) -> std::optional<WheelGroundHit> {
            WheelGroundHit hit;
            return self->getGroundHit(hit) ? std::optional<WheelGroundHit>(hit) : std::nullopt; })
        .addFunction("getWorldPose", &luaWheelWorldPose)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<VehicleDrivetrain, EntityHandle>("VehicleDrivetrain")
        .addConstructor <void (Scene*, Entity)> ()
        .addProperty("valid", &VehicleDrivetrain::isValid)
        .addProperty("enabled", &VehicleDrivetrain::isEnabled, &VehicleDrivetrain::setEnabled)
        .addProperty("throttle", &VehicleDrivetrain::getThrottle, &VehicleDrivetrain::setThrottle)
        .addProperty("steer", &VehicleDrivetrain::getSteer, &VehicleDrivetrain::setSteer)
        .addProperty("brake", &VehicleDrivetrain::getBrake, &VehicleDrivetrain::setBrake)
        .addProperty("handBrake", &VehicleDrivetrain::getHandBrake, &VehicleDrivetrain::setHandBrake)
        .addFunction("setGear", &VehicleDrivetrain::setGear)
        .addProperty("rpm", &VehicleDrivetrain::getRpm)
        .addProperty("gear", &VehicleDrivetrain::getGear)
        .addProperty("speed", &VehicleDrivetrain::getSpeed)
        .addProperty("maxTorque", &VehicleDrivetrain::getMaxTorque, &VehicleDrivetrain::setMaxTorque)
        .addProperty("minRPM", &VehicleDrivetrain::getMinRPM, &VehicleDrivetrain::setMinRPM)
        .addProperty("maxRPM", &VehicleDrivetrain::getMaxRPM, &VehicleDrivetrain::setMaxRPM)
        .addProperty("transmission", &VehicleDrivetrain::getTransmission, &VehicleDrivetrain::setTransmission)
        .addProperty("shiftUpRPM", &VehicleDrivetrain::getShiftUpRPM, &VehicleDrivetrain::setShiftUpRPM)
        .addProperty("shiftDownRPM", &VehicleDrivetrain::getShiftDownRPM, &VehicleDrivetrain::setShiftDownRPM)
        .addProperty("clutchStrength", &VehicleDrivetrain::getClutchStrength, &VehicleDrivetrain::setClutchStrength)
        .addProperty("maxPitchRollAngle", &VehicleDrivetrain::getMaxPitchRollAngle, &VehicleDrivetrain::setMaxPitchRollAngle)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<GameObject>("GameObject")
        // the GameObject's WheelCollider, added with its defaults when it has none
        .addFunction("getWheelCollider", +[](GameObject* self) -> WheelCollider {
            Scene* scene = self->getScene();
            if (!scene->findComponent<WheelColliderComponent>(self->getEntity())){
                scene->addComponent<WheelColliderComponent>(self->getEntity(), {});
            }
            return WheelCollider(scene, self->getEntity());
        })
        .addFunction("removeWheelCollider", +[](GameObject* self) {
            Scene* scene = self->getScene();
            if (scene->findComponent<WheelColliderComponent>(self->getEntity())){
                scene->removeComponent<WheelColliderComponent>(self->getEntity());
            }
        })
        // the GameObject's VehicleDrivetrain (on the vehicle's body), added with its defaults when it has none
        .addFunction("getVehicleDrivetrain", +[](GameObject* self) -> VehicleDrivetrain {
            Scene* scene = self->getScene();
            if (!scene->findComponent<VehicleDrivetrainComponent>(self->getEntity())){
                scene->addComponent<VehicleDrivetrainComponent>(self->getEntity(), {});
            }
            return VehicleDrivetrain(scene, self->getEntity());
        })
        .addFunction("removeVehicleDrivetrain", +[](GameObject* self) {
            Scene* scene = self->getScene();
            if (scene->findComponent<VehicleDrivetrainComponent>(self->getEntity())){
                scene->removeComponent<VehicleDrivetrainComponent>(self->getEntity());
            }
        })
        .endClass();

#endif //DISABLE_LUA_BINDINGS
}

#endif // JASPERENGINE_PHYSICS_3D
