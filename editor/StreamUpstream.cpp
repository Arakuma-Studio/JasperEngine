// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The scene codecs of upstream's components that came with SYNC#4 (origin/main 047eccf9b) and
// have no reflection field table of their own yet: Canvas Group, the on-screen controls (stick,
// button, touch area), Ragdoll and Trail Renderer, with the Line/Trail ribbon helpers. Their
// reflection descriptors (editor/reflect/components/UpstreamReflect.cpp) use these as codecs
// (JE_STREAM_CODEC), as the Vertex Animator's does.

#include "Stream.h"
#include "Catalog.h"
#include "Out.h"
#include "util/YamlNumbers.h"
#include "util/Color.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

using namespace jasperengine;

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
    encodeRibbonFields(node, trail, defaults);

    if (!trail.customShader.empty())
        node["customShader"] = trail.customShader;
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
    decodeRibbonFields(node, trail, defaults);

    trail.customShader = node["customShader"] ? node["customShader"].as<std::string>() : std::string();
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
