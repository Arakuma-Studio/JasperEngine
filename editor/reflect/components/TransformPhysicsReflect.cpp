// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Transform, joints and rigid bodies (docs/specs/S08-reflection.md P2/P3). Field order is the
// YAML key order.

#include "reflect/Components.h"

#include "Factory.h"
#include "Stream.h"
#include "component/Joint2DComponent.h"
#include "component/Joint3DComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"

#include "box2d/box2d.h"

namespace jasperengine::editor::reflect {

namespace {

    // ---------------------------------------------------------------- Transform

    constexpr FieldDesc kTransformFields[] = {
        JE_FIELD(Transform, position, .flags = NonStructural, .updateFlags = UpdateFlags_Transform | UpdateFlags_Layout_Anchors),
        JE_FIELD(Transform, rotation, .flags = NonStructural, .updateFlags = UpdateFlags_Transform, .drawer = "quatEuler"),
        JE_FIELD(Transform, scale, .flags = NonStructural, .updateFlags = UpdateFlags_Transform | UpdateFlags_Rigidbody2D | UpdateFlags_Rigidbody),
        // the world values and matrices the transform system computes: neither edited nor written
        // (docs/specs/S29-version-control.md R20; older files' are read, then recomputed)
        JE_FIELD(Transform, worldPosition, .flags = Derived | Hidden | NotWritten),
        JE_FIELD(Transform, worldRotation, .flags = Derived | Hidden | NotWritten),
        JE_FIELD(Transform, worldScale, .flags = Derived | Hidden | NotWritten),
        JE_FIELD(Transform, localMatrix, .flags = Derived | Hidden | NotWritten),
        JE_FIELD(Transform, modelMatrix, .flags = Derived | Hidden | NotWritten),
        JE_FIELD(Transform, normalMatrix, .flags = Derived | Hidden | NotWritten),
        JE_FIELD(Transform, visible),
        JE_FIELD(Transform, billboardRotation, .updateFlags = UpdateFlags_Transform),
        JE_FIELD(Transform, billboard, .updateFlags = UpdateFlags_Transform),
        JE_FIELD(Transform, fakeBillboard, .updateFlags = UpdateFlags_Transform),
        JE_FIELD(Transform, cylindricalBillboard, .updateFlags = UpdateFlags_Transform),
    };

    // the world values are not in the file: the transform pass computes them (R20)
    void transformPostDecode(void* comp, const DecodeContext&) {
        static_cast<Transform*>(comp)->needUpdate = true;
    }

    std::string transformCodegen(EntityRegistry* registry, Entity entity, const CodegenContext& ctx) {
        return Factory::createTransform(ctx.indentSpaces, registry, entity, ctx.sceneName, ctx.entityName, false, ctx.assignExisting,
                                        ctx.entityVarNames);
    }

    // ---------------------------------------------------------------- Joint 2D

    constexpr FieldDesc kJoint2DFields[] = {
        JE_FIELD(Joint2DComponent, enabled, .flags = CodegenIfChanged),
        JE_FIELD(Joint2DComponent, type, .enumDesc = &enums::kJoint2DType, .updateFlags = UpdateFlags_Joint2D),
        JE_FIELD(Joint2DComponent, bodyA, .ops = &ops::Entity, .updateFlags = UpdateFlags_Joint2D),
        JE_FIELD(Joint2DComponent, bodyB, .ops = &ops::Entity, .updateFlags = UpdateFlags_Joint2D),
        JE_FIELD(Joint2DComponent, anchorA, .updateFlags = UpdateFlags_Joint2D),
        JE_FIELD(Joint2DComponent, anchorB, .updateFlags = UpdateFlags_Joint2D),
        JE_FIELD(Joint2DComponent, axis, .updateFlags = UpdateFlags_Joint2D),
        JE_FIELD(Joint2DComponent, target, .updateFlags = UpdateFlags_Joint2D),
        JE_FIELD(Joint2DComponent, autoAnchors, .updateFlags = UpdateFlags_Joint2D),
        JE_FIELD(Joint2DComponent, rope, .updateFlags = UpdateFlags_Joint2D),
        // upstream (physics-2d / physics-3d): motors, springs, limits, breaking, anchor space
        JE_FIELD(Joint2DComponent, anchorSpace, .enumDesc = &enums::kJointAnchorSpace, .updateFlags = UpdateFlags_Joint2D),
        JE_FIELD(Joint2DComponent, enableCollision, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, breakForce, .ops = &ops::FloatOrInfinity, .flags = CodegenIfChanged, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, breakTorque, .ops = &ops::FloatOrInfinity, .flags = CodegenIfChanged, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, breakAction, .enumDesc = &enums::kJointBreakAction, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, useLimits, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, lowerAngle, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, upperAngle, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, lowerTranslation, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, upperTranslation, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, useMotor, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, motorSpeed, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, maxMotorTorque, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, maxMotorForce, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, enableSpring, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, frequency, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, dampingRatio, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, length, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, autoLength, .updateFlags = UpdateFlags_Joint2D),
        JE_FIELD(Joint2DComponent, minLength, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, maxLength, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, linearFrequency, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, angularFrequency, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, linearDampingRatio, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, angularDampingRatio, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, linearOffset, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, angularOffset, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, autoOffset, .updateFlags = UpdateFlags_Joint2D),
        JE_FIELD(Joint2DComponent, maxForce, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, maxTorque, .updateFlags = UpdateFlags_Joint2D_Params),
        JE_FIELD(Joint2DComponent, correctionFactor, .updateFlags = UpdateFlags_Joint2D_Params),
        // the exported joint is built on load
        JE_FIELD(Joint2DComponent, needUpdateJoint, .flags = NoSave | Derived | Hidden | CodegenRuntime),
    };

    constexpr const char* kJoint2DCodegenOrder[] = {
        "type", "bodyA", "bodyB", "anchorA", "anchorB", "axis", "target", "autoAnchors", "rope", "anchorSpace", "enableCollision", "breakForce", "breakTorque", "breakAction", "useLimits", "lowerAngle", "upperAngle", "lowerTranslation", "upperTranslation", "useMotor", "motorSpeed", "maxMotorTorque", "maxMotorForce", "enableSpring", "frequency", "dampingRatio", "length", "autoLength", "minLength", "maxLength", "linearFrequency", "angularFrequency", "linearDampingRatio", "angularDampingRatio", "linearOffset", "angularOffset", "autoOffset", "maxForce", "maxTorque", "correctionFactor", "needUpdateJoint", "enabled"
    };

    void joint2DPostDecode(void* comp, const DecodeContext& ctx) {
        const Joint2DComponent* old = static_cast<const Joint2DComponent*>(ctx.old);
        if (old && b2Joint_IsValid(old->joint)) static_cast<Joint2DComponent*>(comp)->needUpdateJoint = true;
    }

    // ---------------------------------------------------------------- Joint (3D)

    constexpr uint64_t kJoint3D = UpdateFlags_Joint3D;

    constexpr FieldDesc kJoint3DFields[] = {
        JE_FIELD(Joint3DComponent, enabled, .flags = CodegenIfChanged),
        JE_FIELD(Joint3DComponent, type, .enumDesc = &enums::kJoint3DType, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, bodyA, .ops = &ops::Entity, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, bodyB, .ops = &ops::Entity, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, anchorA, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, anchorB, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, anchor, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, axis, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, normal, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, twistAxis, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, planeAxis, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, axisX, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, axisY, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, limitsMin, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, limitsMax, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, normalHalfConeAngle, .updateFlags = kJoint3D, .unit = Unit::Degrees),
        JE_FIELD(Joint3DComponent, planeHalfConeAngle, .updateFlags = kJoint3D, .unit = Unit::Degrees),
        JE_FIELD(Joint3DComponent, twistMinAngle, .updateFlags = kJoint3D, .unit = Unit::Degrees),
        JE_FIELD(Joint3DComponent, twistMaxAngle, .updateFlags = kJoint3D, .unit = Unit::Degrees),
        JE_FIELD(Joint3DComponent, fixedPointA, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, fixedPointB, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, hingeA, .ops = &ops::Entity, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, hingeB, .ops = &ops::Entity, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, hinge, .ops = &ops::Entity, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, slider, .ops = &ops::Entity, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, numTeethGearA, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, numTeethGearB, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, numTeethRack, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, numTeethGear, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, rackLength, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, pathPoints, .ops = &ops::PushBackListCleared<Vector3>, .flags = NullIfEmpty | ListWhole | ListElements | ListTemplate, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, pathPosition, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, isLooping, .updateFlags = kJoint3D),
        JE_FIELD(Joint3DComponent, autoAnchors, .updateFlags = kJoint3D),
        // upstream (physics-2d / physics-3d): motors, springs, limits, breaking, anchor space
        JE_FIELD(Joint3DComponent, anchorSpace, .enumDesc = &enums::kJointAnchorSpace, .updateFlags = UpdateFlags_Joint3D),
        JE_FIELD(Joint3DComponent, enableCollision, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, breakForce, .ops = &ops::FloatOrInfinity, .flags = CodegenIfChanged, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, breakTorque, .ops = &ops::FloatOrInfinity, .flags = CodegenIfChanged, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, breakAction, .enumDesc = &enums::kJointBreakAction, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, useLimits, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, limitsSpringFrequency, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, limitsSpringDamping, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, useMotor, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, motorTargetVelocity, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, motorMaxForce, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, useSpring, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, springStiffness, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, springDamper, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, springTargetPosition, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, maxFriction, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, minDistance, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, maxDistance, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, coneHalfAngle, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, swingType, .enumDesc = &enums::kJointSwingType, .updateFlags = UpdateFlags_Joint3D),
        JE_FIELD(Joint3DComponent, motorTargetRotation, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, motorSpringFrequency, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, motorSpringDamping, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, xMotion, .enumDesc = &enums::kJointMotion, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, yMotion, .enumDesc = &enums::kJointMotion, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, zMotion, .enumDesc = &enums::kJointMotion, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, angularXMotion, .enumDesc = &enums::kJointMotion, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, angularYMotion, .enumDesc = &enums::kJointMotion, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, angularZMotion, .enumDesc = &enums::kJointMotion, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, linearLimitMin, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, linearLimitMax, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, angularLimitMin, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, angularLimitMax, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, useDrive, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, driveStiffness, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, driveDamping, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, angularDriveStiffness, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, angularDriveDamping, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, driveMaxForce, .ops = &ops::FloatOrInfinity, .flags = CodegenIfChanged, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, driveMaxTorque, .ops = &ops::FloatOrInfinity, .flags = CodegenIfChanged, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, targetPosition, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, targetRotation, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, targetVelocity, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, targetAngularVelocity, .updateFlags = UpdateFlags_Joint3D_Params),
        JE_FIELD(Joint3DComponent, needUpdateJoint, .flags = NoSave | Derived | Hidden | CodegenRuntime),
    };

    constexpr const char* kJoint3DCodegenOrder[] = {
        "type", "bodyA", "bodyB", "anchorA", "anchorB", "anchor", "axis", "normal", "twistAxis", "planeAxis", "axisX", "axisY", "limitsMin",
        "limitsMax", "normalHalfConeAngle", "planeHalfConeAngle", "twistMinAngle", "twistMaxAngle", "fixedPointA", "fixedPointB", "hingeA",
        "hingeB", "hinge", "slider", "numTeethGearA", "numTeethGearB", "numTeethRack", "numTeethGear", "rackLength", "pathPoints",
        "pathPosition", "isLooping", "autoAnchors", "anchorSpace", "enableCollision", "breakForce", "breakTorque", "breakAction", "useLimits", "limitsSpringFrequency", "limitsSpringDamping", "useMotor", "motorTargetVelocity", "motorMaxForce", "useSpring", "springStiffness", "springDamper", "springTargetPosition", "maxFriction", "minDistance", "maxDistance", "coneHalfAngle", "swingType", "motorTargetRotation", "motorSpringFrequency", "motorSpringDamping", "xMotion", "yMotion", "zMotion", "angularXMotion", "angularYMotion", "angularZMotion", "linearLimitMin", "linearLimitMax", "angularLimitMin", "angularLimitMax", "useDrive", "driveStiffness", "driveDamping", "angularDriveStiffness", "angularDriveDamping", "driveMaxForce", "driveMaxTorque", "targetPosition", "targetRotation", "targetVelocity", "targetAngularVelocity", "needUpdateJoint", "enabled"
    };

    void joint3DPostDecode(void* comp, const DecodeContext& ctx) {
        const Joint3DComponent* old = static_cast<const Joint3DComponent*>(ctx.old);
        if (old && old->joint) static_cast<Joint3DComponent*>(comp)->needUpdateJoint = true;
    }

    // ---------------------------------------------------------------- rigid bodies (their own codecs)

    constexpr uint64_t kBody2D = UpdateFlags_Rigidbody2D;

    constexpr FieldDesc kCollider2DFields[] = {
        JE_FIELD(Collider2D, type, .enumDesc = &enums::kCollider2DType, .updateFlags = kBody2D),
        JE_FIELD(Collider2D, pointA, .updateFlags = kBody2D),
        JE_FIELD(Collider2D, pointB, .updateFlags = kBody2D),
        JE_FIELD(Collider2D, radius, .updateFlags = kBody2D, .min = 0.0f),
        // upstream (physics-2d): the polygon / chain points, a list without a count of its own
        JE_FIELD(Collider2D, points, .flags = ListElements | ListTemplate, .updateFlags = kBody2D),
        JE_FIELD(Collider2D, loop, .updateFlags = kBody2D),
        JE_FIELD(Collider2D, density, .updateFlags = kBody2D, .min = 0.0f),
        JE_FIELD(Collider2D, friction, .updateFlags = kBody2D, .min = 0.0f),
        JE_FIELD(Collider2D, restitution, .updateFlags = kBody2D, .min = 0.0f),
        JE_FIELD(Collider2D, sensor, .updateFlags = kBody2D),
        JE_FIELD(Collider2D, enableHitEvents, .updateFlags = kBody2D),
        JE_FIELD(Collider2D, contactEvents, .updateFlags = kBody2D),
        JE_FIELD(Collider2D, preSolveEvents, .updateFlags = kBody2D),
        JE_FIELD(Collider2D, sensorEvents, .updateFlags = kBody2D),
        // upstream (physics-2d / physics-contacts): its .physicsmaterial and its callbacks' receiver
        JE_FIELD(Collider2D, material, .updateFlags = kBody2D),
        JE_FIELD(Collider2D, receiver, .ops = &ops::Entity, .updateFlags = kBody2D),
        // S44: the box of a slice of the GameObject's sheet sprite on every frame ("" = its own shape)
        JE_FIELD(Collider2D, slice, .updateFlags = kBody2D, .tooltip = "A slice of the Sprite's sheet: the collider is its box on every frame."),
    };

    constexpr uint64_t kBody3D = UpdateFlags_Rigidbody;

    constexpr FieldDesc kCollider3DFields[] = {
        JE_FIELD(Collider3D, position, .updateFlags = kBody3D),
        JE_FIELD(Collider3D, rotation, .updateFlags = kBody3D),
        JE_FIELD(Collider3D, type, .enumDesc = &enums::kCollider3DType, .updateFlags = kBody3D),
        JE_FIELD(Collider3D, width, .updateFlags = kBody3D, .min = 0.0f),
        JE_FIELD(Collider3D, height, .updateFlags = kBody3D, .min = 0.0f),
        JE_FIELD(Collider3D, depth, .updateFlags = kBody3D, .min = 0.0f),
        JE_FIELD(Collider3D, radius, .updateFlags = kBody3D, .min = 0.0f),
        JE_FIELD(Collider3D, halfHeight, .updateFlags = kBody3D, .min = 0.0f),
        JE_FIELD(Collider3D, topRadius, .updateFlags = kBody3D, .min = 0.0f),
        JE_FIELD(Collider3D, bottomRadius, .updateFlags = kBody3D, .min = 0.0f),
        JE_FIELD(Collider3D, density, .updateFlags = kBody3D, .min = 0.0f),
        JE_FIELD(Collider3D, source, .enumDesc = &enums::kCollider3DSource, .updateFlags = kBody3D),
        JE_FIELD(Collider3D, sourceEntity, .ops = &ops::Entity, .updateFlags = kBody3D),
        JE_FIELD(Collider3D, samplesSize, .updateFlags = kBody3D),
        // upstream (physics-3d): the mesh / convex points and indices are lists without counts
        JE_FIELD(Collider3D, vertices, .flags = ListElements | ListTemplate, .updateFlags = kBody3D),
        // upstream (physics-3d / physics-contacts): per-collider trigger, material, receiver
        JE_FIELD(Collider3D, isTrigger, .updateFlags = kBody3D),
        JE_FIELD(Collider3D, material, .updateFlags = kBody3D),
        JE_FIELD(Collider3D, receiver, .ops = &ops::Entity, .updateFlags = kBody3D),
    };

}

JE_REFLECT_STRUCT_TRAITS(Collider2D, kCollider2DStruct);
const StructDesc kCollider2DStruct = JE_STRUCT_DESC(Collider2D, "Collider2D", kCollider2DFields);
JE_REFLECT_STRUCT_TRAITS(Collider3D, kCollider3DStruct);
const StructDesc kCollider3DStruct = JE_STRUCT_DESC(Collider3D, "Collider3D", kCollider3DFields);

namespace {

    constexpr FieldDesc kRigidbody2DFields[] = {
        JE_FIELD(Rigidbody2DComponent, enabled),
        JE_FIELD(Rigidbody2DComponent, type, .enumDesc = &enums::kBodyType, .updateFlags = kBody2D),
        // upstream (physics-2d): Unity's Rigidbody2D body settings, applied to the live body
        JE_FIELD(Rigidbody2DComponent, gravityScale, .updateFlags = UpdateFlags_Rigidbody2D_Params),
        JE_FIELD(Rigidbody2DComponent, linearDamping, .updateFlags = UpdateFlags_Rigidbody2D_Params, .min = 0.0f),
        JE_FIELD(Rigidbody2DComponent, angularDamping, .updateFlags = UpdateFlags_Rigidbody2D_Params, .min = 0.0f),
        JE_FIELD(Rigidbody2DComponent, fixedRotation, .updateFlags = UpdateFlags_Rigidbody2D_Params),
        // S96 WP-E: not capped by the scene's Max Rotation Per Step
        JE_FIELD(Rigidbody2DComponent, allowFastRotation, .updateFlags = UpdateFlags_Rigidbody2D_Params),
        JE_FIELD(Rigidbody2DComponent, collisionDetection, .enumDesc = &enums::kCollisionDetection2D, .updateFlags = UpdateFlags_Rigidbody2D_Params),
        JE_FIELD(Rigidbody2DComponent, sleepingMode, .enumDesc = &enums::kSleepMode2D, .updateFlags = UpdateFlags_Rigidbody2D_Params),
        JE_FIELD(Rigidbody2DComponent, useAutoMass, .updateFlags = UpdateFlags_Rigidbody2D_Params),
        JE_FIELD(Rigidbody2DComponent, mass, .updateFlags = UpdateFlags_Rigidbody2D_Params, .min = 0.0f),
        JE_FIELD(Rigidbody2DComponent, autoCenterOfMass, .updateFlags = UpdateFlags_Rigidbody2D_Params),
        JE_FIELD(Rigidbody2DComponent, centerOfMass, .updateFlags = UpdateFlags_Rigidbody2D_Params),
        JE_FIELD(Rigidbody2DComponent, autoInertia, .updateFlags = UpdateFlags_Rigidbody2D_Params),
        JE_FIELD(Rigidbody2DComponent, inertia, .updateFlags = UpdateFlags_Rigidbody2D_Params, .min = 0.0f),
        JE_FIELD(Rigidbody2DComponent, material, .updateFlags = kBody2D),
        // S96 WP-C: its colliders join the nearest ancestor's body (AttachedColliders.h)
        JE_FIELD(Rigidbody2DComponent, attachToParentBody, .updateFlags = kBody2D),
        // upstream: a body's colliders are a list without a count limit (the files still write
        // numColliders, which the codec reads): the whole list is one property (a prefab override
        // when the count changes), each collider's members too
        JE_FIELD(Rigidbody2DComponent, colliders, .flags = ListWhole | ListEachElement | ListTemplate, .updateFlags = kBody2D),
    };

    constexpr FieldDesc kRigidbodyFields[] = {
        JE_FIELD(RigidbodyComponent, enabled),
        JE_FIELD(RigidbodyComponent, type, .enumDesc = &enums::kBodyType, .updateFlags = kBody3D),
        JE_FIELD(RigidbodyComponent, motionQuality, .enumDesc = &enums::kRigidbodyMotionQuality, .updateFlags = kBody3D),
        JE_FIELD(RigidbodyComponent, sensor, .updateFlags = kBody3D),
        JE_FIELD(RigidbodyComponent, gravityFactor, .updateFlags = kBody3D),
        JE_FIELD(RigidbodyComponent, friction, .updateFlags = kBody3D, .min = 0.0f),
        JE_FIELD(RigidbodyComponent, restitution, .updateFlags = kBody3D, .min = 0.0f),
        // read by each fixed step in a water (Water::applyBuoyancy): the body stays as it is
        JE_FIELD(RigidbodyComponent, buoyancy),
        JE_FIELD(RigidbodyComponent, waterDrag),
        JE_FIELD(RigidbodyComponent, waterAngularDrag),
        JE_FIELD(RigidbodyComponent, overrideMassProperties, .updateFlags = kBody3D),
        JE_FIELD(RigidbodyComponent, solidBoxSize, .updateFlags = kBody3D),
        JE_FIELD(RigidbodyComponent, solidBoxDensity, .updateFlags = kBody3D, .min = 0.0f),
        // upstream (physics-3d): Unity's Rigidbody body settings, applied to the live body
        JE_FIELD(RigidbodyComponent, material, .updateFlags = UpdateFlags_Rigidbody_Params),
        JE_FIELD(RigidbodyComponent, linearDamping, .updateFlags = UpdateFlags_Rigidbody_Params, .min = 0.0f),
        JE_FIELD(RigidbodyComponent, angularDamping, .updateFlags = UpdateFlags_Rigidbody_Params, .min = 0.0f),
        JE_FIELD(RigidbodyComponent, maxLinearVelocity, .updateFlags = UpdateFlags_Rigidbody_Params, .min = 0.0f),
        JE_FIELD(RigidbodyComponent, maxAngularVelocity, .updateFlags = UpdateFlags_Rigidbody_Params, .min = 0.0f),
        JE_FIELD(RigidbodyComponent, automaticMass, .updateFlags = UpdateFlags_Rigidbody_Params),
        JE_FIELD(RigidbodyComponent, mass, .updateFlags = UpdateFlags_Rigidbody_Params, .min = 0.0f),
        JE_FIELD(RigidbodyComponent, automaticCenterOfMass, .updateFlags = UpdateFlags_Rigidbody_Params),
        JE_FIELD(RigidbodyComponent, centerOfMass, .updateFlags = UpdateFlags_Rigidbody_Params),
        JE_FIELD(RigidbodyComponent, automaticInertiaTensor, .updateFlags = UpdateFlags_Rigidbody_Params),
        JE_FIELD(RigidbodyComponent, inertiaTensor, .updateFlags = UpdateFlags_Rigidbody_Params),
        JE_FIELD(RigidbodyComponent, inertiaTensorRotation, .updateFlags = UpdateFlags_Rigidbody_Params),
        JE_FIELD(RigidbodyComponent, allowSleeping, .updateFlags = UpdateFlags_Rigidbody_Params),
        JE_FIELD(RigidbodyComponent, collideKinematicVsNonDynamic, .updateFlags = UpdateFlags_Rigidbody_Params),
        // read when the body is drawn
        JE_FIELD(RigidbodyComponent, interpolation, .enumDesc = &enums::kRigidbodyInterpolation),
        // S96 WP-C: its colliders join the nearest ancestor's body (AttachedColliders.h)
        JE_FIELD(RigidbodyComponent, attachToParentBody, .updateFlags = kBody3D),
        JE_FIELD(RigidbodyComponent, colliders, .flags = ListWhole | ListEachElement | ListTemplate, .updateFlags = kBody3D),
    };

    const ComponentCodec kRigidbody2DCodec = JE_STREAM_CODEC(Rigidbody2DComponent, encodeRigidbody2DComponent, decodeRigidbody2DComponent);

    // a body's colliders are sized by its GameObject's scale
    void decodeRigidbody(const YAML::Node& node, void* comp, const DecodeContext& ctx) {
        Transform* transform = ctx.registry ? ctx.registry->findComponent<Transform>(ctx.entity) : nullptr;
        const Vector3 bodyScale = transform ? transform->scale : Vector3::UNIT_SCALE;
        *static_cast<RigidbodyComponent*>(comp) = Stream::decodeRigidbodyComponent(node, static_cast<const RigidbodyComponent*>(ctx.old), bodyScale);
    }

    const ComponentCodec kRigidbodyCodec{
        [](const void* comp, const EncodeContext&) { return Stream::encodeRigidbodyComponent(*static_cast<const RigidbodyComponent*>(comp)); },
        &decodeRigidbody};

    std::string rigidbody2DCodegen(EntityRegistry* registry, Entity entity, const CodegenContext& ctx) {
        return Factory::createRigidbody2DComponent(ctx.indentSpaces, registry, entity, ctx.sceneName, ctx.entityName, ctx.assignExisting,
                                                   ctx.entityVarNames);
    }

    std::string rigidbodyCodegen(EntityRegistry* registry, Entity entity, const CodegenContext& ctx) {
        return Factory::createRigidbodyComponent(ctx.indentSpaces, registry, entity, ctx.sceneName, ctx.entityName, ctx.assignExisting,
                                                 ctx.entityVarNames);
    }

}

const ComponentDesc kTransformDesc{
    .type = ComponentType::Transform, .cppName = "Transform", .key = "transform", .displayName = "Transform",
    .icon = ICON_FA_UP_DOWN_LEFT_RIGHT,
    .body = JE_STRUCT_DESC(Transform, "Transform", kTransformFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(Transform),
    .postDecode = &transformPostDecode,
    .codegen = &transformCodegen,
};

namespace {

    // upstream's joint codecs (StreamUpstream.cpp): legacy defaults for keys older scenes lack,
    // breaking forces written only when finite
    const ComponentCodec kJoint2DCodec = JE_STREAM_CODEC(Joint2DComponent, encodeJoint2DComponent, decodeJoint2DComponent);
    const ComponentCodec kJoint3DCodec = JE_STREAM_CODEC(Joint3DComponent, encodeJoint3DComponent, decodeJoint3DComponent);
    constexpr const char* kJoint2DKnownKeys[] = {"anchorA", "anchorB", "anchorSpace", "angularDampingRatio", "angularFrequency", "angularOffset", "autoAnchors", "autoLength", "autoOffset", "axis", "bodyA", "bodyB", "breakAction", "breakForce", "breakTorque", "correctionFactor", "dampingRatio", "enableCollision", "enableSpring", "enabled", "frequency", "length", "linearDampingRatio", "linearFrequency", "linearOffset", "lowerAngle", "lowerTranslation", "maxForce", "maxLength", "maxMotorForce", "maxMotorTorque", "maxTorque", "minLength", "motorSpeed", "rope", "target", "type", "upperAngle", "upperTranslation", "useLimits", "useMotor"};
    constexpr const char* kJoint3DKnownKeys[] = {"anchor", "anchorA", "anchorB", "anchorSpace", "angularDriveDamping", "angularDriveStiffness", "angularLimitMax", "angularLimitMin", "angularXMotion", "angularYMotion", "angularZMotion", "autoAnchors", "axis", "axisX", "axisY", "bodyA", "bodyB", "breakAction", "breakForce", "breakTorque", "coneHalfAngle", "driveDamping", "driveMaxForce", "driveMaxTorque", "driveStiffness", "enableCollision", "enabled", "fixedPointA", "fixedPointB", "hinge", "hingeA", "hingeB", "isLooping", "limitsMax", "limitsMin", "limitsSpringDamping", "limitsSpringFrequency", "linearLimitMax", "linearLimitMin", "maxDistance", "maxFriction", "minDistance", "motorMaxForce", "motorSpringDamping", "motorSpringFrequency", "motorTargetRotation", "motorTargetVelocity", "normal", "normalHalfConeAngle", "numTeethGear", "numTeethGearA", "numTeethGearB", "numTeethRack", "pathPoints", "pathPosition", "planeAxis", "planeHalfConeAngle", "rackLength", "slider", "springDamper", "springStiffness", "springTargetPosition", "swingType", "targetAngularVelocity", "targetPosition", "targetRotation", "targetVelocity", "twistAxis", "twistMaxAngle", "twistMinAngle", "type", "useDrive", "useLimits", "useMotor", "useSpring", "xMotion", "yMotion", "zMotion"};

}

const ComponentDesc kJoint2DDesc{
    .type = ComponentType::Joint2DComponent, .cppName = "Joint2DComponent", .key = "joint2d", .displayName = "Joint 2D", .icon = ICON_FA_LINK,
    .body = JE_STRUCT_DESC(Joint2DComponent, "Joint2DComponent", kJoint2DFields),
    .hasEnabled = true, .structural = true, JE_COMPONENT_ACCESS(Joint2DComponent),
    .codec = &kJoint2DCodec,
    .postDecode = &joint2DPostDecode,
    .codegenVar = "joint2d", .codegenOrder = kJoint2DCodegenOrder,
    .codegenGuard = "JASPERENGINE_PHYSICS_2D",
    .extraKeys = kJoint2DKnownKeys,
};

const ComponentDesc kJoint3DDesc{
    .type = ComponentType::Joint3DComponent, .cppName = "Joint3DComponent", .key = "joint3d", .displayName = "Joint", .icon = ICON_FA_LINK,
    .body = JE_STRUCT_DESC(Joint3DComponent, "Joint3DComponent", kJoint3DFields),
    .hasEnabled = true, .structural = true, JE_COMPONENT_ACCESS(Joint3DComponent),
    .codec = &kJoint3DCodec,
    .postDecode = &joint3DPostDecode,
    .codegenVar = "joint3d", .codegenOrder = kJoint3DCodegenOrder,
    .codegenGuard = "JASPERENGINE_PHYSICS_3D",
    .extraKeys = kJoint3DKnownKeys,
};

namespace {

    // the keys its codec reads
    constexpr const char* kRigidbody2DKnownKeys[] = {"allowFastRotation", "angularDamping", "attachToParentBody", "autoCenterOfMass", "autoInertia", "centerOfMass", "colliders", "collisionDetection",
                                                      "enabled", "fixedRotation", "gravityScale", "inertia", "linearDamping", "mass", "material",
                                                      "numColliders", "sleepingMode", "type", "useAutoMass"};

}

const ComponentDesc kRigidbody2DDesc{
    .type = ComponentType::Rigidbody2DComponent, .cppName = "Rigidbody2DComponent", .key = "rigidbody2d", .displayName = "Rigidbody 2D",
    .icon = ICON_FA_CIRCLE,
    .body = JE_STRUCT_DESC(Rigidbody2DComponent, "Rigidbody2DComponent", kRigidbody2DFields),
    .hasEnabled = true, .structural = true, JE_COMPONENT_ACCESS(Rigidbody2DComponent),
    .codec = &kRigidbody2DCodec,
    .codegen = &rigidbody2DCodegen,
    .codegenGuard = "JASPERENGINE_PHYSICS_2D",
    .extraKeys = kRigidbody2DKnownKeys,
};

namespace {

    // the keys its codec reads
    constexpr const char* kRigidbodyKnownKeys[] = {"allowSleeping", "allowedDOFs", "angularDamping", "attachToParentBody", "automaticCenterOfMass", "automaticInertiaTensor",
                                                    "automaticMass", "buoyancy", "centerOfMass", "collideKinematicVsNonDynamic", "colliders", "enabled", "friction",
                                                    "gravityFactor", "inertiaTensor", "inertiaTensorRotation", "interpolation", "linearDamping", "mass", "material",
                                                    "maxAngularVelocity", "maxLinearVelocity", "motionQuality", "numColliders", "overrideMassProperties", "restitution",
                                                    "sensor", "solidBoxDensity", "solidBoxSize", "type", "waterAngularDrag", "waterDrag"};

}

const ComponentDesc kRigidbodyDesc{
    .type = ComponentType::RigidbodyComponent, .cppName = "RigidbodyComponent", .key = "rigidbody", .displayName = "Rigidbody",
    .icon = ICON_FA_CUBE,
    .body = JE_STRUCT_DESC(RigidbodyComponent, "RigidbodyComponent", kRigidbodyFields),
    .hasEnabled = true, .structural = true, JE_COMPONENT_ACCESS(RigidbodyComponent),
    .codec = &kRigidbodyCodec,
    .codegen = &rigidbodyCodegen,
    .codegenGuard = "JASPERENGINE_PHYSICS_3D",
    .extraKeys = kRigidbodyKnownKeys,
};

}
