// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JOINT3D_H
#define JOINT3D_H

#include "EntityHandle.h"
#include "component/Joint3DComponent.h"

namespace jasperengine{

    class GameObject;

    // A 3D joint (Joint3DComponent). Setting a field writes the component: motor, spring,
    // limit, friction and break values reach the live joint at once, the type, bodies, anchors,
    // axes and anchor space rebuild it before the next step. The read-outs are 0 without a live
    // joint. Units: metres, the engine's angle unit (per second for angular speeds), N, N m.
    class JASPERENGINE_API Joint3D: public EntityHandle{
    private:
        Joint3DComponent& joint() const;
        // a parameter reaches the live joint (or the next one)
        void parametersChanged();
        // the joint is made again before the next step
        void rebuild();
        // a root joint GameObject this handle owns goes under its bodyA (world pose
        // kept) and the handle stops owning it, so it lives and dies with bodyA as joints made
        // in the editor do
        void liveUnderBodyA();

    public:

        // component checkbox (Unity Behaviour.enabled): a disabled Joint3D is skipped by its system
        void setEnabled(bool enabled);
        bool isEnabled() const;
        // a new root GameObject "Joint 3D" owned by this handle (destroyed with it) until its
        // bodyA is set, when it goes under bodyA (liveUnderBodyA)
        Joint3D(Scene* scene);
        Joint3D(Scene* scene, Entity entity);
        virtual ~Joint3D();

        Joint3D(const Joint3D& rhs);
        Joint3D& operator=(const Joint3D& rhs);
        Joint3D(Joint3D&& rhs) noexcept;
        Joint3D& operator=(Joint3D&& rhs) noexcept;

        JPH::TwoBodyConstraint* getJoltJoint() const;

        // the joint's GameObject
        GameObject getGameObject() const;
        // GameObject.destroy of the joint's GameObject (after delay seconds); this handle stops owning it
        void destroy(float delay = 0.0f);

        // Joints as before local anchors: world points and axes, the other fields as Jolt had them
        void setFixedJoint(Entity bodyA, Entity bodyB);
        void setDistanceJoint(Entity bodyA, Entity bodyB);
        void setDistanceJoint(Entity bodyA, Entity bodyB, Vector3 worldAnchorOnBodyA, Vector3 worldAnchorOnBodyB);
        void setPointJoint(Entity bodyA, Entity bodyB, Vector3 worldAnchor);
        void setHingeJoint(Entity bodyA, Entity bodyB, Vector3 worldAnchor, Vector3 axis, Vector3 normal);
        void setConeJoint(Entity bodyA, Entity bodyB, Vector3 worldAnchor, Vector3 twistAxis);
        void setPrismaticJoint(Entity bodyA, Entity bodyB, Vector3 sliderAxis, float limitsMin, float limitsMax);
        void setSwingTwistJoint(Entity bodyA, Entity bodyB, Vector3 worldAnchor, Vector3 twistAxis, Vector3 planeAxis, float normalHalfConeAngle, float planeHalfConeAngle, float twistMinAngle, float twistMaxAngle);
        void setSixDOFJoint(Entity bodyA, Entity bodyB, Vector3 worldAnchorOnBodyA, Vector3 worldAnchorOnBodyB, Vector3 axisX, Vector3 axisY);
        void setPathJoint(Entity bodyA, Entity bodyB, std::vector<Vector3> positions, std::vector<Vector3> tangents, std::vector<Vector3> normals, Vector3 pathPosition, bool isLooping);
        void setGearJoint(Entity bodyA, Entity bodyB, Entity hingeA, Entity hingeB, int numTeethGearA, int numTeethGearB);
        void setRackAndPinionJoint(Entity bodyA, Entity bodyB, Entity hinge, Entity slider, int numTeethRack, int numTeethGear, int rackLength);
        void setPulleyJoint(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 worldAnchorOnBodyA, Vector3 worldAnchorOnBodyB, Vector3 fixedPointA, Vector3 fixedPointB);

        Joint3DType getType() const;
        // keeps the edited fields; the ones still at the old type's defaults take the new type's
        void setType(Joint3DType type);

        // structure: the joint is made again
        Entity getBodyA() const;
        void setBodyA(Entity body);
        Entity getBodyB() const;
        void setBodyB(Entity body);
        JointAnchorSpace getAnchorSpace() const;
        void setAnchorSpace(JointAnchorSpace space);
        Vector3 getAnchor() const;
        void setAnchor(Vector3 anchor);
        Vector3 getAnchorA() const;
        void setAnchorA(Vector3 anchor);
        Vector3 getAnchorB() const;
        void setAnchorB(Vector3 anchor);
        bool isAutoAnchors() const;
        void setAutoAnchors(bool autoAnchors);
        Vector3 getAxis() const;
        void setAxis(Vector3 axis);
        Vector3 getNormal() const;
        void setNormal(Vector3 normal);
        Vector3 getTwistAxis() const;
        void setTwistAxis(Vector3 axis);
        Vector3 getPlaneAxis() const;
        void setPlaneAxis(Vector3 axis);
        Vector3 getAxisX() const;
        void setAxisX(Vector3 axis);
        Vector3 getAxisY() const;
        void setAxisY(Vector3 axis);
        JointSwingType getSwingType() const;
        void setSwingType(JointSwingType swingType);
        JointMotion getXMotion() const;
        void setXMotion(JointMotion motion);
        JointMotion getYMotion() const;
        void setYMotion(JointMotion motion);
        JointMotion getZMotion() const;
        void setZMotion(JointMotion motion);
        JointMotion getAngularXMotion() const;
        void setAngularXMotion(JointMotion motion);
        JointMotion getAngularYMotion() const;
        void setAngularYMotion(JointMotion motion);
        JointMotion getAngularZMotion() const;
        void setAngularZMotion(JointMotion motion);
        Vector3 getLinearLimitMin() const;
        void setLinearLimitMin(Vector3 limit);
        Vector3 getLinearLimitMax() const;
        void setLinearLimitMax(Vector3 limit);
        Vector3 getAngularLimitMin() const;
        void setAngularLimitMin(Vector3 limit);
        Vector3 getAngularLimitMax() const;
        void setAngularLimitMax(Vector3 limit);

        // parameters: applied to the live joint
        bool isEnableCollision() const;
        void setEnableCollision(bool enableCollision);
        float getBreakForce() const;
        void setBreakForce(float force);
        float getBreakTorque() const;
        void setBreakTorque(float torque);
        JointBreakAction getBreakAction() const;
        void setBreakAction(JointBreakAction action);
        bool isUseLimits() const;
        void setUseLimits(bool useLimits);
        float getLimitsMin() const;
        void setLimitsMin(float limit);
        float getLimitsMax() const;
        void setLimitsMax(float limit);
        float getLimitsSpringFrequency() const;
        void setLimitsSpringFrequency(float frequency);
        float getLimitsSpringDamping() const;
        void setLimitsSpringDamping(float damping);
        bool isUseMotor() const;
        void setUseMotor(bool useMotor);
        float getMotorTargetVelocity() const;
        void setMotorTargetVelocity(float velocity);
        float getMotorMaxForce() const;
        void setMotorMaxForce(float force);
        bool isUseSpring() const;
        void setUseSpring(bool useSpring);
        float getSpringStiffness() const;
        void setSpringStiffness(float stiffness);
        float getSpringDamper() const;
        void setSpringDamper(float damper);
        float getSpringTargetPosition() const;
        void setSpringTargetPosition(float position);
        float getMaxFriction() const;
        void setMaxFriction(float friction);
        float getMinDistance() const;
        void setMinDistance(float distance);
        float getMaxDistance() const;
        void setMaxDistance(float distance);
        float getConeHalfAngle() const;
        void setConeHalfAngle(float angle);
        float getNormalHalfConeAngle() const;
        void setNormalHalfConeAngle(float angle);
        float getPlaneHalfConeAngle() const;
        void setPlaneHalfConeAngle(float angle);
        float getTwistMinAngle() const;
        void setTwistMinAngle(float angle);
        float getTwistMaxAngle() const;
        void setTwistMaxAngle(float angle);
        Quaternion getMotorTargetRotation() const;
        void setMotorTargetRotation(Quaternion rotation);
        float getMotorSpringFrequency() const;
        void setMotorSpringFrequency(float frequency);
        float getMotorSpringDamping() const;
        void setMotorSpringDamping(float damping);
        bool isUseDrive() const;
        void setUseDrive(bool useDrive);
        float getDriveStiffness() const;
        void setDriveStiffness(float stiffness);
        float getDriveDamping() const;
        void setDriveDamping(float damping);
        float getAngularDriveStiffness() const;
        void setAngularDriveStiffness(float stiffness);
        float getAngularDriveDamping() const;
        void setAngularDriveDamping(float damping);
        float getDriveMaxForce() const;
        void setDriveMaxForce(float force);
        float getDriveMaxTorque() const;
        void setDriveMaxTorque(float torque);
        Vector3 getTargetPosition() const;
        void setTargetPosition(Vector3 position);
        Quaternion getTargetRotation() const;
        void setTargetRotation(Quaternion rotation);
        Vector3 getTargetVelocity() const;
        void setTargetVelocity(Vector3 velocity);
        Vector3 getTargetAngularVelocity() const;
        void setTargetAngularVelocity(Vector3 velocity);

        // read-outs of the live joint (0 without one): the force (N) and torque (N m) of the last
        // step, HINGE angle (from the start pose) and angular velocity, PRISMATIC position and
        // speed, DISTANCE length
        Vector3 getCurrentForce() const;
        Vector3 getCurrentTorque() const;
        float getAngle() const;
        float getVelocity() const;
        float getPosition() const;
        float getSpeed() const;
        float getCurrentLength() const;
        bool isBroken() const;
        bool isLive() const;
    };
}

#endif //JOINT3D_H
