// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lights, fog, clouds, mirrors, reflection probes and the sky (docs/specs/S08-reflection.md).
// Field order is each component's YAML key order.

#include "reflect/Components.h"

#include "Factory.h"
#include "Stream.h"
#include "component/FogComponent.h"
#include "component/Light2DComponent.h"
#include "component/LightComponent.h"
#include "component/LocalVolumetricFogComponent.h"
#include "component/MirrorComponent.h"
#include "component/ReflectionProbeComponent.h"
#include "component/ShadowCaster2DComponent.h"
#include "component/SkyboxComponent.h"
#include "component/VolumetricCloudsComponent.h"

namespace jasperengine::editor::reflect {

namespace {

    // ---------------------------------------------------------------- Light

    bool lightNotPoint(const void* light) { return static_cast<const LightComponent*>(light)->type != LightType::POINT; }
    bool lightNotDirectional(const void* light) { return static_cast<const LightComponent*>(light)->type != LightType::DIRECTIONAL; }
    bool lightSpot(const void* light) { return static_cast<const LightComponent*>(light)->type == LightType::SPOT; }
    bool lightSpotUnmasked(const void* light) {
        const LightComponent* l = static_cast<const LightComponent*>(light);
        return l->type == LightType::SPOT && l->spotMask.empty();
    }

    constexpr uint64_t kShadowCamera = UpdateFlags_LightShadowCamera;

    constexpr FieldDesc kLightFields[] = {
        JE_FIELD(LightComponent, enabled, .flags = CodegenIfChanged),
        JE_FIELD(LightComponent, type, .enumDesc = &enums::kLightType, .flags = NoDefault,
                 .updateFlags = UpdateFlags_LightShadowMap | UpdateFlags_LightShadowCamera | UpdateFlags_Scene_Mesh_Variant),
        JE_FIELD(LightComponent, direction, .updateFlags = UpdateFlags_Transform, .drawer = "direction", .visibleIf = &lightNotPoint),
        JE_FIELD(LightComponent, worldDirection, .flags = Derived | Hidden | NoCodegen | NotWritten),
        JE_FIELD(LightComponent, color, .flags = ColorLinear),
        JE_FIELD(LightComponent, range, .updateFlags = kShadowCamera, .unit = Unit::Meters),
        JE_FIELD(LightComponent, intensity),
        JE_FIELD(LightComponent, innerConeCos, .updateFlags = kShadowCamera, .label = "Inner Cone", .drawer = "halfCone",
                 .visibleIf = &lightSpotUnmasked),
        JE_FIELD(LightComponent, outerConeCos, .updateFlags = kShadowCamera, .label = "Outer Cone", .drawer = "halfCone", .visibleIf = &lightSpot),
        JE_FIELD(LightComponent, spotMask, .updateFlags = kShadowCamera, .label = "Mask",
                 .tooltip = "Optional image projected by the spotlight. Alpha controls intensity when transparency is present; otherwise "
                            "luminance is used. Empty uses the default circular cone.",
                 .visibleIf = &lightSpot),
        JE_FIELD(LightComponent, cullingMask, .ops = &ops::LayerMask,
                 .tooltip = "Layers this light lights. GameObjects on other layers get no light and cast no shadow from it."),
        JE_FIELD(LightComponent, shadows, .updateFlags = kShadowCamera | UpdateFlags_Scene_Mesh_Variant, .group = "Shadows"),
        JE_FIELD(LightComponent, automaticShadowCamera, .updateFlags = kShadowCamera, .label = "Auto Near Plane", .group = "Shadows"),
        JE_FIELD(LightComponent, shadowBias, .min = 0.0f, .label = "Bias", .group = "Shadows"),
        JE_FIELD(LightComponent, mapResolution, .updateFlags = UpdateFlags_LightShadowMap | UpdateFlags_Scene_Mesh_Variant, .label = "Resolution",
                 .group = "Shadows", .drawer = "po2Slider"),
        JE_FIELD(LightComponent, shadowCameraNearFar, .flags = Derived | Hidden | NoCodegen),
        JE_FIELD(LightComponent, numShadowCascades, .updateFlags = kShadowCamera | UpdateFlags_Scene_Mesh_Variant, .label = "Cascade Count",
                 .group = "Shadows"),
        // read by RenderSystem when it picks each mesh's lights
        JE_FIELD(LightComponent, shading, .enumDesc = &enums::kLightShading, .flags = OmitIfDefault | CodegenIfChanged,
                 .tooltip = "Where this light is evaluated on materials whose Lighting is Mixed: Per Pixel (specular, shadows, spot masks) "
                            "or Per Vertex (diffuse only, cheapest). Both leaves it to the material (per pixel under Mixed). Materials lit "
                            "Per Pixel or Per Vertex evaluate every light their own way."),
        // the shadow settings added with S10 (docs/rendering/shadows.md), written only away from their defaults
        JE_FIELD(LightComponent, softShadows, .flags = OmitIfDefault, .group = "Shadows"),
        JE_FIELD(LightComponent, shadowStrength, .flags = OmitIfDefault, .min = 0.0f, .max = 1.0f, .label = "Strength",
                 .tooltip = "How dark the shadows are: 0 casts none, 1 full shadow.", .group = "Shadows"),
        JE_FIELD(LightComponent, shadowNormalBias, .flags = OmitIfDefault, .min = 0.0f, .label = "Normal Bias", .group = "Shadows"),
        JE_FIELD(LightComponent, cascadeSplitMode, .enumDesc = &enums::kCascadeSplitMode, .flags = OmitIfDefault, .updateFlags = kShadowCamera,
                 .group = "Shadows"),
        JE_FIELD(LightComponent, cascadeSplits, .flags = OmitIfDefault, .updateFlags = kShadowCamera, .group = "Shadows"),
        JE_FIELD(LightComponent, shadowDistance, .flags = OmitIfDefault, .updateFlags = kShadowCamera, .unit = Unit::Meters, .min = 0.0f,
                 .group = "Shadows"),
        JE_FIELD(LightComponent, shadowAngularDiameter, .flags = OmitIfDefault, .unit = Unit::Degrees, .min = 0.0f, .label = "Angular Diameter",
                 .group = "Shadows"),
        JE_FIELD(LightComponent, shadowSourceRadius, .flags = OmitIfDefault, .unit = Unit::Meters, .min = 0.0f, .label = "Source Radius",
                 .group = "Shadows"),
        JE_FIELD(LightComponent, shadowUpdateMode, .enumDesc = &enums::kShadowUpdateMode, .flags = OmitIfDefault | CodegenIfChanged,
                 .label = "Update Mode", .group = "Shadows"),
        // the volumetric fog's share of the light
        JE_FIELD(LightComponent, volumetricIntensity, .flags = OmitIfDefault | CodegenIfChanged, .min = 0.0f,
                 .tooltip = "How much of this light the volumetric fog scatters (High atmosphere quality, up to 4 point or spot lights, no "
                            "shadows). 0: none.",
                 .visibleIf = &lightNotDirectional),
        // photometric units and colour temperature (util/LightPhotometry.h), resolved on the CPU
        JE_FIELD(LightComponent, intensityUnit, .enumDesc = &enums::kLightUnit, .flags = OmitIfDefault | CodegenIfChanged, .label = "Unit",
                 .tooltip = "What Intensity is in: Engine (unitless), Lux (a directional light), Candela or Lumen (a point or spot light), "
                            "converted through the scene's Physical Light Exposure."),
        JE_FIELD(LightComponent, useColorTemperature, .flags = OmitIfDefault | CodegenIfChanged, .label = "Use Temperature"),
        JE_FIELD(LightComponent, colorTemperature, .ops = &ops::Kelvin, .flags = OmitIfDefault | CodegenIfChanged, .unit = Unit::Kelvin,
                 .uiMin = 1000.0f, .uiMax = 20000.0f, .label = "Temperature"),
        JE_PROXY(LightComponent, "shadowCameraNear", shadowCameraNearFar.x, .flags = NoDefault, .updateFlags = kShadowCamera, .min = 0.0f,
                 .label = "Near Plane", .group = "Shadows"),
        JE_PROXY(LightComponent, "shadowCameraFar", shadowCameraNearFar.y, .flags = NoDefault, .updateFlags = kShadowCamera, .min = 0.0f,
                 .label = "Far Plane", .group = "Shadows"),
    };

    constexpr const char* kLightCodegenOrder[] = {
        "type", "direction", "shadows", "cullingMask", "intensity", "range", "color", "innerConeCos", "outerConeCos", "spotMask",
        "shadowBias", "mapResolution", "automaticShadowCamera", "shadowCameraNear", "shadowCameraFar", "numShadowCascades", "softShadows",
        "shadowStrength", "shadowNormalBias", "cascadeSplitMode", "cascadeSplits", "shadowDistance", "shadowAngularDiameter",
        "shadowSourceRadius", "shadowUpdateMode", "volumetricIntensity", "shading", "intensityUnit", "useColorTemperature", "colorTemperature",
        "enabled"
    };

    // ---------------------------------------------------------------- Light 2D

    constexpr FieldDesc kLight2DFields[] = {
        JE_FIELD(Light2DComponent, enabled, .flags = CodegenIfChanged),
        JE_FIELD(Light2DComponent, color, .flags = ColorLinear, .uiOrder = 4),
        JE_FIELD(Light2DComponent, intensity),
        JE_FIELD(Light2DComponent, range),
        JE_FIELD(Light2DComponent, falloff),
        JE_FIELD(Light2DComponent, height,
                 .tooltip = "Virtual Z height of the light above the 2D plane. Gives normal maps a direction to respond to; 0 disables "
                            "normal response (pure radial light)."),
        JE_FIELD(Light2DComponent, shadows, .updateFlags = UpdateFlags_Scene_Mesh_Variant, .label = "Enabled", .group = "Shadow settings"),
        JE_FIELD(Light2DComponent, shadowBias, .step = 0.001f, .format = "%.4f", .label = "Bias", .group = "Shadow settings"),
        JE_FIELD(Light2DComponent, shadowSoftness, .label = "Softness", .group = "Shadow settings"),
        JE_FIELD(Light2DComponent, mapResolution, .group = "Shadow settings", .drawer = "po2Slider"),
    };

    constexpr const char* kLight2DCodegenOrder[] = {
        "color", "intensity", "range", "falloff", "height", "shadows", "shadowBias", "shadowSoftness", "mapResolution", "enabled"
    };

    // ---------------------------------------------------------------- Shadow Caster 2D

    bool shadowCasterPolygon(const void* caster) {
        return static_cast<const ShadowCaster2DComponent*>(caster)->shape == ShadowCaster2DShape::POLYGON;
    }

    constexpr FieldDesc kShadowCaster2DFields[] = {
        JE_FIELD(ShadowCaster2DComponent, shape, .enumDesc = &enums::kShadowCaster2DShape,
                 .tooltip = "Auto Quad derives the outline from this GameObject's mesh bounds; Polygon uses the point list below (local space)."),
        JE_FIELD(ShadowCaster2DComponent, closed, .visibleIf = &shadowCasterPolygon),
        JE_FIELD(ShadowCaster2DComponent, enabled),
        JE_FIELD(ShadowCaster2DComponent, points, .ops = &ops::PushBackList<Vector2>, .flags = NoDefault | ListWhole, .visibleIf = &shadowCasterPolygon),
    };

    // ---------------------------------------------------------------- Fog

    constexpr FieldDesc kFogFields[] = {
        JE_FIELD(FogComponent, type, .enumDesc = &enums::kFogType,
                 .tooltip = "Linear, Exponential, Exponential Squared: fog by distance. Height: physically based fog, dense up to the Base "
                            "Height and thinning above it, lit by the sun, with an optional volumetric part."),
        JE_FIELD(FogComponent, color, .flags = ColorLinear),
        JE_FIELD(FogComponent, density, .step = 0.001f, .format = "%.4f"),
        JE_FIELD(FogComponent, linearStart),
        JE_FIELD(FogComponent, linearEnd),
        // the height fog's fields, written only away from their defaults: a distance fog's file is as it was
        JE_FIELD(FogComponent, colorMode, .enumDesc = &enums::kFogColorMode, .flags = OmitIfDefault | CodegenIfChanged,
                 .tooltip = "Constant Color: the fog takes its Color. Sky Color: the colour of the sky at the horizon (a cube map Skybox, or a "
                            "sky shader with a horizonColor property).",
                 .group = "Height Fog"),
        JE_FIELD(FogComponent, attenuationDistance, .flags = OmitIfDefault | CodegenIfChanged, .unit = Unit::Meters, .min = 0.0f,
                 .tooltip = "The distance light travels in the fog below the Base Height before 63% of it is absorbed. Smaller: denser fog.",
                 .group = "Height Fog"),
        JE_FIELD(FogComponent, baseHeight, .flags = OmitIfDefault | CodegenIfChanged, .unit = Unit::Meters,
                 .tooltip = "The fog is uniform below this height and thins above it.", .group = "Height Fog"),
        JE_FIELD(FogComponent, maximumHeight, .flags = OmitIfDefault | CodegenIfChanged, .unit = Unit::Meters,
                 .tooltip = "The height where the fog has thinned to a thousandth of its density.", .group = "Height Fog"),
        JE_FIELD(FogComponent, startDistance, .flags = OmitIfDefault | CodegenIfChanged, .unit = Unit::Meters, .min = 0.0f,
                 .tooltip = "No fog nearer the camera than this.", .group = "Height Fog"),
        JE_FIELD(FogComponent, maxFogDistance, .flags = OmitIfDefault | CodegenIfChanged, .unit = Unit::Meters, .min = 0.0f,
                 .tooltip = "No more fog past this distance from the camera.", .group = "Height Fog"),
        JE_FIELD(FogComponent, maxOpacity, .flags = OmitIfDefault | CodegenIfChanged, .min = 0.0f, .max = 1.0f,
                 .tooltip = "The most the fog hides of what is behind it.", .group = "Height Fog"),
        JE_FIELD(FogComponent, directionalInscattering, .flags = OmitIfDefault | CodegenIfChanged, .min = 0.0f, .label = "Intensity",
                 .tooltip = "How much of the main directional light the fog scatters towards the camera.", .group = "Directional Inscattering"),
        JE_FIELD(FogComponent, anisotropy, .flags = OmitIfDefault | CodegenIfChanged,
                 .tooltip = "-1 scatters the light back towards the sun, 0 evenly, 1 forward (a glow around the sun).",
                 .group = "Directional Inscattering"),
        JE_FIELD(FogComponent, skyFill, .flags = OmitIfDefault | CodegenIfChanged,
                 .tooltip = "The sky takes the fog's colour near the horizon, so the fogged ground meets it without a seam.", .group = "Sky"),
        JE_FIELD(FogComponent, skyFillAngle, .flags = OmitIfDefault | CodegenIfChanged, .unit = Unit::Degrees, .min = 0.0f,
                 .tooltip = "How far above the horizon the sky fill reaches.", .group = "Sky"),
        JE_FIELD(FogComponent, volumetric, .flags = OmitIfDefault | CodegenIfChanged, .label = "Enabled",
                 .tooltip = "Raymarches the fog near the camera, with the sun's shadows (light shafts), Local Volumetric Fog volumes and a "
                            "density noise. The main camera only; off at Atmosphere Quality Off.",
                 .group = "Volumetric Fog"),
        JE_FIELD(FogComponent, volumetricDistance, .flags = OmitIfDefault | CodegenIfChanged, .unit = Unit::Meters, .min = 0.0f, .label = "Distance",
                 .tooltip = "How far from the camera the fog is raymarched; the analytic fog continues beyond.", .group = "Volumetric Fog"),
        JE_FIELD(FogComponent, lightShafts, .flags = OmitIfDefault | CodegenIfChanged,
                 .tooltip = "The main directional light's shadows darken the fog behind objects (light shafts). Needs its shadows on.",
                 .group = "Volumetric Fog"),
        JE_FIELD(FogComponent, noiseIntensity, .flags = OmitIfDefault | CodegenIfChanged, .min = 0.0f, .max = 1.0f,
                 .tooltip = "How much a 3D noise varies the fog's density, fading out towards the Distance.", .group = "Volumetric Fog"),
        JE_FIELD(FogComponent, noiseScale, .flags = OmitIfDefault | CodegenIfChanged, .unit = Unit::Meters, .min = 0.0f,
                 .tooltip = "The size of the noise's features.", .group = "Volumetric Fog"),
        JE_FIELD(FogComponent, wind, .flags = OmitIfDefault | CodegenIfChanged, .unit = Unit::MetersPerSecond,
                 .tooltip = "The speed the noise drifts at.", .group = "Volumetric Fog"),
        JE_FIELD(FogComponent, qualityOverride, .enumDesc = &enums::kAtmosphereQualityOverride, .flags = OmitIfDefault | CodegenIfChanged,
                 .tooltip = "Caps this fog's tier below the project's Atmosphere Quality (never raises it).", .group = "Volumetric Fog"),
    };

    // ---------------------------------------------------------------- Local Volumetric Fog

    constexpr FieldDesc kLocalVolumetricFogFields[] = {
        JE_FIELD(LocalVolumetricFogComponent, shape, .enumDesc = &enums::kFogVolumeShape),
        JE_FIELD(LocalVolumetricFogComponent, size, .unit = Unit::Meters),
        JE_FIELD(LocalVolumetricFogComponent, attenuationDistance, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(LocalVolumetricFogComponent, albedo, .flags = ColorLinear),
        JE_FIELD(LocalVolumetricFogComponent, blendDistance, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(LocalVolumetricFogComponent, falloff, .enumDesc = &enums::kFogVolumeFalloff),
        JE_FIELD(LocalVolumetricFogComponent, noiseIntensity, .min = 0.0f, .max = 1.0f),
        JE_FIELD(LocalVolumetricFogComponent, noiseScale, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(LocalVolumetricFogComponent, scrollSpeed, .unit = Unit::MetersPerSecond),
    };

    // ---------------------------------------------------------------- Volumetric Clouds

    constexpr FieldDesc kVolumetricCloudsFields[] = {
        JE_FIELD(VolumetricCloudsComponent, preset, .enumDesc = &enums::kCloudsPreset),
        JE_FIELD(VolumetricCloudsComponent, rendering, .enumDesc = &enums::kCloudsRendering),
        JE_FIELD(VolumetricCloudsComponent, coverage, .min = 0.0f, .max = 1.0f),
        JE_FIELD(VolumetricCloudsComponent, density, .min = 0.0f, .max = 1.0f),
        JE_FIELD(VolumetricCloudsComponent, shapeScale, .min = 0.0f),
        JE_FIELD(VolumetricCloudsComponent, shapeFactor, .min = 0.0f, .max = 1.0f),
        JE_FIELD(VolumetricCloudsComponent, erosionFactor, .min = 0.0f, .max = 1.0f),
        JE_FIELD(VolumetricCloudsComponent, erosionScale, .min = 0.0f),
        JE_FIELD(VolumetricCloudsComponent, bottomAltitude, .unit = Unit::Meters),
        JE_FIELD(VolumetricCloudsComponent, altitudeRange, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(VolumetricCloudsComponent, earthCurvature, .min = 0.0f, .max = 1.0f),
        JE_FIELD(VolumetricCloudsComponent, windDirection, .unit = Unit::Degrees),
        JE_FIELD(VolumetricCloudsComponent, windSpeed, .unit = Unit::MetersPerSecond),
        JE_FIELD(VolumetricCloudsComponent, weatherMap),
        JE_FIELD(VolumetricCloudsComponent, weatherMapScale, .min = 0.0f),
        JE_FIELD(VolumetricCloudsComponent, sunLightDimmer, .min = 0.0f),
        JE_FIELD(VolumetricCloudsComponent, ambientLightDimmer, .min = 0.0f),
        JE_FIELD(VolumetricCloudsComponent, maxDistance, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(VolumetricCloudsComponent, qualityOverride, .enumDesc = &enums::kAtmosphereQualityOverride),
    };

    // ---------------------------------------------------------------- Mirror

    constexpr FieldDesc kMirrorFields[] = {
        JE_FIELD(MirrorComponent, normal,
                 .tooltip = "Reflecting surface normal in local space (the mirror plane is this normal at the GameObject's position). Flip "
                            "its sign if the reflection clips the wrong side. The reflection camera is created automatically.",
                 .drawer = "direction"),
    };

    // ---------------------------------------------------------------- Reflection Probe

    constexpr uint64_t kProbeSampling = UpdateFlags_Reflection_Probe_Sampling;
    constexpr uint64_t kProbeCapture = UpdateFlags_Reflection_Probe_Capture;

    constexpr FieldDesc kReflectionProbeFields[] = {
        JE_FIELD(ReflectionProbeComponent, mode, .enumDesc = &enums::kReflectionProbeMode, .updateFlags = kProbeCapture),
        JE_FIELD(ReflectionProbeComponent, updateMode, .enumDesc = &enums::kReflectionProbeUpdateMode, .updateFlags = kProbeSampling),
        JE_FIELD(ReflectionProbeComponent, boxOffset, .updateFlags = kProbeSampling, .unit = Unit::Meters),
        JE_FIELD(ReflectionProbeComponent, boxSize, .updateFlags = kProbeSampling, .unit = Unit::Meters),
        JE_FIELD(ReflectionProbeComponent, blendDistance, .updateFlags = kProbeSampling, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(ReflectionProbeComponent, intensity, .updateFlags = kProbeSampling, .min = 0.0f),
        JE_FIELD(ReflectionProbeComponent, priority, .updateFlags = kProbeSampling),
        JE_FIELD(ReflectionProbeComponent, resolution, .updateFlags = kProbeCapture),
        JE_FIELD(ReflectionProbeComponent, nearClip, .updateFlags = kProbeCapture, .unit = Unit::Meters),
        JE_FIELD(ReflectionProbeComponent, farClip, .updateFlags = kProbeCapture, .unit = Unit::Meters),
        JE_FIELD(ReflectionProbeComponent, updateInterval, .updateFlags = kProbeSampling, .unit = Unit::Seconds, .min = 0.0f),
        JE_FIELD(ReflectionProbeComponent, includeSky, .updateFlags = kProbeCapture),
        JE_FIELD(ReflectionProbeComponent, cullingMask, .ops = &ops::LayerMask, .updateFlags = kProbeCapture),
        JE_FIELD(ReflectionProbeComponent, texture, .updateFlags = kProbeCapture, .asset = AssetKind::Cubemap),
    };

    void reflectionProbePostDecode(void* comp, const DecodeContext&) {
        static_cast<ReflectionProbeComponent*>(comp)->needUpdate = true;
    }

    // ---------------------------------------------------------------- Skybox (its own codec: the default sky)

    constexpr FieldDesc kSkyboxFields[] = {
        JE_FIELD(SkyboxComponent, texture, .updateFlags = UpdateFlags_Sky_Texture, .asset = AssetKind::Cubemap),
        JE_FIELD(SkyboxComponent, color),
        JE_FIELD(SkyboxComponent, rotation, .updateFlags = UpdateFlags_Sky, .unit = Unit::Degrees),
        JE_FIELD(SkyboxComponent, visible),
        JE_FIELD(SkyboxComponent, customShader, .updateFlags = UpdateFlags_Shader_Reload),
        JE_FIELD(SkyboxComponent, shaderUniforms, .ops = &ops::ShaderUniformValues, .updateFlags = UpdateFlags_Shader_Uniforms),
        JE_FIELD(SkyboxComponent, shaderTextures, .ops = &ops::ShaderTextures, .updateFlags = UpdateFlags_Shader_Reload),
        JE_FIELD(SkyboxComponent, shaderFeatures, .ops = &ops::ShaderFeatures, .updateFlags = UpdateFlags_Shader_Reload),
        JE_FIELD(SkyboxComponent, shaderGlobalBindings, .ops = &ops::ShaderGlobalBindings, .flags = Hidden, .updateFlags = UpdateFlags_Shader_Uniforms),
    };

    const ComponentCodec kSkyboxCodec = JE_STREAM_CODEC(SkyboxComponent, encodeSkyboxComponent, decodeSkyboxComponent);

    std::string skyboxCodegen(EntityRegistry* registry, Entity entity, const CodegenContext& ctx) {
        return Factory::createSkyboxComponent(ctx.indentSpaces, registry, entity, ctx.projectPath, ctx.sceneName, ctx.entityName,
                                              ctx.assignExisting, ctx.entityVarNames);
    }

}

const ComponentDesc kLightDesc{
    .type = ComponentType::LightComponent, .cppName = "LightComponent", .key = "light", .displayName = "Light", .icon = ICON_FA_LIGHTBULB,
    .body = JE_STRUCT_DESC(LightComponent, "LightComponent", kLightFields),
    .hasEnabled = true, .structural = true, JE_COMPONENT_ACCESS(LightComponent),
    .structuralUpdateFlags = UpdateFlags_Scene_Mesh_Variant,
    .codegenVar = "light", .codegenOrder = kLightCodegenOrder,
};

const ComponentDesc kLight2DDesc{
    .type = ComponentType::Light2DComponent, .cppName = "Light2DComponent", .key = "light2d", .displayName = "Light 2D", .icon = ICON_FA_LIGHTBULB,
    .body = JE_STRUCT_DESC(Light2DComponent, "Light2DComponent", kLight2DFields),
    .hasEnabled = true, .structural = true, JE_COMPONENT_ACCESS(Light2DComponent),
    // adding/removing a 2D light toggles the USE_LIGHT2D shader variant
    .structuralUpdateFlags = UpdateFlags_Scene_Mesh_Variant,
    .codegenVar = "light2d", .codegenOrder = kLight2DCodegenOrder,
};

const ComponentDesc kShadowCaster2DDesc{
    .type = ComponentType::ShadowCaster2DComponent, .cppName = "ShadowCaster2DComponent", .key = "shadowcaster2d",
    .displayName = "Shadow Caster 2D", .icon = ICON_FA_CIRCLE_HALF_STROKE,
    .body = JE_STRUCT_DESC(ShadowCaster2DComponent, "ShadowCaster2DComponent", kShadowCaster2DFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(ShadowCaster2DComponent),
    .codegenVar = "shadowcaster2d",
};

const ComponentDesc kFogDesc{
    .type = ComponentType::FogComponent, .cppName = "FogComponent", .key = "fog", .displayName = "Fog", .icon = ICON_FA_SMOG,
    .body = JE_STRUCT_DESC(FogComponent, "FogComponent", kFogFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(FogComponent),
    .structuralUpdateFlags = UpdateFlags_Scene_Mesh_Variant,
    .codegenVar = "fog",
};

const ComponentDesc kLocalVolumetricFogDesc{
    .type = ComponentType::LocalVolumetricFogComponent, .cppName = "LocalVolumetricFogComponent", .key = "localvolumetricfog",
    .displayName = "Local Volumetric Fog", .icon = ICON_FA_SMOG,
    .body = JE_STRUCT_DESC(LocalVolumetricFogComponent, "LocalVolumetricFogComponent", kLocalVolumetricFogFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(LocalVolumetricFogComponent),
    .codegenVar = "volume",
};

const ComponentDesc kVolumetricCloudsDesc{
    .type = ComponentType::VolumetricCloudsComponent, .cppName = "VolumetricCloudsComponent", .key = "volumetricclouds",
    .displayName = "Volumetric Clouds", .icon = ICON_FA_CLOUD,
    .body = JE_STRUCT_DESC(VolumetricCloudsComponent, "VolumetricCloudsComponent", kVolumetricCloudsFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(VolumetricCloudsComponent),
    .codegenVar = "clouds",
};

const ComponentDesc kMirrorDesc{
    .type = ComponentType::MirrorComponent, .cppName = "MirrorComponent", .key = "mirror", .displayName = "Mirror", .icon = ICON_FA_CLONE,
    .body = JE_STRUCT_DESC(MirrorComponent, "MirrorComponent", kMirrorFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(MirrorComponent),
    // adding/removing a mirror toggles the inverted-culling pipeline baked into meshes
    .structuralUpdateFlags = UpdateFlags_Scene_Mesh_Reload,
    .codegenVar = "mirror",
};

const ComponentDesc kReflectionProbeDesc{
    .type = ComponentType::ReflectionProbeComponent, .cppName = "ReflectionProbeComponent", .key = "reflectionprobe",
    .displayName = "Reflection Probe", .icon = ICON_FA_CIRCLE_DOT,
    .body = JE_STRUCT_DESC(ReflectionProbeComponent, "ReflectionProbeComponent", kReflectionProbeFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(ReflectionProbeComponent),
    .postDecode = &reflectionProbePostDecode,
    .structuralUpdateFlags = UpdateFlags_Scene_Mesh_Variant,
    .codegenVar = "probe",
};

// the keys its codec reads
constexpr const char* kSkyboxExtraKeys[] = {"blendMode", "color", "customShader", "depthWrite", "rotation", "shaderFeatures", "shaderGlobalBindings", "shaderTextures",
    "shaderUniforms", "texture", "useDefaultTexture", "visible"};

const ComponentDesc kSkyboxDesc{
    .type = ComponentType::SkyboxComponent, .cppName = "SkyboxComponent", .key = "skybox", .displayName = "Skybox", .icon = ICON_FA_CLOUD_SUN,
    .body = JE_STRUCT_DESC(SkyboxComponent, "SkyboxComponent", kSkyboxFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(SkyboxComponent),
    .codec = &kSkyboxCodec,
    .codegen = &skyboxCodegen,
    .extraKeys = kSkyboxExtraKeys,
};

}
