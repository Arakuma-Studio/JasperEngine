// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AtmosphereDiagnostics.h"

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/FogComponent.h"
#include "component/FogZoneComponent.h"
#include "component/LightComponent.h"
#include "component/LocalVolumetricFogComponent.h"
#include "component/Transform.h"
#include "render/AtmosphereMath.h"
#include "render/AtmosphereQuality.h"
#include "render/FogProfile.h"
#include "render/FogResolve.h"
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
        // the Fog the scene uses (its first) with its look as the main camera sees it (its profile,
        // a blend, the Fog Zones and runtime writes layered: FogScene), and its GameObject
        const FogComponent* fog;
        Entity fogEntity;
        // the Fog as stored (its own fields and its profile reference)
        const FogComponent* ownFog;
        // the active Local Volumetric Fog volumes and Fog Zones
        std::vector<Entity> volumes;
        std::vector<Entity> zones;
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
        if (context.fog || (context.volumes.empty() && context.zones.empty()))
            return;
        if (context.volumes.empty()){
            addDiagnostic(context, "fog_missing", AtmosphereSeverity::Warning, NULL_ENTITY,
                "The scene has Fog Zones but no Fog: a zone blends its profile over the scene's Fog, so without one it changes nothing.",
                "Add a Fog (GameObject > Effects > Fog): the zones' profiles then blend over it.");
            return;
        }
        addDiagnostic(context, "fog_missing", AtmosphereSeverity::Warning, NULL_ENTITY,
            std::string("The scene has Local Volumetric Fog but no Fog: volumes show only through the volumetric fog of a Fog of Type Height.") +
                (context.zones.empty() ? "" : " Its Fog Zones change nothing without a Fog either."),
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

    // the labels of the fields a profile sets, the first few
    std::string profileFieldList(const FogProfileData& profile){
        std::string list;
        size_t listed = 0;
        for (size_t i = 0; i < FOG_FIELD_COUNT; i++){
            if (!profile.set.test(i))
                continue;
            if (listed == 4){
                list += ", ...";
                break;
            }
            list += (listed++ ? ", " : "") + std::string(FogFields::info(static_cast<FogField>(i)).label);
        }
        return list;
    }

    // a profile that sets fields, none of which the scene's fog type reads: profile_no_effect on
    // `entity` (`owner` names it in the message)
    void checkProfileEffect(AtmosphereCheckContext& context, const FogProfileData& profile, const std::string& key, Entity entity,
                            const std::string& owner){
        if (!context.fog || profile.set.none())
            return;
        const FogType type = context.fog->type;
        for (size_t i = 0; i < FOG_FIELD_COUNT; i++){
            if (profile.set.test(i) && FogFields::readByType(type, static_cast<FogField>(i)))
                return;
        }
        const bool height = type == FogType::HEIGHT;
        addDiagnostic(context, "profile_no_effect", AtmosphereSeverity::Warning, entity,
            owner + " profile \"" + key + "\" sets only fields a fog of Type " + diagnosticFogType(type) + " ignores (" +
                profileFieldList(profile) + "): it changes nothing.",
            height ? "Set the Fog's Type to Linear or Exponential, or use a profile that sets the height fog's fields."
                   : "Set the Fog's Type to Height (its height fog fields then apply), or use a profile that sets Color, Density or "
                     "the Linear range.");
    }

    // a profile that is named but does not read: profile_missing (R6); true when it is missing
    bool checkProfileMissing(AtmosphereCheckContext& context, const std::string& key, Entity entity, const std::string& owner,
                             const char* consequence){
        std::string reason;
        if (!FogProfileLibrary::isMissing(key, &reason))
            return false;
        addDiagnostic(context, "profile_missing", AtmosphereSeverity::Warning, entity,
            owner + " profile \"" + key + "\" is missing or does not read" + (reason.empty() ? std::string() : " (" + reason + ")") + ": " +
                consequence,
            "Restore the file, or set the Profile to an existing Fog Profile (or None).");
        return true;
    }

    void checkAtmosphereFogProfile(AtmosphereCheckContext& context){
        if (!context.ownFog)
            return;
        const std::string key = FogScene::profileKey(*context.ownFog);
        if (key.empty())
            return;
        const std::shared_ptr<const FogProfileData> profile = FogScene::profileOf(*context.ownFog);
        if (!profile){
            checkProfileMissing(context, key, context.fogEntity, "The Fog's", "the Fog renders its own values.");
            return;
        }
        checkProfileEffect(context, *profile, key, context.fogEntity, "The Fog's");
    }

    void checkAtmosphereZones(AtmosphereCheckContext& context){
        for (Entity entity : context.zones){
            const FogZoneComponent& zone = context.scene.getComponent<FogZoneComponent>(entity);
            const std::string name = quotedEntityName(context.scene, entity);
            if (!zone.profile.isSet()){
                addDiagnostic(context, "zone_no_effect", AtmosphereSeverity::Warning, entity,
                    "The Fog Zone " + name + " has no profile: it changes nothing.",
                    "Set its Profile to the Fog Profile whose look it blends in.");
                continue;
            }
            const std::string key = !zone.profile.path.empty() ? zone.profile.path : zone.profile.guid.str();
            const std::shared_ptr<const FogProfileData> profile = FogProfileLibrary::getRef(zone.profile);
            if (!profile){
                checkProfileMissing(context, key, entity, "The Fog Zone " + name + "'s", "the zone changes nothing.");
                continue;
            }
            if (profile->set.none()){
                addDiagnostic(context, "zone_no_effect", AtmosphereSeverity::Warning, entity,
                    "The Fog Zone " + name + "'s profile \"" + key + "\" sets no field: the zone changes nothing.",
                    "Set the fields the zone should change in that profile, or use another profile.");
                continue;
            }
            if (!(zone.weight > 0.0f)){
                addDiagnostic(context, "zone_no_effect", AtmosphereSeverity::Warning, entity,
                    "The Fog Zone " + name + " has Weight 0: it changes nothing.",
                    "Raise its Weight (1: its profile's full look inside it).");
                continue;
            }
            checkProfileEffect(context, *profile, key, entity, "The Fog Zone " + name + "'s");
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
        if (context.opacity.at200 < AtmosphereDiagnostics::INVISIBLE_OPACITY &&
            context.opacity.levelAt200 < AtmosphereDiagnostics::INVISIBLE_OPACITY){
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
        checkAtmosphereFogProfile,
        checkAtmosphereZones,
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

namespace {

    // the look of `fog` the camera sees: its own fields, or with its profile, a blend, the Fog
    // Zones at the camera and the runtime writes layered (in `scratch`)
    const FogComponent& atmosphereLookAt(Scene& scene, const FogComponent& fog, Entity camera, FogResolve& scratch){
        if (!FogScene::needsResolve(&scene, fog))
            return fog;
        const Transform* transform = camera != NULL_ENTITY ? scene.findComponent<Transform>(camera) : nullptr;
        FogScene::resolve(&scene, fog, transform ? transform->worldPosition : FogScene::mainCameraPosition(&scene), scratch);
        return scratch.resolved;
    }

    AtmosphereOpacity atmosphereOpacityOf(Scene& scene, const FogComponent* look, Entity camera){
        AtmosphereOpacity opacity;
        if (!look || camera == NULL_ENTITY)
            return opacity;
        const CameraComponent* component = scene.findComponent<CameraComponent>(camera);
        const Transform* transform = scene.findComponent<Transform>(camera);
        if (!component || !transform)
            return opacity;
        // worldDirection is the view's backward axis (RenderSystem::updateCamera)
        Vector3 forward(-component->worldDirection.x, -component->worldDirection.y, -component->worldDirection.z);
        forward = forward.length() > 0.0f ? forward.normalized() : Vector3(0.0f, 0.0f, -1.0f);
        const Vector3& origin = transform->worldPosition;
        opacity.valid = true;
        opacity.at10 = AtmosphereMath::fogOpacity(*look, origin, forward, 10.0f);
        opacity.at50 = AtmosphereMath::fogOpacity(*look, origin, forward, 50.0f);
        opacity.at200 = AtmosphereMath::fogOpacity(*look, origin, forward, 200.0f);
        opacity.levelAt200 = opacity.at200;
        if (look->type == FogType::HEIGHT){
            Vector3 level(forward.x, 0.0f, forward.z);
            level = level.length() > 1e-4f ? level.normalized() : Vector3(0.0f, 0.0f, -1.0f);
            const Vector3 base(origin.x, std::min(origin.y, look->baseHeight), origin.z);
            opacity.levelAt200 = AtmosphereMath::fogOpacity(*look, base, level, 200.0f);
        }
        return opacity;
    }

}

AtmosphereOpacity AtmosphereDiagnostics::estimateOpacity(Scene& scene, Entity camera){
    const FogComponent* fog = scene.findComponentFromIndex<FogComponent>(0);
    const Entity cameraEntity = camera != NULL_ENTITY ? camera : scene.getCamera();
    if (!fog)
        return AtmosphereOpacity();
    FogResolve scratch;
    return atmosphereOpacityOf(scene, &atmosphereLookAt(scene, *fog, cameraEntity, scratch), cameraEntity);
}

AtmosphereReport AtmosphereDiagnostics::run(const AtmosphereDiagnosticsInput& input){
    AtmosphereReport report;
    if (!input.scene)
        return report;
    Scene& scene = *input.scene;
    const Entity camera = input.camera != NULL_ENTITY ? input.camera : scene.getCamera();

    auto fogs = scene.getComponentArray<FogComponent>();
    const FogComponent* fog = fogs->size() > 0 ? &fogs->getComponentFromIndex(0) : nullptr;
    FogResolve resolved;
    const FogComponent* look = fog ? &atmosphereLookAt(scene, *fog, camera, resolved) : nullptr;
    report.opacity = atmosphereOpacityOf(scene, look, camera);

    AtmosphereCheckContext context{input, scene, input.info, look, fog ? fogs->getEntity(0) : NULL_ENTITY, fog, {}, {}, camera,
                                   report.opacity, report.diagnostics};
    auto volumes = scene.getComponentArray<LocalVolumetricFogComponent>();
    for (size_t i = 0; i < volumes->size(); i++){
        if (scene.isEntityActiveInHierarchy(volumes->getEntity(i)))
            context.volumes.push_back(volumes->getEntity(i));
    }
    if (scene.isComponentRegistered<FogZoneComponent>()){
        auto zones = scene.getComponentArray<FogZoneComponent>();
        for (size_t i = 0; i < zones->size(); i++){
            if (scene.isEntityActiveInHierarchy(zones->getEntity(i)))
                context.zones.push_back(zones->getEntity(i));
        }
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
