// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The codecs of editor/Stream.cpp before S08, unchanged (LegacyCodecs.h)

#include "reflect/legacy/LegacyCodecs.h"
#include "component/ActionComponent.h"
#include "component/AlphaTweenComponent.h"
#include "component/AnimationComponent.h"
#include "component/BoneComponent.h"
#include "component/ButtonComponent.h"
#include "component/ColorTweenComponent.h"
#include "component/FogComponent.h"
#include "component/ImageComponent.h"
#include "component/InputFieldComponent.h"
#include "component/Joint2DComponent.h"
#include "component/Joint3DComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/LatticeFollowerComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/Light2DComponent.h"
#include "component/LocalVolumetricFogComponent.h"
#include "component/MirrorComponent.h"
#include "component/PanelComponent.h"
#include "component/PositionTweenComponent.h"
#include "component/ProgressbarComponent.h"
#include "component/RectTransformComponent.h"
#include "component/ReflectionProbeComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/RotationTweenComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/ScaleTweenComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/ShadowCaster2DComponent.h"
#include "component/TextComponent.h"
#include "component/TranslateTracksComponent.h"
#include "component/TweenComponent.h"
#include "component/UIDocumentComponent.h"
#include "component/VolumetricCloudsComponent.h"
#include "particles/ParticleModules.h"
#include "component/FluidSimulationComponent.h"
#include "component/CameraComponent.h"
#include "component/Transform.h"
#include "util/LightPhotometry.h"

#include "Stream.h"
#include "StreamSchema.h"
#include "pool/SoundPool.h"
#include "util/YamlNumbers.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    std::string shadowCaster2DShapeToString(ShadowCaster2DShape shape) {
        return StreamSchema::shadowCaster2DShape().toString(shape);
    }

}

YAML::Node LegacyCodecs::encodeLightComponent(const LightComponent& light) {
    YAML::Node node;
    node["enabled"] = light.enabled;

    node["type"] = lightTypeToString(light.type);
    node["direction"] = encodeVector3(light.direction);
    node["worldDirection"] = encodeVector3(light.worldDirection);
    node["color"] = encodeVector3(light.color);
    node["range"] = light.range;
    node["intensity"] = light.intensity;
    node["innerConeCos"] = light.innerConeCos;
    node["outerConeCos"] = light.outerConeCos;
    node["spotMask"] = encodeTexture(light.spotMask);
    node["cullingMask"] = light.cullingMask;
    node["shadows"] = light.shadows;
    node["automaticShadowCamera"] = light.automaticShadowCamera;
    node["shadowBias"] = light.shadowBias;
    node["mapResolution"] = light.mapResolution;
    node["shadowCameraNearFar"] = encodeVector2(light.shadowCameraNearFar);
    node["numShadowCascades"] = light.numShadowCascades;
    if (light.shading != LightShading::BOTH)
        node["shading"] = lightShadingToString(light.shading);
    // the shadow settings added with S10, written only away from their defaults (so scenes
    // saved before them re-save unchanged)
    const LightComponent defaults;
    if (light.softShadows != defaults.softShadows) node["softShadows"] = light.softShadows;
    if (light.shadowStrength != defaults.shadowStrength) node["shadowStrength"] = light.shadowStrength;
    if (light.shadowNormalBias != defaults.shadowNormalBias) node["shadowNormalBias"] = light.shadowNormalBias;
    if (light.cascadeSplitMode != defaults.cascadeSplitMode)
        node["cascadeSplitMode"] = StreamSchema::cascadeSplitMode().toString(light.cascadeSplitMode);
    if (light.cascadeSplits != defaults.cascadeSplits) node["cascadeSplits"] = encodeVector3(light.cascadeSplits);
    if (light.shadowDistance != defaults.shadowDistance) node["shadowDistance"] = light.shadowDistance;
    if (light.shadowAngularDiameter != defaults.shadowAngularDiameter) node["shadowAngularDiameter"] = light.shadowAngularDiameter;
    if (light.shadowSourceRadius != defaults.shadowSourceRadius) node["shadowSourceRadius"] = light.shadowSourceRadius;
    if (light.shadowUpdateMode != defaults.shadowUpdateMode)
        node["shadowUpdateMode"] = StreamSchema::shadowUpdateMode().toString(light.shadowUpdateMode);
    // the volumetric fog's share of the light (omitted at its default)
    if (light.volumetricIntensity != LightComponent().volumetricIntensity)
        node["volumetricIntensity"] = light.volumetricIntensity;
    // photometric units and colour temperature: written only away from their defaults
    if (light.intensityUnit != LightUnit::ENGINE)
        node["intensityUnit"] = StreamSchema::lightUnit().toString(light.intensityUnit);
    if (light.useColorTemperature)
        node["useColorTemperature"] = true;
    if (light.colorTemperature != LightComponent().colorTemperature)
        node["colorTemperature"] = YamlNumbers::text(finiteOr(light.colorTemperature, LightComponent().colorTemperature));

    return node;
}

LightComponent LegacyCodecs::decodeLightComponent(const YAML::Node& node, const LightComponent* oldLight) {
    LightComponent light;

    // Use old values as defaults if provided
    if (oldLight) {
        light = *oldLight;
    }

    if (node["type"]) light.type = decodeEnum(node, "type", StreamSchema::lightType());
    if (node["direction"]) light.direction = decodeVector3(node["direction"]);
    if (node["worldDirection"]) light.worldDirection = decodeVector3(node["worldDirection"]);
    if (node["color"]) light.color = decodeVector3(node["color"]);
    if (node["range"]) light.range = YamlNumbers::as<float>(node["range"]);
    if (node["intensity"]) light.intensity = YamlNumbers::as<float>(node["intensity"]);
    if (node["innerConeCos"]) light.innerConeCos = YamlNumbers::as<float>(node["innerConeCos"]);
    if (node["outerConeCos"]) light.outerConeCos = YamlNumbers::as<float>(node["outerConeCos"]);
    if (node["spotMask"]) light.spotMask = decodeTexture(node["spotMask"]);
    if (node["cullingMask"]) light.cullingMask = decodeLayerMask(node["cullingMask"], light.cullingMask);
    if (node["shadows"]) light.shadows = YamlNumbers::as<bool>(node["shadows"]);
    if (node["automaticShadowCamera"]) light.automaticShadowCamera = YamlNumbers::as<bool>(node["automaticShadowCamera"]);
    if (node["shadowBias"]) light.shadowBias = YamlNumbers::as<float>(node["shadowBias"]);
    if (node["mapResolution"]) light.mapResolution = YamlNumbers::as<unsigned int>(node["mapResolution"]);
    if (node["shadowCameraNearFar"]) light.shadowCameraNearFar = decodeVector2(node["shadowCameraNearFar"]);
    if (node["numShadowCascades"]) light.numShadowCascades = YamlNumbers::as<unsigned int>(node["numShadowCascades"]);
    // omitted at its default, so an absent key is the default
    light.shading = node["shading"] ? decodeEnum(node, "shading", StreamSchema::lightShading()) : LightShading::BOTH;
    // omitted at their defaults (S10), so an absent key is the default
    const LightComponent defaults;
    light.softShadows = node["softShadows"] ? YamlNumbers::as<bool>(node["softShadows"]) : defaults.softShadows;
    light.shadowStrength = node["shadowStrength"] ? YamlNumbers::as<float>(node["shadowStrength"]) : defaults.shadowStrength;
    light.shadowNormalBias = node["shadowNormalBias"] ? YamlNumbers::as<float>(node["shadowNormalBias"]) : defaults.shadowNormalBias;
    light.cascadeSplitMode = node["cascadeSplitMode"] ? decodeEnum(node, "cascadeSplitMode", StreamSchema::cascadeSplitMode()) : defaults.cascadeSplitMode;
    light.cascadeSplits = node["cascadeSplits"] ? decodeVector3(node["cascadeSplits"]) : defaults.cascadeSplits;
    light.shadowDistance = node["shadowDistance"] ? YamlNumbers::as<float>(node["shadowDistance"]) : defaults.shadowDistance;
    light.shadowAngularDiameter = node["shadowAngularDiameter"] ? YamlNumbers::as<float>(node["shadowAngularDiameter"]) : defaults.shadowAngularDiameter;
    light.shadowSourceRadius = node["shadowSourceRadius"] ? YamlNumbers::as<float>(node["shadowSourceRadius"]) : defaults.shadowSourceRadius;
    light.shadowUpdateMode = node["shadowUpdateMode"] ? decodeEnum(node, "shadowUpdateMode", StreamSchema::shadowUpdateMode()) : defaults.shadowUpdateMode;
    light.volumetricIntensity = node["volumetricIntensity"] ? YamlNumbers::as<float>(node["volumetricIntensity"]) : LightComponent().volumetricIntensity;
    light.intensityUnit = node["intensityUnit"] ? decodeEnum(node, "intensityUnit", StreamSchema::lightUnit()) : LightUnit::ENGINE;
    light.useColorTemperature = node["useColorTemperature"] ? YamlNumbers::as<bool>(node["useColorTemperature"]) : false;
    light.colorTemperature = std::clamp(decodeFinite(node["colorTemperature"], LightComponent().colorTemperature),
                                        LightPhotometry::MIN_TEMPERATURE, LightPhotometry::MAX_TEMPERATURE);

    if (node["enabled"]) light.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return light;
}

YAML::Node LegacyCodecs::encodeLight2DComponent(const Light2DComponent& light) {
    YAML::Node node;
    node["enabled"] = light.enabled;

    node["color"] = encodeVector3(light.color);
    node["intensity"] = light.intensity;
    node["range"] = light.range;
    node["falloff"] = light.falloff;
    node["height"] = light.height;
    node["shadows"] = light.shadows;
    node["shadowBias"] = light.shadowBias;
    node["shadowSoftness"] = light.shadowSoftness;
    node["mapResolution"] = light.mapResolution;

    return node;
}

Light2DComponent LegacyCodecs::decodeLight2DComponent(const YAML::Node& node, const Light2DComponent* oldLight) {
    Light2DComponent light;

    // Use old values as defaults if provided
    if (oldLight) {
        light = *oldLight;
    }

    if (node["color"]) light.color = decodeVector3(node["color"]);
    if (node["intensity"]) light.intensity = YamlNumbers::as<float>(node["intensity"]);
    if (node["range"]) light.range = YamlNumbers::as<float>(node["range"]);
    if (node["falloff"]) light.falloff = YamlNumbers::as<float>(node["falloff"]);
    if (node["height"]) light.height = YamlNumbers::as<float>(node["height"]);
    if (node["shadows"]) light.shadows = YamlNumbers::as<bool>(node["shadows"]);
    if (node["shadowBias"]) light.shadowBias = YamlNumbers::as<float>(node["shadowBias"]);
    if (node["shadowSoftness"]) light.shadowSoftness = YamlNumbers::as<float>(node["shadowSoftness"]);
    if (node["mapResolution"]) light.mapResolution = YamlNumbers::as<unsigned int>(node["mapResolution"]);

    if (node["enabled"]) light.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return light;
}

YAML::Node LegacyCodecs::encodeShadowCaster2DComponent(const ShadowCaster2DComponent& occluder) {
    YAML::Node node;

    node["shape"] = shadowCaster2DShapeToString(occluder.shape);
    node["closed"] = occluder.closed;
    node["enabled"] = occluder.enabled;

    YAML::Node pointsNode(YAML::NodeType::Sequence);
    for (const Vector2& point : occluder.points) {
        pointsNode.push_back(encodeVector2(point));
    }
    node["points"] = pointsNode;

    return node;
}

ShadowCaster2DComponent LegacyCodecs::decodeShadowCaster2DComponent(const YAML::Node& node, const ShadowCaster2DComponent* oldOccluder) {
    ShadowCaster2DComponent occluder;

    if (oldOccluder) {
        occluder = *oldOccluder;
    }

    if (node["shape"]) occluder.shape = decodeEnum(node, "shape", StreamSchema::shadowCaster2DShape());
    if (node["closed"]) occluder.closed = YamlNumbers::as<bool>(node["closed"]);
    if (node["enabled"]) occluder.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["points"]) {
        occluder.points.clear();
        for (const auto& pointNode : node["points"]) {
            occluder.points.push_back(decodeVector2(pointNode));
        }
    }

    return occluder;
}

YAML::Node LegacyCodecs::encodeFogComponent(const FogComponent& fog) {
    YAML::Node node;

    node["type"] = fogTypeToString(fog.type);
    node["color"] = encodeVector3(fog.color);
    node["density"] = fog.density;
    node["linearStart"] = fog.linearStart;
    node["linearEnd"] = fog.linearEnd;

    // the height fog's fields, omitted at their defaults: a distance fog's file is as it was
    const FogComponent defaults;
    if (fog.colorMode != defaults.colorMode) node["colorMode"] = StreamSchema::fogColorMode().toString(fog.colorMode);
    if (fog.attenuationDistance != defaults.attenuationDistance) node["attenuationDistance"] = fog.attenuationDistance;
    if (fog.baseHeight != defaults.baseHeight) node["baseHeight"] = fog.baseHeight;
    if (fog.maximumHeight != defaults.maximumHeight) node["maximumHeight"] = fog.maximumHeight;
    if (fog.startDistance != defaults.startDistance) node["startDistance"] = fog.startDistance;
    if (fog.maxFogDistance != defaults.maxFogDistance) node["maxFogDistance"] = fog.maxFogDistance;
    if (fog.maxOpacity != defaults.maxOpacity) node["maxOpacity"] = fog.maxOpacity;
    if (fog.directionalInscattering != defaults.directionalInscattering) node["directionalInscattering"] = fog.directionalInscattering;
    if (fog.anisotropy != defaults.anisotropy) node["anisotropy"] = fog.anisotropy;
    if (fog.skyFill != defaults.skyFill) node["skyFill"] = fog.skyFill;
    if (fog.skyFillAngle != defaults.skyFillAngle) node["skyFillAngle"] = fog.skyFillAngle;
    if (fog.volumetric != defaults.volumetric) node["volumetric"] = fog.volumetric;
    if (fog.volumetricDistance != defaults.volumetricDistance) node["volumetricDistance"] = fog.volumetricDistance;
    if (fog.lightShafts != defaults.lightShafts) node["lightShafts"] = fog.lightShafts;
    if (fog.noiseIntensity != defaults.noiseIntensity) node["noiseIntensity"] = fog.noiseIntensity;
    if (fog.noiseScale != defaults.noiseScale) node["noiseScale"] = fog.noiseScale;
    if (fog.wind != defaults.wind) node["wind"] = encodeVector3(fog.wind);
    if (fog.qualityOverride != defaults.qualityOverride) node["qualityOverride"] = StreamSchema::atmosphereQualityOverride().toString(fog.qualityOverride);

    return node;
}

FogComponent LegacyCodecs::decodeFogComponent(const YAML::Node& node, const FogComponent* oldFog) {
    FogComponent fog;

    if (oldFog) {
        fog = *oldFog;
    }

    if (node["type"]) fog.type = decodeEnum(node, "type", StreamSchema::fogType());
    if (node["color"]) fog.color = decodeVector3(node["color"]);
    if (node["density"]) fog.density = YamlNumbers::as<float>(node["density"]);
    if (node["linearStart"]) fog.linearStart = YamlNumbers::as<float>(node["linearStart"]);
    if (node["linearEnd"]) fog.linearEnd = YamlNumbers::as<float>(node["linearEnd"]);

    // the height fog's fields are omitted at their defaults, so an absent key is the default,
    // not the old value (paste and play-stop restore decode over an existing component)
    const FogComponent defaults;
    auto readFloat = [&node](const char* key, float fallback) {
        return node[key] ? YamlNumbers::as<float>(node[key]) : fallback;
    };
    auto readBool = [&node](const char* key, bool fallback) {
        return node[key] ? YamlNumbers::as<bool>(node[key]) : fallback;
    };
    fog.colorMode = node["colorMode"] ? decodeEnum(node, "colorMode", StreamSchema::fogColorMode()) : defaults.colorMode;
    fog.attenuationDistance = readFloat("attenuationDistance", defaults.attenuationDistance);
    fog.baseHeight = readFloat("baseHeight", defaults.baseHeight);
    fog.maximumHeight = readFloat("maximumHeight", defaults.maximumHeight);
    fog.startDistance = readFloat("startDistance", defaults.startDistance);
    fog.maxFogDistance = readFloat("maxFogDistance", defaults.maxFogDistance);
    fog.maxOpacity = readFloat("maxOpacity", defaults.maxOpacity);
    fog.directionalInscattering = readFloat("directionalInscattering", defaults.directionalInscattering);
    fog.anisotropy = readFloat("anisotropy", defaults.anisotropy);
    fog.skyFill = readBool("skyFill", defaults.skyFill);
    fog.skyFillAngle = readFloat("skyFillAngle", defaults.skyFillAngle);
    fog.volumetric = readBool("volumetric", defaults.volumetric);
    fog.volumetricDistance = readFloat("volumetricDistance", defaults.volumetricDistance);
    fog.lightShafts = readBool("lightShafts", defaults.lightShafts);
    fog.noiseIntensity = readFloat("noiseIntensity", defaults.noiseIntensity);
    fog.noiseScale = readFloat("noiseScale", defaults.noiseScale);
    fog.wind = node["wind"] ? decodeVector3(node["wind"]) : defaults.wind;
    fog.qualityOverride = node["qualityOverride"] ? decodeEnum(node, "qualityOverride", StreamSchema::atmosphereQualityOverride()) : defaults.qualityOverride;

    return fog;
}

YAML::Node LegacyCodecs::encodeLocalVolumetricFogComponent(const LocalVolumetricFogComponent& volume) {
    YAML::Node node;

    node["shape"] = StreamSchema::fogVolumeShape().toString(volume.shape);
    node["size"] = encodeVector3(volume.size);
    node["attenuationDistance"] = volume.attenuationDistance;
    node["albedo"] = encodeVector3(volume.albedo);
    node["blendDistance"] = volume.blendDistance;
    node["falloff"] = StreamSchema::fogVolumeFalloff().toString(volume.falloff);
    node["noiseIntensity"] = volume.noiseIntensity;
    node["noiseScale"] = volume.noiseScale;
    node["scrollSpeed"] = encodeVector3(volume.scrollSpeed);

    return node;
}

LocalVolumetricFogComponent LegacyCodecs::decodeLocalVolumetricFogComponent(const YAML::Node& node, const LocalVolumetricFogComponent* oldVolume) {
    LocalVolumetricFogComponent volume;
    if (oldVolume) volume = *oldVolume;

    if (node["shape"]) volume.shape = decodeEnum(node, "shape", StreamSchema::fogVolumeShape());
    if (node["size"]) volume.size = decodeVector3(node["size"]);
    if (node["attenuationDistance"]) volume.attenuationDistance = YamlNumbers::as<float>(node["attenuationDistance"]);
    if (node["albedo"]) volume.albedo = decodeVector3(node["albedo"]);
    if (node["blendDistance"]) volume.blendDistance = YamlNumbers::as<float>(node["blendDistance"]);
    if (node["falloff"]) volume.falloff = decodeEnum(node, "falloff", StreamSchema::fogVolumeFalloff());
    if (node["noiseIntensity"]) volume.noiseIntensity = YamlNumbers::as<float>(node["noiseIntensity"]);
    if (node["noiseScale"]) volume.noiseScale = YamlNumbers::as<float>(node["noiseScale"]);
    if (node["scrollSpeed"]) volume.scrollSpeed = decodeVector3(node["scrollSpeed"]);

    return volume;
}

YAML::Node LegacyCodecs::encodeVolumetricCloudsComponent(const VolumetricCloudsComponent& clouds) {
    YAML::Node node;

    node["preset"] = StreamSchema::cloudsPreset().toString(clouds.preset);
    node["rendering"] = StreamSchema::cloudsRendering().toString(clouds.rendering);
    node["coverage"] = clouds.coverage;
    node["density"] = clouds.density;
    node["shapeScale"] = clouds.shapeScale;
    node["shapeFactor"] = clouds.shapeFactor;
    node["erosionFactor"] = clouds.erosionFactor;
    node["erosionScale"] = clouds.erosionScale;
    node["bottomAltitude"] = clouds.bottomAltitude;
    node["altitudeRange"] = clouds.altitudeRange;
    node["earthCurvature"] = clouds.earthCurvature;
    node["windDirection"] = clouds.windDirection;
    node["windSpeed"] = clouds.windSpeed;
    node["weatherMap"] = encodeTexture(clouds.weatherMap);
    node["weatherMapScale"] = clouds.weatherMapScale;
    node["sunLightDimmer"] = clouds.sunLightDimmer;
    node["ambientLightDimmer"] = clouds.ambientLightDimmer;
    node["maxDistance"] = clouds.maxDistance;
    node["qualityOverride"] = StreamSchema::atmosphereQualityOverride().toString(clouds.qualityOverride);

    return node;
}

VolumetricCloudsComponent LegacyCodecs::decodeVolumetricCloudsComponent(const YAML::Node& node, const VolumetricCloudsComponent* oldClouds) {
    VolumetricCloudsComponent clouds;
    if (oldClouds) clouds = *oldClouds;

    if (node["preset"]) clouds.preset = decodeEnum(node, "preset", StreamSchema::cloudsPreset());
    if (node["rendering"]) clouds.rendering = decodeEnum(node, "rendering", StreamSchema::cloudsRendering());
    if (node["coverage"]) clouds.coverage = YamlNumbers::as<float>(node["coverage"]);
    if (node["density"]) clouds.density = YamlNumbers::as<float>(node["density"]);
    if (node["shapeScale"]) clouds.shapeScale = YamlNumbers::as<float>(node["shapeScale"]);
    if (node["shapeFactor"]) clouds.shapeFactor = YamlNumbers::as<float>(node["shapeFactor"]);
    if (node["erosionFactor"]) clouds.erosionFactor = YamlNumbers::as<float>(node["erosionFactor"]);
    if (node["erosionScale"]) clouds.erosionScale = YamlNumbers::as<float>(node["erosionScale"]);
    if (node["bottomAltitude"]) clouds.bottomAltitude = YamlNumbers::as<float>(node["bottomAltitude"]);
    if (node["altitudeRange"]) clouds.altitudeRange = YamlNumbers::as<float>(node["altitudeRange"]);
    if (node["earthCurvature"]) clouds.earthCurvature = YamlNumbers::as<float>(node["earthCurvature"]);
    if (node["windDirection"]) clouds.windDirection = YamlNumbers::as<float>(node["windDirection"]);
    if (node["windSpeed"]) clouds.windSpeed = YamlNumbers::as<float>(node["windSpeed"]);
    if (node["weatherMap"]) clouds.weatherMap = decodeTexture(node["weatherMap"]);
    if (node["weatherMapScale"]) clouds.weatherMapScale = YamlNumbers::as<float>(node["weatherMapScale"]);
    if (node["sunLightDimmer"]) clouds.sunLightDimmer = YamlNumbers::as<float>(node["sunLightDimmer"]);
    if (node["ambientLightDimmer"]) clouds.ambientLightDimmer = YamlNumbers::as<float>(node["ambientLightDimmer"]);
    if (node["maxDistance"]) clouds.maxDistance = YamlNumbers::as<float>(node["maxDistance"]);
    if (node["qualityOverride"]) clouds.qualityOverride = decodeEnum(node, "qualityOverride", StreamSchema::atmosphereQualityOverride());

    return clouds;
}

YAML::Node LegacyCodecs::encodeMirrorComponent(const MirrorComponent& mirror) {
    YAML::Node node;

    node["normal"] = encodeVector3(mirror.normal);

    return node;
}

MirrorComponent LegacyCodecs::decodeMirrorComponent(const YAML::Node& node, const MirrorComponent* oldMirror) {
    MirrorComponent mirror;

    if (oldMirror) {
        mirror = *oldMirror;
    }

    if (node["normal"]) mirror.normal = decodeVector3(node["normal"]);

    return mirror;
}

YAML::Node LegacyCodecs::encodeReflectionProbeComponent(const ReflectionProbeComponent& probe) {
    YAML::Node node;

    node["mode"] = probe.mode == ReflectionProbeMode::DYNAMIC ? "dynamic" : "static";
    switch (probe.updateMode) {
        case ReflectionProbeUpdateMode::ON_MOVE: node["updateMode"] = "on_move"; break;
        case ReflectionProbeUpdateMode::INTERVAL: node["updateMode"] = "interval"; break;
        case ReflectionProbeUpdateMode::MANUAL: node["updateMode"] = "manual"; break;
        default: node["updateMode"] = "on_load"; break;
    }
    node["boxOffset"] = encodeVector3(probe.boxOffset);
    node["boxSize"] = encodeVector3(probe.boxSize);
    node["blendDistance"] = probe.blendDistance;
    node["intensity"] = probe.intensity;
    node["priority"] = probe.priority;
    node["resolution"] = probe.resolution;
    node["nearClip"] = probe.nearClip;
    node["farClip"] = probe.farClip;
    node["updateInterval"] = probe.updateInterval;
    node["includeSky"] = probe.includeSky;
    node["cullingMask"] = probe.cullingMask;
    node["texture"] = encodeTexture(probe.texture);

    return node;
}

ReflectionProbeComponent LegacyCodecs::decodeReflectionProbeComponent(const YAML::Node& node, const ReflectionProbeComponent* oldProbe) {
    ReflectionProbeComponent probe;
    if (oldProbe) probe = *oldProbe;

    if (node["mode"]) {
        probe.mode = node["mode"].as<std::string>() == "dynamic" ? ReflectionProbeMode::DYNAMIC : ReflectionProbeMode::STATIC;
    }
    if (node["updateMode"]) {
        const std::string value = node["updateMode"].as<std::string>();
        if (value == "on_move") probe.updateMode = ReflectionProbeUpdateMode::ON_MOVE;
        else if (value == "interval") probe.updateMode = ReflectionProbeUpdateMode::INTERVAL;
        else if (value == "manual") probe.updateMode = ReflectionProbeUpdateMode::MANUAL;
        else probe.updateMode = ReflectionProbeUpdateMode::ON_LOAD;
    }
    if (node["boxOffset"]) probe.boxOffset = decodeVector3(node["boxOffset"]);
    if (node["boxSize"]) probe.boxSize = decodeVector3(node["boxSize"]);
    if (node["blendDistance"]) probe.blendDistance = YamlNumbers::as<float>(node["blendDistance"]);
    if (node["intensity"]) probe.intensity = YamlNumbers::as<float>(node["intensity"]);
    if (node["priority"]) probe.priority = YamlNumbers::as<int>(node["priority"]);
    if (node["resolution"]) probe.resolution = YamlNumbers::as<unsigned int>(node["resolution"]);
    if (node["nearClip"]) probe.nearClip = YamlNumbers::as<float>(node["nearClip"]);
    if (node["farClip"]) probe.farClip = YamlNumbers::as<float>(node["farClip"]);
    if (node["updateInterval"]) probe.updateInterval = YamlNumbers::as<float>(node["updateInterval"]);
    if (node["includeSky"]) probe.includeSky = YamlNumbers::as<bool>(node["includeSky"]);
    if (node["cullingMask"]) probe.cullingMask = decodeLayerMask(node["cullingMask"], probe.cullingMask);
    if (node["texture"]) probe.texture = decodeTexture(node["texture"]);
    probe.needUpdate = true;

    return probe;
}

YAML::Node LegacyCodecs::encodeUIDocumentComponent(const UIDocumentComponent& document) {
    YAML::Node node;
    node["enabled"] = document.enabled;

    // written when empty too: decoding over a component with a file must clear it
    encodeAssetRef(node, "filename", document.filename);
    node["sortOrder"] = document.sortOrder;

    // phase 1: only what differs from the defaults (decode restores the defaults)
    const UIDocumentComponent defaults;
    if (document.interactive != defaults.interactive) node["interactive"] = document.interactive;
    if (document.renderMode != defaults.renderMode) node["renderMode"] = uiDocumentRenderModeToString(document.renderMode);
    if (document.textureWidth != defaults.textureWidth) node["textureWidth"] = document.textureWidth;
    if (document.textureHeight != defaults.textureHeight) node["textureHeight"] = document.textureHeight;
    if (document.inputSurface != defaults.inputSurface) node["inputSurface"] = document.inputSurface;
    if (document.pixelRatio != defaults.pixelRatio) encodeFinite(node, "pixelRatio", document.pixelRatio);

    return node;
}

UIDocumentComponent LegacyCodecs::decodeUIDocumentComponent(const YAML::Node& node, const UIDocumentComponent* oldDocument) {
    // the old one's runtime state (its loaded document, onLoad/onUnload) stays while the file
    // and the render settings are the same
    UIDocumentComponent document;

    if (oldDocument) {
        document = *oldDocument;
    }

    if (node["filename"]) document.filename = decodeAssetRef(node, "filename");
    if (node["sortOrder"]) document.sortOrder = YamlNumbers::as<int>(node["sortOrder"]);
    if (node["enabled"]) document.enabled = YamlNumbers::as<bool>(node["enabled"]);

    // phase-1 keys are omitted at their defaults, so a missing key is the default, not the
    // old value (paste and play-stop restore decode over an existing component); the
    // clipboard fills a key it has no default for with an empty scalar, which is the default too
    const UIDocumentComponent defaults;
    auto given = [&node](const char* key) {
        const YAML::Node value = node[key];
        return value && !value.IsNull() && !(value.IsScalar() && value.Scalar().empty());
    };
    document.interactive = given("interactive") ? YamlNumbers::as<bool>(node["interactive"]) : defaults.interactive;
    document.renderMode = given("renderMode") ? decodeEnum(node, "renderMode", StreamSchema::uiDocumentRenderMode()) : defaults.renderMode;
    document.textureWidth = given("textureWidth") ? std::clamp(YamlNumbers::as<int>(node["textureWidth"]), 1, 8192) : defaults.textureWidth;
    document.textureHeight = given("textureHeight") ? std::clamp(YamlNumbers::as<int>(node["textureHeight"]), 1, 8192) : defaults.textureHeight;
    document.inputSurface = given("inputSurface") ? YamlNumbers::as<Entity>(node["inputSurface"]) : defaults.inputSurface;
    document.pixelRatio = given("pixelRatio") ? std::max(0.0f, decodeFinite(node["pixelRatio"], defaults.pixelRatio)) : defaults.pixelRatio;

    if (oldDocument && (oldDocument->filename != document.filename || oldDocument->renderMode != document.renderMode ||
                        oldDocument->textureWidth != document.textureWidth || oldDocument->textureHeight != document.textureHeight ||
                        oldDocument->pixelRatio != document.pixelRatio)) {
        document.needReload = true;
    }

    return document;
}

YAML::Node LegacyCodecs::encodeCameraComponent(const CameraComponent& camera) {
    YAML::Node node;
    node["enabled"] = camera.enabled;

    node["type"] = cameraTypeToString(camera.type);
    node["cullingMask"] = camera.cullingMask;
    encodeFiniteVector3(node, "target", camera.target);
    encodeFiniteVector3(node, "up", camera.up);
    encodeFinite(node, "leftClip", camera.leftClip);
    encodeFinite(node, "rightClip", camera.rightClip);
    encodeFinite(node, "bottomClip", camera.bottomClip);
    encodeFinite(node, "topClip", camera.topClip);
    encodePositiveFinite(node, "yfov", camera.yfov);
    encodePositiveFinite(node, "aspect", camera.aspect);
    encodeFinite(node, "nearClip", camera.nearClip);
    encodeFinite(node, "farClip", camera.farClip);
    node["renderToTexture"] = camera.renderToTexture;
    node["transparentSort"] = camera.transparentSort;
    node["depthTest"] = camera.depthTest;
    node["useTarget"] = camera.useTarget;
    node["autoResize"] = camera.autoResize;
    node["framebufferWidth"] = camera.framebufferWidth;
    node["framebufferHeight"] = camera.framebufferHeight;
    node["framebufferFilter"] = textureFilterToString(camera.framebufferFilter);

    return node;
}

CameraComponent LegacyCodecs::decodeCameraComponent(const YAML::Node& node, const CameraComponent* oldCamera) {
    CameraComponent camera;

    // Use old values as defaults if provided
    if (oldCamera) {
        // the oracle frees the framebuffer the assignment replaces (the editor's decode leaked it;
        // no value read depends on it)
        delete camera.framebuffer;
        camera = *oldCamera;
    }

    if (node["type"]) camera.type = decodeEnum(node, "type", StreamSchema::cameraType());
    if (node["cullingMask"]) camera.cullingMask = decodeLayerMask(node["cullingMask"], camera.cullingMask);
    camera.target = decodeFiniteVector3(node["target"], camera.target);
    camera.up = decodeFiniteVector3(node["up"], camera.up);
    camera.leftClip = decodeFinite(node["leftClip"], camera.leftClip);
    camera.rightClip = decodeFinite(node["rightClip"], camera.rightClip);
    camera.bottomClip = decodeFinite(node["bottomClip"], camera.bottomClip);
    camera.topClip = decodeFinite(node["topClip"], camera.topClip);
    camera.yfov = decodePositiveFinite(node["yfov"], camera.yfov);
    camera.aspect = decodePositiveFinite(node["aspect"], camera.aspect);
    camera.nearClip = decodeFinite(node["nearClip"], camera.nearClip);
    camera.farClip = decodeFinite(node["farClip"], camera.farClip);
    if (node["renderToTexture"]) camera.renderToTexture = YamlNumbers::as<bool>(node["renderToTexture"]);
    if (node["transparentSort"]) camera.transparentSort = YamlNumbers::as<bool>(node["transparentSort"]);
    if (node["depthTest"]) camera.depthTest = YamlNumbers::as<bool>(node["depthTest"]);
    if (node["useTarget"]) camera.useTarget = YamlNumbers::as<bool>(node["useTarget"]);
    if (node["autoResize"]) camera.autoResize = YamlNumbers::as<bool>(node["autoResize"]);
    if (node["framebufferWidth"]) camera.framebufferWidth = YamlNumbers::as<unsigned int>(node["framebufferWidth"]);
    if (node["framebufferHeight"]) camera.framebufferHeight = YamlNumbers::as<unsigned int>(node["framebufferHeight"]);
    if (node["framebufferFilter"]) camera.framebufferFilter = decodeEnum(node, "framebufferFilter", StreamSchema::textureFilter());

    if (node["enabled"]) camera.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return camera;
}

YAML::Node LegacyCodecs::encodeAudioSourceComponent(const AudioSourceComponent& audio) {
    YAML::Node node;
    node["enabled"] = audio.enabled;

    node["state"] = audioSourceStateToString(audio.state);
    if (!audio.filename.empty()) encodeAssetRef(node, "filename", audio.filename);
    node["enableClocked"] = audio.enableClocked;
    encodeFinite(node, "volume", audio.volume);
    encodeFinite(node, "speed", audio.speed);
    encodeFinite(node, "pan", audio.pan);
    node["looping"] = audio.looping;
    encodeFinite(node, "loopingPoint", audio.loopingPoint);
    node["protectVoice"] = audio.protectVoice;
    node["inaudibleBehaviorMustTick"] = audio.inaudibleBehaviorMustTick;
    node["inaudibleBehaviorKill"] = audio.inaudibleBehaviorKill;
    encodeFinite(node, "minDistance", audio.minDistance);
    encodeFinite(node, "maxDistance", audio.maxDistance);
    node["attenuationModel"] = audioSourceAttenuationToString(audio.attenuationModel);
    encodeFinite(node, "attenuationRolloffFactor", audio.attenuationRolloffFactor);
    encodeFinite(node, "dopplerFactor", audio.dopplerFactor);
    // (upstream's audio-mixer codec since SYNC#4) written only when not their default: older files
    // stay byte-identical
    const AudioSourceComponent defaults;
    if (!audio.outputGroup.empty()) node["outputGroup"] = audio.outputGroup;
    if (!audio.outputGroupName.empty()) node["outputGroupName"] = audio.outputGroupName;
    if (audio.loadType != defaults.loadType) node["loadType"] = SoundPool::loadTypeName(audio.loadType);
    if (audio.priority != defaults.priority) node["priority"] = audio.priority;
    if (audio.maxVoices != defaults.maxVoices) node["maxVoices"] = audio.maxVoices;
    if (audio.ignoreListenerPause != defaults.ignoreListenerPause) node["ignoreListenerPause"] = audio.ignoreListenerPause;

    return node;
}

AudioSourceComponent LegacyCodecs::decodeAudioSourceComponent(const YAML::Node& node, const AudioSourceComponent* oldAudio) {
    AudioSourceComponent audio;
    std::string oldFilename;

    if (oldAudio) {
        audio = *oldAudio;
        oldFilename = oldAudio->filename;
    }

    if (node["state"]) audio.state = decodeEnum(node, "state", StreamSchema::audioSourceState());
    if (node["filename"]) audio.filename = decodeAssetRef(node, "filename");
    if (node["enableClocked"]) audio.enableClocked = YamlNumbers::as<bool>(node["enableClocked"]);
    if (node["volume"]) audio.volume = decodeFiniteDouble(node["volume"], audio.volume);
    if (node["speed"]) audio.speed = decodeFinite(node["speed"], audio.speed);
    if (node["pan"]) audio.pan = decodeFinite(node["pan"], audio.pan);
    if (node["looping"]) audio.looping = YamlNumbers::as<bool>(node["looping"]);
    if (node["loopingPoint"]) audio.loopingPoint = decodeFiniteDouble(node["loopingPoint"], audio.loopingPoint);
    if (node["protectVoice"]) audio.protectVoice = YamlNumbers::as<bool>(node["protectVoice"]);
    if (node["inaudibleBehaviorMustTick"]) audio.inaudibleBehaviorMustTick = YamlNumbers::as<bool>(node["inaudibleBehaviorMustTick"]);
    if (node["inaudibleBehaviorKill"]) audio.inaudibleBehaviorKill = YamlNumbers::as<bool>(node["inaudibleBehaviorKill"]);
    if (node["minDistance"]) audio.minDistance = decodeFinite(node["minDistance"], audio.minDistance);
    if (node["maxDistance"]) audio.maxDistance = decodeFinite(node["maxDistance"], audio.maxDistance);
    if (node["attenuationModel"]) audio.attenuationModel = decodeEnum(node, "attenuationModel", StreamSchema::audioSourceAttenuation());
    if (node["attenuationRolloffFactor"]) audio.attenuationRolloffFactor = decodeFinite(node["attenuationRolloffFactor"], audio.attenuationRolloffFactor);
    if (node["dopplerFactor"]) audio.dopplerFactor = decodeFinite(node["dopplerFactor"], audio.dopplerFactor);
    // (upstream's audio-mixer codec since SYNC#4) written only when not their default: a key that
    // is not there IS the default, also over an existing component
    const AudioSourceComponent defaults;
    audio.outputGroup = node["outputGroup"] ? node["outputGroup"].as<std::string>() : defaults.outputGroup;
    audio.outputGroupName = node["outputGroupName"] ? node["outputGroupName"].as<std::string>() : defaults.outputGroupName;
    audio.loadType = defaults.loadType;
    if (node["loadType"]) {
        AudioLoadType loadType;
        if (SoundPool::loadTypeFromName(node["loadType"].as<std::string>(), loadType)) audio.loadType = loadType;
    }
    audio.priority = node["priority"] ? std::clamp(YamlNumbers::as<int>(node["priority"]), 0, 256) : defaults.priority;
    audio.maxVoices = node["maxVoices"] ? std::clamp(YamlNumbers::as<int>(node["maxVoices"]), 1, 64) : defaults.maxVoices;
    audio.ignoreListenerPause = node["ignoreListenerPause"] ? YamlNumbers::as<bool>(node["ignoreListenerPause"]) : defaults.ignoreListenerPause;

    bool keepLoadedSample = oldAudio && oldFilename == audio.filename && oldAudio->sample && oldAudio->loaded;
    if (!keepLoadedSample && oldAudio && !oldFilename.empty()) {
        audio.sample.reset();
        SoundPool::remove(oldFilename);
    }
    audio.loaded = keepLoadedSample;
    audio.loadFailed = false;
    audio.length = keepLoadedSample ? oldAudio->length : 0;
    audio.handle = 0;
    audio.playingTime = 0;
    audio.lastPosition = Vector3(0, 0, 0);
    audio.startTrigger = false;
    audio.pauseTrigger = false;
    audio.stopTrigger = false;
    audio.needUpdate = true;

    if (node["enabled"]) audio.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return audio;
}

YAML::Node LegacyCodecs::encodeButtonComponent(const ButtonComponent& button) {
    YAML::Node node;
    node["label"] = button.label;
    node["textureNormal"] = encodeTexture(button.textureNormal);
    node["textureHovered"] = encodeTexture(button.textureHovered);
    node["texturePressed"] = encodeTexture(button.texturePressed);
    node["textureDisabled"] = encodeTexture(button.textureDisabled);
    node["colorNormal"] = encodeVector4(button.colorNormal);
    node["colorHovered"] = encodeVector4(button.colorHovered);
    node["colorPressed"] = encodeVector4(button.colorPressed);
    node["colorDisabled"] = encodeVector4(button.colorDisabled);
    node["disabled"] = button.disabled;
    return node;
}

ButtonComponent LegacyCodecs::decodeButtonComponent(const YAML::Node& node, const ButtonComponent* oldButton) {
    ButtonComponent button;

    if (oldButton) {
        button = *oldButton;
    }

    if (node["label"]) button.label = YamlNumbers::as<Entity>(node["label"]);
    if (node["textureNormal"]) button.textureNormal = decodeTexture(node["textureNormal"]);
    if (node["textureHovered"]) button.textureHovered = decodeTexture(node["textureHovered"]);
    if (node["texturePressed"]) button.texturePressed = decodeTexture(node["texturePressed"]);
    if (node["textureDisabled"]) button.textureDisabled = decodeTexture(node["textureDisabled"]);
    if (node["colorNormal"]) button.colorNormal = decodeVector4(node["colorNormal"]);
    if (node["colorHovered"]) button.colorHovered = decodeVector4(node["colorHovered"]);
    if (node["colorPressed"]) button.colorPressed = decodeVector4(node["colorPressed"]);
    if (node["colorDisabled"]) button.colorDisabled = decodeVector4(node["colorDisabled"]);
    if (node["disabled"]) button.disabled = YamlNumbers::as<bool>(node["disabled"]);

    return button;
}

YAML::Node LegacyCodecs::encodeScrollbarComponent(const ScrollbarComponent& scrollbar) {
    YAML::Node node;
    node["bar"] = scrollbar.bar;
    node["type"] = scrollbarTypeToString(scrollbar.type);
    node["barSize"] = scrollbar.barSize;
    node["step"] = scrollbar.step;
    node["barMarginLeft"] = scrollbar.barMarginLeft;
    node["barMarginRight"] = scrollbar.barMarginRight;
    node["barMarginTop"] = scrollbar.barMarginTop;
    node["barMarginBottom"] = scrollbar.barMarginBottom;
    return node;
}

ScrollbarComponent LegacyCodecs::decodeScrollbarComponent(const YAML::Node& node, const ScrollbarComponent* oldScrollbar) {
    ScrollbarComponent scrollbar;

    if (oldScrollbar) {
        scrollbar = *oldScrollbar;
    }

    if (node["bar"]) scrollbar.bar = YamlNumbers::as<Entity>(node["bar"]);
    if (node["type"]) scrollbar.type = decodeEnum(node, "type", StreamSchema::scrollbarType());
    if (node["barSize"]) scrollbar.barSize = YamlNumbers::as<float>(node["barSize"]);
    if (node["step"]) scrollbar.step = YamlNumbers::as<float>(node["step"]);
    if (node["barMarginLeft"]) scrollbar.barMarginLeft = YamlNumbers::as<int>(node["barMarginLeft"]);
    if (node["barMarginRight"]) scrollbar.barMarginRight = YamlNumbers::as<int>(node["barMarginRight"]);
    if (node["barMarginTop"]) scrollbar.barMarginTop = YamlNumbers::as<int>(node["barMarginTop"]);
    if (node["barMarginBottom"]) scrollbar.barMarginBottom = YamlNumbers::as<int>(node["barMarginBottom"]);

    scrollbar.needUpdateScrollbar = true;

    return scrollbar;
}

YAML::Node LegacyCodecs::encodeProgressbarComponent(const ProgressbarComponent& progressbar) {
    YAML::Node node;
    node["fill"] = progressbar.fill;
    node["type"] = progressbarTypeToString(progressbar.type);
    node["value"] = progressbar.value;
    node["fillMarginLeft"] = progressbar.fillMarginLeft;
    node["fillMarginRight"] = progressbar.fillMarginRight;
    node["fillMarginTop"] = progressbar.fillMarginTop;
    node["fillMarginBottom"] = progressbar.fillMarginBottom;
    return node;
}

ProgressbarComponent LegacyCodecs::decodeProgressbarComponent(const YAML::Node& node, const ProgressbarComponent* oldProgressbar) {
    ProgressbarComponent progressbar;

    if (oldProgressbar) {
        progressbar = *oldProgressbar;
    }

    if (node["fill"]) progressbar.fill = YamlNumbers::as<Entity>(node["fill"]);
    if (node["type"]) progressbar.type = decodeEnum(node, "type", StreamSchema::progressbarType());
    if (node["value"]) progressbar.value = YamlNumbers::as<float>(node["value"]);
    if (node["fillMarginLeft"]) progressbar.fillMarginLeft = YamlNumbers::as<int>(node["fillMarginLeft"]);
    if (node["fillMarginRight"]) progressbar.fillMarginRight = YamlNumbers::as<int>(node["fillMarginRight"]);
    if (node["fillMarginTop"]) progressbar.fillMarginTop = YamlNumbers::as<int>(node["fillMarginTop"]);
    if (node["fillMarginBottom"]) progressbar.fillMarginBottom = YamlNumbers::as<int>(node["fillMarginBottom"]);

    progressbar.needUpdateProgressbar = true;

    return progressbar;
}

YAML::Node LegacyCodecs::encodeInputFieldComponent(const InputFieldComponent& inputfield) {
    YAML::Node node;
    node["text"] = inputfield.text;
    node["selection"] = inputfield.selection;
    node["cursor"] = inputfield.cursor;
    node["cursorBlink"] = inputfield.cursorBlink;
    node["cursorWidth"] = inputfield.cursorWidth;
    node["cursorColor"] = encodeVector4(inputfield.cursorColor);
    node["selectionColor"] = encodeVector4(inputfield.selectionColor);
    node["placeholderColor"] = encodeVector4(inputfield.placeholderColor);
    node["placeholder"] = inputfield.placeholder;
    node["passwordChar"] = std::string(1, inputfield.passwordChar);
    node["cursorIndex"] = inputfield.cursorIndex;
    node["selectionAnchor"] = inputfield.selectionAnchor;
    node["disabled"] = inputfield.disabled;
    node["password"] = inputfield.password;
    return node;
}

InputFieldComponent LegacyCodecs::decodeInputFieldComponent(const YAML::Node& node, const InputFieldComponent* oldInputField) {
    InputFieldComponent inputfield;

    if (oldInputField) {
        inputfield = *oldInputField;
    }

    if (node["text"]) inputfield.text = YamlNumbers::as<Entity>(node["text"]);
    if (node["selection"]) inputfield.selection = YamlNumbers::as<Entity>(node["selection"]);
    if (node["cursor"]) inputfield.cursor = YamlNumbers::as<Entity>(node["cursor"]);
    if (node["cursorBlink"]) inputfield.cursorBlink = YamlNumbers::as<float>(node["cursorBlink"]);
    if (node["cursorWidth"]) inputfield.cursorWidth = YamlNumbers::as<float>(node["cursorWidth"]);
    if (node["cursorColor"]) inputfield.cursorColor = decodeVector4(node["cursorColor"]);
    if (node["selectionColor"]) inputfield.selectionColor = decodeVector4(node["selectionColor"]);
    if (node["placeholderColor"]) inputfield.placeholderColor = decodeVector4(node["placeholderColor"]);
    if (node["placeholder"]) inputfield.placeholder = node["placeholder"].as<std::string>();
    if (node["passwordChar"]) {
        std::string passwordChar = node["passwordChar"].as<std::string>();
        inputfield.passwordChar = passwordChar.empty() ? '*' : passwordChar[0];
    }
    if (node["cursorIndex"]) inputfield.cursorIndex = YamlNumbers::as<int>(node["cursorIndex"]);
    if (node["selectionAnchor"]) inputfield.selectionAnchor = YamlNumbers::as<int>(node["selectionAnchor"]);
    if (node["disabled"]) inputfield.disabled = YamlNumbers::as<bool>(node["disabled"]);
    if (node["password"]) inputfield.password = YamlNumbers::as<bool>(node["password"]);

    inputfield.needUpdateInputField = true;

    return inputfield;
}

YAML::Node LegacyCodecs::encodePanelComponent(const PanelComponent& panel) {
    YAML::Node node;
    node["headerimage"] = panel.headerimage;
    node["headercontainer"] = panel.headercontainer;
    node["headertext"] = panel.headertext;
    node["titleAnchorPreset"] = anchorPresetToString(panel.titleAnchorPreset);
    node["minWidth"] = panel.minWidth;
    node["minHeight"] = panel.minHeight;
    node["headerMarginLeft"] = panel.headerMarginLeft;
    node["headerMarginTop"] = panel.headerMarginTop;
    node["headerMarginRight"] = panel.headerMarginRight;
    node["headerMarginBottom"] = panel.headerMarginBottom;
    node["defaultHeaderMargin"] = panel.defaultHeaderMargin;
    node["resizeMargin"] = panel.resizeMargin;
    node["canMove"] = panel.canMove;
    node["canResize"] = panel.canResize;
    node["canBringToFront"] = panel.canBringToFront;
    return node;
}

PanelComponent LegacyCodecs::decodePanelComponent(const YAML::Node& node, const PanelComponent* oldPanel) {
    PanelComponent panel;

    if (oldPanel) {
        panel = *oldPanel;
    }

    if (node["headerimage"]) panel.headerimage = YamlNumbers::as<Entity>(node["headerimage"]);
    if (node["headercontainer"]) panel.headercontainer = YamlNumbers::as<Entity>(node["headercontainer"]);
    if (node["headertext"]) panel.headertext = YamlNumbers::as<Entity>(node["headertext"]);
    if (node["titleAnchorPreset"]) panel.titleAnchorPreset = decodeEnum(node, "titleAnchorPreset", StreamSchema::anchorPreset());
    if (node["minWidth"]) panel.minWidth = YamlNumbers::as<unsigned int>(node["minWidth"]);
    if (node["minHeight"]) panel.minHeight = YamlNumbers::as<unsigned int>(node["minHeight"]);
    if (node["headerMarginLeft"]) panel.headerMarginLeft = YamlNumbers::as<int>(node["headerMarginLeft"]);
    if (node["headerMarginTop"]) panel.headerMarginTop = YamlNumbers::as<int>(node["headerMarginTop"]);
    if (node["headerMarginRight"]) panel.headerMarginRight = YamlNumbers::as<int>(node["headerMarginRight"]);
    if (node["headerMarginBottom"]) panel.headerMarginBottom = YamlNumbers::as<int>(node["headerMarginBottom"]);
    if (node["defaultHeaderMargin"]) panel.defaultHeaderMargin = YamlNumbers::as<bool>(node["defaultHeaderMargin"]);
    if (node["resizeMargin"]) panel.resizeMargin = YamlNumbers::as<int>(node["resizeMargin"]);
    if (node["canMove"]) panel.canMove = YamlNumbers::as<bool>(node["canMove"]);
    if (node["canResize"]) panel.canResize = YamlNumbers::as<bool>(node["canResize"]);
    if (node["canBringToFront"]) panel.canBringToFront = YamlNumbers::as<bool>(node["canBringToFront"]);

    panel.needUpdatePanel = true;

    return panel;
}

YAML::Node LegacyCodecs::encodeRectTransformComponent(const RectTransformComponent& layout) {
    YAML::Node node;
    node["width"] = layout.width;
    node["height"] = layout.height;
    node["anchorPointLeft"] = layout.anchorPointLeft;
    node["anchorPointTop"] = layout.anchorPointTop;
    node["anchorPointRight"] = layout.anchorPointRight;
    node["anchorPointBottom"] = layout.anchorPointBottom;
    node["anchorOffsetLeft"] = layout.anchorOffsetLeft;
    node["anchorOffsetTop"] = layout.anchorOffsetTop;
    node["anchorOffsetRight"] = layout.anchorOffsetRight;
    node["anchorOffsetBottom"] = layout.anchorOffsetBottom;
    node["positionOffset"] = encodeVector2(layout.positionOffset);
    node["pivot"] = encodeVector2(layout.pivot);
    node["anchorPreset"] = anchorPresetToString(layout.anchorPreset);
    node["usingAnchors"] = layout.usingAnchors;
    node["panel"] = layout.panel;
    node["containerBoxIndex"] = layout.containerBoxIndex;
    node["scissor"] = encodeRect(layout.scissor);
    node["ignoreScissor"] = layout.ignoreScissor;
    node["ignoreEvents"] = layout.ignoreEvents;

    return node;
}

RectTransformComponent LegacyCodecs::decodeRectTransformComponent(const YAML::Node& node, const RectTransformComponent* oldLayout) {
    RectTransformComponent layout;

    // Use old values as defaults if provided
    if (oldLayout) {
        layout = *oldLayout;
    }

    if (node["width"]) layout.width = YamlNumbers::as<unsigned int>(node["width"]);
    if (node["height"]) layout.height = YamlNumbers::as<unsigned int>(node["height"]);
    if (node["anchorPointLeft"]) layout.anchorPointLeft = YamlNumbers::as<float>(node["anchorPointLeft"]);
    if (node["anchorPointTop"]) layout.anchorPointTop = YamlNumbers::as<float>(node["anchorPointTop"]);
    if (node["anchorPointRight"]) layout.anchorPointRight = YamlNumbers::as<float>(node["anchorPointRight"]);
    if (node["anchorPointBottom"]) layout.anchorPointBottom = YamlNumbers::as<float>(node["anchorPointBottom"]);
    if (node["anchorOffsetLeft"]) layout.anchorOffsetLeft = YamlNumbers::as<int>(node["anchorOffsetLeft"]);
    if (node["anchorOffsetTop"]) layout.anchorOffsetTop = YamlNumbers::as<int>(node["anchorOffsetTop"]);
    if (node["anchorOffsetRight"]) layout.anchorOffsetRight = YamlNumbers::as<int>(node["anchorOffsetRight"]);
    if (node["anchorOffsetBottom"]) layout.anchorOffsetBottom = YamlNumbers::as<int>(node["anchorOffsetBottom"]);
    if (node["positionOffset"]) layout.positionOffset = decodeVector2(node["positionOffset"]);
    if (node["pivot"]) layout.pivot = decodeVector2(node["pivot"]);
    if (node["anchorPreset"]) layout.anchorPreset = decodeEnum(node, "anchorPreset", StreamSchema::anchorPreset());
    if (node["usingAnchors"]) layout.usingAnchors = YamlNumbers::as<bool>(node["usingAnchors"]);
    if (node["panel"]) layout.panel = YamlNumbers::as<Entity>(node["panel"]);
    if (node["containerBoxIndex"]) layout.containerBoxIndex = YamlNumbers::as<int>(node["containerBoxIndex"]);
    if (node["scissor"]) layout.scissor = decodeRect(node["scissor"]);
    if (node["ignoreScissor"]) layout.ignoreScissor = YamlNumbers::as<bool>(node["ignoreScissor"]);
    if (node["ignoreEvents"]) layout.ignoreEvents = YamlNumbers::as<bool>(node["ignoreEvents"]);

    return layout;
}

YAML::Node LegacyCodecs::encodeLayoutGroupComponent(const LayoutGroupComponent& container) {
    YAML::Node node;
    node["type"] = layoutGroupTypeToString(container.type);
    node["useAllWrapSpace"] = container.useAllWrapSpace;
    node["wrapCellWidth"] = container.wrapCellWidth;
    node["wrapCellHeight"] = container.wrapCellHeight;
    return node;
}

LayoutGroupComponent LegacyCodecs::decodeLayoutGroupComponent(const YAML::Node& node, const LayoutGroupComponent* oldContainer) {
    LayoutGroupComponent container;

    if (oldContainer) {
        container = *oldContainer;
    }

    if (node["type"]) container.type = decodeEnum(node, "type", StreamSchema::layoutGroupType());
    if (node["useAllWrapSpace"]) container.useAllWrapSpace = YamlNumbers::as<bool>(node["useAllWrapSpace"]);
    if (node["fillAvailableSpace"]) container.useAllWrapSpace = YamlNumbers::as<bool>(node["fillAvailableSpace"]);
    if (node["wrapCellWidth"]) container.wrapCellWidth = YamlNumbers::as<unsigned int>(node["wrapCellWidth"]);
    if (node["wrapCellHeight"]) container.wrapCellHeight = YamlNumbers::as<unsigned int>(node["wrapCellHeight"]);

    return container;
}

YAML::Node LegacyCodecs::encodeTextComponent(const TextComponent& text) {
    YAML::Node node;

    // cut after the last font in use, but always written so a merging decode sees a cleared chain
    size_t numFonts = 0;
    for (size_t i = 0; i < text.font.size(); i++){
        if (!text.font[i].empty()) numFonts = i + 1;
    }
    node["font"] = YAML::Node(YAML::NodeType::Sequence);
    node["font"].SetStyle(YAML::EmitterStyle::Flow);
    bool hasGuids = false;
    for (size_t i = 0; i < numFonts; i++){
        node["font"].push_back(text.font[i].path);
        hasGuids |= !text.font[i].guid.isNull();
    }
    if (hasGuids){
        node["fontGuid"] = YAML::Node(YAML::NodeType::Sequence);
        node["fontGuid"].SetStyle(YAML::EmitterStyle::Flow);
        for (size_t i = 0; i < numFonts; i++){
            node["fontGuid"].push_back(text.font[i].guid.str());
        }
    }
    node["text"] = text.text;
    node["fontSize"] = text.fontSize;
    node["multiline"] = text.multiline;
    node["maxTextSize"] = text.maxTextSize;
    node["fixedWidth"] = text.fixedWidth;
    node["fixedHeight"] = text.fixedHeight;
    node["pivotBaseline"] = text.pivotBaseline;
    node["pivotCentered"] = text.pivotCentered;
    // (upstream's ui-scaling-text codec since SYNC#4) outline, shadow and visible characters only
    // when set: texts from before them stay byte-identical
    const TextComponent d;
    if (text.outlineWidth != d.outlineWidth) node["outlineWidth"] = text.outlineWidth;
    if (text.outlineColor != d.outlineColor) node["outlineColor"] = encodeVector4(text.outlineColor);
    if (text.shadowEnabled != d.shadowEnabled) node["shadowEnabled"] = text.shadowEnabled;
    if (text.shadowColor != d.shadowColor) node["shadowColor"] = encodeVector4(text.shadowColor);
    if (text.shadowOffset != d.shadowOffset) node["shadowOffset"] = encodeVector2(text.shadowOffset);
    if (text.shadowSoftness != d.shadowSoftness) node["shadowSoftness"] = text.shadowSoftness;
    if (text.shadowDilate != d.shadowDilate) node["shadowDilate"] = text.shadowDilate;
    if (text.visibleCharacters != d.visibleCharacters) node["visibleCharacters"] = text.visibleCharacters;

    return node;
}

TextComponent LegacyCodecs::decodeTextComponent(const YAML::Node& node, const TextComponent* oldText) {
    TextComponent text;

    if (oldText) {
        text = *oldText;
    }

    if (node["font"]){
        text.font = FontArray();
        if (node["font"].IsSequence()){
            const YAML::Node guids = node["fontGuid"];
            for (size_t i = 0; i < node["font"].size() && i < text.font.size(); i++){
                text.font[i] = node["font"][i].as<std::string>();
                if (guids && guids.IsSequence() && i < guids.size())
                    text.font[i].guid = Guid::parse(guids[i].as<std::string>());
            }
        }else{
            // before the array: a single font plus a ";" separated fontFallbacks
            text.font[0] = node["font"].as<std::string>();
        }
    }
    if (node["text"]) text.text = node["text"].as<std::string>();
    if (node["fontSize"]) text.fontSize = YamlNumbers::as<unsigned int>(node["fontSize"]);
    if (node["multiline"]) text.multiline = YamlNumbers::as<bool>(node["multiline"]);
    if (node["maxTextSize"]) text.maxTextSize = YamlNumbers::as<unsigned int>(node["maxTextSize"]);
    if (node["fixedWidth"]) text.fixedWidth = YamlNumbers::as<bool>(node["fixedWidth"]);
    if (node["fixedHeight"]) text.fixedHeight = YamlNumbers::as<bool>(node["fixedHeight"]);
    if (node["pivotBaseline"]) text.pivotBaseline = YamlNumbers::as<bool>(node["pivotBaseline"]);
    if (node["pivotCentered"]) text.pivotCentered = YamlNumbers::as<bool>(node["pivotCentered"]);
    // (upstream's ui-scaling-text codec since SYNC#4) absent = the default, also over an old text
    const TextComponent defaults;
    text.outlineWidth = node["outlineWidth"] ? std::max(decodeFinite(node["outlineWidth"], 0.0f), 0.0f) : defaults.outlineWidth;
    text.outlineColor = node["outlineColor"] ? decodeVector4(node["outlineColor"]) : defaults.outlineColor;
    text.shadowEnabled = node["shadowEnabled"] ? YamlNumbers::as<bool>(node["shadowEnabled"]) : defaults.shadowEnabled;
    text.shadowColor = node["shadowColor"] ? decodeVector4(node["shadowColor"]) : defaults.shadowColor;
    text.shadowOffset = node["shadowOffset"] ? decodeVector2(node["shadowOffset"]) : defaults.shadowOffset;
    text.shadowSoftness = node["shadowSoftness"] ? std::max(decodeFinite(node["shadowSoftness"], 0.0f), 0.0f) : defaults.shadowSoftness;
    text.shadowDilate = node["shadowDilate"] ? std::max(decodeFinite(node["shadowDilate"], 0.0f), 0.0f) : defaults.shadowDilate;
    text.visibleCharacters = node["visibleCharacters"] ? std::max(YamlNumbers::as<int>(node["visibleCharacters"]), -1) : defaults.visibleCharacters;

    return text;
}

YAML::Node LegacyCodecs::encodeImageComponent(const ImageComponent& image) {
    YAML::Node node;
    node["patchMarginLeft"] = image.patchMarginLeft;
    node["patchMarginRight"] = image.patchMarginRight;
    node["patchMarginTop"] = image.patchMarginTop;
    node["patchMarginBottom"] = image.patchMarginBottom;
    node["textureScaleFactor"] = image.textureScaleFactor;

    return node;
}

ImageComponent LegacyCodecs::decodeImageComponent(const YAML::Node& node, const ImageComponent* oldImage) {
    ImageComponent image;

    // Use old values as defaults if provided
    if (oldImage) {
        image = *oldImage;
    }

    if (node["patchMarginLeft"]) image.patchMarginLeft = YamlNumbers::as<int>(node["patchMarginLeft"]);
    if (node["patchMarginRight"]) image.patchMarginRight = YamlNumbers::as<int>(node["patchMarginRight"]);
    if (node["patchMarginTop"]) image.patchMarginTop = YamlNumbers::as<int>(node["patchMarginTop"]);
    if (node["patchMarginBottom"]) image.patchMarginBottom = YamlNumbers::as<int>(node["patchMarginBottom"]);
    if (node["textureScaleFactor"]) image.textureScaleFactor = YamlNumbers::as<float>(node["textureScaleFactor"]);

    return image;
}

YAML::Node LegacyCodecs::encodeTransform(const Transform& transform) {
    YAML::Node transformNode;

    transformNode["position"] = encodeVector3(transform.position);
    transformNode["rotation"] = encodeQuaternion(transform.rotation);
    transformNode["scale"] = encodeVector3(transform.scale);
    transformNode["worldPosition"] = encodeVector3(transform.worldPosition);
    transformNode["worldRotation"] = encodeQuaternion(transform.worldRotation);
    transformNode["worldScale"] = encodeVector3(transform.worldScale);
    transformNode["localMatrix"] = encodeMatrix4(transform.localMatrix);
    transformNode["modelMatrix"] = encodeMatrix4(transform.modelMatrix);
    transformNode["normalMatrix"] = encodeMatrix4(transform.normalMatrix);
    transformNode["visible"] = YamlNumbers::text(transform.visible);
    transformNode["billboardRotation"] = encodeQuaternion(transform.billboardRotation);
    transformNode["billboard"] = YamlNumbers::text(transform.billboard);
    transformNode["fakeBillboard"] = YamlNumbers::text(transform.fakeBillboard);
    transformNode["cylindricalBillboard"] = YamlNumbers::text(transform.cylindricalBillboard);

    return transformNode;
}

Transform LegacyCodecs::decodeTransform(const YAML::Node& node, const Transform* oldTransform) {
    Transform transform;

    // Use old values as defaults if provided
    if (oldTransform) {
        transform = *oldTransform;
    }

    if (node["position"]) transform.position = decodeVector3(node["position"]);
    if (node["rotation"]) transform.rotation = decodeQuaternion(node["rotation"]);
    if (node["scale"]) transform.scale = decodeVector3(node["scale"]);
    if (node["worldPosition"]) transform.worldPosition = decodeVector3(node["worldPosition"]);
    if (node["worldRotation"]) transform.worldRotation = decodeQuaternion(node["worldRotation"]);
    if (node["worldScale"]) transform.worldScale = decodeVector3(node["worldScale"]);
    if (node["localMatrix"]) transform.localMatrix = decodeMatrix4(node["localMatrix"]);
    if (node["modelMatrix"]) transform.modelMatrix = decodeMatrix4(node["modelMatrix"]);
    if (node["normalMatrix"]) transform.normalMatrix = decodeMatrix4(node["normalMatrix"]);
    if (node["visible"]) transform.visible = YamlNumbers::as<bool>(node["visible"]);
    if (node["billboardRotation"]) transform.billboardRotation = decodeQuaternion(node["billboardRotation"]);
    if (node["billboard"]) transform.billboard = YamlNumbers::as<bool>(node["billboard"]);
    if (node["fakeBillboard"]) transform.fakeBillboard = YamlNumbers::as<bool>(node["fakeBillboard"]);
    if (node["cylindricalBillboard"]) transform.cylindricalBillboard = YamlNumbers::as<bool>(node["cylindricalBillboard"]);

    return transform;
}

YAML::Node LegacyCodecs::encodeJoint2DComponent(const Joint2DComponent& joint) {
    YAML::Node node;
    node["enabled"] = joint.enabled;

    node["type"] = joint2DTypeToString(joint.type);
    node["bodyA"] = joint.bodyA;
    node["bodyB"] = joint.bodyB;
    node["anchorA"] = encodeVector2(joint.anchorA);
    node["anchorB"] = encodeVector2(joint.anchorB);
    node["axis"] = encodeVector2(joint.axis);
    node["target"] = encodeVector2(joint.target);
    node["autoAnchors"] = joint.autoAnchors;
    node["rope"] = joint.rope;

    return node;
}

Joint2DComponent LegacyCodecs::decodeJoint2DComponent(const YAML::Node& node, const Joint2DComponent* oldJoint) {
    Joint2DComponent joint;

    if (oldJoint) {
        joint = *oldJoint;
    }

    if (node["type"]) joint.type = decodeEnum(node, "type", StreamSchema::joint2DType());
    if (node["bodyA"]) joint.bodyA = YamlNumbers::as<Entity>(node["bodyA"]);
    if (node["bodyB"]) joint.bodyB = YamlNumbers::as<Entity>(node["bodyB"]);
    if (node["anchorA"]) joint.anchorA = decodeVector2(node["anchorA"]);
    if (node["anchorB"]) joint.anchorB = decodeVector2(node["anchorB"]);
    if (node["axis"]) joint.axis = decodeVector2(node["axis"]);
    if (node["target"]) joint.target = decodeVector2(node["target"]);
    if (node["autoAnchors"]) joint.autoAnchors = YamlNumbers::as<bool>(node["autoAnchors"]);
    if (node["rope"]) joint.rope = YamlNumbers::as<bool>(node["rope"]);

    if (oldJoint && b2Joint_IsValid(oldJoint->joint)) {
        joint.needUpdateJoint = true;
    }

    if (node["enabled"]) joint.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return joint;
}

YAML::Node LegacyCodecs::encodeJoint3DComponent(const Joint3DComponent& joint) {
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

    return node;
}

Joint3DComponent LegacyCodecs::decodeJoint3DComponent(const YAML::Node& node, const Joint3DComponent* oldJoint) {
    Joint3DComponent joint;

    if (oldJoint) {
        joint = *oldJoint;
    }

    if (node["type"]) joint.type = decodeEnum(node, "type", StreamSchema::joint3DType());
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

    if (oldJoint && oldJoint->joint) {
        joint.needUpdateJoint = true;
    }

    if (node["enabled"]) joint.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return joint;
}

YAML::Node LegacyCodecs::encodeActionComponent(const ActionComponent& action) {
    YAML::Node node;

    node["state"] = actionStateToString(action.state);
    node["speed"] = action.speed;
    node["target"] = action.target;
    node["ownedTarget"] = action.ownedTarget;

    return node;
}

ActionComponent LegacyCodecs::decodeActionComponent(const YAML::Node& node, const ActionComponent* oldAction) {
    ActionComponent action;

    if (oldAction) {
        action = *oldAction;
    }

    action.timecount = 0;
    action.startTrigger = false;
    action.stopTrigger = false;
    action.pauseTrigger = false;

    if (node["state"]) action.state = decodeEnum(node, "state", StreamSchema::actionState());
    if (node["speed"]) action.speed = YamlNumbers::as<float>(node["speed"]);
    if (node["target"]) action.target = YamlNumbers::as<Entity>(node["target"]);
    if (node["ownedTarget"]) action.ownedTarget = YamlNumbers::as<bool>(node["ownedTarget"]);

    return action;
}

YAML::Node LegacyCodecs::encodeTweenComponent(const TweenComponent& timed) {
    YAML::Node node;

    node["duration"] = timed.duration;
    node["loop"] = timed.loop;
    node["pingPong"] = timed.pingPong;
    node["function"] = encodeEase(timed.function);
    // (upstream's codec since SYNC#4) written when set: older files stay byte-identical
    if (timed.useUnscaledTime) node["useUnscaledTime"] = true;

    return node;
}

TweenComponent LegacyCodecs::decodeTweenComponent(const YAML::Node& node, const TweenComponent* oldTimed) {
    TweenComponent timed;

    if (oldTimed) {
        timed = *oldTimed;
    }

    timed.time = 0;
    timed.value = 0;

    if (node["duration"]) timed.duration = YamlNumbers::as<float>(node["duration"]);
    if (node["loop"]) timed.loop = YamlNumbers::as<bool>(node["loop"]);
    if (node["pingPong"]) timed.pingPong = YamlNumbers::as<bool>(node["pingPong"]);
    if (node["function"]) timed.function = decodeEase(node["function"], oldTimed ? &oldTimed->function : nullptr);
    // written only when set, so absent reads as false (the tables' OmitIfDefault rule; upstream's
    // decode kept the base's value)
    timed.useUnscaledTime = node["useUnscaledTime"] ? YamlNumbers::as<bool>(node["useUnscaledTime"]) : false;

    return timed;
}

YAML::Node LegacyCodecs::encodePositionTweenComponent(const PositionTweenComponent& posAction) {
    YAML::Node node;

    node["endPosition"] = encodeVector3(posAction.endPosition);
    node["startPosition"] = encodeVector3(posAction.startPosition);

    return node;
}

PositionTweenComponent LegacyCodecs::decodePositionTweenComponent(const YAML::Node& node, const PositionTweenComponent* oldPosAction) {
    PositionTweenComponent posAction;

    if (oldPosAction) {
        posAction = *oldPosAction;
    }

    if (node["endPosition"]) posAction.endPosition = decodeVector3(node["endPosition"]);
    if (node["startPosition"]) posAction.startPosition = decodeVector3(node["startPosition"]);

    return posAction;
}

YAML::Node LegacyCodecs::encodeRotationTweenComponent(const RotationTweenComponent& rotAction) {
    YAML::Node node;

    node["endRotation"] = encodeQuaternion(rotAction.endRotation);
    node["startRotation"] = encodeQuaternion(rotAction.startRotation);
    node["shortestPath"] = rotAction.shortestPath;
    node["spin"] = rotAction.spin;
    node["axis"] = encodeVector3(rotAction.axis);
    node["angle"] = rotAction.angle;

    return node;
}

RotationTweenComponent LegacyCodecs::decodeRotationTweenComponent(const YAML::Node& node, const RotationTweenComponent* oldRotAction) {
    RotationTweenComponent rotAction;

    if (oldRotAction) {
        rotAction = *oldRotAction;
    }

    if (node["endRotation"]) rotAction.endRotation = decodeQuaternion(node["endRotation"]);
    if (node["startRotation"]) rotAction.startRotation = decodeQuaternion(node["startRotation"]);
    if (node["shortestPath"]) rotAction.shortestPath = YamlNumbers::as<bool>(node["shortestPath"]);
    if (node["spin"]) rotAction.spin = YamlNumbers::as<bool>(node["spin"]);
    if (node["axis"]) rotAction.axis = decodeVector3(node["axis"]);
    if (node["angle"]) rotAction.angle = YamlNumbers::as<float>(node["angle"]);

    return rotAction;
}

YAML::Node LegacyCodecs::encodeScaleTweenComponent(const ScaleTweenComponent& scaleTween) {
    YAML::Node node;

    node["endScale"] = encodeVector3(scaleTween.endScale);
    node["startScale"] = encodeVector3(scaleTween.startScale);

    return node;
}

ScaleTweenComponent LegacyCodecs::decodeScaleTweenComponent(const YAML::Node& node, const ScaleTweenComponent* oldScaleTween) {
    ScaleTweenComponent scaleTween;

    if (oldScaleTween) {
        scaleTween = *oldScaleTween;
    }

    if (node["endScale"]) scaleTween.endScale = decodeVector3(node["endScale"]);
    if (node["startScale"]) scaleTween.startScale = decodeVector3(node["startScale"]);

    return scaleTween;
}

YAML::Node LegacyCodecs::encodeColorTweenComponent(const ColorTweenComponent& colorTween) {
    YAML::Node node;

    node["endColor"] = encodeVector3(colorTween.endColor);
    node["startColor"] = encodeVector3(colorTween.startColor);
    node["useSRGB"] = colorTween.useSRGB;

    return node;
}

ColorTweenComponent LegacyCodecs::decodeColorTweenComponent(const YAML::Node& node, const ColorTweenComponent* oldColorTween) {
    ColorTweenComponent colorTween;

    if (oldColorTween) {
        colorTween = *oldColorTween;
    }

    if (node["endColor"]) colorTween.endColor = decodeVector3(node["endColor"]);
    if (node["startColor"]) colorTween.startColor = decodeVector3(node["startColor"]);
    if (node["useSRGB"]) colorTween.useSRGB = YamlNumbers::as<bool>(node["useSRGB"]);

    return colorTween;
}

YAML::Node LegacyCodecs::encodeAlphaTweenComponent(const AlphaTweenComponent& alphaTween) {
    YAML::Node node;

    node["endAlpha"] = alphaTween.endAlpha;
    node["startAlpha"] = alphaTween.startAlpha;
    // (upstream's codec since SYNC#4) written when not AUTO: older files stay byte-identical
    if (alphaTween.alphaTarget != AlphaTweenTarget::AUTO) node["alphaTarget"] = editor::Stream::alphaTweenTargetToString(alphaTween.alphaTarget);

    return node;
}

AlphaTweenComponent LegacyCodecs::decodeAlphaTweenComponent(const YAML::Node& node, const AlphaTweenComponent* oldAlphaTween) {
    AlphaTweenComponent alphaTween;

    if (oldAlphaTween) {
        alphaTween = *oldAlphaTween;
    }

    if (node["endAlpha"]) alphaTween.endAlpha = YamlNumbers::as<float>(node["endAlpha"]);
    if (node["startAlpha"]) alphaTween.startAlpha = YamlNumbers::as<float>(node["startAlpha"]);
    // written only away from AUTO, so absent reads as AUTO (the tables' OmitIfDefault rule)
    alphaTween.alphaTarget = node["alphaTarget"] ? editor::Stream::stringToAlphaTweenTarget(node["alphaTarget"].as<std::string>())
                                                 : AlphaTweenTarget::AUTO;

    return alphaTween;
}

YAML::Node LegacyCodecs::encodeAnimationComponent(const AnimationComponent& animation) {
    YAML::Node node;

    node["loop"] = animation.loop;
    node["duration"] = animation.duration;
    node["ownedActions"] = animation.ownedActions;
    node["defaultFadeTime"] = animation.defaultFadeTime;

    YAML::Node actionsNode;
    for (const auto& frame : animation.actions) {
        YAML::Node frameNode;
        frameNode["track"] = frame.track;
        frameNode["startTime"] = frame.startTime;
        frameNode["duration"] = frame.duration;
        frameNode["action"] = frame.action;
        actionsNode.push_back(frameNode);
    }
    node["actions"] = actionsNode;

    return node;
}

AnimationComponent LegacyCodecs::decodeAnimationComponent(const YAML::Node& node, const AnimationComponent* oldAnimation) {
    AnimationComponent animation;

    if (oldAnimation) {
        animation = *oldAnimation;
    }

    if (node["loop"]) animation.loop = YamlNumbers::as<bool>(node["loop"]);
    if (node["duration"]) animation.duration = YamlNumbers::as<float>(node["duration"]);
    if (node["ownedActions"]) animation.ownedActions = YamlNumbers::as<bool>(node["ownedActions"]);
    if (node["defaultFadeTime"]) animation.defaultFadeTime = YamlNumbers::as<float>(node["defaultFadeTime"]);

    if (node["actions"]) {
        animation.actions.clear();
        for (const YAML::Node& frameNode : node["actions"]) {
            ActionFrame frame;
            if (frameNode["track"]) frame.track = YamlNumbers::as<uint32_t>(frameNode["track"]);
            if (frameNode["startTime"]) frame.startTime = YamlNumbers::as<float>(frameNode["startTime"]);
            if (frameNode["duration"]) frame.duration = YamlNumbers::as<float>(frameNode["duration"]);
            if (frameNode["action"]) frame.action = YamlNumbers::as<Entity>(frameNode["action"]);
            animation.actions.push_back(frame);
        }
    }

    return animation;
}

YAML::Node LegacyCodecs::encodeBoneComponent(const BoneComponent& bone) {
    YAML::Node node;

    node["model"] = static_cast<uint32_t>(bone.model);
    node["index"] = bone.index;
    node["bindPosition"] = encodeVector3(bone.bindPosition);
    node["bindRotation"] = encodeQuaternion(bone.bindRotation);
    node["bindScale"] = encodeVector3(bone.bindScale);
    node["offsetMatrix"] = encodeMatrix4(bone.offsetMatrix);

    return node;
}

BoneComponent LegacyCodecs::decodeBoneComponent(const YAML::Node& node, const BoneComponent* oldBone) {
    BoneComponent bone;

    if (oldBone) {
        bone = *oldBone;
    }

    if (node["model"]) bone.model = static_cast<Entity>(YamlNumbers::as<uint32_t>(node["model"]));
    if (node["index"]) bone.index = YamlNumbers::as<int>(node["index"]);
    if (node["bindPosition"]) bone.bindPosition = decodeVector3(node["bindPosition"]);
    if (node["bindRotation"]) bone.bindRotation = decodeQuaternion(node["bindRotation"]);
    if (node["bindScale"]) bone.bindScale = decodeVector3(node["bindScale"]);
    if (node["offsetMatrix"]) bone.offsetMatrix = decodeMatrix4(node["offsetMatrix"]);

    return bone;
}

YAML::Node LegacyCodecs::encodePostProcessing(const PostProcessingSettings& settings) {
    YAML::Node node(YAML::NodeType::Map);
    static const PostProcessingSettings defaults;
    using FieldType = PostProcessingSettings::FieldType;
    for (const PostProcessingSettings::Field& field : PostProcessingSettings::getFields()) {
        const void* value = settings.getFieldRef(field);
        const void* def = defaults.getFieldRef(field);
        switch (field.type) {
            case FieldType::Bool:
                if (*(const bool*)value != *(const bool*)def) node[field.name] = *(const bool*)value;
                break;
            case FieldType::Int:
                if (*(const int*)value != *(const int*)def) node[field.name] = *(const int*)value;
                break;
            case FieldType::Float:
                if (*(const float*)value != *(const float*)def) node[field.name] = *(const float*)value;
                break;
            case FieldType::Vector3:
                if (*(const Vector3*)value != *(const Vector3*)def) node[field.name] = encodeVector3(*(const Vector3*)value);
                break;
            case FieldType::String:
                if (*(const std::string*)value != *(const std::string*)def) node[field.name] = *(const std::string*)value;
                break;
            case FieldType::Asset:
                if (*(const AssetRef*)value != *(const AssetRef*)def) encodeAssetRef(node, field.name, *(const AssetRef*)value);
                break;
            case FieldType::Enum: {
                const int v = *(const int*)value;
                if (v != *(const int*)def) node[field.name] = std::string(field.enumNames[v]);
                break;
            }
        }
    }
    return node;
}

PostProcessingSettings LegacyCodecs::decodePostProcessing(const YAML::Node& node) {
    PostProcessingSettings settings;
    if (!node || !node.IsMap()) return settings;
    using FieldType = PostProcessingSettings::FieldType;
    for (const PostProcessingSettings::Field& field : PostProcessingSettings::getFields()) {
        const YAML::Node value = node[field.name];
        if (!value) continue;
        void* ref = field.ref(settings);
        switch (field.type) {
            case FieldType::Bool: *(bool*)ref = YamlNumbers::as<bool>(value); break;
            case FieldType::Int: *(int*)ref = YamlNumbers::as<int>(value); break;
            case FieldType::Float: *(float*)ref = decodeFinite(value, *(float*)ref); break;
            case FieldType::Vector3: *(Vector3*)ref = decodeVector3(value); break;
            case FieldType::String: *(std::string*)ref = value.as<std::string>(); break;
            case FieldType::Asset: *(AssetRef*)ref = decodeAssetRef(node, field.name); break;
            case FieldType::Enum: {
                const std::string name = value.as<std::string>();
                for (int i = 0; field.enumNames[i]; i++) {
                    if (name == field.enumNames[i]) *(int*)ref = i;
                }
                break;
            }
        }
    }
    settings.clamp();
    return settings;
}

// ── AnimationComponent ──

YAML::Node LegacyCodecs::encodeKeyframeTracksComponent(const KeyframeTracksComponent& tracks) {
    YAML::Node node;

    node["index"] = tracks.index;
    node["interpolation"] = tracks.interpolation;
    node["loop"] = tracks.loop;
    node["relative"] = tracks.relative;

    YAML::Node timesNode;
    for (float t : tracks.times) {
        timesNode.push_back(t);
    }
    node["times"] = timesNode;

    // Imported linear tracks keep this list empty, while explicit entries from
    // the Properties editor must survive a save even when they are linear.
    if (!tracks.easings.empty()) {
        YAML::Node easingsNode;
        for (EaseType e : tracks.easings) {
            easingsNode.push_back(easeTypeToString(e));
        }
        node["easings"] = easingsNode;
    }

    return node;
}

KeyframeTracksComponent LegacyCodecs::decodeKeyframeTracksComponent(const YAML::Node& node, const KeyframeTracksComponent* oldTracks) {
    KeyframeTracksComponent tracks;

    if (oldTracks) {
        tracks = *oldTracks;
    }

    // a decoded track takes its base again when it starts
    tracks.hasBase = false;

    if (node["index"]) tracks.index = YamlNumbers::as<int>(node["index"]);
    if (node["interpolation"]) tracks.interpolation = YamlNumbers::as<float>(node["interpolation"]);
    if (node["loop"]) tracks.loop = YamlNumbers::as<bool>(node["loop"]);
    if (node["relative"]) tracks.relative = YamlNumbers::as<bool>(node["relative"]);

    if (node["times"]) {
        tracks.times.clear();
        for (const YAML::Node& t : node["times"]) {
            tracks.times.push_back(YamlNumbers::as<float>(t));
        }
    }

    tracks.easings.clear();
    if (node["easings"]) {
        for (const YAML::Node& e : node["easings"]) {
            tracks.easings.push_back(decodeEnumValue(e, StreamSchema::easeType()));
        }
    }

    return tracks;
}

// Value/tangent list plumbing shared by the keyframe track components below
template<typename T, typename EncodeFn>
static YAML::Node encodeList(const std::vector<T>& values, EncodeFn encodeValue) {
    YAML::Node node;
    for (const auto& v : values) {
        node.push_back(encodeValue(v));
    }
    return node;
}

template<typename T, typename DecodeFn>
static void decodeListInto(const YAML::Node& node, std::vector<T>& values, DecodeFn decodeValue) {
    values.clear();
    for (const YAML::Node& v : node) {
        values.push_back(decodeValue(v));
    }
}

// Encodes values plus the optional Hermite tangents (GLTF CUBICSPLINE clips);
// decode clears tangents that are absent from the file
template<typename Tracks, typename EncodeFn>
static YAML::Node encodeTrackLists(const Tracks& tracks, EncodeFn encodeValue) {
    YAML::Node node;
    node["values"] = encodeList(tracks.values, encodeValue);
    if (!tracks.inTangents.empty()) {
        node["inTangents"] = encodeList(tracks.inTangents, encodeValue);
    }
    if (!tracks.outTangents.empty()) {
        node["outTangents"] = encodeList(tracks.outTangents, encodeValue);
    }
    return node;
}

template<typename Tracks, typename DecodeFn>
static void decodeTrackLists(const YAML::Node& node, Tracks& tracks, DecodeFn decodeValue) {
    if (node["values"]) {
        decodeListInto(node["values"], tracks.values, decodeValue);
    }
    tracks.inTangents.clear();
    if (node["inTangents"]) {
        decodeListInto(node["inTangents"], tracks.inTangents, decodeValue);
    }
    tracks.outTangents.clear();
    if (node["outTangents"]) {
        decodeListInto(node["outTangents"], tracks.outTangents, decodeValue);
    }
}

YAML::Node LegacyCodecs::encodeTranslateTracksComponent(const TranslateTracksComponent& tracks) {
    return encodeTrackLists(tracks, encodeVector3);
}

TranslateTracksComponent LegacyCodecs::decodeTranslateTracksComponent(const YAML::Node& node, const TranslateTracksComponent* oldTracks) {
    TranslateTracksComponent tracks;

    if (oldTracks) {
        tracks = *oldTracks;
    }

    decodeTrackLists(node, tracks, decodeVector3);

    return tracks;
}

YAML::Node LegacyCodecs::encodeRotateTracksComponent(const RotateTracksComponent& tracks) {
    return encodeTrackLists(tracks, encodeQuaternion);
}

RotateTracksComponent LegacyCodecs::decodeRotateTracksComponent(const YAML::Node& node, const RotateTracksComponent* oldTracks) {
    RotateTracksComponent tracks;

    if (oldTracks) {
        tracks = *oldTracks;
    }

    decodeTrackLists(node, tracks, decodeQuaternion);

    return tracks;
}

YAML::Node LegacyCodecs::encodeScaleTracksComponent(const ScaleTracksComponent& tracks) {
    return encodeTrackLists(tracks, encodeVector3);
}

ScaleTracksComponent LegacyCodecs::decodeScaleTracksComponent(const YAML::Node& node, const ScaleTracksComponent* oldTracks) {
    ScaleTracksComponent tracks;

    if (oldTracks) {
        tracks = *oldTracks;
    }

    decodeTrackLists(node, tracks, decodeVector3);

    return tracks;
}

YAML::Node LegacyCodecs::encodeFluidSimulationComponent(const FluidSimulationComponent& fluid) {
    YAML::Node node;
    node["enabled"] = fluid.enabled;

    // only what differs from the defaults (Pavel's); decode restores the defaults
    const FluidSimulationComponent defaults;
    if (fluid.output != defaults.output) node["output"] = fluidOutputToString(fluid.output);
    if (fluid.interactive != defaults.interactive) node["interactive"] = fluid.interactive;
    if (fluid.aspectRatio != defaults.aspectRatio) encodeFinite(node, "aspectRatio", fluid.aspectRatio);
    if (fluid.simResolution != defaults.simResolution) node["simResolution"] = fluid.simResolution;
    if (fluid.dyeResolution != defaults.dyeResolution) node["dyeResolution"] = fluid.dyeResolution;
    if (fluid.densityDissipation != defaults.densityDissipation) encodeFinite(node, "densityDissipation", fluid.densityDissipation);
    if (fluid.velocityDissipation != defaults.velocityDissipation) encodeFinite(node, "velocityDissipation", fluid.velocityDissipation);
    if (fluid.pressure != defaults.pressure) encodeFinite(node, "pressure", fluid.pressure);
    if (fluid.pressureIterations != defaults.pressureIterations) node["pressureIterations"] = fluid.pressureIterations;
    if (fluid.curl != defaults.curl) encodeFinite(node, "curl", fluid.curl);
    if (fluid.gradientScale != defaults.gradientScale) encodeFinite(node, "gradientScale", fluid.gradientScale);
    if (fluid.simulationRate != defaults.simulationRate) encodeFinite(node, "simulationRate", fluid.simulationRate);
    if (fluid.maxSubsteps != defaults.maxSubsteps) node["maxSubsteps"] = fluid.maxSubsteps;
    if (fluid.timeScale != defaults.timeScale) encodeFinite(node, "timeScale", fluid.timeScale);
    if (fluid.paused != defaults.paused) node["paused"] = fluid.paused;
    if (fluid.splatRadius != defaults.splatRadius) encodeFinite(node, "splatRadius", fluid.splatRadius);
    if (fluid.splatForce != defaults.splatForce) encodeFinite(node, "splatForce", fluid.splatForce);
    if (fluid.colorful != defaults.colorful) node["colorful"] = fluid.colorful;
    if (fluid.colorUpdateSpeed != defaults.colorUpdateSpeed) encodeFinite(node, "colorUpdateSpeed", fluid.colorUpdateSpeed);
    if (fluid.splatsOnStart != defaults.splatsOnStart) node["splatsOnStart"] = fluid.splatsOnStart;
    if (fluid.randomSplatCount != defaults.randomSplatCount) node["randomSplatCount"] = fluid.randomSplatCount;
    if (fluid.seed != defaults.seed) node["seed"] = fluid.seed;
    if (fluid.shading != defaults.shading) node["shading"] = fluid.shading;
    if (fluid.backColor != defaults.backColor) encodeFiniteVector3(node, "backColor", fluid.backColor);
    if (fluid.transparent != defaults.transparent) node["transparent"] = fluid.transparent;
    if (fluid.bloom != defaults.bloom) node["bloom"] = fluid.bloom;
    if (fluid.bloomIterations != defaults.bloomIterations) node["bloomIterations"] = fluid.bloomIterations;
    if (fluid.bloomResolution != defaults.bloomResolution) node["bloomResolution"] = fluid.bloomResolution;
    if (fluid.bloomIntensity != defaults.bloomIntensity) encodeFinite(node, "bloomIntensity", fluid.bloomIntensity);
    if (fluid.bloomThreshold != defaults.bloomThreshold) encodeFinite(node, "bloomThreshold", fluid.bloomThreshold);
    if (fluid.bloomSoftKnee != defaults.bloomSoftKnee) encodeFinite(node, "bloomSoftKnee", fluid.bloomSoftKnee);
    if (fluid.sunrays != defaults.sunrays) node["sunrays"] = fluid.sunrays;
    if (fluid.sunraysResolution != defaults.sunraysResolution) node["sunraysResolution"] = fluid.sunraysResolution;
    if (fluid.sunraysWeight != defaults.sunraysWeight) encodeFinite(node, "sunraysWeight", fluid.sunraysWeight);

    return node;
}

FluidSimulationComponent LegacyCodecs::decodeFluidSimulationComponent(const YAML::Node& node, const FluidSimulationComponent* oldFluid) {
    // the old one's runtime state (its simulator, which textures linked to "fluid|<entity>"
    // show, and what is queued for it) stays
    FluidSimulationComponent fluid;
    if (oldFluid) {
        fluid = *oldFluid;
    }

    if (node["enabled"]) fluid.enabled = YamlNumbers::as<bool>(node["enabled"]);

    // keys are omitted at their defaults, so a missing key is the default, not the old value
    // (paste and play-stop restore decode over an existing component); the clipboard fills a
    // key it has no default for with an empty scalar, which is the default too
    const FluidSimulationComponent defaults;
    auto given = [&node](const char* key) {
        const YAML::Node value = node[key];
        return value && !value.IsNull() && !(value.IsScalar() && value.Scalar().empty());
    };
    fluid.output = given("output") ? decodeEnum(node, "output", StreamSchema::fluidOutput()) : defaults.output;
    fluid.interactive = given("interactive") ? YamlNumbers::as<bool>(node["interactive"]) : defaults.interactive;
    fluid.aspectRatio = given("aspectRatio") ? decodeFinite(node["aspectRatio"], defaults.aspectRatio) : defaults.aspectRatio;
    fluid.simResolution = given("simResolution") ? YamlNumbers::as<unsigned int>(node["simResolution"]) : defaults.simResolution;
    fluid.dyeResolution = given("dyeResolution") ? YamlNumbers::as<unsigned int>(node["dyeResolution"]) : defaults.dyeResolution;
    fluid.densityDissipation = given("densityDissipation") ? decodeFinite(node["densityDissipation"], defaults.densityDissipation) : defaults.densityDissipation;
    fluid.velocityDissipation = given("velocityDissipation") ? decodeFinite(node["velocityDissipation"], defaults.velocityDissipation) : defaults.velocityDissipation;
    fluid.pressure = given("pressure") ? decodeFinite(node["pressure"], defaults.pressure) : defaults.pressure;
    fluid.pressureIterations = given("pressureIterations") ? YamlNumbers::as<unsigned int>(node["pressureIterations"]) : defaults.pressureIterations;
    fluid.curl = given("curl") ? decodeFinite(node["curl"], defaults.curl) : defaults.curl;
    fluid.gradientScale = given("gradientScale") ? decodeFinite(node["gradientScale"], defaults.gradientScale) : defaults.gradientScale;
    fluid.simulationRate = given("simulationRate") ? decodeFinite(node["simulationRate"], defaults.simulationRate) : defaults.simulationRate;
    fluid.maxSubsteps = given("maxSubsteps") ? YamlNumbers::as<unsigned int>(node["maxSubsteps"]) : defaults.maxSubsteps;
    fluid.timeScale = given("timeScale") ? decodeFinite(node["timeScale"], defaults.timeScale) : defaults.timeScale;
    fluid.paused = given("paused") ? YamlNumbers::as<bool>(node["paused"]) : defaults.paused;
    fluid.splatRadius = given("splatRadius") ? decodeFinite(node["splatRadius"], defaults.splatRadius) : defaults.splatRadius;
    fluid.splatForce = given("splatForce") ? decodeFinite(node["splatForce"], defaults.splatForce) : defaults.splatForce;
    fluid.colorful = given("colorful") ? YamlNumbers::as<bool>(node["colorful"]) : defaults.colorful;
    fluid.colorUpdateSpeed = given("colorUpdateSpeed") ? decodeFinite(node["colorUpdateSpeed"], defaults.colorUpdateSpeed) : defaults.colorUpdateSpeed;
    fluid.splatsOnStart = given("splatsOnStart") ? YamlNumbers::as<bool>(node["splatsOnStart"]) : defaults.splatsOnStart;
    fluid.randomSplatCount = given("randomSplatCount") ? YamlNumbers::as<unsigned int>(node["randomSplatCount"]) : defaults.randomSplatCount;
    fluid.seed = given("seed") ? YamlNumbers::as<uint32_t>(node["seed"]) : defaults.seed;
    fluid.shading = given("shading") ? YamlNumbers::as<bool>(node["shading"]) : defaults.shading;
    fluid.backColor = given("backColor") ? decodeFiniteVector3(node["backColor"], defaults.backColor) : defaults.backColor;
    fluid.transparent = given("transparent") ? YamlNumbers::as<bool>(node["transparent"]) : defaults.transparent;
    fluid.bloom = given("bloom") ? YamlNumbers::as<bool>(node["bloom"]) : defaults.bloom;
    fluid.bloomIterations = given("bloomIterations") ? YamlNumbers::as<unsigned int>(node["bloomIterations"]) : defaults.bloomIterations;
    fluid.bloomResolution = given("bloomResolution") ? YamlNumbers::as<unsigned int>(node["bloomResolution"]) : defaults.bloomResolution;
    fluid.bloomIntensity = given("bloomIntensity") ? decodeFinite(node["bloomIntensity"], defaults.bloomIntensity) : defaults.bloomIntensity;
    fluid.bloomThreshold = given("bloomThreshold") ? decodeFinite(node["bloomThreshold"], defaults.bloomThreshold) : defaults.bloomThreshold;
    fluid.bloomSoftKnee = given("bloomSoftKnee") ? decodeFinite(node["bloomSoftKnee"], defaults.bloomSoftKnee) : defaults.bloomSoftKnee;
    fluid.sunrays = given("sunrays") ? YamlNumbers::as<bool>(node["sunrays"]) : defaults.sunrays;
    fluid.sunraysResolution = given("sunraysResolution") ? YamlNumbers::as<unsigned int>(node["sunraysResolution"]) : defaults.sunraysResolution;
    fluid.sunraysWeight = given("sunraysWeight") ? decodeFinite(node["sunraysWeight"], defaults.sunraysWeight) : defaults.sunraysWeight;

    // what the start depends on: a new seed or start burst restarts it (sizes and the look
    // are read by FluidSimulator each frame)
    if (oldFluid && (oldFluid->seed != fluid.seed || oldFluid->splatsOnStart != fluid.splatsOnStart ||
                     oldFluid->randomSplatCount != fluid.randomSplatCount)) {
        fluid.needReset = true;
    }

    return fluid;
}

YAML::Node LegacyCodecs::encodeLatticeFollowerComponent(const LatticeFollowerComponent& follower) {
    YAML::Node node;
    node["enabled"] = YamlNumbers::text(follower.enabled);

    node["lattice"] = follower.lattice;
    node["rest"] = follower.rest;
    node["applyPosition"] = YamlNumbers::text(follower.applyPosition);
    node["applyRotation"] = YamlNumbers::text(follower.applyRotation);
    node["applyScale"] = YamlNumbers::text(follower.applyScale);
    encodeFinite(node, "weight", follower.weight);

    return node;
}

LatticeFollowerComponent LegacyCodecs::decodeLatticeFollowerComponent(const YAML::Node& node, const LatticeFollowerComponent* oldFollower) {
    LatticeFollowerComponent follower;

    if (oldFollower) {
        follower = *oldFollower;
    }

    if (node["enabled"]) follower.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["lattice"]) follower.lattice = YamlNumbers::as<Entity>(node["lattice"]);
    if (node["rest"]) follower.rest = YamlNumbers::as<Entity>(node["rest"]);
    if (node["applyPosition"]) follower.applyPosition = YamlNumbers::as<bool>(node["applyPosition"]);
    if (node["applyRotation"]) follower.applyRotation = YamlNumbers::as<bool>(node["applyRotation"]);
    if (node["applyScale"]) follower.applyScale = YamlNumbers::as<bool>(node["applyScale"]);
    if (node["weight"]) follower.weight = decodeFinite(node["weight"], follower.weight);

    return follower;
}
