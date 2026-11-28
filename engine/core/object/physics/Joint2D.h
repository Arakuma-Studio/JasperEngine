// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JOINT2D_H
#define JOINT2D_H

#include "EntityHandle.h"
#include "component/Joint2DComponent.h"

namespace jasperengine{

    class GameObject;

    // A 2D joint (Joint2DComponent). Setting a field writes the component: motor, spring,
    // limit and break values reach the live joint at once, the type, bodies, anchors, axis and
    // anchor space rebuild it before the next step. The read-outs are 0 without a live joint.
    // Units: points, the engine's angle unit (per second for angular speeds), N, N m, Hz.
    class JASPERENGINE_API Joint2D: public EntityHandle{
    private:
        Joint2DComponent& joint() const;
        // a parameter reaches the live joint (or the next one)
        void parametersChanged();
        // the joint is made again before the next step
        void rebuild();
        // a root joint GameObject this handle owns goes under its bodyA (world pose
        // kept) and the handle stops owning it, so it lives and dies with bodyA as joints made
        // in the editor do
        void liveUnderBodyA();

    public:

        // component checkbox (Unity Behaviour.enabled): a disabled Joint2D is skipped by its system
        void setEnabled(bool enabled);
        bool isEnabled() const;
        // a new root GameObject "Joint 2D" owned by this handle (destroyed with it) until its
        // bodyA is set, when it goes under bodyA (liveUnderBodyA)
        Joint2D(Scene* scene);
        Joint2D(Scene* scene, Entity entity);
        virtual ~Joint2D();

        Joint2D(const Joint2D& rhs);
        Joint2D& operator=(const Joint2D& rhs);
        Joint2D(Joint2D&& rhs) noexcept;
        Joint2D& operator=(Joint2D&& rhs) noexcept;

        b2JointId getBox2DJoint() const;

        // the joint's GameObject
        GameObject getGameObject() const;
        // GameObject.destroy of the joint's GameObject (after delay seconds); this handle stops owning it
        void destroy(float delay = 0.0f);

        // Joints as before local anchors: world points, Box2D's defaults for the other fields
        void setDistanceJoint(Entity bodyA, Entity bodyB);
        void setDistanceJoint(Entity bodyA, Entity bodyB, Vector2 worldAnchorOnBodyA, Vector2 worldAnchorOnBodyB, bool rope);
        void setRevoluteJoint(Entity bodyA, Entity bodyB, Vector2 worldAnchor);
        void setPrismaticJoint(Entity bodyA, Entity bodyB, Vector2 worldAnchor, Vector2 worldAxis);
        void setMouseJoint(Entity bodyA, Entity bodyB, Vector2 target);
        void setWheelJoint(Entity bodyA, Entity bodyB, Vector2 worldAnchor, Vector2 worldAxis);
        void setWeldJoint(Entity bodyA, Entity bodyB, Vector2 worldAnchor);
        void setMotorJoint(Entity bodyA, Entity bodyB);

        Joint2DType getType() const;
        // keeps the edited fields; the ones still at the old type's defaults take the new type's
        void setType(Joint2DType type);

        Entity getBodyA() const;
        void setBodyA(Entity body);
        Entity getBodyB() const;
        void setBodyB(Entity body);

        JointAnchorSpace getAnchorSpace() const;
        void setAnchorSpace(JointAnchorSpace space);
        Vector2 getAnchorA() const;
        void setAnchorA(Vector2 anchor);
        Vector2 getAnchorB() const;
        void setAnchorB(Vector2 anchor);
        bool isAutoAnchors() const;
        void setAutoAnchors(bool autoAnchors);
        Vector2 getAxis() const;
        void setAxis(Vector2 axis);
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
        float getLowerAngle() const;
        void setLowerAngle(float angle);
        float getUpperAngle() const;
        void setUpperAngle(float angle);
        float getLowerTranslation() const;
        void setLowerTranslation(float translation);
        float getUpperTranslation() const;
        void setUpperTranslation(float translation);

        bool isUseMotor() const;
        void setUseMotor(bool useMotor);
        float getMotorSpeed() const;
        void setMotorSpeed(float speed);
        float getMaxMotorTorque() const;
        void setMaxMotorTorque(float torque);
        float getMaxMotorForce() const;
        void setMaxMotorForce(float force);

        bool isEnableSpring() const;
        void setEnableSpring(bool enableSpring);
        float getFrequency() const;
        void setFrequency(float frequency);
        float getDampingRatio() const;
        void setDampingRatio(float dampingRatio);

        float getLength() const;
        void setLength(float length);
        bool isAutoLength() const;
        void setAutoLength(bool autoLength);
        float getMinLength() const;
        void setMinLength(float length);
        float getMaxLength() const;
        void setMaxLength(float length);
        bool isRope() const;
        void setRope(bool rope);

        float getLinearFrequency() const;
        void setLinearFrequency(float frequency);
        float getAngularFrequency() const;
        void setAngularFrequency(float frequency);
        float getLinearDampingRatio() const;
        void setLinearDampingRatio(float dampingRatio);
        float getAngularDampingRatio() const;
        void setAngularDampingRatio(float dampingRatio);

        Vector2 getLinearOffset() const;
        void setLinearOffset(Vector2 offset);
        float getAngularOffset() const;
        void setAngularOffset(float offset);
        bool isAutoOffset() const;
        void setAutoOffset(bool autoOffset);
        float getMaxForce() const;
        void setMaxForce(float force);
        float getMaxTorque() const;
        void setMaxTorque(float torque);
        float getCorrectionFactor() const;
        void setCorrectionFactor(float factor);

        Vector2 getTarget() const;
        void setTarget(Vector2 target);

        // read-outs of the live joint
        Vector2 getReactionForce() const;
        float getReactionTorque() const;
        float getJointAngle() const;
        float getJointSpeed() const;
        float getJointTranslation() const;
        float getJointLinearSpeed() const;
        float getMotorTorque() const;
        float getMotorForce() const;
        float getCurrentLength() const;
        JointLimitState2D getLimitState() const;
        float getReferenceAngle() const;
        bool isBroken() const;
        bool isLive() const;
    };
}

#endif //JOINT2D_H
