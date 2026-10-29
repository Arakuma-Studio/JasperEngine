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
        // the world values and matrices the transform system computes: saved, not edited
        JE_FIELD(Transform, worldPosition, .flags = Derived | Hidden),
        JE_FIELD(Transform, worldRotation, .flags = Derived | Hidden),
        JE_FIELD(Transform, worldScale, .flags = Derived | Hidden),
        JE_FIELD(Transform, localMatrix, .flags = Derived | Hidden),
        JE_FIELD(Transform, modelMatrix, .flags = Derived | Hidden),
        JE_FIELD(Transform, normalMatrix, .flags = Derived | Hidden),
        JE_FIELD(Transform, visible),
        JE_FIELD(Transform, billboardRotation, .updateFlags = UpdateFlags_Transform),
        JE_FIELD(Transform, billboard, .updateFlags = UpdateFlags_Transform),
        JE_FIELD(Transform, fakeBillboard, .updateFlags = UpdateFlags_Transform),
        JE_FIELD(Transform, cylindricalBillboard, .updateFlags = UpdateFlags_Transform),
    };

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
        // the exported joint is built on load
        JE_FIELD(Joint2DComponent, needUpdateJoint, .flags = NoSave | Derived | Hidden | CodegenRuntime),
    };

    constexpr const char* kJoint2DCodegenOrder[] = {
        "type", "bodyA", "bodyB", "anchorA", "anchorB", "axis", "target", "autoAnchors", "rope", "needUpdateJoint", "enabled"
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
        JE_FIELD(Joint3DComponent, needUpdateJoint, .flags = NoSave | Derived | Hidden | CodegenRuntime),
    };

    constexpr const char* kJoint3DCodegenOrder[] = {
        "type", "bodyA", "bodyB", "anchorA", "anchorB", "anchor", "axis", "normal", "twistAxis", "planeAxis", "axisX", "axisY", "limitsMin",
        "limitsMax", "normalHalfConeAngle", "planeHalfConeAngle", "twistMinAngle", "twistMaxAngle", "fixedPointA", "fixedPointB", "hingeA",
        "hingeB", "hinge", "slider", "numTeethGearA", "numTeethGearB", "numTeethRack", "numTeethGear", "rackLength", "pathPoints",
        "pathPosition", "isLooping", "autoAnchors", "needUpdateJoint", "enabled"
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
        JE_FIELD(Collider2D, vertices, .flags = ListElements | ListTemplate, .updateFlags = kBody2D, .count = JE_COUNT(Collider2D, numVertices)),
        JE_FIELD(Collider2D, loop, .updateFlags = kBody2D),
        JE_FIELD(Collider2D, density, .updateFlags = kBody2D, .min = 0.0f),
        JE_FIELD(Collider2D, friction, .updateFlags = kBody2D, .min = 0.0f),
        JE_FIELD(Collider2D, restitution, .updateFlags = kBody2D, .min = 0.0f),
        JE_FIELD(Collider2D, sensor, .updateFlags = kBody2D),
        JE_FIELD(Collider2D, enableHitEvents, .updateFlags = kBody2D),
        JE_FIELD(Collider2D, contactEvents, .updateFlags = kBody2D),
        JE_FIELD(Collider2D, preSolveEvents, .updateFlags = kBody2D),
        JE_FIELD(Collider2D, sensorEvents, .updateFlags = kBody2D),
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
        JE_FIELD(Collider3D, vertices, .flags = ListElements | ListTemplate, .updateFlags = kBody3D, .count = JE_COUNT(Collider3D, numVertices)),
        JE_FIELD(Collider3D, numVertices, .ops = &ops::UIntAs<uint16_t>, .updateFlags = kBody3D),
        JE_FIELD(Collider3D, numIndices, .ops = &ops::UIntAs<uint16_t>, .updateFlags = kBody3D),
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
        JE_FIELD(Rigidbody2DComponent, numColliders, .ops = &ops::UIntAs<size_t>, .updateFlags = kBody2D),
        JE_FIELD(Rigidbody2DComponent, colliders, .flags = ListEachElement | ListTemplate, .updateFlags = kBody2D,
                 .count = JE_COUNT(Rigidbody2DComponent, numColliders)),
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
        JE_FIELD(RigidbodyComponent, numColliders, .ops = &ops::UIntAs<size_t>, .updateFlags = kBody3D),
        JE_FIELD(RigidbodyComponent, colliders, .flags = ListEachElement | ListTemplate, .updateFlags = kBody3D,
                 .count = JE_COUNT(RigidbodyComponent, numColliders)),
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
    .codegen = &transformCodegen,
};

const ComponentDesc kJoint2DDesc{
    .type = ComponentType::Joint2DComponent, .cppName = "Joint2DComponent", .key = "joint2d", .displayName = "Joint 2D", .icon = ICON_FA_LINK,
    .body = JE_STRUCT_DESC(Joint2DComponent, "Joint2DComponent", kJoint2DFields),
    .hasEnabled = true, .structural = true, JE_COMPONENT_ACCESS(Joint2DComponent),
    .postDecode = &joint2DPostDecode,
    .codegenVar = "joint2d", .codegenOrder = kJoint2DCodegenOrder,
    .codegenGuard = "JASPERENGINE_PHYSICS_2D",
};

const ComponentDesc kJoint3DDesc{
    .type = ComponentType::Joint3DComponent, .cppName = "Joint3DComponent", .key = "joint3d", .displayName = "Joint", .icon = ICON_FA_LINK,
    .body = JE_STRUCT_DESC(Joint3DComponent, "Joint3DComponent", kJoint3DFields),
    .hasEnabled = true, .structural = true, JE_COMPONENT_ACCESS(Joint3DComponent),
    .postDecode = &joint3DPostDecode,
    .codegenVar = "joint3d", .codegenOrder = kJoint3DCodegenOrder,
    .codegenGuard = "JASPERENGINE_PHYSICS_3D",
};

namespace {

    // the keys its codec reads
    constexpr const char* kRigidbody2DKnownKeys[] = {"colliders", "enabled", "numColliders", "type"};

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
    constexpr const char* kRigidbodyKnownKeys[] = {"allowedDOFs", "buoyancy", "colliders", "enabled", "friction", "gravityFactor", "motionQuality", "numColliders", "restitution", "sensor", "type", "waterAngularDrag", "waterDrag"};

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
