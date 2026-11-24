// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Joint2D.h"

#include "Scene.h"
#include "object/GameObject.h"
#include "manager/ObjectManager.h"
#include "subsystem/PhysicsSystem.h"
#include "component/Joint2DComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/Transform.h"
#include "util/Angle.h"

#include <cmath>

using namespace jasperengine;

namespace {

    // where a joint value counts as at its limit (Box2D's slops)
    const float linearSlop = 0.005f;
    const float angularSlop = 2.0f / 180.0f * (float)M_PI;

    JointLimitState2D limitStateOf(float value, float lower, float upper, float slop){
        if (std::fabs(upper - lower) < 2.0f * slop){
            return JointLimitState2D::EQUAL;
        }
        if (value <= lower + slop){
            return JointLimitState2D::LOWER;
        }
        if (value >= upper - slop){
            return JointLimitState2D::UPPER;
        }
        return JointLimitState2D::INACTIVE;
    }

    b2Vec2 pointVelocity(b2BodyId body, b2Vec2 worldPoint){
        const b2Vec2 v = b2Body_GetLinearVelocity(body);
        const float w = b2Body_GetAngularVelocity(body);
        const b2Vec2 r = b2Sub(worldPoint, b2Body_GetWorldCenterOfMass(body));
        return b2Add(v, b2CrossSV(w, r));
    }

}

Joint2D::Joint2D(Scene* scene): EntityHandle(scene){
    // a GameObject, so it can go under its bodyA
    addComponent<Transform>();
    scene->setEntityName(entity, "Joint 2D");
    addComponent<Joint2DComponent>();
}

Joint2D::Joint2D(Scene* scene, Entity entity): EntityHandle(scene, entity){

}

Joint2D::~Joint2D() = default;
Joint2D::Joint2D(const Joint2D& rhs) = default;
Joint2D& Joint2D::operator=(const Joint2D& rhs) = default;
Joint2D::Joint2D(Joint2D&& rhs) noexcept = default;
Joint2D& Joint2D::operator=(Joint2D&& rhs) noexcept = default;

Joint2DComponent& Joint2D::joint() const{
    return getComponent<Joint2DComponent>();
}

void Joint2D::parametersChanged(){
    Joint2DComponent& component = joint();
    if (b2Joint_IsValid(component.joint)){
        scene->getSystem<PhysicsSystem>()->applyJoint2DParameters(component);
    }
}

void Joint2D::rebuild(){
    joint().needUpdateJoint = true;
}

void Joint2D::liveUnderBodyA(){
    const Entity bodyA = joint().bodyA;
    if (!entityOwned || bodyA == NULL_ENTITY || bodyA == entity || !scene->isEntityCreated(bodyA) ||
            !scene->findComponent<Transform>(bodyA) || scene->isParentOf(entity, bodyA)){
        return;
    }
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform || transform->parent != NULL_ENTITY){
        return;
    }
    const GameObject body(scene, bodyA);
    GameObject(scene, entity).setParent(&body, true);
    if (scene->getComponent<Transform>(entity).parent == bodyA){
        setEntityOwned(false);
    }
}

GameObject Joint2D::getGameObject() const{
    return GameObject(scene, entity);
}

void Joint2D::destroy(float delay){
    // the handle must not destroy the entity (or a later one with its id) again
    setEntityOwned(false);
    ObjectManager::destroy(scene, entity, delay);
}

b2JointId Joint2D::getBox2DJoint() const{
    return joint().joint;
}

void Joint2D::setDistanceJoint(Entity bodyA, Entity bodyB){
    Joint2DComponent& component = joint();
    setJoint2DLegacyParameters(component, Joint2DType::DISTANCE);
    component.needUpdateJoint = !scene->getSystem<PhysicsSystem>()->loadDistanceJoint2D(entity, component, bodyA, bodyB, Vector2::ZERO, Vector2::ZERO, true, false);
    liveUnderBodyA();
}

void Joint2D::setDistanceJoint(Entity bodyA, Entity bodyB, Vector2 worldAnchorOnBodyA, Vector2 worldAnchorOnBodyB, bool rope){
    Joint2DComponent& component = joint();
    setJoint2DLegacyParameters(component, Joint2DType::DISTANCE);
    component.needUpdateJoint = !scene->getSystem<PhysicsSystem>()->loadDistanceJoint2D(entity, component, bodyA, bodyB, worldAnchorOnBodyA, worldAnchorOnBodyB, false, rope);
    liveUnderBodyA();
}

void Joint2D::setRevoluteJoint(Entity bodyA, Entity bodyB, Vector2 worldAnchor){
    Joint2DComponent& component = joint();
    setJoint2DLegacyParameters(component, Joint2DType::REVOLUTE);
    component.needUpdateJoint = !scene->getSystem<PhysicsSystem>()->loadRevoluteJoint2D(entity, component, bodyA, bodyB, worldAnchor);
    liveUnderBodyA();
}

void Joint2D::setPrismaticJoint(Entity bodyA, Entity bodyB, Vector2 worldAnchor, Vector2 worldAxis){
    Joint2DComponent& component = joint();
    setJoint2DLegacyParameters(component, Joint2DType::PRISMATIC);
    component.needUpdateJoint = !scene->getSystem<PhysicsSystem>()->loadPrismaticJoint2D(entity, component, bodyA, bodyB, worldAnchor, worldAxis);
    liveUnderBodyA();
}

void Joint2D::setMouseJoint(Entity bodyA, Entity bodyB, Vector2 target){
    Joint2DComponent& component = joint();
    setJoint2DLegacyParameters(component, Joint2DType::MOUSE);
    component.needUpdateJoint = !scene->getSystem<PhysicsSystem>()->loadMouseJoint2D(entity, component, bodyA, bodyB, target);
    liveUnderBodyA();
}

void Joint2D::setWheelJoint(Entity bodyA, Entity bodyB, Vector2 worldAnchor, Vector2 worldAxis){
    Joint2DComponent& component = joint();
    setJoint2DLegacyParameters(component, Joint2DType::WHEEL);
    component.needUpdateJoint = !scene->getSystem<PhysicsSystem>()->loadWheelJoint2D(entity, component, bodyA, bodyB, worldAnchor, worldAxis);
    liveUnderBodyA();
}

void Joint2D::setWeldJoint(Entity bodyA, Entity bodyB, Vector2 worldAnchor){
    Joint2DComponent& component = joint();
    setJoint2DLegacyParameters(component, Joint2DType::WELD);
    component.needUpdateJoint = !scene->getSystem<PhysicsSystem>()->loadWeldJoint2D(entity, component, bodyA, bodyB, worldAnchor);
    liveUnderBodyA();
}

void Joint2D::setMotorJoint(Entity bodyA, Entity bodyB){
    Joint2DComponent& component = joint();
    setJoint2DLegacyParameters(component, Joint2DType::MOTOR);
    component.needUpdateJoint = !scene->getSystem<PhysicsSystem>()->loadMotorJoint2D(entity, component, bodyA, bodyB);
    liveUnderBodyA();
}

Joint2DType Joint2D::getType() const{
    return joint().type;
}

void Joint2D::setType(Joint2DType type){
    Joint2DComponent& component = joint();
    if (component.type != type){
        changeJoint2DType(component, type);
        rebuild();
    }
}

#define JOINT2D_REBUILD_FIELD(Type, Field, Getter, Setter) \
    Type Joint2D::Getter() const{ return joint().Field; } \
    void Joint2D::Setter(Type value){ \
        Joint2DComponent& component = joint(); \
        if (component.Field != value){ component.Field = value; rebuild(); } \
    }

#define JOINT2D_LIVE_FIELD(Type, Field, Getter, Setter) \
    Type Joint2D::Getter() const{ return joint().Field; } \
    void Joint2D::Setter(Type value){ \
        joint().Field = value; \
        parametersChanged(); \
    }

JOINT2D_REBUILD_FIELD(Entity, bodyB, getBodyB, setBodyB)
JOINT2D_REBUILD_FIELD(JointAnchorSpace, anchorSpace, getAnchorSpace, setAnchorSpace)
JOINT2D_REBUILD_FIELD(Vector2, anchorA, getAnchorA, setAnchorA)
JOINT2D_REBUILD_FIELD(Vector2, anchorB, getAnchorB, setAnchorB)
JOINT2D_REBUILD_FIELD(bool, autoAnchors, isAutoAnchors, setAutoAnchors)
JOINT2D_REBUILD_FIELD(Vector2, axis, getAxis, setAxis)
JOINT2D_REBUILD_FIELD(bool, autoLength, isAutoLength, setAutoLength)
JOINT2D_REBUILD_FIELD(bool, autoOffset, isAutoOffset, setAutoOffset)

JOINT2D_LIVE_FIELD(bool, enableCollision, isEnableCollision, setEnableCollision)
JOINT2D_LIVE_FIELD(bool, useLimits, isUseLimits, setUseLimits)
JOINT2D_LIVE_FIELD(float, lowerAngle, getLowerAngle, setLowerAngle)
JOINT2D_LIVE_FIELD(float, upperAngle, getUpperAngle, setUpperAngle)
JOINT2D_LIVE_FIELD(float, lowerTranslation, getLowerTranslation, setLowerTranslation)
JOINT2D_LIVE_FIELD(float, upperTranslation, getUpperTranslation, setUpperTranslation)
JOINT2D_LIVE_FIELD(bool, useMotor, isUseMotor, setUseMotor)
JOINT2D_LIVE_FIELD(float, motorSpeed, getMotorSpeed, setMotorSpeed)
JOINT2D_LIVE_FIELD(float, maxMotorTorque, getMaxMotorTorque, setMaxMotorTorque)
JOINT2D_LIVE_FIELD(float, maxMotorForce, getMaxMotorForce, setMaxMotorForce)
JOINT2D_LIVE_FIELD(bool, enableSpring, isEnableSpring, setEnableSpring)
JOINT2D_LIVE_FIELD(float, frequency, getFrequency, setFrequency)
JOINT2D_LIVE_FIELD(float, dampingRatio, getDampingRatio, setDampingRatio)
JOINT2D_LIVE_FIELD(float, minLength, getMinLength, setMinLength)
JOINT2D_LIVE_FIELD(float, maxLength, getMaxLength, setMaxLength)
JOINT2D_LIVE_FIELD(bool, rope, isRope, setRope)
JOINT2D_LIVE_FIELD(float, linearFrequency, getLinearFrequency, setLinearFrequency)
JOINT2D_LIVE_FIELD(float, angularFrequency, getAngularFrequency, setAngularFrequency)
JOINT2D_LIVE_FIELD(float, linearDampingRatio, getLinearDampingRatio, setLinearDampingRatio)
JOINT2D_LIVE_FIELD(float, angularDampingRatio, getAngularDampingRatio, setAngularDampingRatio)
JOINT2D_LIVE_FIELD(float, maxForce, getMaxForce, setMaxForce)
JOINT2D_LIVE_FIELD(float, maxTorque, getMaxTorque, setMaxTorque)
JOINT2D_LIVE_FIELD(float, correctionFactor, getCorrectionFactor, setCorrectionFactor)
JOINT2D_LIVE_FIELD(Vector2, target, getTarget, setTarget)

#undef JOINT2D_REBUILD_FIELD
#undef JOINT2D_LIVE_FIELD

Entity Joint2D::getBodyA() const{
    return joint().bodyA;
}

void Joint2D::setBodyA(Entity body){
    Joint2DComponent& component = joint();
    if (component.bodyA != body){
        component.bodyA = body;
        rebuild();
    }
    liveUnderBodyA();
}

float Joint2D::getLength() const{
    return joint().length;
}

void Joint2D::setLength(float length){
    Joint2DComponent& component = joint();
    component.length = length;
    // a set length is the one the joint keeps (Unity: autoConfigureDistance off)
    if (component.autoLength){
        component.autoLength = false;
    }
    parametersChanged();
}

Vector2 Joint2D::getLinearOffset() const{
    return joint().linearOffset;
}

void Joint2D::setLinearOffset(Vector2 offset){
    Joint2DComponent& component = joint();
    component.linearOffset = offset;
    component.autoOffset = false;
    parametersChanged();
}

float Joint2D::getAngularOffset() const{
    return joint().angularOffset;
}

void Joint2D::setAngularOffset(float offset){
    Joint2DComponent& component = joint();
    component.angularOffset = offset;
    component.autoOffset = false;
    parametersChanged();
}

float Joint2D::getBreakForce() const{
    return joint().breakForce;
}

void Joint2D::setBreakForce(float force){
    Joint2DComponent& component = joint();
    component.breakForce = std::isnan(force) ? std::numeric_limits<float>::infinity() : force;
    component.breakArmed = true;
}

float Joint2D::getBreakTorque() const{
    return joint().breakTorque;
}

void Joint2D::setBreakTorque(float torque){
    Joint2DComponent& component = joint();
    component.breakTorque = std::isnan(torque) ? std::numeric_limits<float>::infinity() : torque;
    component.breakArmed = true;
}

JointBreakAction Joint2D::getBreakAction() const{
    return joint().breakAction;
}

void Joint2D::setBreakAction(JointBreakAction action){
    joint().breakAction = action;
}

namespace{
    b2JointType box2DJointType(Joint2DType type){
        switch (type){
            case Joint2DType::DISTANCE: return b2_distanceJoint;
            case Joint2DType::REVOLUTE: return b2_revoluteJoint;
            case Joint2DType::PRISMATIC: return b2_prismaticJoint;
            case Joint2DType::MOUSE: return b2_mouseJoint;
            case Joint2DType::WHEEL: return b2_wheelJoint;
            case Joint2DType::WELD: return b2_weldJoint;
            case Joint2DType::MOTOR: return b2_motorJoint;
        }
        return b2_distanceJoint;
    }
}

bool Joint2D::isLive() const{
    const Joint2DComponent& component = joint();
    // a type (or body, anchor...) change waits for the rebuild: until then the Box2D joint is
    // the old one, whose type-specific getters must not run for the new type
    return b2Joint_IsValid(component.joint) && !component.needUpdateJoint &&
           b2Joint_GetType(component.joint) == box2DJointType(component.type) &&
           !scene->getSystem<PhysicsSystem>()->isSteppingWorld2D();
}

Vector2 Joint2D::getReactionForce() const{
    if (!isLive()) return Vector2::ZERO;
    const b2Vec2 force = b2Joint_GetConstraintForce(joint().joint);
    return Vector2(force.x, force.y);
}

float Joint2D::getReactionTorque() const{
    if (!isLive()) return 0.0f;
    return b2Joint_GetConstraintTorque(joint().joint);
}

float Joint2D::getJointAngle() const{
    if (!isLive()) return 0.0f;
    const Joint2DComponent& component = joint();
    if (component.type == Joint2DType::REVOLUTE){
        return Angle::radToDefault(b2RevoluteJoint_GetAngle(component.joint));
    }
    if (component.type == Joint2DType::WHEEL){
        const b2BodyId bodyA = b2Joint_GetBodyA(component.joint);
        const b2BodyId bodyB = b2Joint_GetBodyB(component.joint);
        return Angle::radToDefault(b2UnwindAngle(b2RelativeAngle(b2Body_GetRotation(bodyB), b2Body_GetRotation(bodyA)) - component.referenceAngle));
    }
    return 0.0f;
}

float Joint2D::getJointSpeed() const{
    if (!isLive()) return 0.0f;
    const Joint2DComponent& component = joint();
    if (component.type == Joint2DType::REVOLUTE || component.type == Joint2DType::WHEEL){
        const b2BodyId bodyA = b2Joint_GetBodyA(component.joint);
        const b2BodyId bodyB = b2Joint_GetBodyB(component.joint);
        return Angle::radToDefault(b2Body_GetAngularVelocity(bodyB) - b2Body_GetAngularVelocity(bodyA));
    }
    return 0.0f;
}

float Joint2D::getJointTranslation() const{
    if (!isLive()) return 0.0f;
    const Joint2DComponent& component = joint();
    const float ptm = scene->getSystem<PhysicsSystem>()->getPointsToMeterScale2D();
    if (component.type == Joint2DType::PRISMATIC){
        return b2PrismaticJoint_GetTranslation(component.joint) * ptm;
    }
    if (component.type == Joint2DType::WHEEL){
        const b2BodyId bodyA = b2Joint_GetBodyA(component.joint);
        const b2BodyId bodyB = b2Joint_GetBodyB(component.joint);
        const b2Vec2 pA = b2Body_GetWorldPoint(bodyA, b2Joint_GetLocalAnchorA(component.joint));
        const b2Vec2 pB = b2Body_GetWorldPoint(bodyB, b2Joint_GetLocalAnchorB(component.joint));
        const b2Vec2 axis = b2Body_GetWorldVector(bodyA, b2Vec2{component.localAxisA.x, component.localAxisA.y});
        return b2Dot(b2Sub(pB, pA), axis) * ptm;
    }
    return 0.0f;
}

float Joint2D::getJointLinearSpeed() const{
    if (!isLive()) return 0.0f;
    const Joint2DComponent& component = joint();
    const float ptm = scene->getSystem<PhysicsSystem>()->getPointsToMeterScale2D();
    if (component.type == Joint2DType::PRISMATIC){
        return b2PrismaticJoint_GetSpeed(component.joint) * ptm;
    }
    if (component.type == Joint2DType::WHEEL){
        const b2BodyId bodyA = b2Joint_GetBodyA(component.joint);
        const b2BodyId bodyB = b2Joint_GetBodyB(component.joint);
        const b2Vec2 pA = b2Body_GetWorldPoint(bodyA, b2Joint_GetLocalAnchorA(component.joint));
        const b2Vec2 pB = b2Body_GetWorldPoint(bodyB, b2Joint_GetLocalAnchorB(component.joint));
        const b2Vec2 axis = b2Body_GetWorldVector(bodyA, b2Vec2{component.localAxisA.x, component.localAxisA.y});
        return b2Dot(b2Sub(pointVelocity(bodyB, pB), pointVelocity(bodyA, pA)), axis) * ptm;
    }
    return 0.0f;
}

float Joint2D::getMotorTorque() const{
    if (!isLive()) return 0.0f;
    const Joint2DComponent& component = joint();
    if (component.type == Joint2DType::REVOLUTE) return b2RevoluteJoint_GetMotorTorque(component.joint);
    if (component.type == Joint2DType::WHEEL) return b2WheelJoint_GetMotorTorque(component.joint);
    return 0.0f;
}

float Joint2D::getMotorForce() const{
    if (!isLive()) return 0.0f;
    const Joint2DComponent& component = joint();
    if (component.type == Joint2DType::PRISMATIC) return b2PrismaticJoint_GetMotorForce(component.joint);
    if (component.type == Joint2DType::DISTANCE) return b2DistanceJoint_GetMotorForce(component.joint);
    return 0.0f;
}

float Joint2D::getCurrentLength() const{
    if (!isLive()) return 0.0f;
    const Joint2DComponent& component = joint();
    if (component.type != Joint2DType::DISTANCE) return 0.0f;
    return b2DistanceJoint_GetCurrentLength(component.joint) * scene->getSystem<PhysicsSystem>()->getPointsToMeterScale2D();
}

JointLimitState2D Joint2D::getLimitState() const{
    if (!isLive()) return JointLimitState2D::INACTIVE;
    const Joint2DComponent& component = joint();
    const b2JointId id = component.joint;
    switch (component.type){
        case Joint2DType::REVOLUTE:
            if (!b2RevoluteJoint_IsLimitEnabled(id)) return JointLimitState2D::INACTIVE;
            return limitStateOf(b2RevoluteJoint_GetAngle(id), b2RevoluteJoint_GetLowerLimit(id), b2RevoluteJoint_GetUpperLimit(id), angularSlop);
        case Joint2DType::PRISMATIC:
            if (!b2PrismaticJoint_IsLimitEnabled(id)) return JointLimitState2D::INACTIVE;
            return limitStateOf(b2PrismaticJoint_GetTranslation(id), b2PrismaticJoint_GetLowerLimit(id), b2PrismaticJoint_GetUpperLimit(id), linearSlop);
        case Joint2DType::WHEEL: {
            if (!b2WheelJoint_IsLimitEnabled(id)) return JointLimitState2D::INACTIVE;
            const float ptm = scene->getSystem<PhysicsSystem>()->getPointsToMeterScale2D();
            return limitStateOf(getJointTranslation() / ptm, b2WheelJoint_GetLowerLimit(id), b2WheelJoint_GetUpperLimit(id), linearSlop);
        }
        case Joint2DType::DISTANCE:
            if (!b2DistanceJoint_IsLimitEnabled(id)) return JointLimitState2D::INACTIVE;
            return limitStateOf(b2DistanceJoint_GetCurrentLength(id), b2DistanceJoint_GetMinLength(id), b2DistanceJoint_GetMaxLength(id), linearSlop);
        default:
            return JointLimitState2D::INACTIVE;
    }
}

float Joint2D::getReferenceAngle() const{
    return Angle::radToDefault(joint().referenceAngle);
}

bool Joint2D::isBroken() const{
    return joint().broken;
}

void Joint2D::setEnabled(bool enabled){
    Joint2DComponent& component = getComponent<Joint2DComponent>();
    // a joint broken with DISABLE and enabled again is a fresh joint that can break again
    if (enabled && !component.enabled && component.broken){
        component.broken = false;
        component.breakArmed = true;
        component.needUpdateJoint = true;
    }
    component.enabled = enabled;
}

bool Joint2D::isEnabled() const{
    return getComponent<Joint2DComponent>().enabled;
}
