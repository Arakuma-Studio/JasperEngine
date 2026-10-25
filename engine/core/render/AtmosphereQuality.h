// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ATMOSPHERE_QUALITY_H
#define ATMOSPHERE_QUALITY_H

#include "Export.h"

namespace jasperengine{

    // The atmosphere's quality tier (volumetric fog and volumetric clouds). Analytic height
    // fog and the sky fill are never tiered. The project sets one per platform group
    // (Project Settings > Quality > Atmosphere Quality); a game may override it at runtime
    // (Engine::setAtmosphereQuality).
    enum class AtmosphereQuality{
        OFF,
        LOW,
        MEDIUM,
        HIGH
    };

    // A component's own cap of the tier: downward only, PROJECT keeps the running tier
    enum class AtmosphereQualityOverride{
        PROJECT,
        LOW,
        MEDIUM
    };

    // How volumetric clouds are drawn (VolumetricCloudsComponent::rendering)
    enum class CloudsRendering{
        AUTO,     // Sky Map on Low and Medium, Screen on High
        SKY_MAP,  // a world-direction map of the sky, refreshed time-sliced; every camera
        SCREEN    // a quarter-resolution march per pixel, in front of far geometry; main camera
    };

    // What a tier runs (docs/specs/S12-atmosphere.md R11). Volumetric fog: its targets at
    // 1/fogDivisor of the colour pass per axis, fogSteps march steps, fogLocalLights point/spot
    // lights and fogVolumes Local Volumetric Fog volumes. Clouds: the mode AUTO picks, the Sky
    // Map's side and the frames one refresh takes, the march's primary and light steps,
    // detail erosion, the Screen mode's divisor.
    struct AtmosphereTier{
        AtmosphereQuality quality = AtmosphereQuality::OFF;
        int fogDivisor = 4;
        int fogSteps = 0;
        int fogLocalLights = 0;
        int fogVolumes = 0;
        CloudsRendering cloudsAuto = CloudsRendering::SKY_MAP;
        int skyMapSize = 0;
        int skyMapSliceFrames = 1;
        int cloudSteps = 0;
        int cloudLightSteps = 0;
        bool cloudErosion = false;
        int cloudScreenDivisor = 4;
    };

    // the most steps any tier marches (the shaders' compile-time loop bounds)
    constexpr int ATMOSPHERE_MAX_FOG_STEPS = 64;
    constexpr int ATMOSPHERE_MAX_FOG_VOLUMES = 8;
    constexpr int ATMOSPHERE_MAX_FOG_LIGHTS = 4;
    constexpr int ATMOSPHERE_MAX_CLOUD_STEPS = 96;
    constexpr int ATMOSPHERE_MAX_CLOUD_LIGHT_STEPS = 6;

    class JASPERENGINE_API AtmosphereTiers{
    public:
        // the parameters of a tier (OFF: nothing runs)
        static AtmosphereTier get(AtmosphereQuality quality);
        // a tier capped by a component's override (never raised)
        static AtmosphereQuality apply(AtmosphereQuality quality, AtmosphereQualityOverride cap);
        // the mode clouds draw with at a tier (AUTO resolved)
        static CloudsRendering resolveClouds(CloudsRendering rendering, AtmosphereQuality quality);

        static const char* getName(AtmosphereQuality quality);
    };

}

#endif // ATMOSPHERE_QUALITY_H
