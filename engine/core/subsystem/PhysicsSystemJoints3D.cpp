// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The 3D joints of PhysicsSystem (physics-3d 6.1.8): every Joint3DComponent field into the Jolt
// constraint settings, in the joint's anchor space (WORLD: world points and axes, the scenes
// saved before local anchors; LOCAL: Unity, anchors and axes in the bodies' frames), the live
// parameters, breaking (onJointBreak) and the read-outs.

#include "PhysicsSystem.h"
#include "Scene.h"
#include "object/physics/Joint3D.h"
#include "script/ScriptMessages.h"
#include "util/Angle.h"
#include "util/JoltPhysicsAux.h"
#include "physics/JoltShapeBuilder.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <optional>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine;

#ifdef JASPERENGINE_PHYSICS_3D

namespace {

    JPH::Vec3 toJolt(const Vector3& v){
        return JPH::Vec3(v.x, v.y, v.z);
    }

    Vector3 fromJolt(JPH::Vec3Arg v){
        return Vector3(v.GetX(), v.GetY(), v.GetZ());
    }

    // a unit vector, or the fallback (and a warning naming what) when it has no length
    Vector3 unitOr(const Vector3& v, const Vector3& fallback, const char* what){
        const float lengthSq = v.x * v.x + v.y * v.y + v.z * v.z;
        if (lengthSq < 1.0e-6f){
            if (what){
                Log::warn("%s is zero. Using default axis (%g, %g, %g).", what, fallback.x, fallback.y, fallback.z);
            }
            return fallback;
        }
        const float inv = 1.0f / std::sqrt(lengthSq);
        return Vector3(v.x * inv, v.y * inv, v.z * inv);
    }

    // a unit vector perpendicular to axis, from `normal` (the old engine's fallbacks)
    Vector3 perpendicular(const Vector3& axis, Vector3 normal, const char* what){
        float normalLengthSq = normal.x * normal.x + normal.y * normal.y + normal.z * normal.z;
        if (normalLengthSq < 1.0e-6f){
            if (what){
                Log::warn("%s is zero. Using default normal.", what);
            }
            normal = (std::fabs(axis.y) < 0.999f) ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(0.0f, 0.0f, 1.0f);
        }
        const float axisDotNormal = axis.x * normal.x + axis.y * normal.y + axis.z * normal.z;
        normal = Vector3(normal.x - axis.x * axisDotNormal, normal.y - axis.y * axisDotNormal, normal.z - axis.z * axisDotNormal);
        normalLengthSq = normal.x * normal.x + normal.y * normal.y + normal.z * normal.z;
        if (normalLengthSq < 1.0e-6f){
            normal = Vector3(-axis.z, 0.0f, axis.x);
            normalLengthSq = normal.x * normal.x + normal.y * normal.y + normal.z * normal.z;
        }
        if (normalLengthSq < 1.0e-6f){
            return Vector3(0.0f, 0.0f, 1.0f);
        }
        const float inv = 1.0f / std::sqrt(normalLengthSq);
        return Vector3(normal.x * inv, normal.y * inv, normal.z * inv);
    }

    float nonNegative(float value){
        return (std::isfinite(value) && value > 0.0f) ? value : 0.0f;
    }

    // an infinite (or bad) force limit is Jolt's FLT_MAX
    float forceLimit(float value){
        return (std::isfinite(value) && value >= 0.0f) ? value : FLT_MAX;
    }

    // Unity spring / damper: Jolt's stiffness and damping spring
    JPH::SpringSettings stiffnessSpring(float stiffness, float damper){
        return JPH::SpringSettings(JPH::ESpringMode::StiffnessAndDamping, nonNegative(stiffness), nonNegative(damper));
    }

    // The point Jolt's fixed and slider constraints take with mAutoDetectPoint, as they took it
    // before every body could switch type: a static body (now made with motion properties, so
    // Jolt would weigh it in) anchors at the other body's centre of mass
    JPH::RVec3 autoDetectedPoint(const JPH::Body& body1, const JPH::Body& body2){
        if (body1.IsStatic()) return body2.GetCenterOfMassPosition();
        if (body2.IsStatic()) return body1.GetCenterOfMassPosition();
        const JPH::Real inverseMass1 = JPH::Real(body1.GetMotionPropertiesUnchecked()->GetInverseMassUnchecked());
        const JPH::Real inverseMass2 = JPH::Real(body2.GetMotionPropertiesUnchecked()->GetInverseMassUnchecked());
        const JPH::Real total = inverseMass1 + inverseMass2;
        if (total == JPH::Real(0)) return body1.GetCenterOfMassPosition();
        return (inverseMass1 * body1.GetCenterOfMassPosition() + inverseMass2 * body2.GetCenterOfMassPosition()) / total;
    }

    // LOCAL joints read as Unity's: the angle, slide, twist and six-DOF coordinates of bodyA (the
    // joint's own body) relative to the connected body. Jolt measures body 2 (bodyB) relative to
    // body 1 (bodyA), so LOCAL values are Jolt's negated (-1); WORLD joints keep Jolt's (+1) as
    // the engine did before
    float jointSign(const Joint3DComponent& joint){
        return joint.anchorSpace == JointAnchorSpace::LOCAL ? -1.0f : 1.0f;
    }

    // soft limits: 0 Hz = hard
    JPH::SpringSettings limitSpring(const Joint3DComponent& joint){
        return JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, nonNegative(joint.limitsSpringFrequency), nonNegative(joint.limitsSpringDamping));
    }

    // HINGE: the limits in the joint's frame turned by their centre c, so a range not containing 0
    // (Unity's 10..50) still contains it for Jolt: [min - c, max - c], the start pose at -c
    struct HingeLimits{
        bool active = false;
        float center = 0.0f;
        float min = -JPH::JPH_PI;
        float max = JPH::JPH_PI;
    };

    HingeLimits hingeLimits(const Joint3DComponent& joint, bool local, Entity entity){
        HingeLimits limits;
        if (!joint.useLimits){
            return limits;
        }
        float minRad = Angle::defaultToRad(joint.limitsMin);
        float maxRad = Angle::defaultToRad(joint.limitsMax);
        if (!std::isfinite(minRad) || !std::isfinite(maxRad)){
            return limits;
        }
        if (maxRad < minRad){
            std::swap(minRad, maxRad);
        }
        float center = local ? 0.5f * (minRad + maxRad) : 0.0f;
        float half = 0.5f * (maxRad - minRad);
        if (half > JPH::JPH_PI){
            Log::warn("Hinge joint %u: limits wider than 360 degrees, clamped to 180 each side of their centre", entity);
            half = JPH::JPH_PI;
        }
        // Jolt asserts on equal limits without a spring
        const float minimum = JPH::DegreesToRadians(0.01f);
        if (half < minimum && !(joint.limitsSpringFrequency > 0.0f)){
            Log::warn("Hinge joint %u: equal limits widened to +-0.01 degrees (use a FIXED joint to hold an angle)", entity);
            half = minimum;
        }
        if (local){
            limits.min = -half;
            limits.max = half;
        }else{
            // WORLD: as given, measured from the start pose (Jolt needs 0 inside: clamped to it)
            limits.min = std::max(-JPH::JPH_PI, std::min(0.0f, minRad));
            limits.max = std::min(JPH::JPH_PI, std::max(0.0f, maxRad));
            if (limits.max - limits.min < 2.0f * minimum){
                limits.min = -minimum;
                limits.max = minimum;
            }
        }
        limits.center = center;
        limits.active = limits.min > -JPH::JPH_PI || limits.max < JPH::JPH_PI;
        return limits;
    }

    // PRISMATIC: the same with the slide (metres); WORLD keeps the old engine's normalisation
    struct SliderLimits{
        bool active = false;
        bool degenerate = false;
        float center = 0.0f;
        float min = -FLT_MAX;
        float max = FLT_MAX;
    };

    SliderLimits sliderLimits(const Joint3DComponent& joint, bool local){
        SliderLimits limits;
        if (!joint.useLimits){
            return limits;
        }
        float limitsMin = joint.limitsMin;
        float limitsMax = joint.limitsMax;
        if (limitsMax < limitsMin){
            std::swap(limitsMin, limitsMax);
        }
        if (local){
            limits.center = 0.5f * (limitsMin + limitsMax);
            limitsMin -= limits.center;
            limitsMax -= limits.center;
        }else{
            // Jolt's slider starts at position 0 and requires 0 to be inside [min, max].
            // Older scenes may serialize absolute-only positive/negative ranges (e.g. [1.8, 27.4]),
            // so normalize them to a valid interval while preserving range size.
            if (limitsMin > 0.0f){
                float offset = limitsMin;
                limitsMin -= offset;
                limitsMax -= offset;
                Log::warn("Prismatic 3D joint limits did not include 0. Normalized range to [{0}, {1}]", limitsMin, limitsMax);
            }else if (limitsMax < 0.0f){
                float offset = limitsMax;
                limitsMin -= offset;
                limitsMax -= offset;
                Log::warn("Prismatic 3D joint limits did not include 0. Normalized range to [{0}, {1}]", limitsMin, limitsMax);
            }
        }
        if (std::fabs(limitsMax - limitsMin) < 1.0e-6f && !(joint.limitsSpringFrequency > 0.0f)){
            limits.degenerate = true;
        }
        limits.min = limitsMin;
        limits.max = limitsMax;
        limits.active = true;
        return limits;
    }

    JPH::SixDOFConstraintSettings::EAxis sixDOFAxis(int index){
        return (JPH::SixDOFConstraintSettings::EAxis)index;
    }

    JointMotion sixDOFMotion(const Joint3DComponent& joint, int index){
        switch (index){
            case 0: return joint.xMotion;
            case 1: return joint.yMotion;
            case 2: return joint.zMotion;
            case 3: return joint.angularXMotion;
            case 4: return joint.angularYMotion;
            default: return joint.angularZMotion;
        }
    }

    float vectorAxis(const Vector3& v, int index){
        return index == 0 ? v.x : (index == 1 ? v.y : v.z);
    }

    // A six-DOF axis's range as Jolt takes it: LOCKED 0..0, FREE unbounded (rotations +-pi),
    // LIMITED the fields (radians for rotations), Jolt's sign for LOCAL joints (jointSign), the
    // cone swing axes symmetric (Jolt keeps only their maximum)
    void sixDOFRange(const Joint3DComponent& joint, int axis, float& minValue, float& maxValue){
        const JointMotion motion = sixDOFMotion(joint, axis);
        const bool linear = axis < 3;
        if (motion == JointMotion::LOCKED){
            minValue = maxValue = 0.0f;
            return;
        }
        if (motion == JointMotion::FREE){
            minValue = linear ? -FLT_MAX : -JPH::JPH_PI;
            maxValue = linear ? FLT_MAX : JPH::JPH_PI;
            return;
        }
        minValue = linear ? vectorAxis(joint.linearLimitMin, axis) : Angle::defaultToRad(vectorAxis(joint.angularLimitMin, axis - 3));
        maxValue = linear ? vectorAxis(joint.linearLimitMax, axis) : Angle::defaultToRad(vectorAxis(joint.angularLimitMax, axis - 3));
        if (maxValue < minValue) std::swap(minValue, maxValue);
        if (joint.anchorSpace == JointAnchorSpace::LOCAL){
            const float low = minValue;
            minValue = -maxValue;
            maxValue = -low;
        }
        if (!linear){
            minValue = std::max(-JPH::JPH_PI, minValue);
            maxValue = std::min(JPH::JPH_PI, maxValue);
            if (axis > 3 && joint.swingType == JointSwingType::CONE){
                const float half = std::max(std::fabs(minValue), std::fabs(maxValue));
                minValue = -half;
                maxValue = half;
            }
        }
    }

    // a drive on the axis: its stiffness and damping (linear or angular)
    void sixDOFDrive(const Joint3DComponent& joint, int axis, float& stiffness, float& damping){
        stiffness = nonNegative(axis < 3 ? joint.driveStiffness : joint.angularDriveStiffness);
        damping = nonNegative(axis < 3 ? joint.driveDamping : joint.angularDriveDamping);
    }

    // gears and racks: whether their two bodies are kept apart. LOCAL (new) ones always (their
    // teeth overlap); WORLD (old scenes) ones as before, by enableCollision only
    bool ignoresPair(const Joint3DComponent& joint){
        if (joint.bodyB == NULL_ENTITY) return false;
        if (!joint.enableCollision) return true;
        return (joint.type == Joint3DType::GEAR || joint.type == Joint3DType::RACKANDPINON) && joint.anchorSpace == JointAnchorSpace::LOCAL;
    }

    // the force (N) of an impulse over the step: the length of `impulse` / dt
    Vector3 perStep(JPH::Vec3Arg impulse, float dt){
        return fromJolt(impulse / dt);
    }

}

bool PhysicsSystem::loadJoint3D(Entity entity, Joint3DComponent& joint){
    // The step holds the constraint list; the update loop retries any joint without one.
    if (steppingWorld3D){
        Log::error("Cannot load a 3D joint while the physics world is stepping, retrying next update");
        return false;
    }

    if (joint.joint){
        destroyJoint3D(joint);
    }

    if (joint.bodyA == NULL_ENTITY){
        return false;
    }
    // a joint of a scene saved before local anchors needed both bodies; a new one without
    // bodyB holds bodyA to the world (Unity connectedBody = null)
    if (joint.bodyB == NULL_ENTITY && joint.anchorSpace == JointAnchorSpace::WORLD){
        return false;
    }

    return buildJoint3D(entity, joint);
}

bool PhysicsSystem::buildJoint3D(Entity entity, Joint3DComponent& joint){
    // tried again every step while it cannot be made (a body missing): reported once
    auto jointError = [&joint](const std::string& message){
        if (!joint.loadFailed){
            Log::error("%s", message.c_str());
        }
        joint.loadFailed = true;
        return false;
    };

    if (steppingWorld3D){
        return false;
    }
    if (joint.joint){
        destroyJoint3D(joint);
    }

    const Entity bodyAEntity = joint.bodyA;
    const Entity bodyBEntity = joint.bodyB;
    // no bodyB (LOCAL only): held to the world, anchorB a world point (Unity)
    const bool toWorld = bodyBEntity == NULL_ENTITY;
    if (bodyAEntity == NULL_ENTITY || !scene->isEntityCreated(bodyAEntity) || !scene->findComponent<RigidbodyComponent>(bodyAEntity) ||
            (!toWorld && (!scene->isEntityCreated(bodyBEntity) || !scene->findComponent<RigidbodyComponent>(bodyBEntity)))){
        return jointError("Cannot create joint, error in bodyA or bodyB");
    }
    if (bodyAEntity == bodyBEntity){
        return jointError("Cannot create 3D joint: bodyA and bodyB are the same body");
    }
    if (toWorld && (joint.type == Joint3DType::GEAR || joint.type == Joint3DType::RACKANDPINON)){
        return jointError("Cannot create gear or rack 3D joint without bodyB");
    }

    if (!loadRigidbody(bodyAEntity) || (!toWorld && !loadRigidbody(bodyBEntity))){
        return jointError("Cannot create 3D joint, bodyA or bodyB has no valid physics shape/body");
    }

    RigidbodyComponent& bodyAComp = scene->getComponent<RigidbodyComponent>(bodyAEntity);
    RigidbodyComponent* bodyBComp = toWorld ? nullptr : &scene->getComponent<RigidbodyComponent>(bodyBEntity);
    if (bodyAComp.body.IsInvalid() || (bodyBComp && bodyBComp->body.IsInvalid())){
        return jointError("Cannot create 3D joint, bodyA or bodyB body id is invalid");
    }

    // the bodies at their Transforms, on copies of their components (the follow state of the
    // step stays as it is), as the old loaders did
    {
        RigidbodyComponent copyA = bodyAComp;
        updateRigidbodyPosition(scene->getSignature(bodyAEntity), bodyAEntity, copyA);
        if (bodyBComp){
            RigidbodyComponent copyB = *bodyBComp;
            updateRigidbodyPosition(scene->getSignature(bodyBEntity), bodyBEntity, copyB);
        }
    }

    // gears and racks drive two other joints, which must exist
    JPH::TwoBodyConstraint* drivenA = nullptr;
    JPH::TwoBodyConstraint* drivenB = nullptr;
    if (joint.type == Joint3DType::GEAR || joint.type == Joint3DType::RACKANDPINON){
        const Entity first = joint.type == Joint3DType::GEAR ? joint.hingeA : joint.hinge;
        const Entity second = joint.type == Joint3DType::GEAR ? joint.hingeB : joint.slider;
        Joint3DComponent* jointA = (first != NULL_ENTITY && scene->isEntityCreated(first)) ? scene->findComponent<Joint3DComponent>(first) : nullptr;
        Joint3DComponent* jointB = (second != NULL_ENTITY && scene->isEntityCreated(second)) ? scene->findComponent<Joint3DComponent>(second) : nullptr;
        if (!jointA || !jointB){
            return jointError(joint.type == Joint3DType::GEAR ? "Cannot create joint, error in hingeA or hingeB" : "Cannot create joint, error in hinge or slider");
        }
        const Joint3DType secondType = joint.type == Joint3DType::GEAR ? Joint3DType::HINGE : Joint3DType::PRISMATIC;
        if (jointA->type != Joint3DType::HINGE || jointB->type != secondType){
            return jointError(joint.type == Joint3DType::GEAR ? "Cannot create joint, hingeA or hingeB is not hinge type"
                                                              : "Cannot create joint, hinge or slider is not hinge and prismatic (slider) types");
        }
        // made first (they may come later in the joint list)
        if (!jointA->joint){
            if (buildJoint3D(first, *jointA)) jointA->needUpdateJoint = false;
        }
        if (!jointB->joint){
            if (buildJoint3D(second, *jointB)) jointB->needUpdateJoint = false;
        }
        if (!jointA->joint || !jointB->joint ||
                jointA->joint->GetSubType() != JPH::EConstraintSubType::Hinge ||
                jointB->joint->GetSubType() != (joint.type == Joint3DType::GEAR ? JPH::EConstraintSubType::Hinge : JPH::EConstraintSubType::Slider)){
            return jointError("Cannot create gear or rack 3D joint: the joints it drives are not made yet");
        }
        drivenA = jointA->joint;
        drivenB = jointB->joint;
    }

    if (joint.type == Joint3DType::PATH && joint.pathPoints.size() < 2){
        return jointError("Cannot create path 3D joint, need at least 2 path points");
    }

    const JPH::BodyLockInterface& bodyLockInterface = getBodyLockInterface3D();
    JPH::BodyID bodies[2] = {bodyAComp.body, bodyBComp ? bodyBComp->body : JPH::BodyID()};
    // held while the constraint is made; released before the parameters and the pair filter
    // go through the (locking) body interface
    std::optional<JPH::BodyLockMultiWrite> lock;
    lock.emplace(bodyLockInterface, bodies, bodyBComp ? 2 : 1);
    JPH::Body* lockBodyA = lock->GetBody(0);
    JPH::Body* lockBodyB = bodyBComp ? lock->GetBody(1) : &JPH::Body::sFixedToWorld;
    if (!lockBodyA || !lockBodyB){
        return jointError("Cannot create 3D joint, bodyA or bodyB is invalid");
    }
    JPH::Body& A = *lockBodyA;
    JPH::Body& B = *lockBodyB;

    const bool local = joint.anchorSpace == JointAnchorSpace::LOCAL;
    const Vector3 scaleA = JoltShapeBuilder::entityWorldScale(scene, bodyAEntity);
    const Vector3 scaleB = toWorld ? Vector3::UNIT_SCALE : JoltShapeBuilder::entityWorldScale(scene, bodyBEntity);

    // LOCAL: the anchors are points of the bodies, scaled like collider positions; the axes are
    // directions of bodyA's frame (mirrored with it). WORLD: as given.
    auto pointOn = [](const JPH::Body& body, const Vector3& localPoint, const Vector3& scale) -> Vector3 {
        return fromJolt(JPH::Vec3(body.GetPosition()) + body.GetRotation() * toJolt(localPoint * scale));
    };
    auto localPointOn = [](const JPH::Body& body, const Vector3& worldPoint, const Vector3& scale) -> Vector3 {
        const Vector3 p = fromJolt(body.GetRotation().Conjugated() * (toJolt(worldPoint) - JPH::Vec3(body.GetPosition())));
        return Vector3(scale.x != 0.0f ? p.x / scale.x : p.x, scale.y != 0.0f ? p.y / scale.y : p.y, scale.z != 0.0f ? p.z / scale.z : p.z);
    };
    auto directionA = [&](const Vector3& axis) -> Vector3 {
        if (!local) return axis;
        const Vector3 mirrored = axis * Vector3(scaleA.x < 0.0f ? -1.0f : 1.0f, scaleA.y < 0.0f ? -1.0f : 1.0f, scaleA.z < 0.0f ? -1.0f : 1.0f);
        return fromJolt(A.GetRotation() * toJolt(mirrored));
    };

    // bodyA's anchor and the connected one (LOCAL autoAnchors: where bodyA's is, written back,
    // Unity autoConfigureConnectedAnchor; DISTANCE keeps each body's own point)
    Vector3 worldAnchorA = joint.anchor;
    Vector3 worldAnchorB = joint.anchor;
    if (local){
        worldAnchorA = pointOn(A, joint.anchor, scaleA);
        if (joint.autoAnchors && joint.type != Joint3DType::DISTANCE && joint.type != Joint3DType::PULLEY){
            worldAnchorB = worldAnchorA;
            joint.anchorB = localPointOn(B, worldAnchorA, scaleB);
        }else{
            worldAnchorB = pointOn(B, joint.anchorB, scaleB);
        }
    }

    joint.limitsCenter = 0.0f;
    JPH::TwoBodyConstraint* constraint = nullptr;

    switch (joint.type){
        case Joint3DType::FIXED: {
            JPH::FixedConstraintSettings settings;
            if (local){
                settings.mPoint1 = settings.mPoint2 = toJolt(worldAnchorA);
            }else{
                settings.mPoint1 = settings.mPoint2 = autoDetectedPoint(A, B);
            }
            constraint = settings.Create(A, B);
            break;
        }
        case Joint3DType::DISTANCE: {
            Vector3 anchorA = joint.anchorA;
            Vector3 anchorB = joint.anchorB;
            if (local){
                anchorA = worldAnchorA;
                anchorB = worldAnchorB;
            }else if (joint.autoAnchors){
                anchorA = fromJolt(JPH::Vec3(A.GetPosition()));
                anchorB = fromJolt(JPH::Vec3(B.GetPosition()));
            }
            JPH::DistanceConstraintSettings settings;
            settings.mPoint1 = toJolt(anchorA);
            settings.mPoint2 = toJolt(anchorB);
            // -1: the distance now (Jolt's own rule, resolved here so a given bound and a
            // measured one never cross)
            float minDistance = std::isfinite(joint.minDistance) ? joint.minDistance : -1.0f;
            float maxDistance = std::isfinite(joint.maxDistance) ? joint.maxDistance : -1.0f;
            if ((minDistance < 0.0f) != (maxDistance < 0.0f)){
                const float current = (anchorB - anchorA).length();
                if (minDistance < 0.0f) minDistance = std::min(current, maxDistance);
                else maxDistance = std::max(current, minDistance);
            }
            if (minDistance >= 0.0f && maxDistance >= 0.0f && maxDistance < minDistance){
                std::swap(minDistance, maxDistance);
            }
            settings.mMinDistance = minDistance;
            settings.mMaxDistance = maxDistance;
            if (joint.useSpring){
                settings.mLimitsSpringSettings = stiffnessSpring(joint.springStiffness, joint.springDamper);
            }else{
                settings.mLimitsSpringSettings = limitSpring(joint);
            }
            constraint = settings.Create(A, B);
            break;
        }
        case Joint3DType::POINT: {
            JPH::PointConstraintSettings settings;
            settings.mPoint1 = toJolt(worldAnchorA);
            settings.mPoint2 = toJolt(worldAnchorB);
            constraint = settings.Create(A, B);
            break;
        }
        case Joint3DType::HINGE: {
            const Vector3 axis = unitOr(directionA(joint.axis), Vector3(1.0f, 0.0f, 0.0f), "Hinge 3D joint axis");
            const Vector3 normal = perpendicular(axis, directionA(joint.normal), "Hinge 3D joint normal");
            const HingeLimits limits = hingeLimits(joint, local, entity);

            JPH::HingeConstraintSettings settings;
            settings.mPoint1 = toJolt(worldAnchorA);
            settings.mPoint2 = toJolt(worldAnchorB);
            settings.mHingeAxis1 = settings.mHingeAxis2 = toJolt(axis);
            settings.mNormalAxis1 = settings.mNormalAxis2 = toJolt(normal);
            if (limits.center != 0.0f){
                // turned by the centre: Jolt reads +c at the start pose, the user's 0
                // (user = c - Jolt, jointSign)
                settings.mNormalAxis2 = JPH::Quat::sRotation(toJolt(axis), limits.center) * toJolt(normal);
            }
            if (limits.active){
                settings.mLimitsMin = limits.min;
                settings.mLimitsMax = limits.max;
                settings.mLimitsSpringSettings = limitSpring(joint);
            }
            settings.mMaxFrictionTorque = nonNegative(joint.maxFriction);
            joint.limitsCenter = limits.center;
            constraint = settings.Create(A, B);
            break;
        }
        case Joint3DType::CONE: {
            const Vector3 twistAxis = unitOr(directionA(joint.twistAxis), Vector3(1.0f, 0.0f, 0.0f), "Cone 3D joint twist axis");
            JPH::ConeConstraintSettings settings;
            settings.mPoint1 = toJolt(worldAnchorA);
            settings.mPoint2 = toJolt(worldAnchorB);
            settings.mTwistAxis1 = settings.mTwistAxis2 = toJolt(twistAxis);
            settings.mHalfConeAngle = std::min(JPH::JPH_PI, std::max(0.0f, Angle::defaultToRad(joint.coneHalfAngle)));
            constraint = settings.Create(A, B);
            break;
        }
        case Joint3DType::PRISMATIC: {
            const Vector3 axis = unitOr(directionA(joint.axis), Vector3(1.0f, 0.0f, 0.0f), "Prismatic 3D joint axis");
            const SliderLimits limits = sliderLimits(joint, local);
            if (limits.degenerate){
                Log::warn("Prismatic 3D joint with equal limits is unsupported by Jolt SliderConstraint. Using fixed joint instead.");
                JPH::FixedConstraintSettings settings;
                settings.mPoint1 = settings.mPoint2 = local ? toJolt(worldAnchorA) : autoDetectedPoint(A, B);
                constraint = settings.Create(A, B);
                break;
            }
            JPH::SliderConstraintSettings settings;
            settings.SetSliderAxis(toJolt(axis));
            if (local){
                // shifted by the limits' centre: Jolt reads +c at the start, the user's 0
                // (user = c - Jolt, jointSign)
                settings.mAutoDetectPoint = false;
                settings.mPoint1 = toJolt(worldAnchorA - axis * limits.center);
                settings.mPoint2 = toJolt(worldAnchorB);
            }else{
                settings.mPoint1 = settings.mPoint2 = autoDetectedPoint(A, B);
            }
            if (limits.active){
                settings.mLimitsMin = limits.min;
                settings.mLimitsMax = limits.max;
                settings.mLimitsSpringSettings = limitSpring(joint);
            }
            settings.mMaxFrictionForce = nonNegative(joint.maxFriction);
            joint.limitsCenter = limits.center;
            constraint = settings.Create(A, B);
            break;
        }
        case Joint3DType::SWINGTWIST: {
            const Vector3 twistAxis = unitOr(directionA(joint.twistAxis), Vector3(1.0f, 0.0f, 0.0f), "SwingTwist 3D joint twist axis");
            const Vector3 planeAxis = perpendicular(twistAxis, directionA(joint.planeAxis), "SwingTwist 3D joint plane axis");
            float twistMin = joint.twistMinAngle;
            float twistMax = joint.twistMaxAngle;
            if (twistMax < twistMin){
                std::swap(twistMin, twistMax);
            }
            constexpr float kMinConeAngle = 0.0f;
            constexpr float kMaxConeAngle = 179.9f;
            JPH::SwingTwistConstraintSettings settings;
            settings.mPosition1 = toJolt(worldAnchorA);
            settings.mPosition2 = toJolt(worldAnchorB);
            settings.mTwistAxis1 = settings.mTwistAxis2 = toJolt(twistAxis);
            settings.mPlaneAxis1 = settings.mPlaneAxis2 = toJolt(planeAxis);
            settings.mSwingType = joint.swingType == JointSwingType::PYRAMID ? JPH::ESwingType::Pyramid : JPH::ESwingType::Cone;
            settings.mNormalHalfConeAngle = Angle::defaultToRad(std::clamp(joint.normalHalfConeAngle, Angle::degToDefault(kMinConeAngle), Angle::degToDefault(kMaxConeAngle)));
            settings.mPlaneHalfConeAngle = Angle::defaultToRad(std::clamp(joint.planeHalfConeAngle, Angle::degToDefault(kMinConeAngle), Angle::degToDefault(kMaxConeAngle)));
            if (local){
                // Unity's low / high twist of bodyA: Jolt's twist of bodyB, negated
                const float lowTwist = twistMin;
                twistMin = -twistMax;
                twistMax = -lowTwist;
            }
            settings.mTwistMinAngle = std::max(-JPH::JPH_PI, Angle::defaultToRad(twistMin));
            settings.mTwistMaxAngle = std::min(JPH::JPH_PI, Angle::defaultToRad(twistMax));
            settings.mMaxFrictionTorque = nonNegative(joint.maxFriction);
            constraint = settings.Create(A, B);
            break;
        }
        case Joint3DType::SIXDOF: {
            const Vector3 axisX = unitOr(directionA(joint.axisX), Vector3(1.0f, 0.0f, 0.0f), "SixDOF 3D joint axisX");
            const Vector3 axisY = perpendicular(axisX, directionA(joint.axisY), "SixDOF 3D joint axisY");
            JPH::SixDOFConstraintSettings settings;
            if (local){
                settings.mPosition1 = toJolt(worldAnchorA);
                settings.mPosition2 = toJolt(worldAnchorB);
            }else{
                settings.mPosition1 = toJolt(joint.anchorA);
                settings.mPosition2 = toJolt(joint.anchorB);
            }
            settings.mAxisX1 = settings.mAxisX2 = toJolt(axisX);
            settings.mAxisY1 = settings.mAxisY2 = toJolt(axisY);
            settings.mSwingType = joint.swingType == JointSwingType::PYRAMID ? JPH::ESwingType::Pyramid : JPH::ESwingType::Cone;
            for (int axis = 0; axis < 6; axis++){
                const JointMotion motion = sixDOFMotion(joint, axis);
                if (motion == JointMotion::LOCKED){
                    settings.MakeFixedAxis(sixDOFAxis(axis));
                }else if (motion == JointMotion::LIMITED){
                    float minValue, maxValue;
                    sixDOFRange(joint, axis, minValue, maxValue);
                    settings.SetLimitedAxis(sixDOFAxis(axis), minValue, maxValue);
                }else{
                    settings.MakeFreeAxis(sixDOFAxis(axis));
                }
            }
            for (int axis = 0; axis < 6; axis++){
                if (sixDOFMotion(joint, axis) != JointMotion::LOCKED){
                    settings.mMaxFriction[axis] = nonNegative(joint.maxFriction);
                }
            }
            constraint = settings.Create(A, B);
            break;
        }
        case Joint3DType::PATH: {
            JPH::Ref<JPH::PathConstraintPathHermite> path = new JPH::PathConstraintPathHermite;
            for (size_t i = 0; i < joint.pathPoints.size(); i++){
                size_t prev = (i == 0) ? (joint.isLooping ? joint.pathPoints.size() - 1 : 0) : i - 1;
                size_t next = (i + 1 >= joint.pathPoints.size()) ? (joint.isLooping ? 0 : joint.pathPoints.size() - 1) : i + 1;
                const Vector3 tangent = unitOr(joint.pathPoints[next] - joint.pathPoints[prev], Vector3(1.0f, 0.0f, 0.0f), nullptr);
                Vector3 normal = (std::fabs(tangent.y) < 0.999f) ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(0.0f, 0.0f, 1.0f);
                normal = perpendicular(tangent, normal, nullptr);
                path->AddPoint(toJolt(joint.pathPoints[i]), toJolt(tangent), toJolt(normal));
            }
            path->SetIsLooping(joint.isLooping);
            JPH::PathConstraintSettings settings;
            settings.mPath = path;
            settings.mPathPosition = toJolt(joint.pathPosition);
            constraint = settings.Create(A, B);
            break;
        }
        case Joint3DType::GEAR: {
            // the driven hinges' axes, in world space (their settings are body-local)
            const JPH::Vec3 axisA = static_cast<JPH::HingeConstraint*>(drivenA)->GetBody1()->GetRotation() *
                                    static_cast<JPH::HingeConstraint*>(drivenA)->GetLocalSpaceHingeAxis1();
            const JPH::Vec3 axisB = static_cast<JPH::HingeConstraint*>(drivenB)->GetBody1()->GetRotation() *
                                    static_cast<JPH::HingeConstraint*>(drivenB)->GetLocalSpaceHingeAxis1();
            int teethA = joint.numTeethGearA;
            int teethB = joint.numTeethGearB;
            if (teethA <= 0 || teethB <= 0){
                Log::warn("Gear 3D joint received non-positive teeth count. Clamping to at least 1.");
                teethA = std::max(1, teethA);
                teethB = std::max(1, teethB);
            }
            JPH::GearConstraintSettings settings;
            settings.mHingeAxis1 = axisA.Normalized();
            settings.mHingeAxis2 = axisB.Normalized();
            settings.SetRatio(teethA, teethB);
            constraint = settings.Create(A, B);
            ((JPH::GearConstraint*)constraint)->SetConstraints(drivenA, drivenB);
            break;
        }
        case Joint3DType::RACKANDPINON: {
            const JPH::HingeConstraint* drivenHinge = static_cast<JPH::HingeConstraint*>(drivenA);
            const JPH::SliderConstraint* drivenSlider = static_cast<JPH::SliderConstraint*>(drivenB);
            const JPH::Vec3 hingeAxis = drivenHinge->GetBody1()->GetRotation() * drivenHinge->GetLocalSpaceHingeAxis1();
            // a slider's axis in its body 1's frame (its settings hold it for body 2 too)
            const JPH::Vec3 sliderAxis = drivenSlider->GetBody1()->GetRotation() * (drivenSlider->GetConstraintToBody1Matrix().GetAxisX());
            int teethRack = joint.numTeethRack;
            int teethGear = joint.numTeethGear;
            int rackLength = joint.rackLength;
            if (teethRack <= 0 || teethGear <= 0 || rackLength <= 0){
                Log::warn("RackAndPinion 3D joint received non-positive ratio inputs. Clamping to at least 1.");
                teethRack = std::max(1, teethRack);
                teethGear = std::max(1, teethGear);
                rackLength = std::max(1, rackLength);
            }
            JPH::RackAndPinionConstraintSettings settings;
            settings.mHingeAxis = hingeAxis.Normalized();
            settings.mSliderAxis = sliderAxis.Normalized();
            settings.SetRatio(teethRack, (float)rackLength, teethGear);
            constraint = settings.Create(A, B);
            ((JPH::RackAndPinionConstraint*)constraint)->SetConstraints(drivenA, drivenB);
            break;
        }
        case Joint3DType::PULLEY: {
            JPH::PulleyConstraintSettings settings;
            settings.mBodyPoint1 = toJolt(local ? worldAnchorA : joint.anchorA);
            settings.mBodyPoint2 = toJolt(local ? worldAnchorB : joint.anchorB);
            settings.mFixedPoint1 = toJolt(joint.fixedPointA);
            settings.mFixedPoint2 = toJolt(joint.fixedPointB);
            constraint = settings.Create(A, B);
            break;
        }
    }

    if (!constraint){
        return jointError("Cannot create 3D joint of entity " + std::to_string(entity));
    }

    joint.joint = constraint;
    world3D.AddConstraint(joint.joint);
    lock.reset();
    joint.loadFailed = false;
    joint.broken = false;
    joint.breakArmed = true;
    joint.currentForce = Vector3::ZERO;
    joint.currentTorque = Vector3::ZERO;

    // the connected bodies do not collide (Unity enableCollision false); gears and racks never
    // did (their teeth overlap)
    releaseJointPair3D(joint);
    if (ignoresPair(joint)){
        changePairFilter3D([bodyAEntity, bodyBEntity](JoltPairFilter3D& filter){ filter.addJoint(bodyAEntity, bodyBEntity); });
        joint.ignoredA = bodyAEntity;
        joint.ignoredB = bodyBEntity;
    }

    applyJoint3DParameters(entity, joint);
    return true;
}

void PhysicsSystem::releaseJointPair3D(Joint3DComponent& joint){
    if (joint.ignoredA != NULL_ENTITY && joint.ignoredB != NULL_ENTITY){
        const Entity a = joint.ignoredA, b = joint.ignoredB;
        changePairFilter3D([a, b](JoltPairFilter3D& filter){ filter.removeJoint(a, b); });
        if (!steppingWorld3D){
            for (Entity entity : {joint.ignoredA, joint.ignoredB}){
                RigidbodyComponent* body = scene->isEntityCreated(entity) ? scene->findComponent<RigidbodyComponent>(entity) : nullptr;
                if (body && !body->body.IsInvalid()){
                    getBodyInterface3D().ActivateBody(body->body);
                }
            }
        }
    }
    joint.ignoredA = NULL_ENTITY;
    joint.ignoredB = NULL_ENTITY;
}

void PhysicsSystem::applyJoint3DParameters(Entity entity, Joint3DComponent& joint){
    joint.needApplyParameters = false;
    if (!joint.joint){
        return;
    }

    // enableCollision: the pair filter, live
    const bool wantsIgnored = ignoresPair(joint);
    const bool isIgnored = joint.ignoredA != NULL_ENTITY;
    if (wantsIgnored != isIgnored){
        if (wantsIgnored){
            const Entity a = joint.bodyA, b = joint.bodyB;
            changePairFilter3D([a, b](JoltPairFilter3D& filter){ filter.addJoint(a, b); });
            joint.ignoredA = joint.bodyA;
            joint.ignoredB = joint.bodyB;
        }else{
            releaseJointPair3D(joint);
        }
        for (Entity bodyEntity : {joint.bodyA, joint.bodyB}){
            RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(bodyEntity);
            if (body && !body->body.IsInvalid()){
                wakeAround3D(body->body);
            }
        }
    }

    const bool local = joint.anchorSpace == JointAnchorSpace::LOCAL;

    switch (joint.type){
        case Joint3DType::HINGE: {
            JPH::HingeConstraint* hinge = static_cast<JPH::HingeConstraint*>(joint.joint);
            const HingeLimits limits = hingeLimits(joint, local, entity);
            // a new centre turns the joint's frame, from the frame it was made with (its zero stays)
            if (std::fabs(limits.center - joint.limitsCenter) > 1.0e-6f){
                if (!recenterJoint3D(entity, joint, limits.center)){
                    joint.needUpdateJoint = true;
                }
                return;
            }
            hinge->SetLimits(limits.active ? limits.min : -JPH::JPH_PI, limits.active ? limits.max : JPH::JPH_PI);
            hinge->SetLimitsSpringSettings(limitSpring(joint));
            hinge->SetMaxFrictionTorque(nonNegative(joint.maxFriction));
            JPH::MotorSettings& motor = hinge->GetMotorSettings();
            if (joint.useMotor){
                if (joint.useSpring){
                    Log::warn("Hinge joint %u: the motor and the spring are both on, the motor drives it", entity);
                }
                motor.SetTorqueLimit(forceLimit(joint.motorMaxForce));
                hinge->SetMotorState(JPH::EMotorState::Velocity);
                hinge->SetTargetAngularVelocity(jointSign(joint) * Angle::defaultToRad(joint.motorTargetVelocity));
            }else if (joint.useSpring){
                motor.mSpringSettings = stiffnessSpring(joint.springStiffness, joint.springDamper);
                motor.SetTorqueLimit(FLT_MAX);
                hinge->SetMotorState(JPH::EMotorState::Position);
                hinge->SetTargetAngle(jointSign(joint) * (Angle::defaultToRad(joint.springTargetPosition) - joint.limitsCenter));
            }else{
                hinge->SetMotorState(JPH::EMotorState::Off);
            }
            break;
        }
        case Joint3DType::PRISMATIC: {
            const SliderLimits limits = sliderLimits(joint, local);
            if (joint.joint->GetSubType() != JPH::EConstraintSubType::Slider){
                // made as a fixed joint for equal limits: a slider again once they differ
                if (!limits.degenerate){
                    joint.needUpdateJoint = true;
                    return;
                }
                break;
            }
            JPH::SliderConstraint* slider = static_cast<JPH::SliderConstraint*>(joint.joint);
            if (limits.degenerate){
                joint.needUpdateJoint = true;
                return;
            }
            if (std::fabs(limits.center - joint.limitsCenter) > 1.0e-6f){
                if (!recenterJoint3D(entity, joint, limits.center)){
                    joint.needUpdateJoint = true;
                }
                return;
            }
            slider->SetLimits(limits.active ? limits.min : -FLT_MAX, limits.active ? limits.max : FLT_MAX);
            slider->SetLimitsSpringSettings(limitSpring(joint));
            slider->SetMaxFrictionForce(nonNegative(joint.maxFriction));
            JPH::MotorSettings& motor = slider->GetMotorSettings();
            if (joint.useMotor){
                if (joint.useSpring){
                    Log::warn("Prismatic joint %u: the motor and the spring are both on, the motor drives it", entity);
                }
                motor.SetForceLimit(forceLimit(joint.motorMaxForce));
                slider->SetMotorState(JPH::EMotorState::Velocity);
                slider->SetTargetVelocity(jointSign(joint) * joint.motorTargetVelocity);
            }else if (joint.useSpring){
                motor.mSpringSettings = stiffnessSpring(joint.springStiffness, joint.springDamper);
                motor.SetForceLimit(FLT_MAX);
                slider->SetMotorState(JPH::EMotorState::Position);
                slider->SetTargetPosition(jointSign(joint) * (joint.springTargetPosition - joint.limitsCenter));
            }else{
                slider->SetMotorState(JPH::EMotorState::Off);
            }
            break;
        }
        case Joint3DType::DISTANCE: {
            JPH::DistanceConstraint* distance = static_cast<JPH::DistanceConstraint*>(joint.joint);
            // -1 keeps what the joint was made with
            float minDistance = (std::isfinite(joint.minDistance) && joint.minDistance >= 0.0f) ? joint.minDistance : distance->GetMinDistance();
            float maxDistance = (std::isfinite(joint.maxDistance) && joint.maxDistance >= 0.0f) ? joint.maxDistance : distance->GetMaxDistance();
            if (maxDistance < minDistance) std::swap(minDistance, maxDistance);
            distance->SetDistance(minDistance, maxDistance);
            distance->SetLimitsSpringSettings(joint.useSpring ? stiffnessSpring(joint.springStiffness, joint.springDamper) : limitSpring(joint));
            break;
        }
        case Joint3DType::CONE: {
            static_cast<JPH::ConeConstraint*>(joint.joint)->SetHalfConeAngle(std::min(JPH::JPH_PI, std::max(0.0f, Angle::defaultToRad(joint.coneHalfAngle))));
            break;
        }
        case Joint3DType::SWINGTWIST: {
            JPH::SwingTwistConstraint* swingTwist = static_cast<JPH::SwingTwistConstraint*>(joint.joint);
            float twistMin = joint.twistMinAngle;
            float twistMax = joint.twistMaxAngle;
            if (twistMax < twistMin) std::swap(twistMin, twistMax);
            if (local){
                const float lowTwist = twistMin;
                twistMin = -twistMax;
                twistMax = -lowTwist;
            }
            const float coneMax = JPH::DegreesToRadians(179.9f);
            swingTwist->SetNormalHalfConeAngle(std::clamp(Angle::defaultToRad(joint.normalHalfConeAngle), 0.0f, coneMax));
            swingTwist->SetPlaneHalfConeAngle(std::clamp(Angle::defaultToRad(joint.planeHalfConeAngle), 0.0f, coneMax));
            // each setter checks min <= max with the other's current value: the one moving away first
            const float newMin = std::max(-JPH::JPH_PI, Angle::defaultToRad(twistMin));
            const float newMax = std::min(JPH::JPH_PI, Angle::defaultToRad(twistMax));
            if (newMin > swingTwist->GetTwistMaxAngle()){
                swingTwist->SetTwistMaxAngle(newMax);
                swingTwist->SetTwistMinAngle(newMin);
            }else{
                swingTwist->SetTwistMinAngle(newMin);
                swingTwist->SetTwistMaxAngle(newMax);
            }
            swingTwist->SetMaxFrictionTorque(nonNegative(joint.maxFriction));
            if (joint.useMotor){
                // a position drive to motorTargetRotation (joint frame)
                const JPH::MotorSettings motor(nonNegative(joint.motorSpringFrequency), nonNegative(joint.motorSpringDamping), FLT_MAX, forceLimit(joint.motorMaxForce));
                swingTwist->GetSwingMotorSettings() = motor;
                swingTwist->GetTwistMotorSettings() = motor;
                swingTwist->SetSwingMotorState(JPH::EMotorState::Position);
                swingTwist->SetTwistMotorState(JPH::EMotorState::Position);
                // bodyA's rotation relative to the connected body (LOCAL): Jolt's inverse
                const Quaternion& q = joint.motorTargetRotation;
                const JPH::Quat target = JPH::Quat(q.x, q.y, q.z, q.w).Normalized();
                swingTwist->SetTargetOrientationCS(local ? target.Conjugated() : target);
            }else{
                swingTwist->SetSwingMotorState(JPH::EMotorState::Off);
                swingTwist->SetTwistMotorState(JPH::EMotorState::Off);
            }
            break;
        }
        case Joint3DType::SIXDOF: {
            JPH::SixDOFConstraint* sixDOF = static_cast<JPH::SixDOFConstraint*>(joint.joint);
            // motions and limits, live (Jolt works out which axes are fixed or free)
            JPH::Vec3 linearMin, linearMax, angularMin, angularMax;
            for (int axis = 0; axis < 6; axis++){
                float minValue, maxValue;
                sixDOFRange(joint, axis, minValue, maxValue);
                if (axis < 3){
                    linearMin.SetComponent(axis, minValue);
                    linearMax.SetComponent(axis, maxValue);
                }else{
                    angularMin.SetComponent(axis - 3, minValue);
                    angularMax.SetComponent(axis - 3, maxValue);
                }
            }
            sixDOF->SetTranslationLimits(linearMin, linearMax);
            sixDOF->SetRotationLimits(angularMin, angularMax);
            joint.dampedAxes = 0;
            for (int axis = 0; axis < 6; axis++){
                const JPH::SixDOFConstraintSettings::EAxis eaxis = sixDOFAxis(axis);
                if (sixDOFMotion(joint, axis) == JointMotion::LOCKED){
                    sixDOF->SetMotorState(eaxis, JPH::EMotorState::Off);
                    continue;
                }
                sixDOF->SetMaxFriction(eaxis, nonNegative(joint.maxFriction));
                float stiffness, damping;
                sixDOFDrive(joint, axis, stiffness, damping);
                // Unity's drives: a stiffness springs to the target pose; a damping alone pulls
                // towards the target velocity with damping x the velocity error (a velocity motor
                // whose force limit follows that error, set before each step:
                // updateSixDOFDampers3D); neither: no drive
                if (!joint.useDrive || (stiffness <= 0.0f && damping <= 0.0f)){
                    sixDOF->SetMotorState(eaxis, JPH::EMotorState::Off);
                    continue;
                }
                const bool linear = axis < 3;
                JPH::MotorSettings& motor = sixDOF->GetMotorSettings(eaxis);
                if (stiffness > 0.0f){
                    motor.mSpringSettings = stiffnessSpring(stiffness, damping);
                    if (linear){
                        motor.SetForceLimit(forceLimit(joint.driveMaxForce));
                    }else{
                        motor.SetTorqueLimit(forceLimit(joint.driveMaxTorque));
                    }
                    sixDOF->SetMotorState(eaxis, JPH::EMotorState::Position);
                }else{
                    if (linear){
                        motor.SetForceLimit(0.0f);
                    }else{
                        motor.SetTorqueLimit(0.0f);
                    }
                    sixDOF->SetMotorState(eaxis, JPH::EMotorState::Velocity);
                    joint.dampedAxes |= (uint8_t)(1u << axis);
                }
            }
            // LOCAL targets are bodyA's relative to the connected body: Jolt's negated (jointSign)
            const float sign = jointSign(joint);
            sixDOF->SetTargetPositionCS(toJolt(joint.targetPosition * sign));
            const Quaternion& q = joint.targetRotation;
            const JPH::Quat target = JPH::Quat(q.x, q.y, q.z, q.w).Normalized();
            sixDOF->SetTargetOrientationCS(local ? target.Conjugated() : target);
            sixDOF->SetTargetVelocityCS(toJolt(joint.targetVelocity * sign));
            const Vector3 angularVelocity(Angle::defaultToRad(joint.targetAngularVelocity.x), Angle::defaultToRad(joint.targetAngularVelocity.y),
                                          Angle::defaultToRad(joint.targetAngularVelocity.z));
            sixDOF->SetTargetAngularVelocityCS(toJolt(angularVelocity * sign));
            break;
        }
        default:
            break;
    }

    // a sleeping pair would not see the change
    JPH::Body* bodyA = joint.joint->GetBody1();
    JPH::Body* bodyB = joint.joint->GetBody2();
    JPH::BodyInterface& body_interface = getBodyInterface3D();
    if (bodyA && bodyA != &JPH::Body::sFixedToWorld && !bodyA->IsStatic() && bodyA->IsInBroadPhase()) body_interface.ActivateBody(bodyA->GetID());
    if (bodyB && bodyB != &JPH::Body::sFixedToWorld && !bodyB->IsStatic() && bodyB->IsInBroadPhase()) body_interface.ActivateBody(bodyB->GetID());
}

void PhysicsSystem::destroyJoint3D(Joint3DComponent& joint){
    releaseJointPair3D(joint);
    if (joint.joint){
        invalidateDependentJoints3D(joint.joint);
        // Parked like a body, the Ref keeping the constraint alive until the flush.
        if (steppingWorld3D){
            pendingJointDestroy3D.push_back(joint.joint);
        }else{
            world3D.RemoveConstraint(joint.joint);
        }

        joint.joint = NULL;
    }
}

float PhysicsSystem::getJoint3DPosition(const Joint3DComponent& joint){
    if (!joint.joint){
        return 0.0f;
    }
    if (joint.type == Joint3DType::HINGE && joint.joint->GetSubType() == JPH::EConstraintSubType::Hinge){
        return jointSign(joint) * static_cast<JPH::HingeConstraint*>(joint.joint)->GetCurrentAngle() + joint.limitsCenter;
    }
    if (joint.type == Joint3DType::PRISMATIC && joint.joint->GetSubType() == JPH::EConstraintSubType::Slider){
        return jointSign(joint) * static_cast<JPH::SliderConstraint*>(joint.joint)->GetCurrentPosition() + joint.limitsCenter;
    }
    return 0.0f;
}

float PhysicsSystem::getJoint3DSpeed(const Joint3DComponent& joint){
    if (!joint.joint){
        return 0.0f;
    }
    const JPH::Body* body1 = joint.joint->GetBody1();
    const JPH::Body* body2 = joint.joint->GetBody2();
    // the joint's primary axis (hinge axis, slide direction) in world space
    const JPH::Vec3 axis = (body1->GetRotation() * joint.joint->GetConstraintToBody1Matrix().GetAxisX()).Normalized();
    if (joint.type == Joint3DType::HINGE){
        return jointSign(joint) * (body2->GetAngularVelocity() - body1->GetAngularVelocity()).Dot(axis);
    }
    if (joint.type == Joint3DType::PRISMATIC){
        const JPH::RVec3 point1 = body1->GetCenterOfMassTransform() * joint.joint->GetConstraintToBody1Matrix().GetTranslation();
        const JPH::RVec3 point2 = body2->GetCenterOfMassTransform() * joint.joint->GetConstraintToBody2Matrix().GetTranslation();
        return jointSign(joint) * (body2->GetPointVelocity(point2) - body1->GetPointVelocity(point1)).Dot(axis);
    }
    return 0.0f;
}

float PhysicsSystem::getJoint3DLength(const Joint3DComponent& joint){
    if (!joint.joint){
        return 0.0f;
    }
    const JPH::Body* body1 = joint.joint->GetBody1();
    const JPH::Body* body2 = joint.joint->GetBody2();
    const JPH::RVec3 point1 = body1->GetCenterOfMassTransform() * joint.joint->GetConstraintToBody1Matrix().GetTranslation();
    const JPH::RVec3 point2 = body2->GetCenterOfMassTransform() * joint.joint->GetConstraintToBody2Matrix().GetTranslation();
    return (float)(point2 - point1).Length();
}

void PhysicsSystem::checkJointBreaks3D(float dt){
    if (!(dt > 0.0f)){
        return;
    }
    auto joints3d = scene->getComponentArray<Joint3DComponent>();
    for (size_t i = 0; i < joints3d->size(); i++){
        Joint3DComponent& joint = joints3d->getComponentFromIndex(i);
        // broken with DISABLE, then enabled again some other way (Inspector, Catalog): it can break again
        if (joint.broken && joint.enabled && joint.breakAction != JointBreakAction::DESTROY){
            joint.broken = false;
            joint.breakArmed = true;
        }
        if (!joint.joint){
            continue;
        }

        // the impulses of the step's constraint parts, as force and torque (Unity
        // currentForce / currentTorque)
        Vector3 force = Vector3::ZERO;
        Vector3 torque = Vector3::ZERO;
        JPH::TwoBodyConstraint* constraint = joint.joint;
        switch (constraint->GetSubType()){
            case JPH::EConstraintSubType::Fixed: {
                const JPH::FixedConstraint* c = static_cast<const JPH::FixedConstraint*>(constraint);
                force = perStep(c->GetTotalLambdaPosition(), dt);
                torque = perStep(c->GetTotalLambdaRotation(), dt);
                break;
            }
            case JPH::EConstraintSubType::Point: {
                force = perStep(static_cast<const JPH::PointConstraint*>(constraint)->GetTotalLambdaPosition(), dt);
                break;
            }
            case JPH::EConstraintSubType::Hinge: {
                const JPH::HingeConstraint* c = static_cast<const JPH::HingeConstraint*>(constraint);
                force = perStep(c->GetTotalLambdaPosition(), dt);
                const JPH::Vector<2> rotation = c->GetTotalLambdaRotation();
                const float axial = c->GetTotalLambdaRotationLimits() + c->GetTotalLambdaMotor();
                torque = Vector3(rotation[0], rotation[1], axial) / dt;
                break;
            }
            case JPH::EConstraintSubType::Slider: {
                const JPH::SliderConstraint* c = static_cast<const JPH::SliderConstraint*>(constraint);
                const JPH::Vector<2> position = c->GetTotalLambdaPosition();
                const float axial = c->GetTotalLambdaPositionLimits() + c->GetTotalLambdaMotor();
                force = Vector3(axial, position[0], position[1]) / dt;
                torque = perStep(c->GetTotalLambdaRotation(), dt);
                break;
            }
            case JPH::EConstraintSubType::Distance: {
                const float lambda = static_cast<const JPH::DistanceConstraint*>(constraint)->GetTotalLambdaPosition();
                const JPH::Body* body1 = constraint->GetBody1();
                const JPH::Body* body2 = constraint->GetBody2();
                const JPH::RVec3 point1 = body1->GetCenterOfMassTransform() * constraint->GetConstraintToBody1Matrix().GetTranslation();
                const JPH::RVec3 point2 = body2->GetCenterOfMassTransform() * constraint->GetConstraintToBody2Matrix().GetTranslation();
                const JPH::Vec3 direction = JPH::Vec3(point2 - point1).NormalizedOr(JPH::Vec3::sAxisY());
                force = fromJolt(direction * (lambda / dt));
                break;
            }
            case JPH::EConstraintSubType::Cone: {
                const JPH::ConeConstraint* c = static_cast<const JPH::ConeConstraint*>(constraint);
                force = perStep(c->GetTotalLambdaPosition(), dt);
                torque = Vector3(c->GetTotalLambdaRotation() / dt, 0.0f, 0.0f);
                break;
            }
            case JPH::EConstraintSubType::SwingTwist: {
                const JPH::SwingTwistConstraint* c = static_cast<const JPH::SwingTwistConstraint*>(constraint);
                force = perStep(c->GetTotalLambdaPosition(), dt);
                torque = Vector3(c->GetTotalLambdaTwist(), c->GetTotalLambdaSwingY(), c->GetTotalLambdaSwingZ()) / dt + perStep(c->GetTotalLambdaMotor(), dt);
                break;
            }
            case JPH::EConstraintSubType::SixDOF: {
                const JPH::SixDOFConstraint* c = static_cast<const JPH::SixDOFConstraint*>(constraint);
                force = perStep(c->GetTotalLambdaPosition() + c->GetTotalLambdaMotorTranslation(), dt);
                torque = perStep(c->GetTotalLambdaRotation() + c->GetTotalLambdaMotorRotation(), dt);
                break;
            }
            default:
                break;
        }
        joint.currentForce = force;
        joint.currentTorque = torque;

        if (joint.broken){
            continue;
        }
        const bool finiteForce = std::isfinite(joint.breakForce);
        const bool finiteTorque = std::isfinite(joint.breakTorque);
        if (!finiteForce && !finiteTorque){
            continue;
        }
        const float forceLength = force.length();
        const bool over = (finiteForce && forceLength > joint.breakForce) || (finiteTorque && torque.length() > joint.breakTorque);
        if (!over){
            // a callback-only joint reports again once the load went back under the thresholds
            joint.breakArmed = true;
            continue;
        }
        if (!joint.breakArmed || joint.breakAction == JointBreakAction::IGNORE){
            continue;
        }

        const Entity entity = joints3d->getEntity(i);
        joint.breakArmed = false;
        switch (joint.breakAction){
            case JointBreakAction::CALLBACK_ONLY:
                pendingJointBreaks3D.push_back({entity, joint.bodyA, forceLength, false});
                break;
            case JointBreakAction::DISABLE:
                // leaves the world before the next step, as any disabled joint
                joint.enabled = false;
                joint.broken = true;
                pendingJointBreaks3D.push_back({entity, joint.bodyA, forceLength, false});
                break;
            case JointBreakAction::DESTROY:
                joint.broken = true;
                // it holds no more: out of the world now, the component goes after the callbacks
                destroyJoint3D(joint);
                pendingJointBreaks3D.push_back({entity, joint.bodyA, forceLength, true});
                break;
            default:
                break;
        }
    }
}

void PhysicsSystem::dispatchJointBreaks3D(){
    if (pendingJointBreaks3D.empty()){
        return;
    }
    std::vector<JointBreak3D> breaks;
    breaks.swap(pendingJointBreaks3D);

    // onJointBreak(force, joint) on the joint's GameObject, and on bodyA's when it is another one
    // (Unity's joint lives on its Rigidbody)
    for (const JointBreak3D& jointBreak : breaks){
        const Entity jointEntity = jointBreak.joint;
        const float force = jointBreak.force;
        auto pushArgs = [this, jointEntity, force](lua_State* L){
            lua_pushnumber(L, force);
            if (!luabridge::push(L, Joint3D(scene, jointEntity))){
                lua_pushnil(L);
            }
            return 2;
        };
        auto callCpp = [this, jointEntity, force](ScriptBase& script){
            Joint3D joint(scene, jointEntity);
            script.onJointBreak(force, joint);
        };
        // a callback of the step may have removed the joint already: no message then
        if (!scene->isEntityCreated(jointEntity) || !scene->findComponent<Joint3DComponent>(jointEntity)){
            continue;
        }
        ScriptMessages::send(scene, jointEntity, ScriptMessage::JointBreak, pushArgs, callCpp);
        if (jointBreak.bodyA != NULL_ENTITY && jointBreak.bodyA != jointEntity && scene->isEntityCreated(jointBreak.bodyA) &&
                scene->findComponent<Joint3DComponent>(jointEntity)){
            ScriptMessages::send(scene, jointBreak.bodyA, ScriptMessage::JointBreak, pushArgs, callCpp);
        }
    }

    // DESTROY: the component goes after every callback of the step
    for (const JointBreak3D& jointBreak : breaks){
        if (!jointBreak.destroy || !scene->isEntityCreated(jointBreak.joint)){
            continue;
        }
        Joint3DComponent* joint = scene->findComponent<Joint3DComponent>(jointBreak.joint);
        if (joint && joint->broken){
            scene->removeComponent<Joint3DComponent>(jointBreak.joint);
        }
    }
}

// ---- the old loaders (Lua PhysicsSystem:load*Joint3D, Joint3D:set*Joint): world points, the
// other fields as Jolt had them, made now ----

namespace {
    Entity jointEntityOf(Scene* scene, const Joint3DComponent& joint){
        auto joints3d = scene->getComponentArray<Joint3DComponent>();
        for (size_t i = 0; i < joints3d->size(); i++){
            if (&joints3d->getComponentFromIndex(i) == &joint){
                return joints3d->getEntity(i);
            }
        }
        return NULL_ENTITY;
    }
}

bool PhysicsSystem::loadFixedJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB){
    setJoint3DLegacyParameters(joint, Joint3DType::FIXED);
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    return buildJoint3D(jointEntityOf(scene, joint), joint);
}

bool PhysicsSystem::loadDistanceJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchorA, Vector3 anchorB, bool autoAnchors){
    setJoint3DLegacyParameters(joint, Joint3DType::DISTANCE);
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    joint.anchorA = anchorA;
    joint.anchorB = anchorB;
    joint.autoAnchors = autoAnchors;
    return buildJoint3D(jointEntityOf(scene, joint), joint);
}

bool PhysicsSystem::loadPointJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchor){
    setJoint3DLegacyParameters(joint, Joint3DType::POINT);
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    joint.anchor = anchor;
    return buildJoint3D(jointEntityOf(scene, joint), joint);
}

bool PhysicsSystem::loadHingeJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchor, Vector3 axis, Vector3 normal){
    setJoint3DLegacyParameters(joint, Joint3DType::HINGE);
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    joint.anchor = anchor;
    joint.axis = axis;
    joint.normal = normal;
    return buildJoint3D(jointEntityOf(scene, joint), joint);
}

bool PhysicsSystem::loadConeJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchor, Vector3 twistAxis){
    setJoint3DLegacyParameters(joint, Joint3DType::CONE);
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    joint.anchor = anchor;
    joint.twistAxis = twistAxis;
    return buildJoint3D(jointEntityOf(scene, joint), joint);
}

bool PhysicsSystem::loadPrismaticJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 sliderAxis, float limitsMin, float limitsMax){
    setJoint3DLegacyParameters(joint, Joint3DType::PRISMATIC);
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    joint.axis = sliderAxis;
    joint.limitsMin = limitsMin;
    joint.limitsMax = limitsMax;
    return buildJoint3D(jointEntityOf(scene, joint), joint);
}

bool PhysicsSystem::loadSwingTwistJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchor, Vector3 twistAxis, Vector3 planeAxis, float normalHalfConeAngle, float planeHalfConeAngle, float twistMinAngle, float twistMaxAngle){
    setJoint3DLegacyParameters(joint, Joint3DType::SWINGTWIST);
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    joint.anchor = anchor;
    joint.twistAxis = twistAxis;
    joint.planeAxis = planeAxis;
    joint.normalHalfConeAngle = normalHalfConeAngle;
    joint.planeHalfConeAngle = planeHalfConeAngle;
    joint.twistMinAngle = twistMinAngle;
    joint.twistMaxAngle = twistMaxAngle;
    return buildJoint3D(jointEntityOf(scene, joint), joint);
}

bool PhysicsSystem::loadSixDOFJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchorA, Vector3 anchorB, Vector3 axisX, Vector3 axisY){
    setJoint3DLegacyParameters(joint, Joint3DType::SIXDOF);
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    joint.anchorA = anchorA;
    joint.anchorB = anchorB;
    joint.axisX = axisX;
    joint.axisY = axisY;
    return buildJoint3D(jointEntityOf(scene, joint), joint);
}

bool PhysicsSystem::loadPathJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, std::vector<Vector3> positions, std::vector<Vector3> tangents, std::vector<Vector3> normals, Vector3 pathPosition, bool isLooping){
    // the path's tangents and normals are made from its points
    (void)tangents;
    (void)normals;
    setJoint3DLegacyParameters(joint, Joint3DType::PATH);
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    joint.pathPoints = positions;
    joint.pathPosition = pathPosition;
    joint.isLooping = isLooping;
    return buildJoint3D(jointEntityOf(scene, joint), joint);
}

bool PhysicsSystem::loadGearJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Entity hingeA, Entity hingeB, int numTeethGearA, int numTeethGearB){
    setJoint3DLegacyParameters(joint, Joint3DType::GEAR);
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    joint.hingeA = hingeA;
    joint.hingeB = hingeB;
    joint.numTeethGearA = numTeethGearA;
    joint.numTeethGearB = numTeethGearB;
    return buildJoint3D(jointEntityOf(scene, joint), joint);
}

bool PhysicsSystem::loadRackAndPinionJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Entity hinge, Entity slider, int numTeethRack, int numTeethGear, int rackLength){
    setJoint3DLegacyParameters(joint, Joint3DType::RACKANDPINON);
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    joint.hinge = hinge;
    joint.slider = slider;
    joint.numTeethRack = numTeethRack;
    joint.numTeethGear = numTeethGear;
    joint.rackLength = rackLength;
    return buildJoint3D(jointEntityOf(scene, joint), joint);
}

bool PhysicsSystem::loadPulleyJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchorA, Vector3 anchorB, Vector3 fixedPointA, Vector3 fixedPointB){
    setJoint3DLegacyParameters(joint, Joint3DType::PULLEY);
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    joint.anchorA = anchorA;
    joint.anchorB = anchorB;
    joint.fixedPointA = fixedPointA;
    joint.fixedPointB = fixedPointB;
    return buildJoint3D(jointEntityOf(scene, joint), joint);
}

// ---- live re-centring, dependents, damped six-DOF axes ----------------------------------------

// A hinge or slider whose limits moved their centre: made again from the frames it holds (body
// local, as it was first made), turned or shifted by the change, so its zero stays where it was
// whatever the bodies' poses now (rebuilding from the poses would move it)
bool PhysicsSystem::recenterJoint3D(Entity entity, Joint3DComponent& joint, float center){
    if (!joint.joint || steppingWorld3D){
        return false;
    }
    const float delta = center - joint.limitsCenter;
    JPH::Ref<JPH::ConstraintSettings> base = joint.joint->GetConstraintSettings();
    JPH::Body* body1 = joint.joint->GetBody1();
    JPH::Body* body2 = joint.joint->GetBody2();
    if (!base || !body1 || !body2){
        return false;
    }
    JPH::TwoBodyConstraint* made = nullptr;
    if (joint.type == Joint3DType::HINGE && joint.joint->GetSubType() == JPH::EConstraintSubType::Hinge){
        JPH::HingeConstraintSettings* settings = static_cast<JPH::HingeConstraintSettings*>(base.GetPtr());
        settings->mNormalAxis2 = (JPH::Quat::sRotation(settings->mHingeAxis2.Normalized(), delta) * settings->mNormalAxis2).Normalized();
        made = settings->Create(*body1, *body2);
    }else if (joint.type == Joint3DType::PRISMATIC && joint.joint->GetSubType() == JPH::EConstraintSubType::Slider){
        JPH::SliderConstraintSettings* settings = static_cast<JPH::SliderConstraintSettings*>(base.GetPtr());
        settings->mPoint1 -= JPH::RVec3(settings->mSliderAxis1.Normalized() * delta);
        made = settings->Create(*body1, *body2);
    }
    if (!made){
        return false;
    }
    JPH::Ref<JPH::TwoBodyConstraint> previous = joint.joint;
    invalidateDependentJoints3D(joint.joint);
    world3D.RemoveConstraint(joint.joint);
    joint.joint = made;
    world3D.AddConstraint(joint.joint);
    joint.limitsCenter = center;
    applyJoint3DParameters(entity, joint);
    return true;
}

// gears and racks hold the joints they drive: one of those going takes them with it (made
// again once it is back)
void PhysicsSystem::invalidateDependentJoints3D(const JPH::TwoBodyConstraint* constraint){
    if (!constraint){
        return;
    }
    auto joints3d = scene->getComponentArray<Joint3DComponent>();
    for (size_t i = 0; i < joints3d->size(); i++){
        Joint3DComponent& other = joints3d->getComponentFromIndex(i);
        if (!other.joint || (other.type != Joint3DType::GEAR && other.type != Joint3DType::RACKANDPINON)){
            continue;
        }
        const Entity first = other.type == Joint3DType::GEAR ? other.hingeA : other.hinge;
        const Entity second = other.type == Joint3DType::GEAR ? other.hingeB : other.slider;
        for (Entity driven : {first, second}){
            const Joint3DComponent* drivenJoint = (driven != NULL_ENTITY && scene->isEntityCreated(driven)) ? scene->findComponent<Joint3DComponent>(driven) : nullptr;
            if (drivenJoint && drivenJoint->joint == constraint){
                destroyJoint3D(other);
                other.needUpdateJoint = true;
                break;
            }
        }
    }
}

// Unity's damper-only drives (no stiffness): force = damping x (target velocity - velocity) on
// the axis. Jolt's velocity motor reaches its target at once, so its force limit is set to that
// force before each step (it then applies exactly it, or less when that would overshoot)
void PhysicsSystem::updateSixDOFDampers3D(){
    auto joints3d = scene->getComponentArray<Joint3DComponent>();
    for (size_t i = 0; i < joints3d->size(); i++){
        Joint3DComponent& joint = joints3d->getComponentFromIndex(i);
        if (!joint.joint || joint.type != Joint3DType::SIXDOF || joint.dampedAxes == 0 ||
                joint.joint->GetSubType() != JPH::EConstraintSubType::SixDOF){
            continue;
        }
        JPH::SixDOFConstraint* sixDOF = static_cast<JPH::SixDOFConstraint*>(joint.joint);
        const JPH::Body* body1 = sixDOF->GetBody1();
        const JPH::Body* body2 = sixDOF->GetBody2();
        const JPH::RMat44 frame1 = body1->GetCenterOfMassTransform() * sixDOF->GetConstraintToBody1Matrix();
        const JPH::Quat inverse = frame1.GetQuaternion().Conjugated();
        const JPH::RVec3 point = frame1.GetTranslation();
        const JPH::Vec3 linear = inverse * (body2->GetPointVelocity(point) - body1->GetPointVelocity(point));
        const JPH::Vec3 angular = inverse * (body2->GetAngularVelocity() - body1->GetAngularVelocity());
        const JPH::Vec3 targetLinear = sixDOF->GetTargetVelocityCS();
        const JPH::Vec3 targetAngular = sixDOF->GetTargetAngularVelocityCS();
        for (int axis = 0; axis < 6; axis++){
            if (!(joint.dampedAxes & (1u << axis))){
                continue;
            }
            const bool isLinear = axis < 3;
            float stiffness, damping;
            sixDOFDrive(joint, axis, stiffness, damping);
            const float error = isLinear ? targetLinear[axis] - linear[axis] : targetAngular[axis - 3] - angular[axis - 3];
            const float cap = forceLimit(isLinear ? joint.driveMaxForce : joint.driveMaxTorque);
            const float limit = std::min(cap, damping * std::fabs(error));
            JPH::MotorSettings& motor = sixDOF->GetMotorSettings(sixDOFAxis(axis));
            if (isLinear){
                motor.SetForceLimit(limit);
            }else{
                motor.SetTorqueLimit(limit);
            }
        }
    }
}

#endif
