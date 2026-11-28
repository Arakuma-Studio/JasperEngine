// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WHEEL_COLLIDER_H
#define WHEEL_COLLIDER_H

#include "object/EntityHandle.h"
#include "object/physics/Collision.h"
#include "component/WheelColliderComponent.h"

namespace jasperengine{

    // Unity's WheelHit with the collider it touched (Lua's WheelHit, WheelCollider:getGroundHit())
    struct JASPERENGINE_API WheelGroundHit{
        Vector3 point;
        Vector3 normal;
        Vector3 forwardDir;
        Vector3 sidewaysDir;
        float force = 0.0f;
        float forwardSlip = 0.0f;
        float sidewaysSlip = 0.0f;
        // the touched collider of a Rigidbody (index -1: ground with no Rigidbody, a terrain world's)
        ColliderRef collider;
    };

    // A GameObject's WheelCollider (Unity's): Lua self.entity:getWheelCollider() or
    // WheelCollider(scene, entity). The inputs (motorTorque, brakeTorque, steerAngle) reach the
    // vehicle at the next fixed step; the read-outs are those of the last one.
    class JASPERENGINE_API WheelCollider: public EntityHandle{
    private:
        WheelColliderComponent& component() const;

    public:
        WheelCollider(Scene* scene, Entity entity);
        virtual ~WheelCollider();

        WheelCollider(const WheelCollider& rhs);
        WheelCollider& operator=(const WheelCollider& rhs);
        WheelCollider(WheelCollider&& rhs) noexcept;
        WheelCollider& operator=(WheelCollider&& rhs) noexcept;

        // the entity has the component
        bool isValid() const;

        void setEnabled(bool enabled);
        bool isEnabled() const;

        void setCenter(const Vector3& center);
        Vector3 getCenter() const;
        void setRadius(float radius);
        float getRadius() const;
        void setMass(float mass);
        float getMass() const;
        void setWheelDampingRate(float rate);
        float getWheelDampingRate() const;
        void setSuspensionDistance(float distance);
        float getSuspensionDistance() const;
        void setForceAppPointDistance(float distance);
        float getForceAppPointDistance() const;
        void setSuspensionSpring(const JointSpring3D& spring);
        JointSpring3D getSuspensionSpring() const;
        void setForwardFriction(const WheelFrictionCurve& curve);
        WheelFrictionCurve getForwardFriction() const;
        void setSidewaysFriction(const WheelFrictionCurve& curve);
        WheelFrictionCurve getSidewaysFriction() const;
        void setMaxSteerAngle(float angle);
        float getMaxSteerAngle() const;
        void setMaxBrakeTorque(float torque);
        float getMaxBrakeTorque() const;
        void setMaxHandBrakeTorque(float torque);
        float getMaxHandBrakeTorque() const;
        void setDriveTransform(bool drive);
        bool isDriveTransform() const;

        void setMotorTorque(float torque);
        float getMotorTorque() const;
        void setBrakeTorque(float torque);
        float getBrakeTorque() const;
        void setSteerAngle(float angle);
        float getSteerAngle() const;

        bool isGrounded() const;
        float getRpm() const;
        float getSprungMass() const;
        float getSuspensionCompression() const;
        // the body it is a wheel of (NULL_ENTITY: no vehicle has it)
        Entity getVehicleBody() const;

        // false when the wheel touched nothing in the last step
        bool getGroundHit(WheelGroundHit& hit) const;
        // the wheel's drawn pose (suspension, steer, spin) after the last step; before the first,
        // its GameObject's pose at its center
        void getWorldPose(Vector3& position, Quaternion& rotation) const;
    };

}

#endif //WHEEL_COLLIDER_H
