// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Upstream's components (the origin/main merge of SYNC#4) on S08's reflection tables
// (docs/specs/S08-reflection.md): the Trail Renderer (lines and trails), the Canvas Group (ui
// canvas group), the on-screen stick, button and touch area (ui multitouch) and the Ragdoll
// (physics 3D). Each keeps upstream's codec (StreamUpstream.cpp: the keys its files have); the
// tables give the properties, schema, Inspector metadata, the exported code and the cooked
// scenes' fields.
// The Trail Renderer's widthCurve and colorGradient are S27's Curve and Gradient (math/Curve.h).

#include "reflect/Components.h"
#include "reflect/components/MaterialReflect.h"

#include "Factory.h"
#include "Stream.h"
#include "component/CanvasGroupComponent.h"
#include "component/OnScreenControlComponents.h"
#include "component/RagdollComponent.h"
#include "component/TrailRendererComponent.h"

namespace jasperengine::editor::reflect {

namespace {

    // ---------------------------------------------------------------- Trail Renderer

    constexpr uint64_t kTrail = UpdateFlags_Trail;
    constexpr uint64_t kTrailShader = UpdateFlags_Shader_Reload;

    constexpr FieldDesc kTrailRendererFields[] = {
        JE_FIELD(TrailRendererComponent, enabled),
        JE_FIELD(TrailRendererComponent, time, .updateFlags = kTrail, .min = 0.0f, .tooltip = "Seconds a point of the trail lives."),
        JE_FIELD(TrailRendererComponent, minVertexDistance, .updateFlags = kTrail, .min = 0.0f),
        JE_FIELD(TrailRendererComponent, widthMultiplier, .updateFlags = kTrail, .min = 0.0f),
        JE_FIELD(TrailRendererComponent, widthCurve, .updateFlags = kTrail, .tooltip = "The width over the trail (0 the head, 1 the tail), times Width Multiplier.",
                 .drawer = "curve"),
        JE_FIELD(TrailRendererComponent, widthUnit, .enumDesc = &enums::kLineWidthUnit, .updateFlags = kTrail),
        JE_FIELD(TrailRendererComponent, colorGradient, .updateFlags = kTrail, .tooltip = "The colour over the trail by age (0 the head, 1 the oldest point).",
                 .drawer = "gradient"),
        JE_FIELD(TrailRendererComponent, numCornerVertices, .updateFlags = kTrail),
        JE_FIELD(TrailRendererComponent, numCapVertices, .updateFlags = kTrail),
        JE_FIELD(TrailRendererComponent, alignment, .enumDesc = &enums::kLineAlignment, .updateFlags = kTrail),
        JE_FIELD(TrailRendererComponent, textureMode, .enumDesc = &enums::kLineTextureMode, .updateFlags = kTrail),
        JE_FIELD(TrailRendererComponent, textureScale, .updateFlags = kTrail),
        JE_FIELD(TrailRendererComponent, emitting),
        JE_FIELD(TrailRendererComponent, autodestruct),
        JE_FIELD(TrailRendererComponent, sortPriority, .updateFlags = kTrail),
        // S52: the 2D draw order and the transparent queue's first keys (a layer id: SortingLayers)
        JE_FIELD(TrailRendererComponent, sortingLayer, .flags = OmitIfDefault | CodegenIfChanged, .min = 0.0f, .label = "Sorting Layer", .drawer = "sortingLayer"),
        JE_FIELD(TrailRendererComponent, sortingOrder, .flags = OmitIfDefault | CodegenIfChanged, .min = -32768.0f, .max = 32767.0f, .label = "Order in Layer"),
        JE_FIELD(TrailRendererComponent, customShader, .updateFlags = kTrailShader),
        JE_FIELD(TrailRendererComponent, shaderUniforms, .ops = &ops::ShaderUniformValues, .updateFlags = UpdateFlags_Shader_Uniforms),
        JE_FIELD(TrailRendererComponent, shaderTextures, .ops = &ops::ShaderTextures, .updateFlags = kTrailShader),
        JE_FIELD(TrailRendererComponent, shaderFeatures, .ops = &ops::ShaderFeatures, .updateFlags = kTrailShader),
        JE_FIELD(TrailRendererComponent, shaderGlobalBindings, .ops = &ops::ShaderGlobalBindings, .flags = Hidden, .updateFlags = UpdateFlags_Shader_Uniforms),
        JE_FIELD(TrailRendererComponent, blendMode, .enumDesc = &enums::kMaterialBlendMode, .updateFlags = kTrailShader),
        JE_FIELD(TrailRendererComponent, depthWrite, .enumDesc = &enums::kMaterialDepthWrite, .updateFlags = kTrailShader),
        JE_FIELD(TrailRendererComponent, stencil, .flags = ListWhole, .updateFlags = kTrailShader),
        JE_FIELD(TrailRendererComponent, colorWrite, .ops = &ops::ColorWrite, .updateFlags = kTrailShader, .min = -1.0f, .max = 15.0f),
    };

    const ComponentCodec kTrailRendererCodec = JE_STREAM_CODEC(TrailRendererComponent, encodeTrailRendererComponent, decodeTrailRendererComponent);

    // upstream's name of the component (Catalog::getComponentType("trail"))
    constexpr const char* kTrailRendererAliases[] = {"trail"};

    constexpr const char* kTrailRendererKnownKeys[] = {"alignment", "autodestruct", "blendMode", "colorGradient", "colorWrite", "customShader",
                                                       "customShaderGuid", "depthWrite", "emitting", "enabled", "minVertexDistance",
                                                       "numCapVertices", "numCornerVertices", "shaderFeatures", "shaderGlobalBindings",
                                                       "shaderTextures", "shaderUniforms", "sortPriority", "sortingLayer", "sortingOrder", "stencil",
                                                       "textureMode", "textureScale", "time", "widthCurve", "widthMultiplier", "widthUnit"};

    // ---------------------------------------------------------------- Canvas Group

    constexpr uint64_t kCanvasGroup = UpdateFlags_CanvasGroup;

    constexpr FieldDesc kCanvasGroupFields[] = {
        JE_FIELD(CanvasGroupComponent, enabled, .updateFlags = kCanvasGroup),
        // writes are clamped into 0..1 (upstream: Catalog::updateEntity clamps it), not refused
        JE_FIELD(CanvasGroupComponent, alpha, .flags = Clamped, .updateFlags = kCanvasGroup, .min = 0.0f, .max = 1.0f),
        JE_FIELD(CanvasGroupComponent, interactable, .updateFlags = kCanvasGroup),
        JE_FIELD(CanvasGroupComponent, blocksRaycasts, .updateFlags = kCanvasGroup),
        JE_FIELD(CanvasGroupComponent, ignoreParentGroups, .updateFlags = kCanvasGroup),
        JE_FIELD(CanvasGroupComponent, renderMode, .enumDesc = &enums::kCanvasGroupRenderMode, .updateFlags = kCanvasGroup),
    };

    const ComponentCodec kCanvasGroupCodec = JE_STREAM_CODEC(CanvasGroupComponent, encodeCanvasGroupComponent, decodeCanvasGroupComponent);

    constexpr const char* kCanvasGroupKnownKeys[] = {"alpha", "blocksRaycasts", "enabled", "ignoreParentGroups", "interactable", "renderMode"};

    // ---------------------------------------------------------------- on-screen controls

    constexpr FieldDesc kOnScreenStickFields[] = {
        JE_FIELD(OnScreenStickComponent, enabled),
        JE_FIELD(OnScreenStickComponent, mode, .enumDesc = &enums::kOnScreenStickMode),
        JE_FIELD(OnScreenStickComponent, background, .ops = &ops::Entity),
        JE_FIELD(OnScreenStickComponent, handle, .ops = &ops::Entity),
        JE_FIELD(OnScreenStickComponent, handleRange, .min = 0.0f),
        JE_FIELD(OnScreenStickComponent, deadZone, .min = 0.0f, .max = 1.0f),
        JE_FIELD(OnScreenStickComponent, moveThreshold, .min = 0.0f),
        JE_FIELD(OnScreenStickComponent, axes, .enumDesc = &enums::kOnScreenStickAxes),
        JE_FIELD(OnScreenStickComponent, snapX),
        JE_FIELD(OnScreenStickComponent, snapY),
        JE_FIELD(OnScreenStickComponent, hideWhenReleased),
        JE_FIELD(OnScreenStickComponent, clampToArea),
        JE_FIELD(OnScreenStickComponent, horizontalAction),
        JE_FIELD(OnScreenStickComponent, verticalAction),
        JE_FIELD(OnScreenStickComponent, visibility, .enumDesc = &enums::kOnScreenVisibility),
    };

    const ComponentCodec kOnScreenStickCodec = JE_STREAM_CODEC(OnScreenStickComponent, encodeOnScreenStickComponent, decodeOnScreenStickComponent);

    constexpr const char* kOnScreenStickKnownKeys[] = {"axes", "background", "clampToArea", "deadZone", "enabled", "handle", "handleRange",
        "hideWhenReleased", "horizontalAction", "mode", "moveThreshold", "snapX", "snapY", "verticalAction", "visibility"};

    constexpr FieldDesc kOnScreenButtonFields[] = {
        JE_FIELD(OnScreenButtonComponent, enabled),
        JE_FIELD(OnScreenButtonComponent, action),
        JE_FIELD(OnScreenButtonComponent, value, .min = -1.0f, .max = 1.0f),
        JE_FIELD(OnScreenButtonComponent, pressWhenPointerEnters),
        JE_FIELD(OnScreenButtonComponent, releaseWhenPointerLeaves),
        JE_FIELD(OnScreenButtonComponent, visibility, .enumDesc = &enums::kOnScreenVisibility),
    };

    const ComponentCodec kOnScreenButtonCodec = JE_STREAM_CODEC(OnScreenButtonComponent, encodeOnScreenButtonComponent, decodeOnScreenButtonComponent);

    constexpr const char* kOnScreenButtonKnownKeys[] = {"action", "enabled", "pressWhenPointerEnters", "releaseWhenPointerLeaves", "value",
        "visibility"};

    constexpr FieldDesc kTouchAreaFields[] = {
        JE_FIELD(TouchAreaComponent, enabled),
        JE_FIELD(TouchAreaComponent, horizontalAction),
        JE_FIELD(TouchAreaComponent, verticalAction),
        JE_FIELD(TouchAreaComponent, sensitivity),
        JE_FIELD(TouchAreaComponent, invertX),
        JE_FIELD(TouchAreaComponent, invertY),
        JE_FIELD(TouchAreaComponent, visibility, .enumDesc = &enums::kOnScreenVisibility),
    };

    const ComponentCodec kTouchAreaCodec = JE_STREAM_CODEC(TouchAreaComponent, encodeTouchAreaComponent, decodeTouchAreaComponent);

    constexpr const char* kTouchAreaKnownKeys[] = {"enabled", "horizontalAction", "invertX", "invertY", "sensitivity", "verticalAction",
        "visibility"};

    // ---------------------------------------------------------------- Ragdoll

    constexpr uint64_t kRagdoll = UpdateFlags_Ragdoll;

    constexpr FieldDesc kRagdollFields[] = {
        JE_FIELD(RagdollComponent, enabled),
        JE_FIELD(RagdollComponent, simulated),
        JE_FIELD(RagdollComponent, root, .ops = &ops::Entity, .updateFlags = kRagdoll),
        JE_FIELD(RagdollComponent, animator, .ops = &ops::Entity, .updateFlags = kRagdoll),
        JE_FIELD(RagdollComponent, inheritVelocity),
        JE_FIELD(RagdollComponent, selfCollision),
        JE_FIELD(RagdollComponent, totalMass, .min = 0.0f),
    };

    const ComponentCodec kRagdollCodec = JE_STREAM_CODEC(RagdollComponent, encodeRagdollComponent, decodeRagdollComponent);

    constexpr const char* kRagdollKnownKeys[] = {"animator", "enabled", "inheritVelocity", "root", "selfCollision", "simulated", "totalMass"};

}

const ComponentDesc kTrailRendererDesc{
    .type = ComponentType::TrailRendererComponent, .cppName = "TrailRendererComponent", .key = "trailrenderer", .displayName = "Trail Renderer",
    .icon = ICON_FA_WIND,
    .body = JE_STRUCT_DESC(TrailRendererComponent, "TrailRendererComponent", kTrailRendererFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(TrailRendererComponent),
    .codec = &kTrailRendererCodec,
    .keyAliases = kTrailRendererAliases,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createTrailRendererComponent),
    .extraKeys = kTrailRendererKnownKeys,
};

const ComponentDesc kCanvasGroupDesc{
    .type = ComponentType::CanvasGroupComponent, .cppName = "CanvasGroupComponent", .key = "canvasgroup", .displayName = "Canvas Group",
    .icon = ICON_FA_LAYER_GROUP,
    .body = JE_STRUCT_DESC(CanvasGroupComponent, "CanvasGroupComponent", kCanvasGroupFields),
    .hasEnabled = true, .structural = true, JE_COMPONENT_ACCESS(CanvasGroupComponent),
    .codec = &kCanvasGroupCodec,
    .structuralUpdateFlags = kCanvasGroup,
    .decodeApply = DecodeApply::MarkOnly,
    .codegenVar = "canvasgroup",
    .extraKeys = kCanvasGroupKnownKeys,
};

const ComponentDesc kOnScreenStickDesc{
    .type = ComponentType::OnScreenStickComponent, .cppName = "OnScreenStickComponent", .key = "onscreenstick", .displayName = "On-Screen Stick",
    .icon = ICON_FA_GAMEPAD,
    .body = JE_STRUCT_DESC(OnScreenStickComponent, "OnScreenStickComponent", kOnScreenStickFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(OnScreenStickComponent),
    .codec = &kOnScreenStickCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegenVar = "stick",
    .extraKeys = kOnScreenStickKnownKeys,
};

const ComponentDesc kOnScreenButtonDesc{
    .type = ComponentType::OnScreenButtonComponent, .cppName = "OnScreenButtonComponent", .key = "onscreenbutton", .displayName = "On-Screen Button",
    .icon = ICON_FA_CIRCLE_DOT,
    .body = JE_STRUCT_DESC(OnScreenButtonComponent, "OnScreenButtonComponent", kOnScreenButtonFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(OnScreenButtonComponent),
    .codec = &kOnScreenButtonCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegenVar = "button",
    .extraKeys = kOnScreenButtonKnownKeys,
};

const ComponentDesc kTouchAreaDesc{
    .type = ComponentType::TouchAreaComponent, .cppName = "TouchAreaComponent", .key = "toucharea", .displayName = "Touch Area",
    .icon = ICON_FA_HAND_POINTER,
    .body = JE_STRUCT_DESC(TouchAreaComponent, "TouchAreaComponent", kTouchAreaFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(TouchAreaComponent),
    .codec = &kTouchAreaCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegenVar = "touchArea",
    .extraKeys = kTouchAreaKnownKeys,
};

const ComponentDesc kRagdollDesc{
    .type = ComponentType::RagdollComponent, .cppName = "RagdollComponent", .key = "ragdoll", .displayName = "Ragdoll",
    .icon = ICON_FA_PERSON_FALLING,
    .body = JE_STRUCT_DESC(RagdollComponent, "RagdollComponent", kRagdollFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(RagdollComponent),
    .codec = &kRagdollCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegenVar = "ragdoll",
    .codegenGuard = "JASPERENGINE_PHYSICS_3D",
    .extraKeys = kRagdollKnownKeys,
};

}
