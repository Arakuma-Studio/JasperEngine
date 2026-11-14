// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AtmosphereDiagnostics.h"

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/FogComponent.h"
#include "component/LightComponent.h"
#include "component/LocalVolumetricFogComponent.h"
#include "component/Transform.h"
#include "render/AtmosphereMath.h"
#include "render/AtmosphereQuality.h"
#include "subsystem/AtmosphereRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;

namespace {

    // what every check reads, and the list it appends to
    struct AtmosphereCheckContext{
        const AtmosphereDiagnosticsInput& input;
        Scene& scene;
        // the view's last frame (null: none drawn yet)
        const AtmosphereInfo* info;
        // the Fog the scene uses (its first), and its GameObject
        FogComponent* fog;
        Entity fogEntity;
        // the active Local Volumetric Fog volumes
        std::vector<Entity> volumes;
        // the main camera
        Entity camera;
        const AtmosphereOpacity& opacity;
        std::vector<AtmosphereDiagnostic>& out;
    };

    void addDiagnostic(AtmosphereCheckContext& context, const char* code, AtmosphereSeverity severity, Entity entity, std::string message, std::string fix){
        AtmosphereDiagnostic diagnostic;
        diagnostic.code = code;
        diagnostic.severity = severity;
        diagnostic.entity = entity;
        diagnostic.message = std::move(message);
        diagnostic.fix = std::move(fix);
        context.out.push_back(std::move(diagnostic));
    }

    std::string quotedEntityName(const Scene& scene, Entity entity){
        return "\"" + scene.getEntityName(entity) + "\"";
    }

    std::string diagnosticNumber(float value, const char* format){
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), format, value);
        return buffer;
    }

    std::string opacityPercent(float share){
        return diagnosticNumber(share * 100.0f, share < 0.1f ? "%.1f%%" : "%.0f%%");
    }

    const char* diagnosticFogType(FogType type){
        switch (type){
            case FogType::LINEAR: return "Linear";
            case FogType::EXPONENTIAL: return "Exponential";
            case FogType::EXPONENTIALSQUARED: return "Exponential Squared";
            case FogType::HEIGHT: return "Height";
        }
        return "Linear";
    }

    const char* diagnosticPlatformGroup(){
        const Platform platform = Engine::getPlatform();
        if (platform == Platform::Web)
            return "Web";
        if (platform == Platform::Android || platform == Platform::iOS)
            return "Mobile";
        return "Desktop";
    }

    bool viewDrewFrame(const AtmosphereCheckContext& context){
        return context.info && context.info->frame != 0;
    }

    // the tier the Fog's volumetric fog runs at in the view (its Quality Override applied)
    AtmosphereTier viewFogTier(const AtmosphereCheckContext& context){
        return AtmosphereTiers::get(AtmosphereTiers::apply(context.info->quality, context.fog->qualityOverride));
    }

    bool volumetricFogAsked(const AtmosphereCheckContext& context){
        return context.fog && context.fog->type == FogType::HEIGHT && context.fog->volumetric;
    }

    // the brightest active directional light with light (the sun the fog scatters), else NULL_ENTITY
    Entity brightestSun(Scene& scene, const LightComponent** found){
        Entity best = NULL_ENTITY;
        float brightest = 0.0f;
        auto lights = scene.getComponentArray<LightComponent>();
        for (size_t i = 0; i < lights->size(); i++){
            const LightComponent& light = lights->getComponentFromIndex(i);
            const Entity entity = lights->getEntity(i);
            if (light.type != LightType::DIRECTIONAL || !light.enabled || !scene.isEntityActiveInHierarchy(entity))
                continue;
            const float brightness = light.intensity * (0.2126f * light.color.x + 0.7152f * light.color.y + 0.0722f * light.color.z);
            if (brightness <= brightest)
                continue;
            brightest = brightness;
            best = entity;
            if (found)
                *found = &light;
        }
        return best;
    }

    // ---------------------------------------------------------------- the checks

    void checkAtmosphereFogMissing(AtmosphereCheckContext& context){
        if (context.fog || context.volumes.empty())
            return;
        addDiagnostic(context, "fog_missing", AtmosphereSeverity::Warning, NULL_ENTITY,
            "The scene has Local Volumetric Fog but no Fog: volumes show only through the volumetric fog of a Fog of Type Height.",
            "Add a Fog (GameObject > Effects > Fog), set its Type to Height and turn on Volumetric Fog > Enabled.");
    }

    void checkAtmosphereSecondFog(AtmosphereCheckContext& context){
        auto fogs = context.scene.getComponentArray<FogComponent>();
        for (size_t i = 1; i < fogs->size(); i++){
            const Entity entity = fogs->getEntity(i);
            addDiagnostic(context, "fog_second_ignored", AtmosphereSeverity::Warning, entity,
                "The Fog on " + quotedEntityName(context.scene, entity) + " is ignored: a scene uses only its first Fog, the one on " +
                    quotedEntityName(context.scene, context.fogEntity) + ".",
                "Remove one of the two Fog components, and keep the look on the other.");
        }
    }

    void checkAtmosphereVolumetricNeedsHeight(AtmosphereCheckContext& context){
        if (!context.fog)
            return;
        const FogComponent& fog = *context.fog;
        const bool height = fog.type == FogType::HEIGHT;
        if (fog.volumetric && !height){
            addDiagnostic(context, "volumetric_needs_height", AtmosphereSeverity::Warning, context.fogEntity,
                std::string("Volumetric Fog is on, but it needs a Fog of Type Height and this one is ") + diagnosticFogType(fog.type) +
                    ": nothing is raymarched.",
                "Set the Fog's Type to Height (its height fog fields then apply), or turn Volumetric Fog off.");
        }else if (!context.volumes.empty() && !(height && fog.volumetric)){
            addDiagnostic(context, "volumetric_needs_height", AtmosphereSeverity::Warning, context.fogEntity,
                height ? std::string("The scene has Local Volumetric Fog, but the Fog's Volumetric Fog is off: volumes show only through it.")
                       : std::string("The scene has Local Volumetric Fog, but the Fog is ") + diagnosticFogType(fog.type) +
                             ": volumes show only through the volumetric fog of a Fog of Type Height.",
                height ? "Turn on the Fog's Volumetric Fog > Enabled."
                       : "Set the Fog's Type to Height and turn on Volumetric Fog > Enabled.");
        }
    }

    void checkAtmosphereVolumetricQuality(AtmosphereCheckContext& context){
        if (!volumetricFogAsked(context) || !viewDrewFrame(context) || context.info->quality != AtmosphereQuality::OFF)
            return;
        const std::string& source = context.info->qualitySource;
        std::string message;
        std::string fix;
        if (source == "runtime"){
            message = "A script set the Atmosphere Quality to Off (Engine.setAtmosphereQuality): the volumetric fog does not run.";
            fix = "Set a tier in that script (Low, Medium or High), or call Engine.resetAtmosphereQuality() to use the project's.";
        }else if (source == "game_view_preview"){
            message = "The Game view previews Atmosphere Quality Off (its toolbar): the volumetric fog does not run in it.";
            fix = "Set the Game view's Atmosphere Quality preview to Project or a tier.";
        }else if (source == "device_tier"){
            message = "The device tier in effect turns Volumetrics off (Project Settings > Quality > Device Tiers): the volumetric fog does "
                      "not run.";
            fix = "Clear that tier's Volumetrics cell, or simulate another device tier.";
        }else{
            message = std::string("Project Settings > Quality > Atmosphere Quality (") + diagnosticPlatformGroup() +
                      ") is Off: the volumetric fog does not run.";
            fix = std::string("Set Atmosphere Quality (") + diagnosticPlatformGroup() + ") to Low, Medium or High.";
        }
        addDiagnostic(context, "volumetric_quality_off", AtmosphereSeverity::Warning, context.fogEntity, message, fix);
    }

    void checkAtmosphereMainCameraOnly(AtmosphereCheckContext& context){
        if (!volumetricFogAsked(context))
            return;
        // the view drew through another camera only (the Scene view while playing)
        if (viewDrewFrame(context) && context.info->quality != AtmosphereQuality::OFF && !context.info->fogDisabled
                && !context.info->volumetricActive && !context.info->mainCameraPass){
            addDiagnostic(context, "volumetric_main_camera_only", AtmosphereSeverity::Info, context.fogEntity,
                "This view does not draw through the scene's main camera, and the volumetric fog draws for the main camera only: here the "
                "analytic height fog shows instead.",
                "Look through the main camera (the Game view), or make the camera you look through the scene's main camera.");
        }
        // cameras that render to a texture get the analytic fog
        auto cameras = context.scene.getComponentArray<CameraComponent>();
        for (size_t i = 0; i < cameras->size(); i++){
            const CameraComponent& camera = cameras->getComponentFromIndex(i);
            const Entity entity = cameras->getEntity(i);
            if (entity == context.camera || !camera.renderToTexture || !camera.enabled || !context.scene.isEntityActiveInHierarchy(entity))
                continue;
            addDiagnostic(context, "volumetric_main_camera_only", AtmosphereSeverity::Info, entity,
                "The camera " + quotedEntityName(context.scene, entity) + " renders to a texture: it gets the analytic height fog, not the volumetric "
                "fog, which draws for the main camera only.",
                "None needed for the analytic look; to see the volumetric fog, look through the main camera.");
        }
    }

    void checkAtmosphereShafts(AtmosphereCheckContext& context){
        if (!volumetricFogAsked(context) || !viewDrewFrame(context) || !context.info->volumetricActive)
            return;
        const LightComponent* sun = nullptr;
        const Entity sunEntity = brightestSun(context.scene, &sun);
        if (!context.fog->lightShafts){
            if (sun && sun->shadows){
                addDiagnostic(context, "shafts_off", AtmosphereSeverity::Info, context.fogEntity,
                    "Light Shafts is off: the shadows of " + quotedEntityName(context.scene, sunEntity) + " do not darken the volumetric fog.",
                    "Turn on the Fog's Volumetric Fog > Light Shafts for light shafts.");
            }
            return;
        }
        if (context.info->lightShafts)
            return;
        if (sunEntity == NULL_ENTITY){
            addDiagnostic(context, "shafts_sun_no_shadows", AtmosphereSeverity::Warning, context.fogEntity,
                "Light Shafts is on, but the scene has no active directional light with Intensity above 0: there are no sun shadows to "
                "darken the fog.",
                "Add a Directional Light and turn on its Shadows, or turn Light Shafts off.");
        }else if (!sun->shadows){
            addDiagnostic(context, "shafts_sun_no_shadows", AtmosphereSeverity::Warning, context.fogEntity,
                "Light Shafts is on, but the main directional light " + quotedEntityName(context.scene, sunEntity) +
                    " casts no shadows: nothing darkens the fog.",
                "Turn on Shadows on " + quotedEntityName(context.scene, sunEntity) + ", or turn Light Shafts off.");
        }else{
            addDiagnostic(context, "shafts_sun_no_shadows", AtmosphereSeverity::Warning, context.fogEntity,
                "Light Shafts is on and " + quotedEntityName(context.scene, sunEntity) + " has Shadows on, but this view drew no sun shadow map "
                "(shadows are off for the view or the project's shadow quality): nothing darkens the fog.",
                "Turn shadows on for the view and in Project Settings > Quality, or turn Light Shafts off.");
        }
    }

    void checkAtmosphereLights(AtmosphereCheckContext& context){
        if (!context.fog || !viewDrewFrame(context))
            return;
        for (const AtmosphereFogLight& light : context.info->fogLights){
            if (light.scattered || light.entity == NULL_ENTITY)
                continue;
            const std::string name = quotedEntityName(context.scene, light.entity);
            std::string message;
            std::string fix;
            if (light.reason == "intensity"){
                message = "The light " + name + " has Intensity 0: it scatters no light in the volumetric fog.";
                fix = "Raise its Intensity above 0.";
            }else if (light.reason == "volumetric_intensity"){
                message = "The light " + name + " has Volumetric Intensity 0: it does not light the volumetric fog.";
                fix = "Raise its Volumetric Intensity above 0.";
            }else if (light.reason == "distance"){
                message = "The light " + name + " is farther from the camera than the Fog's Volumetric Fog > Distance (" +
                          diagnosticNumber(context.fog->volumetricDistance, "%.0f m") + "): it does not light the volumetric fog.";
                fix = "Raise the Fog's Volumetric Fog > Distance, or bring the light nearer.";
            }else if (light.reason == "frustum"){
                message = "The light " + name + " reaches nothing the camera sees this frame: it does not light the volumetric fog.";
                fix = "None needed: it scatters again once its range is in view.";
            }else if (light.reason == "tier_cap"){
                const AtmosphereTier tier = viewFogTier(context);
                message = "The light " + name + " is past the " + std::to_string(tier.fogLocalLights) + " fog lights Atmosphere Quality " +
                          AtmosphereTiers::getName(tier.quality) + " scatters (the nearest to the camera are used): it does not light the "
                          "volumetric fog.";
                fix = "Raise the Atmosphere Quality, or set Volumetric Intensity 0 on a nearer light that need not scatter.";
            }else{
                message = "The light " + name + " does not light the volumetric fog (" + light.reason + ").";
                fix = "See the light's Volumetric Intensity and the Fog's Volumetric Fog settings.";
            }
            addDiagnostic(context, "light_not_scattered", AtmosphereSeverity::Info, light.entity, message, fix);
        }
    }

    void checkAtmosphereVolumes(AtmosphereCheckContext& context){
        if (!context.fog || !viewDrewFrame(context))
            return;
        for (const AtmosphereFogVolume& volume : context.info->fogVolumes){
            if (volume.used || volume.entity == NULL_ENTITY)
                continue;
            const std::string name = quotedEntityName(context.scene, volume.entity);
            std::string message;
            std::string fix;
            if (volume.reason == "distance"){
                message = "The volume " + name + " is farther from the camera than the Fog's Volumetric Fog > Distance (" +
                          diagnosticNumber(context.fog->volumetricDistance, "%.0f m") + "): it is not drawn.";
                fix = "Raise the Fog's Volumetric Fog > Distance, or bring the volume nearer.";
            }else if (volume.reason == "frustum"){
                message = "The volume " + name + " is outside the camera's view this frame: it is not drawn.";
                fix = "None needed: it draws again once it is in view.";
            }else if (volume.reason == "tier_cap"){
                const AtmosphereTier tier = viewFogTier(context);
                message = "The volume " + name + " is past the " + std::to_string(tier.fogVolumes) + " volumes Atmosphere Quality " +
                          AtmosphereTiers::getName(tier.quality) + " draws (the nearest to the camera are used): it is not drawn.";
                fix = "Raise the Atmosphere Quality (Low 2, Medium 4, High 8 volumes), or merge volumes nearer the camera.";
            }else{
                message = "The volume " + name + " is not drawn (" + volume.reason + ").";
                fix = "See the Fog's Volumetric Fog settings.";
            }
            addDiagnostic(context, "volume_culled", AtmosphereSeverity::Info, volume.entity, message, fix);
        }
    }

    void checkAtmosphereSceneViewFog(AtmosphereCheckContext& context){
        if (!context.fog || !context.input.sceneViewFogDisabled)
            return;
        addDiagnostic(context, "scene_view_fog_disabled", AtmosphereSeverity::Warning, context.fogEntity,
            "The Scene view's Disable fog display setting is on: the Scene view draws no fog (the Game view and the game do).",
            "Turn off Disable fog in the Scene view's display settings (Rendering).");
    }

    void checkAtmosphereSkyHorizon(AtmosphereCheckContext& context){
        if (!context.fog || context.fog->type != FogType::HEIGHT || context.fog->colorMode != FogColorMode::SKY || !viewDrewFrame(context))
            return;
        if (context.info->horizonSource != "fog_color")
            return;
        addDiagnostic(context, "sky_mode_no_horizon", AtmosphereSeverity::Warning, context.fogEntity,
            "The sky has no horizon colour (it is not a cube map and its shader declares no horizonColor): the fog uses its Color.",
            "Use a cube map Skybox or a sky shader with a horizonColor property, or set Color Mode to Constant Color.");
    }

    void checkAtmosphereLitLight(AtmosphereCheckContext& context){
        if (!context.fog || context.fog->type != FogType::HEIGHT || context.fog->colorMode != FogColorMode::LIT)
            return;
        const FogComponent& fog = *context.fog;
        const LightComponent* sun = nullptr;
        const bool sunLights = brightestSun(context.scene, &sun) != NULL_ENTITY && fog.directionalInscattering > 0.0f;
        const Vector3 ambient = context.scene.getGlobalIlluminationColorLinear() *
                                (context.scene.getGlobalIlluminationIntensity() * std::max(fog.ambientMultiplier, 0.0f));
        const bool ambientLights = ambient.x > 0.0f || ambient.y > 0.0f || ambient.z > 0.0f;
        const bool emits = fog.emissive.x > 0.0f || fog.emissive.y > 0.0f || fog.emissive.z > 0.0f;
        if (sunLights || ambientLights || emits)
            return;
        addDiagnostic(context, "lit_no_light", AtmosphereSeverity::Warning, context.fogEntity,
            "Color Mode Lit lights the fog with the sun and the scene's ambient light, and it gets neither (no directional light with "
            "Intensity above 0 scattered, Global Illumination Intensity or the fog's Ambient Multiplier 0, no Emissive): the fog is black.",
            "Add a Directional Light or raise the scene's Global Illumination Intensity, raise the fog's Ambient Multiplier or Emissive, or "
            "set Color Mode to Constant Color.");
    }

    void checkAtmosphereOpacity(AtmosphereCheckContext& context){
        if (!context.fog || !context.opacity.valid)
            return;
        const FogComponent& fog = *context.fog;
        const bool height = fog.type == FogType::HEIGHT;
        if (context.opacity.at200 < AtmosphereDiagnostics::INVISIBLE_OPACITY){
            std::string fix;
            if (height)
                fix = "Lower Attenuation Distance (denser fog), raise Maximum Height or Max Opacity, or lower Start Distance.";
            else if (fog.type == FogType::LINEAR)
                fix = "Lower Linear Start and Linear End.";
            else
                fix = "Raise Density.";
            addDiagnostic(context, "fog_invisible", AtmosphereSeverity::Warning, context.fogEntity,
                "The fog hides an estimated " + opacityPercent(context.opacity.at200) +
                    " of what lies 200 m ahead of the main camera: too little to see (the analytic fog; volumes and noise not counted).",
                fix);
        }else if (context.opacity.at10 > AtmosphereDiagnostics::OPAQUE_OPACITY){
            std::string fix;
            if (height)
                fix = "Raise Attenuation Distance (thinner fog), lower Maximum Height or Max Opacity, or raise Start Distance.";
            else if (fog.type == FogType::LINEAR)
                fix = "Raise Linear Start and Linear End.";
            else
                fix = "Lower Density.";
            addDiagnostic(context, "fog_opaque", AtmosphereSeverity::Warning, context.fogEntity,
                "The fog hides an estimated " + opacityPercent(context.opacity.at10) +
                    " of what lies 10 m ahead of the main camera: the scene is almost all fog (the analytic fog; volumes and noise not "
                    "counted).",
                fix);
        }
    }

    // in the order their diagnostics read: the setup first, then what the frame left out, then the look
    void (*const ATMOSPHERE_CHECKS[])(AtmosphereCheckContext&) = {
        checkAtmosphereFogMissing,
        checkAtmosphereSecondFog,
        checkAtmosphereVolumetricNeedsHeight,
        checkAtmosphereVolumetricQuality,
        checkAtmosphereMainCameraOnly,
        checkAtmosphereShafts,
        checkAtmosphereLights,
        checkAtmosphereVolumes,
        checkAtmosphereSceneViewFog,
        checkAtmosphereSkyHorizon,
        checkAtmosphereLitLight,
        checkAtmosphereOpacity,
    };

}

AtmosphereOpacity AtmosphereDiagnostics::estimateOpacity(Scene& scene, Entity camera){
    AtmosphereOpacity opacity;
    FogComponent* fog = scene.findComponentFromIndex<FogComponent>(0);
    const Entity cameraEntity = camera != NULL_ENTITY ? camera : scene.getCamera();
    if (!fog || cameraEntity == NULL_ENTITY)
        return opacity;
    const CameraComponent* component = scene.findComponent<CameraComponent>(cameraEntity);
    const Transform* transform = scene.findComponent<Transform>(cameraEntity);
    if (!component || !transform)
        return opacity;
    // worldDirection is the view's backward axis (RenderSystem::updateCamera)
    Vector3 forward(-component->worldDirection.x, -component->worldDirection.y, -component->worldDirection.z);
    forward = forward.length() > 0.0f ? forward.normalized() : Vector3(0.0f, 0.0f, -1.0f);
    const Vector3& origin = transform->worldPosition;
    opacity.valid = true;
    opacity.at10 = AtmosphereMath::fogOpacity(*fog, origin, forward, 10.0f);
    opacity.at50 = AtmosphereMath::fogOpacity(*fog, origin, forward, 50.0f);
    opacity.at200 = AtmosphereMath::fogOpacity(*fog, origin, forward, 200.0f);
    return opacity;
}

AtmosphereReport AtmosphereDiagnostics::run(const AtmosphereDiagnosticsInput& input){
    AtmosphereReport report;
    if (!input.scene)
        return report;
    Scene& scene = *input.scene;
    const Entity camera = input.camera != NULL_ENTITY ? input.camera : scene.getCamera();
    report.opacity = estimateOpacity(scene, camera);

    auto fogs = scene.getComponentArray<FogComponent>();
    FogComponent* fog = fogs->size() > 0 ? &fogs->getComponentFromIndex(0) : nullptr;
    AtmosphereCheckContext context{input, scene, input.info, fog, fog ? fogs->getEntity(0) : NULL_ENTITY, {}, camera, report.opacity,
                               report.diagnostics};
    auto volumes = scene.getComponentArray<LocalVolumetricFogComponent>();
    for (size_t i = 0; i < volumes->size(); i++){
        if (scene.isEntityActiveInHierarchy(volumes->getEntity(i)))
            context.volumes.push_back(volumes->getEntity(i));
    }
    for (auto check : ATMOSPHERE_CHECKS)
        check(context);
    return report;
}

const char* AtmosphereDiagnostics::severityName(AtmosphereSeverity severity){
    switch (severity){
        case AtmosphereSeverity::Info: return "info";
        case AtmosphereSeverity::Warning: return "warning";
        case AtmosphereSeverity::Error: return "error";
    }
    return "warning";
}
