// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Joint3D.h"

#include "Scene.h"
#include "object/GameObject.h"
#include "manager/ObjectManager.h"
#include "subsystem/PhysicsSystem.h"
#include "component/Joint3DComponent.h"
#include "component/Transform.h"
#include "util/Angle.h"

#include <cmath>

using namespace jasperengine;

Joint3D::Joint3D(Scene* scene): EntityHandle(scene){
    // a GameObject, so it can go under its bodyA
    addComponent<Transform>();
    scene->setEntityName(entity, "Joint 3D");
    addComponent<Joint3DComponent>();
}

Joint3D::Joint3D(Scene* scene, Entity entity): EntityHandle(scene, entity){

}

Joint3D::~Joint3D() = default;
Joint3D::Joint3D(const Joint3D& rhs) = default;
Joint3D& Joint3D::operator=(const Joint3D& rhs) = default;
Joint3D::Joint3D(Joint3D&& rhs) noexcept = default;
Joint3D& Joint3D::operator=(Joint3D&& rhs) noexcept = default;

Joint3DComponent& Joint3D::joint() const{
    return getComponent<Joint3DComponent>();
}

void Joint3D::parametersChanged(){
    joint().needApplyParameters = true;
}

void Joint3D::rebuild(){
    joint().needUpdateJoint = true;
}

void Joint3D::liveUnderBodyA(){
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

GameObject Joint3D::getGameObject() const{
    return GameObject(scene, entity);
}

void Joint3D::destroy(float delay){
    // the handle must not destroy the entity (or a later one with its id) again
    setEntityOwned(false);
    ObjectManager::destroy(scene, entity, delay);
}

JPH::TwoBodyConstraint* Joint3D::getJoltJoint() const{
    return joint().joint;
}

namespace {
    // the helpers' joints: world points, every new field as before it existed, made now
    bool makeLegacyJoint(Scene* scene, Entity entity, Joint3DComponent& joint, Joint3DType type, Entity bodyA, Entity bodyB){
        setJoint3DLegacyParameters(joint, type);
        joint.bodyA = bodyA;
        joint.bodyB = bodyB;
        return scene->getSystem<PhysicsSystem>()->buildJoint3D(entity, joint);
    }
}

void Joint3D::setFixedJoint(Entity bodyA, Entity bodyB){
    Joint3DComponent& joint = this->joint();
    joint.needUpdateJoint = !makeLegacyJoint(scene, entity, joint, Joint3DType::FIXED, bodyA, bodyB);
    liveUnderBodyA();
}

void Joint3D::setDistanceJoint(Entity bodyA, Entity bodyB){
    Joint3DComponent& joint = this->joint();
    joint.autoAnchors = true;
    joint.anchorA = Vector3::ZERO;
    joint.anchorB = Vector3::ZERO;
    joint.needUpdateJoint = !makeLegacyJoint(scene, entity, joint, Joint3DType::DISTANCE, bodyA, bodyB);
    liveUnderBodyA();
}

void Joint3D::setDistanceJoint(Entity bodyA, Entity bodyB, Vector3 worldAnchorOnBodyA, Vector3 worldAnchorOnBodyB){
    Joint3DComponent& joint = this->joint();
    joint.anchorA = worldAnchorOnBodyA;
    joint.anchorB = worldAnchorOnBodyB;
    // world anchors given: they are used (the two-argument form takes the body origins)
    joint.autoAnchors = false;
    joint.needUpdateJoint = !makeLegacyJoint(scene, entity, joint, Joint3DType::DISTANCE, bodyA, bodyB);
    liveUnderBodyA();
}

void Joint3D::setPointJoint(Entity bodyA, Entity bodyB, Vector3 worldAnchor){
    Joint3DComponent& joint = this->joint();
    joint.anchor = worldAnchor;
    joint.needUpdateJoint = !makeLegacyJoint(scene, entity, joint, Joint3DType::POINT, bodyA, bodyB);
    liveUnderBodyA();
}

void Joint3D::setHingeJoint(Entity bodyA, Entity bodyB, Vector3 worldAnchor, Vector3 axis, Vector3 normal){
    Joint3DComponent& joint = this->joint();
    joint.anchor = worldAnchor;
    joint.axis = axis;
    joint.normal = normal;
    joint.needUpdateJoint = !makeLegacyJoint(scene, entity, joint, Joint3DType::HINGE, bodyA, bodyB);
    liveUnderBodyA();
}

void Joint3D::setConeJoint(Entity bodyA, Entity bodyB, Vector3 worldAnchor, Vector3 twistAxis){
    Joint3DComponent& joint = this->joint();
    joint.anchor = worldAnchor;
    joint.twistAxis = twistAxis;
    joint.needUpdateJoint = !makeLegacyJoint(scene, entity, joint, Joint3DType::CONE, bodyA, bodyB);
    liveUnderBodyA();
}

void Joint3D::setPrismaticJoint(Entity bodyA, Entity bodyB, Vector3 sliderAxis, float limitsMin, float limitsMax){
    Joint3DComponent& joint = this->joint();
    joint.axis = sliderAxis;
    joint.limitsMin = limitsMin;
    joint.limitsMax = limitsMax;
    joint.needUpdateJoint = !makeLegacyJoint(scene, entity, joint, Joint3DType::PRISMATIC, bodyA, bodyB);
    liveUnderBodyA();
}

void Joint3D::setSwingTwistJoint(Entity bodyA, Entity bodyB, Vector3 worldAnchor, Vector3 twistAxis, Vector3 planeAxis, float normalHalfConeAngle, float planeHalfConeAngle, float twistMinAngle, float twistMaxAngle){
    Joint3DComponent& joint = this->joint();
    joint.anchor = worldAnchor;
    joint.twistAxis = twistAxis;
    joint.planeAxis = planeAxis;
    joint.normalHalfConeAngle = normalHalfConeAngle;
    joint.planeHalfConeAngle = planeHalfConeAngle;
    joint.twistMinAngle = twistMinAngle;
    joint.twistMaxAngle = twistMaxAngle;
    joint.needUpdateJoint = !makeLegacyJoint(scene, entity, joint, Joint3DType::SWINGTWIST, bodyA, bodyB);
    liveUnderBodyA();
}

void Joint3D::setSixDOFJoint(Entity bodyA, Entity bodyB, Vector3 worldAnchorOnBodyA, Vector3 worldAnchorOnBodyB, Vector3 axisX, Vector3 axisY){
    Joint3DComponent& joint = this->joint();
    joint.anchorA = worldAnchorOnBodyA;
    joint.anchorB = worldAnchorOnBodyB;
    joint.axisX = axisX;
    joint.axisY = axisY;
    joint.needUpdateJoint = !makeLegacyJoint(scene, entity, joint, Joint3DType::SIXDOF, bodyA, bodyB);
    liveUnderBodyA();
}

void Joint3D::setPathJoint(Entity bodyA, Entity bodyB, std::vector<Vector3> positions, std::vector<Vector3> tangents, std::vector<Vector3> normals, Vector3 pathPosition, bool isLooping){
    Joint3DComponent& joint = this->joint();
    setJoint3DLegacyParameters(joint, Joint3DType::PATH);
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    joint.pathPoints = positions;
    joint.pathPosition = pathPosition;
    joint.isLooping = isLooping;
    joint.needUpdateJoint = !scene->getSystem<PhysicsSystem>()->loadPathJoint3D(joint, bodyA, bodyB, positions, tangents, normals, pathPosition, isLooping);
    liveUnderBodyA();
}

void Joint3D::setGearJoint(Entity bodyA, Entity bodyB, Entity hingeA, Entity hingeB, int numTeethGearA, int numTeethGearB){
    Joint3DComponent& joint = this->joint();
    joint.hingeA = hingeA;
    joint.hingeB = hingeB;
    joint.numTeethGearA = numTeethGearA;
    joint.numTeethGearB = numTeethGearB;
    joint.needUpdateJoint = !makeLegacyJoint(scene, entity, joint, Joint3DType::GEAR, bodyA, bodyB);
    liveUnderBodyA();
}

void Joint3D::setRackAndPinionJoint(Entity bodyA, Entity bodyB, Entity hinge, Entity slider, int numTeethRack, int numTeethGear, int rackLength){
    Joint3DComponent& joint = this->joint();
    joint.hinge = hinge;
    joint.slider = slider;
    joint.numTeethRack = numTeethRack;
    joint.numTeethGear = numTeethGear;
    joint.rackLength = rackLength;
    joint.needUpdateJoint = !makeLegacyJoint(scene, entity, joint, Joint3DType::RACKANDPINON, bodyA, bodyB);
    liveUnderBodyA();
}

void Joint3D::setPulleyJoint(Joint3DComponent& unused, Entity bodyA, Entity bodyB, Vector3 worldAnchorOnBodyA, Vector3 worldAnchorOnBodyB, Vector3 fixedPointA, Vector3 fixedPointB){
    (void)unused;
    Joint3DComponent& joint = this->joint();
    joint.anchorA = worldAnchorOnBodyA;
    joint.anchorB = worldAnchorOnBodyB;
    joint.fixedPointA = fixedPointA;
    joint.fixedPointB = fixedPointB;
    joint.needUpdateJoint = !makeLegacyJoint(scene, entity, joint, Joint3DType::PULLEY, bodyA, bodyB);
    liveUnderBodyA();
}

Joint3DType Joint3D::getType() const{
    return joint().type;
}

void Joint3D::setType(Joint3DType type){
    changeJoint3DType(joint(), type);
    rebuild();
}

void Joint3D::setEnabled(bool enabled){
    joint().enabled = enabled;
}

bool Joint3D::isEnabled() const{
    return joint().enabled;
}

// ---- structure: made again before the next step ----

Entity Joint3D::getBodyA() const{
    return joint().bodyA;
}

void Joint3D::setBodyA(Entity value){
    joint().bodyA = value;
    rebuild();
    liveUnderBodyA();
}

Entity Joint3D::getBodyB() const{
    return joint().bodyB;
}

void Joint3D::setBodyB(Entity value){
    joint().bodyB = value;
    rebuild();
}

JointAnchorSpace Joint3D::getAnchorSpace() const{
    return joint().anchorSpace;
}

void Joint3D::setAnchorSpace(JointAnchorSpace value){
    joint().anchorSpace = value;
    rebuild();
}

Vector3 Joint3D::getAnchor() const{
    return joint().anchor;
}

void Joint3D::setAnchor(Vector3 value){
    joint().anchor = value;
    rebuild();
}

Vector3 Joint3D::getAnchorA() const{
    return joint().anchorA;
}

void Joint3D::setAnchorA(Vector3 value){
    joint().anchorA = value;
    rebuild();
}

Vector3 Joint3D::getAnchorB() const{
    return joint().anchorB;
}

void Joint3D::setAnchorB(Vector3 value){
    joint().anchorB = value;
    rebuild();
}

bool Joint3D::isAutoAnchors() const{
    return joint().autoAnchors;
}

void Joint3D::setAutoAnchors(bool value){
    joint().autoAnchors = value;
    rebuild();
}

Vector3 Joint3D::getAxis() const{
    return joint().axis;
}

void Joint3D::setAxis(Vector3 value){
    joint().axis = value;
    rebuild();
}

Vector3 Joint3D::getNormal() const{
    return joint().normal;
}

void Joint3D::setNormal(Vector3 value){
    joint().normal = value;
    rebuild();
}

Vector3 Joint3D::getTwistAxis() const{
    return joint().twistAxis;
}

void Joint3D::setTwistAxis(Vector3 value){
    joint().twistAxis = value;
    rebuild();
}

Vector3 Joint3D::getPlaneAxis() const{
    return joint().planeAxis;
}

void Joint3D::setPlaneAxis(Vector3 value){
    joint().planeAxis = value;
    rebuild();
}

Vector3 Joint3D::getAxisX() const{
    return joint().axisX;
}

void Joint3D::setAxisX(Vector3 value){
    joint().axisX = value;
    rebuild();
}

Vector3 Joint3D::getAxisY() const{
    return joint().axisY;
}

void Joint3D::setAxisY(Vector3 value){
    joint().axisY = value;
    rebuild();
}

JointSwingType Joint3D::getSwingType() const{
    return joint().swingType;
}

void Joint3D::setSwingType(JointSwingType value){
    joint().swingType = value;
    rebuild();
}

JointMotion Joint3D::getXMotion() const{
    return joint().xMotion;
}

void Joint3D::setXMotion(JointMotion value){
    joint().xMotion = value;
    parametersChanged();
}

JointMotion Joint3D::getYMotion() const{
    return joint().yMotion;
}

void Joint3D::setYMotion(JointMotion value){
    joint().yMotion = value;
    parametersChanged();
}

JointMotion Joint3D::getZMotion() const{
    return joint().zMotion;
}

void Joint3D::setZMotion(JointMotion value){
    joint().zMotion = value;
    parametersChanged();
}

JointMotion Joint3D::getAngularXMotion() const{
    return joint().angularXMotion;
}

void Joint3D::setAngularXMotion(JointMotion value){
    joint().angularXMotion = value;
    parametersChanged();
}

JointMotion Joint3D::getAngularYMotion() const{
    return joint().angularYMotion;
}

void Joint3D::setAngularYMotion(JointMotion value){
    joint().angularYMotion = value;
    parametersChanged();
}

JointMotion Joint3D::getAngularZMotion() const{
    return joint().angularZMotion;
}

void Joint3D::setAngularZMotion(JointMotion value){
    joint().angularZMotion = value;
    parametersChanged();
}

Vector3 Joint3D::getLinearLimitMin() const{
    return joint().linearLimitMin;
}

void Joint3D::setLinearLimitMin(Vector3 value){
    joint().linearLimitMin = value;
    parametersChanged();
}

Vector3 Joint3D::getLinearLimitMax() const{
    return joint().linearLimitMax;
}

void Joint3D::setLinearLimitMax(Vector3 value){
    joint().linearLimitMax = value;
    parametersChanged();
}

Vector3 Joint3D::getAngularLimitMin() const{
    return joint().angularLimitMin;
}

void Joint3D::setAngularLimitMin(Vector3 value){
    joint().angularLimitMin = value;
    parametersChanged();
}

Vector3 Joint3D::getAngularLimitMax() const{
    return joint().angularLimitMax;
}

void Joint3D::setAngularLimitMax(Vector3 value){
    joint().angularLimitMax = value;
    parametersChanged();
}

// ---- parameters: applied to the live joint ----

bool Joint3D::isEnableCollision() const{
    return joint().enableCollision;
}

void Joint3D::setEnableCollision(bool value){
    joint().enableCollision = value;
    parametersChanged();
}

float Joint3D::getBreakForce() const{
    return joint().breakForce;
}

void Joint3D::setBreakForce(float value){
    joint().breakForce = value;
    parametersChanged();
}

float Joint3D::getBreakTorque() const{
    return joint().breakTorque;
}

void Joint3D::setBreakTorque(float value){
    joint().breakTorque = value;
    parametersChanged();
}

JointBreakAction Joint3D::getBreakAction() const{
    return joint().breakAction;
}

void Joint3D::setBreakAction(JointBreakAction value){
    joint().breakAction = value;
    parametersChanged();
}

bool Joint3D::isUseLimits() const{
    return joint().useLimits;
}

void Joint3D::setUseLimits(bool value){
    joint().useLimits = value;
    parametersChanged();
}

float Joint3D::getLimitsMin() const{
    return joint().limitsMin;
}

void Joint3D::setLimitsMin(float value){
    joint().limitsMin = value;
    parametersChanged();
}

float Joint3D::getLimitsMax() const{
    return joint().limitsMax;
}

void Joint3D::setLimitsMax(float value){
    joint().limitsMax = value;
    parametersChanged();
}

float Joint3D::getLimitsSpringFrequency() const{
    return joint().limitsSpringFrequency;
}

void Joint3D::setLimitsSpringFrequency(float value){
    joint().limitsSpringFrequency = value;
    parametersChanged();
}

float Joint3D::getLimitsSpringDamping() const{
    return joint().limitsSpringDamping;
}

void Joint3D::setLimitsSpringDamping(float value){
    joint().limitsSpringDamping = value;
    parametersChanged();
}

bool Joint3D::isUseMotor() const{
    return joint().useMotor;
}

void Joint3D::setUseMotor(bool value){
    joint().useMotor = value;
    parametersChanged();
}

float Joint3D::getMotorTargetVelocity() const{
    return joint().motorTargetVelocity;
}

void Joint3D::setMotorTargetVelocity(float value){
    joint().motorTargetVelocity = value;
    parametersChanged();
}

float Joint3D::getMotorMaxForce() const{
    return joint().motorMaxForce;
}

void Joint3D::setMotorMaxForce(float value){
    joint().motorMaxForce = value;
    parametersChanged();
}

bool Joint3D::isUseSpring() const{
    return joint().useSpring;
}

void Joint3D::setUseSpring(bool value){
    joint().useSpring = value;
    parametersChanged();
}

float Joint3D::getSpringStiffness() const{
    return joint().springStiffness;
}

void Joint3D::setSpringStiffness(float value){
    joint().springStiffness = value;
    parametersChanged();
}

float Joint3D::getSpringDamper() const{
    return joint().springDamper;
}

void Joint3D::setSpringDamper(float value){
    joint().springDamper = value;
    parametersChanged();
}

float Joint3D::getSpringTargetPosition() const{
    return joint().springTargetPosition;
}

void Joint3D::setSpringTargetPosition(float value){
    joint().springTargetPosition = value;
    parametersChanged();
}

float Joint3D::getMaxFriction() const{
    return joint().maxFriction;
}

void Joint3D::setMaxFriction(float value){
    joint().maxFriction = value;
    parametersChanged();
}

float Joint3D::getMinDistance() const{
    return joint().minDistance;
}

void Joint3D::setMinDistance(float value){
    joint().minDistance = value;
    parametersChanged();
}

float Joint3D::getMaxDistance() const{
    return joint().maxDistance;
}

void Joint3D::setMaxDistance(float value){
    joint().maxDistance = value;
    parametersChanged();
}

float Joint3D::getConeHalfAngle() const{
    return joint().coneHalfAngle;
}

void Joint3D::setConeHalfAngle(float value){
    joint().coneHalfAngle = value;
    parametersChanged();
}

float Joint3D::getNormalHalfConeAngle() const{
    return joint().normalHalfConeAngle;
}

void Joint3D::setNormalHalfConeAngle(float value){
    joint().normalHalfConeAngle = value;
    parametersChanged();
}

float Joint3D::getPlaneHalfConeAngle() const{
    return joint().planeHalfConeAngle;
}

void Joint3D::setPlaneHalfConeAngle(float value){
    joint().planeHalfConeAngle = value;
    parametersChanged();
}

float Joint3D::getTwistMinAngle() const{
    return joint().twistMinAngle;
}

void Joint3D::setTwistMinAngle(float value){
    joint().twistMinAngle = value;
    parametersChanged();
}

float Joint3D::getTwistMaxAngle() const{
    return joint().twistMaxAngle;
}

void Joint3D::setTwistMaxAngle(float value){
    joint().twistMaxAngle = value;
    parametersChanged();
}

Quaternion Joint3D::getMotorTargetRotation() const{
    return joint().motorTargetRotation;
}

void Joint3D::setMotorTargetRotation(Quaternion value){
    joint().motorTargetRotation = value;
    parametersChanged();
}

float Joint3D::getMotorSpringFrequency() const{
    return joint().motorSpringFrequency;
}

void Joint3D::setMotorSpringFrequency(float value){
    joint().motorSpringFrequency = value;
    parametersChanged();
}

float Joint3D::getMotorSpringDamping() const{
    return joint().motorSpringDamping;
}

void Joint3D::setMotorSpringDamping(float value){
    joint().motorSpringDamping = value;
    parametersChanged();
}

bool Joint3D::isUseDrive() const{
    return joint().useDrive;
}

void Joint3D::setUseDrive(bool value){
    joint().useDrive = value;
    parametersChanged();
}

float Joint3D::getDriveStiffness() const{
    return joint().driveStiffness;
}

void Joint3D::setDriveStiffness(float value){
    joint().driveStiffness = value;
    parametersChanged();
}

float Joint3D::getDriveDamping() const{
    return joint().driveDamping;
}

void Joint3D::setDriveDamping(float value){
    joint().driveDamping = value;
    parametersChanged();
}

float Joint3D::getAngularDriveStiffness() const{
    return joint().angularDriveStiffness;
}

void Joint3D::setAngularDriveStiffness(float value){
    joint().angularDriveStiffness = value;
    parametersChanged();
}

float Joint3D::getAngularDriveDamping() const{
    return joint().angularDriveDamping;
}

void Joint3D::setAngularDriveDamping(float value){
    joint().angularDriveDamping = value;
    parametersChanged();
}

float Joint3D::getDriveMaxForce() const{
    return joint().driveMaxForce;
}

void Joint3D::setDriveMaxForce(float value){
    joint().driveMaxForce = value;
    parametersChanged();
}

float Joint3D::getDriveMaxTorque() const{
    return joint().driveMaxTorque;
}

void Joint3D::setDriveMaxTorque(float value){
    joint().driveMaxTorque = value;
    parametersChanged();
}

Vector3 Joint3D::getTargetPosition() const{
    return joint().targetPosition;
}

void Joint3D::setTargetPosition(Vector3 value){
    joint().targetPosition = value;
    parametersChanged();
}

Quaternion Joint3D::getTargetRotation() const{
    return joint().targetRotation;
}

void Joint3D::setTargetRotation(Quaternion value){
    joint().targetRotation = value;
    parametersChanged();
}

Vector3 Joint3D::getTargetVelocity() const{
    return joint().targetVelocity;
}

void Joint3D::setTargetVelocity(Vector3 value){
    joint().targetVelocity = value;
    parametersChanged();
}

Vector3 Joint3D::getTargetAngularVelocity() const{
    return joint().targetAngularVelocity;
}

void Joint3D::setTargetAngularVelocity(Vector3 value){
    joint().targetAngularVelocity = value;
    parametersChanged();
}

// ---- read-outs ----

Vector3 Joint3D::getCurrentForce() const{
    return joint().joint ? joint().currentForce : Vector3::ZERO;
}

Vector3 Joint3D::getCurrentTorque() const{
    return joint().joint ? joint().currentTorque : Vector3::ZERO;
}

float Joint3D::getAngle() const{
    const Joint3DComponent& j = joint();
    if (!j.joint || j.type != Joint3DType::HINGE) return 0.0f;
    return Angle::radToDefault(scene->getSystem<PhysicsSystem>()->getJoint3DPosition(j));
}

float Joint3D::getVelocity() const{
    const Joint3DComponent& j = joint();
    if (!j.joint || j.type != Joint3DType::HINGE) return 0.0f;
    return Angle::radToDefault(scene->getSystem<PhysicsSystem>()->getJoint3DSpeed(j));
}

float Joint3D::getPosition() const{
    const Joint3DComponent& j = joint();
    if (!j.joint || j.type != Joint3DType::PRISMATIC) return 0.0f;
    return scene->getSystem<PhysicsSystem>()->getJoint3DPosition(j);
}

float Joint3D::getSpeed() const{
    const Joint3DComponent& j = joint();
    if (!j.joint || j.type != Joint3DType::PRISMATIC) return 0.0f;
    return scene->getSystem<PhysicsSystem>()->getJoint3DSpeed(j);
}

float Joint3D::getCurrentLength() const{
    const Joint3DComponent& j = joint();
    if (!j.joint || j.type != Joint3DType::DISTANCE) return 0.0f;
    return scene->getSystem<PhysicsSystem>()->getJoint3DLength(j);
}

bool Joint3D::isBroken() const{
    return joint().broken;
}

bool Joint3D::isLive() const{
    return joint().joint != nullptr;
}
