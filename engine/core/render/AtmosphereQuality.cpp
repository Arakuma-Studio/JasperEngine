// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AtmosphereQuality.h"

using namespace jasperengine;

AtmosphereTier AtmosphereTiers::get(AtmosphereQuality quality){
    AtmosphereTier tier;
    tier.quality = quality;
    switch (quality){
        case AtmosphereQuality::LOW:
            tier.fogDivisor = 4;
            tier.fogSteps = 16;
            tier.fogLocalLights = 0;
            tier.fogVolumes = 2;
            tier.cloudsAuto = CloudsRendering::SKY_MAP;
            tier.skyMapSize = 256;
            tier.skyMapSliceFrames = 8;
            tier.cloudSteps = 32;
            tier.cloudLightSteps = 3;
            tier.cloudErosion = false;
            break;
        case AtmosphereQuality::MEDIUM:
            tier.fogDivisor = 2;
            tier.fogSteps = 32;
            tier.fogLocalLights = 0;
            tier.fogVolumes = 4;
            tier.cloudsAuto = CloudsRendering::SKY_MAP;
            tier.skyMapSize = 512;
            tier.skyMapSliceFrames = 4;
            tier.cloudSteps = 64;
            tier.cloudLightSteps = 4;
            tier.cloudErosion = true;
            break;
        case AtmosphereQuality::HIGH:
            tier.fogDivisor = 2;
            tier.fogSteps = 64;
            tier.fogLocalLights = 4;
            tier.fogVolumes = 8;
            tier.cloudsAuto = CloudsRendering::SCREEN;
            // a Sky Map forced on High (rendering SKY_MAP) refreshes as Medium's, marched as High's
            tier.skyMapSize = 512;
            tier.skyMapSliceFrames = 4;
            tier.cloudSteps = 96;
            tier.cloudLightSteps = 6;
            tier.cloudErosion = true;
            break;
        case AtmosphereQuality::OFF:
            break;
    }
    return tier;
}

AtmosphereQuality AtmosphereTiers::apply(AtmosphereQuality quality, AtmosphereQualityOverride cap){
    AtmosphereQuality limit = AtmosphereQuality::HIGH;
    if (cap == AtmosphereQualityOverride::LOW)
        limit = AtmosphereQuality::LOW;
    else if (cap == AtmosphereQualityOverride::MEDIUM)
        limit = AtmosphereQuality::MEDIUM;
    return ((int)quality < (int)limit) ? quality : limit;
}

CloudsRendering AtmosphereTiers::resolveClouds(CloudsRendering rendering, AtmosphereQuality quality){
    if (rendering != CloudsRendering::AUTO)
        return rendering;
    return get(quality).cloudsAuto;
}

const char* AtmosphereTiers::getName(AtmosphereQuality quality){
    switch (quality){
        case AtmosphereQuality::OFF: return "Off";
        case AtmosphereQuality::LOW: return "Low";
        case AtmosphereQuality::MEDIUM: return "Medium";
        case AtmosphereQuality::HIGH: return "High";
    }
    return "Off";
}
