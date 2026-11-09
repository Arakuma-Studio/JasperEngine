// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The scene codecs of upstream's components that came with SYNC#4 (origin/main 047eccf9b) and
// have no reflection field table of their own yet: Canvas Group, the on-screen controls (stick,
// button, touch area), Ragdoll and Trail Renderer, with the Line/Trail ribbon helpers. Their
// reflection descriptors (editor/reflect/components/UpstreamReflect.cpp) use these as codecs
// (JE_STREAM_CODEC), as the Vertex Animator's does.

#include "Stream.h"
#include "component/CanvasGroupComponent.h"
#include "component/OnScreenControlComponents.h"
#include "component/RagdollComponent.h"
#include "component/TrailRendererComponent.h"
#include "component/Joint2DComponent.h"
#include "component/Joint3DComponent.h"
#include "Catalog.h"
#include "Out.h"
#include "util/YamlNumbers.h"
#include "util/Color.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

using namespace jasperengine;

namespace {

    const char* stickModeToString(OnScreenStickMode mode) {
        switch (mode) {
            case OnScreenStickMode::FLOATING: return "floating";
            case OnScreenStickMode::DYNAMIC: return "dynamic";
            default: return "fixed";
        }
    }

    OnScreenStickMode stringToStickMode(const std::string& text) {
        if (text == "floating") return OnScreenStickMode::FLOATING;
        if (text == "dynamic") return OnScreenStickMode::DYNAMIC;
        return OnScreenStickMode::FIXED;
    }

    const char* stickAxesToString(OnScreenStickAxes axes) {
        switch (axes) {
            case OnScreenStickAxes::HORIZONTAL: return "horizontal";
            case OnScreenStickAxes::VERTICAL: return "vertical";
            default: return "both";
        }
    }

    OnScreenStickAxes stringToStickAxes(const std::string& text) {
        if (text == "horizontal") return OnScreenStickAxes::HORIZONTAL;
        if (text == "vertical") return OnScreenStickAxes::VERTICAL;
        return OnScreenStickAxes::BOTH;
    }

    const char* visibilityToString(OnScreenVisibility visibility) {
        return visibility == OnScreenVisibility::TOUCH_DEVICES_ONLY ? "touch_devices_only" : "always";
    }

    OnScreenVisibility stringToVisibility(const std::string& text) {
        return text == "touch_devices_only" ? OnScreenVisibility::TOUCH_DEVICES_ONLY : OnScreenVisibility::ALWAYS;
    }

}

YAML::Node editor::Stream::encodeOnScreenStickComponent(const OnScreenStickComponent& stick) {
    YAML::Node node;
    node["enabled"] = stick.enabled;
    node["mode"] = stickModeToString(stick.mode);
    node["background"] = stick.background;
    node["handle"] = stick.handle;
    node["handleRange"] = stick.handleRange;
    node["deadZone"] = stick.deadZone;
    node["moveThreshold"] = stick.moveThreshold;
    node["axes"] = stickAxesToString(stick.axes);
    node["snapX"] = stick.snapX;
    node["snapY"] = stick.snapY;
    node["hideWhenReleased"] = stick.hideWhenReleased;
    node["clampToArea"] = stick.clampToArea;
    node["horizontalAction"] = stick.horizontalAction;
    node["verticalAction"] = stick.verticalAction;
    node["visibility"] = visibilityToString(stick.visibility);
    return node;
}

OnScreenStickComponent editor::Stream::decodeOnScreenStickComponent(const YAML::Node& node, const OnScreenStickComponent* oldStick) {
    OnScreenStickComponent stick;
    if (oldStick) {
        stick = *oldStick;
    }

    if (node["enabled"]) stick.enabled = node["enabled"].as<bool>();
    if (node["mode"]) stick.mode = stringToStickMode(node["mode"].as<std::string>());
    if (node["background"]) stick.background = YamlNumbers::as<Entity>(node["background"]);
    if (node["handle"]) stick.handle = YamlNumbers::as<Entity>(node["handle"]);
    if (node["handleRange"]) stick.handleRange = std::max(0.0f, YamlNumbers::as<float>(node["handleRange"]));
    if (node["deadZone"]) stick.deadZone = std::clamp(YamlNumbers::as<float>(node["deadZone"]), 0.0f, 1.0f);
    if (node["moveThreshold"]) stick.moveThreshold = std::max(0.0f, YamlNumbers::as<float>(node["moveThreshold"]));
    if (node["axes"]) stick.axes = stringToStickAxes(node["axes"].as<std::string>());
    if (node["snapX"]) stick.snapX = node["snapX"].as<bool>();
    if (node["snapY"]) stick.snapY = node["snapY"].as<bool>();
    if (node["hideWhenReleased"]) stick.hideWhenReleased = node["hideWhenReleased"].as<bool>();
    if (node["clampToArea"]) stick.clampToArea = node["clampToArea"].as<bool>();
    if (node["horizontalAction"]) stick.horizontalAction = node["horizontalAction"].as<std::string>();
    if (node["verticalAction"]) stick.verticalAction = node["verticalAction"].as<std::string>();
    if (node["visibility"]) stick.visibility = stringToVisibility(node["visibility"].as<std::string>());

    return stick;
}

YAML::Node editor::Stream::encodeOnScreenButtonComponent(const OnScreenButtonComponent& button) {
    YAML::Node node;
    node["enabled"] = button.enabled;
    node["action"] = button.action;
    node["value"] = button.value;
    node["pressWhenPointerEnters"] = button.pressWhenPointerEnters;
    node["releaseWhenPointerLeaves"] = button.releaseWhenPointerLeaves;
    node["visibility"] = visibilityToString(button.visibility);
    return node;
}

OnScreenButtonComponent editor::Stream::decodeOnScreenButtonComponent(const YAML::Node& node, const OnScreenButtonComponent* oldButton) {
    OnScreenButtonComponent button;
    if (oldButton) {
        button = *oldButton;
    }

    if (node["enabled"]) button.enabled = node["enabled"].as<bool>();
    if (node["action"]) button.action = node["action"].as<std::string>();
    if (node["value"]) button.value = std::clamp(YamlNumbers::as<float>(node["value"]), -1.0f, 1.0f);
    if (node["pressWhenPointerEnters"]) button.pressWhenPointerEnters = node["pressWhenPointerEnters"].as<bool>();
    if (node["releaseWhenPointerLeaves"]) button.releaseWhenPointerLeaves = node["releaseWhenPointerLeaves"].as<bool>();
    if (node["visibility"]) button.visibility = stringToVisibility(node["visibility"].as<std::string>());

    return button;
}

YAML::Node editor::Stream::encodeTouchAreaComponent(const TouchAreaComponent& area) {
    YAML::Node node;
    node["enabled"] = area.enabled;
    node["horizontalAction"] = area.horizontalAction;
    node["verticalAction"] = area.verticalAction;
    node["sensitivity"] = area.sensitivity;
    node["invertX"] = area.invertX;
    node["invertY"] = area.invertY;
    node["visibility"] = visibilityToString(area.visibility);
    return node;
}

TouchAreaComponent editor::Stream::decodeTouchAreaComponent(const YAML::Node& node, const TouchAreaComponent* oldArea) {
    TouchAreaComponent area;
    if (oldArea) {
        area = *oldArea;
    }

    if (node["enabled"]) area.enabled = node["enabled"].as<bool>();
    if (node["horizontalAction"]) area.horizontalAction = node["horizontalAction"].as<std::string>();
    if (node["verticalAction"]) area.verticalAction = node["verticalAction"].as<std::string>();
    if (node["sensitivity"]) area.sensitivity = YamlNumbers::as<float>(node["sensitivity"]);
    if (node["invertX"]) area.invertX = node["invertX"].as<bool>();
    if (node["invertY"]) area.invertY = node["invertY"].as<bool>();
    if (node["visibility"]) area.visibility = stringToVisibility(node["visibility"].as<std::string>());

    return area;
}

YAML::Node editor::Stream::encodeCanvasGroupComponent(const CanvasGroupComponent& group) {
    YAML::Node node;
    node["enabled"] = group.enabled;
    node["alpha"] = group.alpha;
    node["interactable"] = group.interactable;
    node["blocksRaycasts"] = group.blocksRaycasts;
    node["ignoreParentGroups"] = group.ignoreParentGroups;
    node["renderMode"] = canvasGroupRenderModeToString(group.renderMode);
    return node;
}

CanvasGroupComponent editor::Stream::decodeCanvasGroupComponent(const YAML::Node& node, const CanvasGroupComponent* oldGroup) {
    CanvasGroupComponent group;

    if (oldGroup) {
        // the authored fields; the runtime block stays the scene's
        group = *oldGroup;
    }

    if (node["enabled"]) group.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["alpha"]) {
        const float alpha = YamlNumbers::as<float>(node["alpha"]);
        if (!std::isnan(alpha)) group.alpha = std::clamp(alpha, 0.0f, 1.0f);
    }
    if (node["interactable"]) group.interactable = YamlNumbers::as<bool>(node["interactable"]);
    if (node["blocksRaycasts"]) group.blocksRaycasts = YamlNumbers::as<bool>(node["blocksRaycasts"]);
    if (node["ignoreParentGroups"]) group.ignoreParentGroups = YamlNumbers::as<bool>(node["ignoreParentGroups"]);
    if (node["renderMode"]) group.renderMode = stringToCanvasGroupRenderMode(node["renderMode"].as<std::string>());

    return group;
}

YAML::Node editor::Stream::encodeRagdollComponent(const RagdollComponent& ragdoll) {
    YAML::Node node;
    node["enabled"] = ragdoll.enabled;
    node["simulated"] = ragdoll.simulated;
    node["root"] = ragdoll.root;
    node["animator"] = ragdoll.animator;
    node["inheritVelocity"] = ragdoll.inheritVelocity;
    node["selfCollision"] = ragdoll.selfCollision;
    node["totalMass"] = ragdoll.totalMass;
    return node;
}

RagdollComponent editor::Stream::decodeRagdollComponent(const YAML::Node& node, const RagdollComponent* oldRagdoll) {
    RagdollComponent ragdoll;
    if (oldRagdoll) {
        ragdoll = *oldRagdoll;
        ragdoll.needCollectParts = true;
    }
    if (node["enabled"]) ragdoll.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["simulated"]) ragdoll.simulated = YamlNumbers::as<bool>(node["simulated"]);
    if (node["root"]) ragdoll.root = YamlNumbers::as<Entity>(node["root"]);
    if (node["animator"]) ragdoll.animator = YamlNumbers::as<Entity>(node["animator"]);
    if (node["inheritVelocity"]) ragdoll.inheritVelocity = YamlNumbers::as<bool>(node["inheritVelocity"]);
    if (node["selfCollision"]) ragdoll.selfCollision = YamlNumbers::as<bool>(node["selfCollision"]);
    if (node["totalMass"]) ragdoll.totalMass = YamlNumbers::as<float>(node["totalMass"]);
    return ragdoll;
}

std::string editor::Stream::lineRendererModeToString(LineRendererMode mode) {
    return mode == LineRendererMode::Polyline ? "polyline" : "segments";
}

LineRendererMode editor::Stream::stringToLineRendererMode(const std::string& str) {
    return (str == "polyline" || str == "Polyline") ? LineRendererMode::Polyline : LineRendererMode::Segments;
}

std::string editor::Stream::lineWidthUnitToString(LineWidthUnit unit) {
    return unit == LineWidthUnit::Pixels ? "pixels" : "world";
}

LineWidthUnit editor::Stream::stringToLineWidthUnit(const std::string& str) {
    return (str == "pixels" || str == "Pixels") ? LineWidthUnit::Pixels : LineWidthUnit::World;
}

std::string editor::Stream::lineAlignmentToString(LineAlignment alignment) {
    return alignment == LineAlignment::TransformZ ? "transform_z" : "view";
}

LineAlignment editor::Stream::stringToLineAlignment(const std::string& str) {
    return (str == "transform_z" || str == "TransformZ" || str == "transformZ") ? LineAlignment::TransformZ : LineAlignment::View;
}

std::string editor::Stream::lineTextureModeToString(LineTextureMode mode) {
    switch (mode) {
        case LineTextureMode::Stretch: return "stretch";
        case LineTextureMode::Tile: return "tile";
        case LineTextureMode::DistributePerSegment: return "distribute_per_segment";
        case LineTextureMode::RepeatPerSegment: return "repeat_per_segment";
    }
    return "stretch";
}

LineTextureMode editor::Stream::stringToLineTextureMode(const std::string& str) {
    if (str == "tile" || str == "Tile") return LineTextureMode::Tile;
    if (str == "distribute_per_segment" || str == "DistributePerSegment") return LineTextureMode::DistributePerSegment;
    if (str == "repeat_per_segment" || str == "RepeatPerSegment") return LineTextureMode::RepeatPerSegment;
    return LineTextureMode::Stretch;
}

// The settings LineRendererComponent and TrailRendererComponent share (width, colour,
// joints, caps, texture layout, sort order), each written away from its default only
template<typename T>
void editor::Stream::encodeRibbonFields(YAML::Node& node, const T& component, const T& defaults) {
    if (component.widthMultiplier != defaults.widthMultiplier) node["widthMultiplier"] = YamlNumbers::text(component.widthMultiplier);
    if (component.widthCurve != defaults.widthCurve) node["widthCurve"] = editor::Stream::encodeCurve(component.widthCurve);
    if (component.widthUnit != defaults.widthUnit) node["widthUnit"] = editor::Stream::lineWidthUnitToString(component.widthUnit);
    if (component.colorGradient != defaults.colorGradient) node["colorGradient"] = editor::Stream::encodeGradient(component.colorGradient);
    if (component.numCornerVertices != defaults.numCornerVertices) node["numCornerVertices"] = component.numCornerVertices;
    if (component.numCapVertices != defaults.numCapVertices) node["numCapVertices"] = component.numCapVertices;
    if (component.alignment != defaults.alignment) node["alignment"] = editor::Stream::lineAlignmentToString(component.alignment);
    if (component.textureMode != defaults.textureMode) node["textureMode"] = editor::Stream::lineTextureModeToString(component.textureMode);
    if (component.textureScale != defaults.textureScale) node["textureScale"] = editor::Stream::encodeVector2(component.textureScale);
    if (component.sortPriority != defaults.sortPriority) node["sortPriority"] = component.sortPriority;
}

template<typename T>
void editor::Stream::decodeRibbonFields(const YAML::Node& node, T& component, const T& defaults) {
    component.widthMultiplier = node["widthMultiplier"] ? std::max(0.0f, editor::Stream::decodeFinite(node["widthMultiplier"], defaults.widthMultiplier)) : defaults.widthMultiplier;
    component.widthCurve = node["widthCurve"] ? editor::Stream::decodeCurve(node["widthCurve"], defaults.widthCurve) : defaults.widthCurve;
    component.widthUnit = node["widthUnit"] ? editor::Stream::stringToLineWidthUnit(node["widthUnit"].as<std::string>()) : defaults.widthUnit;
    component.colorGradient = node["colorGradient"] ? editor::Stream::decodeGradient(node["colorGradient"], defaults.colorGradient) : defaults.colorGradient;
    component.numCornerVertices = node["numCornerVertices"]
        ? std::min(YamlNumbers::as<unsigned int>(node["numCornerVertices"]), LINE_RIBBON_MAX_FAN_VERTICES) : defaults.numCornerVertices;
    component.numCapVertices = node["numCapVertices"]
        ? std::min(YamlNumbers::as<unsigned int>(node["numCapVertices"]), LINE_RIBBON_MAX_FAN_VERTICES) : defaults.numCapVertices;
    component.alignment = node["alignment"] ? editor::Stream::stringToLineAlignment(node["alignment"].as<std::string>()) : defaults.alignment;
    component.textureMode = node["textureMode"] ? editor::Stream::stringToLineTextureMode(node["textureMode"].as<std::string>()) : defaults.textureMode;
    component.textureScale = node["textureScale"] ? editor::Stream::decodeVector2(node["textureScale"]) : defaults.textureScale;
    component.sortPriority = node["sortPriority"] ? YamlNumbers::as<int>(node["sortPriority"]) : defaults.sortPriority;
}

void editor::Stream::encodeLineRibbonSettings(YAML::Node& node, const LineRendererComponent& lines) {
    static const LineRendererComponent defaults;
    if (lines.mode != defaults.mode) node["mode"] = lineRendererModeToString(lines.mode);
    if (!lines.positions.empty()) {
        YAML::Node positions(YAML::NodeType::Sequence);
        for (const Vector3& position : lines.positions) positions.push_back(encodeVector3(position));
        node["positions"] = positions;
    }
    if (lines.loop != defaults.loop) node["loop"] = lines.loop;
    if (lines.useWorldSpace != defaults.useWorldSpace) node["useWorldSpace"] = lines.useWorldSpace;
    encodeRibbonFields(node, lines, defaults);
}

void editor::Stream::decodeLineRibbonSettings(const YAML::Node& node, LineRendererComponent& lines) {
    static const LineRendererComponent defaults;
    lines.mode = node["mode"] ? stringToLineRendererMode(node["mode"].as<std::string>()) : defaults.mode;
    lines.positions.clear();
    if (node["positions"] && node["positions"].IsSequence()) {
        for (const YAML::Node& position : node["positions"]) lines.positions.push_back(decodeVector3(position));
    }
    lines.loop = node["loop"] ? YamlNumbers::as<bool>(node["loop"]) : defaults.loop;
    lines.useWorldSpace = node["useWorldSpace"] ? YamlNumbers::as<bool>(node["useWorldSpace"]) : defaults.useWorldSpace;
    decodeRibbonFields(node, lines, defaults);
}

YAML::Node editor::Stream::encodeTrailRendererComponent(const TrailRendererComponent& trail) {
    static const TrailRendererComponent defaults;
    YAML::Node node;
    node["enabled"] = trail.enabled;
    node["time"] = YamlNumbers::text(trail.time);
    node["minVertexDistance"] = YamlNumbers::text(trail.minVertexDistance);
    // the Trail Renderer's own default width is 1 (Unity's): always written
    node["widthMultiplier"] = YamlNumbers::text(trail.widthMultiplier);
    if (trail.emitting != defaults.emitting) node["emitting"] = trail.emitting;
    if (trail.autodestruct != defaults.autodestruct) node["autodestruct"] = trail.autodestruct;
    encodeSortingFields(node, trail.sortingLayer, trail.sortingOrder);
    encodeRibbonFields(node, trail, defaults);

    if (!trail.customShader.empty())
        encodeShaderRef(node, "customShader", trail.customShader);
    if (!trail.shaderUniforms.empty())
        node["shaderUniforms"] = encodeShaderUniforms(trail.shaderUniforms);
    encodeComponentShaderLibrary(node, trail.shaderTextures, trail.shaderFeatures, &trail.blendMode, &trail.depthWrite, true);
    encodeStencilAndColorWrite(node, trail.stencil, trail.colorWrite);
    if (!trail.shaderGlobalBindings.empty())
        node["shaderGlobalBindings"] = encodeShaderGlobalBindings(trail.shaderGlobalBindings);
    return node;
}

TrailRendererComponent editor::Stream::decodeTrailRendererComponent(const YAML::Node& node, const TrailRendererComponent* oldTrail) {
    static const TrailRendererComponent defaults;
    TrailRendererComponent trail;
    if (oldTrail) { trail = *oldTrail; }

    trail.enabled = node["enabled"] ? YamlNumbers::as<bool>(node["enabled"]) : defaults.enabled;
    trail.time = node["time"] ? std::max(0.0f, decodeFinite(node["time"], defaults.time)) : defaults.time;
    trail.minVertexDistance = node["minVertexDistance"] ? std::max(0.0f, decodeFinite(node["minVertexDistance"], defaults.minVertexDistance)) : defaults.minVertexDistance;
    trail.emitting = node["emitting"] ? YamlNumbers::as<bool>(node["emitting"]) : defaults.emitting;
    trail.autodestruct = node["autodestruct"] ? YamlNumbers::as<bool>(node["autodestruct"]) : defaults.autodestruct;
    decodeSortingFields(node, trail.sortingLayer, trail.sortingOrder);
    decodeRibbonFields(node, trail, defaults);

    trail.customShader = node["customShader"] ? decodeShaderRef(node, "customShader") : ShaderRef();
    trail.shaderUniforms = decodeShaderUniforms(node["shaderUniforms"]);
    trail.needUpdateShaderUniforms = true;
    decodeComponentShaderLibrary(node, trail.shaderTextures, trail.shaderFeatures, &trail.blendMode, &trail.depthWrite);
    decodeStencilAndColorWrite(node, trail.stencil, trail.colorWrite);
    trail.shaderGlobalBindings = decodeShaderGlobalBindings(node["shaderGlobalBindings"]);

    // the runtime starts again: no points, nothing loaded
    trail.points.reset();
    trail.hasLastHeadPosition = false;
    trail.idleTime = 0.0f;
    trail.warnedCapacity = false;
    trail.destroyRequested = false;
    trail.needUpdateRibbon = true;
    trail.needUpdateBuffer = false;
    trail.worldAABB.setNull();
    if (oldTrail && oldTrail->loaded) {
        // decoded over a loaded trail: reloaded by the render system (its GPU objects freed)
        trail.needReload = true;
    } else {
        trail.loaded = false;
        trail.loadCalled = false;
        trail.needReload = false;
        trail.loadedRibbonKey = 0;
        trail.ribbon = LineRibbonData();
    }
    return trail;
}

// upstream's joint and interpolation enum names (their codecs are upstream's)
std::string editor::Stream::jointAnchorSpaceToString(JointAnchorSpace space) {
    return space == JointAnchorSpace::LOCAL ? "local" : "world";
}

JointAnchorSpace editor::Stream::stringToJointAnchorSpace(const std::string& str) {
    return str == "local" ? JointAnchorSpace::LOCAL : JointAnchorSpace::WORLD;
}

std::string editor::Stream::jointBreakActionToString(JointBreakAction action) {
    switch (action) {
        case JointBreakAction::IGNORE: return "ignore";
        case JointBreakAction::CALLBACK_ONLY: return "callback_only";
        case JointBreakAction::DISABLE: return "disable";
        default: return "destroy";
    }
}

JointBreakAction editor::Stream::stringToJointBreakAction(const std::string& str) {
    if (str == "ignore") return JointBreakAction::IGNORE;
    if (str == "callback_only") return JointBreakAction::CALLBACK_ONLY;
    if (str == "disable") return JointBreakAction::DISABLE;
    return JointBreakAction::DESTROY;
}

std::string editor::Stream::jointMotionToString(JointMotion motion) {
    switch (motion) {
        case JointMotion::LOCKED: return "locked";
        case JointMotion::LIMITED: return "limited";
        default: return "free";
    }
}

JointMotion editor::Stream::stringToJointMotion(const std::string& str) {
    if (str == "locked") return JointMotion::LOCKED;
    if (str == "limited") return JointMotion::LIMITED;
    return JointMotion::FREE;
}

std::string editor::Stream::rigidbodyInterpolationToString(RigidbodyInterpolation interpolation) {
    switch (interpolation) {
        case RigidbodyInterpolation::NONE: return "none";
        case RigidbodyInterpolation::INTERPOLATE: return "interpolate";
        case RigidbodyInterpolation::EXTRAPOLATE: return "extrapolate";
        default: return "scene";
    }
}

RigidbodyInterpolation editor::Stream::stringToRigidbodyInterpolation(const std::string& str) {
    if (str == "none") return RigidbodyInterpolation::NONE;
    if (str == "interpolate") return RigidbodyInterpolation::INTERPOLATE;
    if (str == "extrapolate") return RigidbodyInterpolation::EXTRAPOLATE;
    return RigidbodyInterpolation::SCENE;
}

// upstream's joint codecs (physics-2d / physics-3d): a key missing from a scene saved before the
// fields existed reads as what the joint had then (makeLegacyJoint2D, setJoint3DLegacyParameters)
YAML::Node editor::Stream::encodeJoint2DComponent(const Joint2DComponent& joint) {
    YAML::Node node;
    node["enabled"] = joint.enabled;

    node["type"] = joint2DTypeToString(joint.type);
    node["bodyA"] = joint.bodyA;
    node["bodyB"] = joint.bodyB;
    node["anchorSpace"] = jointAnchorSpaceToString(joint.anchorSpace);
    node["anchorA"] = encodeVector2(joint.anchorA);
    node["anchorB"] = encodeVector2(joint.anchorB);
    node["axis"] = encodeVector2(joint.axis);
    node["target"] = encodeVector2(joint.target);
    node["autoAnchors"] = joint.autoAnchors;
    node["rope"] = joint.rope;
    node["enableCollision"] = joint.enableCollision;
    // infinite (never breaks) is the missing key
    if (std::isfinite(joint.breakForce)) node["breakForce"] = joint.breakForce;
    if (std::isfinite(joint.breakTorque)) node["breakTorque"] = joint.breakTorque;
    node["breakAction"] = jointBreakActionToString(joint.breakAction);
    node["useLimits"] = joint.useLimits;
    node["lowerAngle"] = joint.lowerAngle;
    node["upperAngle"] = joint.upperAngle;
    node["lowerTranslation"] = joint.lowerTranslation;
    node["upperTranslation"] = joint.upperTranslation;
    node["useMotor"] = joint.useMotor;
    node["motorSpeed"] = joint.motorSpeed;
    node["maxMotorTorque"] = joint.maxMotorTorque;
    node["maxMotorForce"] = joint.maxMotorForce;
    node["enableSpring"] = joint.enableSpring;
    node["frequency"] = joint.frequency;
    node["dampingRatio"] = joint.dampingRatio;
    node["length"] = joint.length;
    node["autoLength"] = joint.autoLength;
    node["minLength"] = joint.minLength;
    node["maxLength"] = joint.maxLength;
    node["linearFrequency"] = joint.linearFrequency;
    node["angularFrequency"] = joint.angularFrequency;
    node["linearDampingRatio"] = joint.linearDampingRatio;
    node["angularDampingRatio"] = joint.angularDampingRatio;
    node["linearOffset"] = encodeVector2(joint.linearOffset);
    node["angularOffset"] = joint.angularOffset;
    node["autoOffset"] = joint.autoOffset;
    node["maxForce"] = joint.maxForce;
    node["maxTorque"] = joint.maxTorque;
    node["correctionFactor"] = joint.correctionFactor;

    return node;
}

Joint2DComponent editor::Stream::decodeJoint2DComponent(const YAML::Node& node, const Joint2DComponent* oldJoint) {
    Joint2DComponent joint;

    if (oldJoint) {
        joint = *oldJoint;
        joint.loadFailed = false;
        if (node["type"]) joint.type = stringToJoint2DType(node["type"].as<std::string>());
    } else {
        // a key missing from a scene saved before is what the joint had then (Box2D's defaults
        // and world anchors), so old scenes simulate as they did
        const Joint2DType type = node["type"] ? stringToJoint2DType(node["type"].as<std::string>()) : Joint2DType::DISTANCE;
        joint = makeLegacyJoint2D(type);
    }

    if (node["bodyA"]) joint.bodyA = YamlNumbers::as<Entity>(node["bodyA"]);
    if (node["bodyB"]) joint.bodyB = YamlNumbers::as<Entity>(node["bodyB"]);
    if (node["anchorSpace"]) joint.anchorSpace = stringToJointAnchorSpace(node["anchorSpace"].as<std::string>());
    if (node["anchorA"]) joint.anchorA = decodeVector2(node["anchorA"]);
    if (node["anchorB"]) joint.anchorB = decodeVector2(node["anchorB"]);
    if (node["axis"]) joint.axis = decodeVector2(node["axis"]);
    if (node["target"]) joint.target = decodeVector2(node["target"]);
    if (node["autoAnchors"]) joint.autoAnchors = YamlNumbers::as<bool>(node["autoAnchors"]);
    if (node["rope"]) joint.rope = YamlNumbers::as<bool>(node["rope"]);
    if (node["enableCollision"]) joint.enableCollision = YamlNumbers::as<bool>(node["enableCollision"]);
    // written only when finite: absent is "never", also over an existing component
    joint.breakForce = node["breakForce"] ? YamlNumbers::as<float>(node["breakForce"]) : std::numeric_limits<float>::infinity();
    joint.breakTorque = node["breakTorque"] ? YamlNumbers::as<float>(node["breakTorque"]) : std::numeric_limits<float>::infinity();
    if (node["breakAction"]) joint.breakAction = stringToJointBreakAction(node["breakAction"].as<std::string>());
    if (node["useLimits"]) joint.useLimits = YamlNumbers::as<bool>(node["useLimits"]);
    if (node["lowerAngle"]) joint.lowerAngle = YamlNumbers::as<float>(node["lowerAngle"]);
    if (node["upperAngle"]) joint.upperAngle = YamlNumbers::as<float>(node["upperAngle"]);
    if (node["lowerTranslation"]) joint.lowerTranslation = YamlNumbers::as<float>(node["lowerTranslation"]);
    if (node["upperTranslation"]) joint.upperTranslation = YamlNumbers::as<float>(node["upperTranslation"]);
    if (node["useMotor"]) joint.useMotor = YamlNumbers::as<bool>(node["useMotor"]);
    if (node["motorSpeed"]) joint.motorSpeed = YamlNumbers::as<float>(node["motorSpeed"]);
    if (node["maxMotorTorque"]) joint.maxMotorTorque = YamlNumbers::as<float>(node["maxMotorTorque"]);
    if (node["maxMotorForce"]) joint.maxMotorForce = YamlNumbers::as<float>(node["maxMotorForce"]);
    if (node["enableSpring"]) joint.enableSpring = YamlNumbers::as<bool>(node["enableSpring"]);
    if (node["frequency"]) joint.frequency = YamlNumbers::as<float>(node["frequency"]);
    if (node["dampingRatio"]) joint.dampingRatio = YamlNumbers::as<float>(node["dampingRatio"]);
    if (node["length"]) joint.length = YamlNumbers::as<float>(node["length"]);
    if (node["autoLength"]) joint.autoLength = YamlNumbers::as<bool>(node["autoLength"]);
    if (node["minLength"]) joint.minLength = YamlNumbers::as<float>(node["minLength"]);
    if (node["maxLength"]) joint.maxLength = YamlNumbers::as<float>(node["maxLength"]);
    if (node["linearFrequency"]) joint.linearFrequency = YamlNumbers::as<float>(node["linearFrequency"]);
    if (node["angularFrequency"]) joint.angularFrequency = YamlNumbers::as<float>(node["angularFrequency"]);
    if (node["linearDampingRatio"]) joint.linearDampingRatio = YamlNumbers::as<float>(node["linearDampingRatio"]);
    if (node["angularDampingRatio"]) joint.angularDampingRatio = YamlNumbers::as<float>(node["angularDampingRatio"]);
    if (node["linearOffset"]) joint.linearOffset = decodeVector2(node["linearOffset"]);
    if (node["angularOffset"]) joint.angularOffset = YamlNumbers::as<float>(node["angularOffset"]);
    if (node["autoOffset"]) joint.autoOffset = YamlNumbers::as<bool>(node["autoOffset"]);
    if (node["maxForce"]) joint.maxForce = YamlNumbers::as<float>(node["maxForce"]);
    if (node["maxTorque"]) joint.maxTorque = YamlNumbers::as<float>(node["maxTorque"]);
    if (node["correctionFactor"]) joint.correctionFactor = YamlNumbers::as<float>(node["correctionFactor"]);

    if (oldJoint && b2Joint_IsValid(oldJoint->joint)) {
        joint.needUpdateJoint = true;
    }
    joint.needApplyParameters = false;
    joint.broken = false;
    joint.breakArmed = true;

    if (node["enabled"]) joint.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return joint;
}

YAML::Node editor::Stream::encodeJoint3DComponent(const Joint3DComponent& joint) {
    YAML::Node node;
    node["enabled"] = joint.enabled;

    node["type"] = joint3DTypeToString(joint.type);
    node["bodyA"] = joint.bodyA;
    node["bodyB"] = joint.bodyB;
    node["anchorA"] = encodeVector3(joint.anchorA);
    node["anchorB"] = encodeVector3(joint.anchorB);
    node["anchor"] = encodeVector3(joint.anchor);
    node["axis"] = encodeVector3(joint.axis);
    node["normal"] = encodeVector3(joint.normal);
    node["twistAxis"] = encodeVector3(joint.twistAxis);
    node["planeAxis"] = encodeVector3(joint.planeAxis);
    node["axisX"] = encodeVector3(joint.axisX);
    node["axisY"] = encodeVector3(joint.axisY);
    node["limitsMin"] = joint.limitsMin;
    node["limitsMax"] = joint.limitsMax;
    node["normalHalfConeAngle"] = joint.normalHalfConeAngle;
    node["planeHalfConeAngle"] = joint.planeHalfConeAngle;
    node["twistMinAngle"] = joint.twistMinAngle;
    node["twistMaxAngle"] = joint.twistMaxAngle;
    node["fixedPointA"] = encodeVector3(joint.fixedPointA);
    node["fixedPointB"] = encodeVector3(joint.fixedPointB);
    node["hingeA"] = joint.hingeA;
    node["hingeB"] = joint.hingeB;
    node["hinge"] = joint.hinge;
    node["slider"] = joint.slider;
    node["numTeethGearA"] = joint.numTeethGearA;
    node["numTeethGearB"] = joint.numTeethGearB;
    node["numTeethRack"] = joint.numTeethRack;
    node["numTeethGear"] = joint.numTeethGear;
    node["rackLength"] = joint.rackLength;
    YAML::Node pathPointsNode;
    for (const Vector3& point : joint.pathPoints){
        pathPointsNode.push_back(encodeVector3(point));
    }
    node["pathPoints"] = pathPointsNode;
    node["pathPosition"] = encodeVector3(joint.pathPosition);
    node["isLooping"] = joint.isLooping;
    node["autoAnchors"] = joint.autoAnchors;
    node["anchorSpace"] = jointAnchorSpaceToString(joint.anchorSpace);
    node["enableCollision"] = joint.enableCollision;
    // only when finite (YAML has no infinity the other readers agree on)
    if (std::isfinite(joint.breakForce)) node["breakForce"] = joint.breakForce;
    if (std::isfinite(joint.breakTorque)) node["breakTorque"] = joint.breakTorque;
    node["breakAction"] = jointBreakActionToString(joint.breakAction);
    node["useLimits"] = joint.useLimits;
    node["limitsSpringFrequency"] = joint.limitsSpringFrequency;
    node["limitsSpringDamping"] = joint.limitsSpringDamping;
    node["useMotor"] = joint.useMotor;
    node["motorTargetVelocity"] = joint.motorTargetVelocity;
    node["motorMaxForce"] = joint.motorMaxForce;
    node["useSpring"] = joint.useSpring;
    node["springStiffness"] = joint.springStiffness;
    node["springDamper"] = joint.springDamper;
    node["springTargetPosition"] = joint.springTargetPosition;
    node["maxFriction"] = joint.maxFriction;
    node["minDistance"] = joint.minDistance;
    node["maxDistance"] = joint.maxDistance;
    node["coneHalfAngle"] = joint.coneHalfAngle;
    node["swingType"] = joint.swingType == JointSwingType::PYRAMID ? "pyramid" : "cone";
    node["motorTargetRotation"] = encodeQuaternion(joint.motorTargetRotation);
    node["motorSpringFrequency"] = joint.motorSpringFrequency;
    node["motorSpringDamping"] = joint.motorSpringDamping;
    node["xMotion"] = jointMotionToString(joint.xMotion);
    node["yMotion"] = jointMotionToString(joint.yMotion);
    node["zMotion"] = jointMotionToString(joint.zMotion);
    node["angularXMotion"] = jointMotionToString(joint.angularXMotion);
    node["angularYMotion"] = jointMotionToString(joint.angularYMotion);
    node["angularZMotion"] = jointMotionToString(joint.angularZMotion);
    node["linearLimitMin"] = encodeVector3(joint.linearLimitMin);
    node["linearLimitMax"] = encodeVector3(joint.linearLimitMax);
    node["angularLimitMin"] = encodeVector3(joint.angularLimitMin);
    node["angularLimitMax"] = encodeVector3(joint.angularLimitMax);
    node["useDrive"] = joint.useDrive;
    node["driveStiffness"] = joint.driveStiffness;
    node["driveDamping"] = joint.driveDamping;
    node["angularDriveStiffness"] = joint.angularDriveStiffness;
    node["angularDriveDamping"] = joint.angularDriveDamping;
    if (std::isfinite(joint.driveMaxForce)) node["driveMaxForce"] = joint.driveMaxForce;
    if (std::isfinite(joint.driveMaxTorque)) node["driveMaxTorque"] = joint.driveMaxTorque;
    node["targetPosition"] = encodeVector3(joint.targetPosition);
    node["targetRotation"] = encodeQuaternion(joint.targetRotation);
    node["targetVelocity"] = encodeVector3(joint.targetVelocity);
    node["targetAngularVelocity"] = encodeVector3(joint.targetAngularVelocity);

    return node;
}

Joint3DComponent editor::Stream::decodeJoint3DComponent(const YAML::Node& node, const Joint3DComponent* oldJoint) {
    Joint3DComponent joint;

    const Joint3DType type = node["type"] ? stringToJoint3DType(node["type"].as<std::string>()) : Joint3DType::FIXED;
    if (oldJoint) {
        joint = *oldJoint;
        joint.loadFailed = false;
    }
    // a key missing from a scene saved before is what the joint had then (Jolt's defaults, world
    // anchors, the connected bodies colliding), so old scenes simulate as they did
    if (!node["anchorSpace"]) {
        setJoint3DLegacyParameters(joint, type);
    }
    joint.type = type;
    if (node["bodyA"]) joint.bodyA = YamlNumbers::as<Entity>(node["bodyA"]);
    if (node["bodyB"]) joint.bodyB = YamlNumbers::as<Entity>(node["bodyB"]);
    if (node["anchorA"]) joint.anchorA = decodeVector3(node["anchorA"]);
    if (node["anchorB"]) joint.anchorB = decodeVector3(node["anchorB"]);
    if (node["anchor"]) joint.anchor = decodeVector3(node["anchor"]);
    if (node["axis"]) joint.axis = decodeVector3(node["axis"]);
    if (node["normal"]) joint.normal = decodeVector3(node["normal"]);
    if (node["twistAxis"]) joint.twistAxis = decodeVector3(node["twistAxis"]);
    if (node["planeAxis"]) joint.planeAxis = decodeVector3(node["planeAxis"]);
    if (node["axisX"]) joint.axisX = decodeVector3(node["axisX"]);
    if (node["axisY"]) joint.axisY = decodeVector3(node["axisY"]);
    if (node["limitsMin"]) joint.limitsMin = YamlNumbers::as<float>(node["limitsMin"]);
    if (node["limitsMax"]) joint.limitsMax = YamlNumbers::as<float>(node["limitsMax"]);
    if (node["normalHalfConeAngle"]) joint.normalHalfConeAngle = YamlNumbers::as<float>(node["normalHalfConeAngle"]);
    if (node["planeHalfConeAngle"]) joint.planeHalfConeAngle = YamlNumbers::as<float>(node["planeHalfConeAngle"]);
    if (node["twistMinAngle"]) joint.twistMinAngle = YamlNumbers::as<float>(node["twistMinAngle"]);
    if (node["twistMaxAngle"]) joint.twistMaxAngle = YamlNumbers::as<float>(node["twistMaxAngle"]);
    if (node["fixedPointA"]) joint.fixedPointA = decodeVector3(node["fixedPointA"]);
    if (node["fixedPointB"]) joint.fixedPointB = decodeVector3(node["fixedPointB"]);
    if (node["hingeA"]) joint.hingeA = YamlNumbers::as<Entity>(node["hingeA"]);
    if (node["hingeB"]) joint.hingeB = YamlNumbers::as<Entity>(node["hingeB"]);
    if (node["hinge"]) joint.hinge = YamlNumbers::as<Entity>(node["hinge"]);
    if (node["slider"]) joint.slider = YamlNumbers::as<Entity>(node["slider"]);
    if (node["numTeethGearA"]) joint.numTeethGearA = YamlNumbers::as<int>(node["numTeethGearA"]);
    if (node["numTeethGearB"]) joint.numTeethGearB = YamlNumbers::as<int>(node["numTeethGearB"]);
    if (node["numTeethRack"]) joint.numTeethRack = YamlNumbers::as<int>(node["numTeethRack"]);
    if (node["numTeethGear"]) joint.numTeethGear = YamlNumbers::as<int>(node["numTeethGear"]);
    if (node["rackLength"]) joint.rackLength = YamlNumbers::as<int>(node["rackLength"]);
    if (node["pathPoints"]){
        joint.pathPoints.clear();
        for (const YAML::Node& pointNode : node["pathPoints"]){
            joint.pathPoints.push_back(decodeVector3(pointNode));
        }
    }
    if (node["pathPosition"]) joint.pathPosition = decodeVector3(node["pathPosition"]);
    if (node["isLooping"]) joint.isLooping = YamlNumbers::as<bool>(node["isLooping"]);
    if (node["autoAnchors"]) joint.autoAnchors = YamlNumbers::as<bool>(node["autoAnchors"]);
    if (node["anchorSpace"]) joint.anchorSpace = stringToJointAnchorSpace(node["anchorSpace"].as<std::string>());
    if (node["enableCollision"]) joint.enableCollision = YamlNumbers::as<bool>(node["enableCollision"]);
    // written only when finite: missing is infinite, over a live component too
    joint.breakForce = node["breakForce"] ? YamlNumbers::as<float>(node["breakForce"]) : std::numeric_limits<float>::infinity();
    joint.breakTorque = node["breakTorque"] ? YamlNumbers::as<float>(node["breakTorque"]) : std::numeric_limits<float>::infinity();
    if (node["breakAction"]) joint.breakAction = stringToJointBreakAction(node["breakAction"].as<std::string>());
    if (node["useLimits"]) joint.useLimits = YamlNumbers::as<bool>(node["useLimits"]);
    if (node["limitsSpringFrequency"]) joint.limitsSpringFrequency = YamlNumbers::as<float>(node["limitsSpringFrequency"]);
    if (node["limitsSpringDamping"]) joint.limitsSpringDamping = YamlNumbers::as<float>(node["limitsSpringDamping"]);
    if (node["useMotor"]) joint.useMotor = YamlNumbers::as<bool>(node["useMotor"]);
    if (node["motorTargetVelocity"]) joint.motorTargetVelocity = YamlNumbers::as<float>(node["motorTargetVelocity"]);
    if (node["motorMaxForce"]) joint.motorMaxForce = YamlNumbers::as<float>(node["motorMaxForce"]);
    if (node["useSpring"]) joint.useSpring = YamlNumbers::as<bool>(node["useSpring"]);
    if (node["springStiffness"]) joint.springStiffness = YamlNumbers::as<float>(node["springStiffness"]);
    if (node["springDamper"]) joint.springDamper = YamlNumbers::as<float>(node["springDamper"]);
    if (node["springTargetPosition"]) joint.springTargetPosition = YamlNumbers::as<float>(node["springTargetPosition"]);
    if (node["maxFriction"]) joint.maxFriction = YamlNumbers::as<float>(node["maxFriction"]);
    if (node["minDistance"]) joint.minDistance = YamlNumbers::as<float>(node["minDistance"]);
    if (node["maxDistance"]) joint.maxDistance = YamlNumbers::as<float>(node["maxDistance"]);
    if (node["coneHalfAngle"]) joint.coneHalfAngle = YamlNumbers::as<float>(node["coneHalfAngle"]);
    if (node["swingType"]) joint.swingType = node["swingType"].as<std::string>() == "pyramid" ? JointSwingType::PYRAMID : JointSwingType::CONE;
    if (node["motorTargetRotation"]) joint.motorTargetRotation = decodeQuaternion(node["motorTargetRotation"]);
    if (node["motorSpringFrequency"]) joint.motorSpringFrequency = YamlNumbers::as<float>(node["motorSpringFrequency"]);
    if (node["motorSpringDamping"]) joint.motorSpringDamping = YamlNumbers::as<float>(node["motorSpringDamping"]);
    if (node["xMotion"]) joint.xMotion = stringToJointMotion(node["xMotion"].as<std::string>());
    if (node["yMotion"]) joint.yMotion = stringToJointMotion(node["yMotion"].as<std::string>());
    if (node["zMotion"]) joint.zMotion = stringToJointMotion(node["zMotion"].as<std::string>());
    if (node["angularXMotion"]) joint.angularXMotion = stringToJointMotion(node["angularXMotion"].as<std::string>());
    if (node["angularYMotion"]) joint.angularYMotion = stringToJointMotion(node["angularYMotion"].as<std::string>());
    if (node["angularZMotion"]) joint.angularZMotion = stringToJointMotion(node["angularZMotion"].as<std::string>());
    if (node["linearLimitMin"]) joint.linearLimitMin = decodeVector3(node["linearLimitMin"]);
    if (node["linearLimitMax"]) joint.linearLimitMax = decodeVector3(node["linearLimitMax"]);
    if (node["angularLimitMin"]) joint.angularLimitMin = decodeVector3(node["angularLimitMin"]);
    if (node["angularLimitMax"]) joint.angularLimitMax = decodeVector3(node["angularLimitMax"]);
    if (node["useDrive"]) joint.useDrive = YamlNumbers::as<bool>(node["useDrive"]);
    if (node["driveStiffness"]) joint.driveStiffness = YamlNumbers::as<float>(node["driveStiffness"]);
    if (node["driveDamping"]) joint.driveDamping = YamlNumbers::as<float>(node["driveDamping"]);
    if (node["angularDriveStiffness"]) joint.angularDriveStiffness = YamlNumbers::as<float>(node["angularDriveStiffness"]);
    if (node["angularDriveDamping"]) joint.angularDriveDamping = YamlNumbers::as<float>(node["angularDriveDamping"]);
    joint.driveMaxForce = node["driveMaxForce"] ? YamlNumbers::as<float>(node["driveMaxForce"]) : std::numeric_limits<float>::infinity();
    joint.driveMaxTorque = node["driveMaxTorque"] ? YamlNumbers::as<float>(node["driveMaxTorque"]) : std::numeric_limits<float>::infinity();
    if (node["targetPosition"]) joint.targetPosition = decodeVector3(node["targetPosition"]);
    if (node["targetRotation"]) joint.targetRotation = decodeQuaternion(node["targetRotation"]);
    if (node["targetVelocity"]) joint.targetVelocity = decodeVector3(node["targetVelocity"]);
    if (node["targetAngularVelocity"]) joint.targetAngularVelocity = decodeVector3(node["targetAngularVelocity"]);

    if (oldJoint && oldJoint->joint) {
        joint.needUpdateJoint = true;
    }

    if (node["enabled"]) joint.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return joint;
}
