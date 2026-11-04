// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LIGHT_COMPONENT_H
#define LIGHT_COMPONENT_H

#include "registry/TagManager.h"
#include "math/Vector3.h"
#include "render/FramebufferRender.h"
#include "texture/Texture.h"
#include "render/shadow/CascadeFit.h"
#include "render/shadow/ShadowTypes.h"
#include "Engine.h"
#include <cmath>
#include "math/Plane.h"
#include "render/CameraRender.h"

namespace jasperengine{

    // Which shader stage evaluates this light on materials whose lighting is MIXED.
    // BOTH has no preference (it follows the material; per pixel under MIXED).
    enum class LightShading{
        BOTH,
        PIXEL,
        VERTEX
    };

    enum class LightType{
        DIRECTIONAL,
        POINT,
        SPOT
    };

    // What LightComponent::intensity is in (util/LightPhotometry.h). ENGINE is the engine's own
    // unitless intensity, drawn as it is. The physical units are converted on the CPU through the
    // scene's Physical Light Exposure (SceneSettings::physicalLightEV100): LUX for a directional
    // light, CANDELA and LUMEN for a point or spot light.
    enum class LightUnit{
        ENGINE,
        LUX,
        CANDELA,
        LUMEN
    };

    struct LightCamera{
        CameraRender render;
        Matrix4 lightViewMatrix;
        Matrix4 lightProjectionMatrix;
        Matrix4 lightViewProjectionMatrix;
        Plane frustumPlanes[6];
        // for point light all cameras are same calculated value that is used by distanceToDepthValue in shader
        Vector2 nearFar = Vector2(0.0, 0.0);
    };

    struct JASPERENGINE_API LightComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

        LightType type = LightType::DIRECTIONAL;

        Vector3 direction;
        Vector3 worldDirection;

        // linear; with useColorTemperature, the filter the temperature's colour goes through
        Vector3 color = Vector3(1.0, 1.0, 1.0);

        float range = 0.0;
        float intensity = 1.0;
        LightUnit intensityUnit = LightUnit::ENGINE;
        // the colour of a black body at colorTemperature (Kelvin, 1000..20000; 6500 is white)
        // times color
        bool useColorTemperature = false;
        float colorTemperature = 6500.0f;

        // Runtime only: the colour and unitless intensity every renderer consumer reads, made from
        // the authored values above by LightPhotometry::resolveLight when one of them or the
        // scene's EV100 changes (resolvedKey). Never serialized.
        Vector3 resolvedColor = Vector3(1.0, 1.0, 1.0);
        float resolvedIntensity = 1.0;
        struct ResolvedKey{
            Vector3 color;
            float intensity = NAN;
            float colorTemperature = 0.0f;
            float ev100 = 0.0f;
            LightUnit intensityUnit = LightUnit::ENGINE;
            LightType type = LightType::DIRECTIONAL;
            bool useColorTemperature = false;
        } resolvedKey;

        float innerConeCos = 0.766044438f; // cos(Angle::defaultToRad(80 / 2));
        float outerConeCos = 0.642787635f; // cos(Angle::defaultToRad(100 / 2));

        // Optional projected grayscale/alpha mask. An empty mask keeps the default
        // circular angular attenuation; a loaded mask defines the complete spot shape.
        Texture spotMask;

        // Runtime projection state derived from spotMask and the entity transform.
        // These values are intentionally not serialized.
        Vector3 worldUp;
        float spotMaskAspect = 1.0f;
        bool spotMaskReady = false;
        int spotMaskTile = -1; // spot mask atlas tile of the ready mask

        Vector3 getLocalSpotProjectionUp() const {
            Vector3 localDirection = direction.normalized();
            if (localDirection == Vector3::ZERO){
                return Vector3(0, 1, 0);
            }

            Vector3 referenceUp = Vector3(0, 1, 0);
            if (std::abs(localDirection.dotProduct(referenceUp)) > 0.999f){
                referenceUp = Vector3(0, 0, 1);
            }

            Vector3 localRight = localDirection.crossProduct(referenceUp).normalized();
            return localRight.crossProduct(localDirection).normalized();
        }

        // Unity Light.cullingMask: GameObjects on a layer whose bit is clear are neither
        // lit by this light nor cast its shadow
        uint32_t cullingMask = LAYER_MASK_EVERYTHING;

        LightShading shading = LightShading::BOTH;

        // how much a point or spot light lights the volumetric fog (High atmosphere quality;
        // 0: none). The sun's share of the fog is the Fog's directional inscattering.
        float volumetricIntensity = 1.0f;

        // Shadows (docs/rendering/shadows.md). Unity's Shadow Type is shadows + softShadows:
        // No Shadows, Hard Shadows (one bilinear comparison) or Soft Shadows (the tier's filter)
        bool shadows = false;
        bool softShadows = true;
        float shadowStrength = 1.0f;            // 0 no shadow .. 1 full
        bool automaticShadowCamera = true;
        // biases in shadow texels, whatever the resolution and cascade: the depth offset
        // towards the light, and the offset along the normal (scaled by 1 - n.l)
        float shadowBias = 1.0f;
        float shadowNormalBias = 1.0f;
        unsigned int mapResolution = 0;         // texels per side of each map; 0: the quality tier's
        Vector2 shadowCameraNearFar = Vector2(0.1f, 10.0f); // when automatic it gets value from scene camera or light range
        unsigned int numShadowCascades = 0;     // directional: 1..4; 0: the quality tier's
        CascadeSplitMode cascadeSplitMode = CascadeSplitMode::AUTO;
        Vector3 cascadeSplits = Vector3(0.067f, 0.2f, 0.467f); // MANUAL: fractions of the shadow distance
        float shadowDistance = 0.0f;            // directional: metres; 0: the scene's, else the tier's
        float shadowAngularDiameter = 0.53f;    // directional: degrees of the light's disc (soft radius, PCSS)
        float shadowSourceRadius = 0.05f;       // spot and point: metres (soft radius, PCSS)
        ShadowUpdateMode shadowUpdateMode = ShadowUpdateMode::AUTO;

        ShadowType getShadowType() const{
            return !shadows ? ShadowType::NONE : (softShadows ? ShadowType::SOFT : ShadowType::HARD);
        }
        void setShadowType(ShadowType type){
            shadows = type != ShadowType::NONE;
            if (shadows)
                softShadows = type == ShadowType::SOFT;
        }

        LightCamera cameras[6];
        // the scene camera's cascade fit (directional), kept for the fit freeze
        CascadeFitResult cascadeFit;
        bool hasCascadeFit = false;
        // base slot in the shadow atlas (projective for directional/spot, point atlas
        // for omni); -1 when the light has no shadow allocation this frame
        int shadowMapIndex = -1;
        // the cascades it draws this frame (numShadowCascades or the tier's, at most the tier's)
        unsigned int shadowCascades = 0;
        // texels per side of each of its maps this frame (the planner's)
        unsigned int shadowSlotResolution[6] = {};

        // runtime cache state (RenderSystem): bumped when a setting its shadow maps depend on
        // changes, so the cached slots are redrawn; requestShadowUpdate() redraws an On Demand light
        uint64_t shadowRevision = 1;
        uint64_t shadowSettingsHash = 0;
        bool shadowUpdateRequested = false;

        bool needUpdateShadowCamera = false;
        bool needUpdateShadowMap = false; // framebuffers
    };
    
}

#endif //LIGHT_COMPONENT_H
