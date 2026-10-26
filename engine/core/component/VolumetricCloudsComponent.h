// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOLUMETRIC_CLOUDS_COMPONENT_H
#define VOLUMETRIC_CLOUDS_COMPONENT_H

#include "texture/Texture.h"
#include "render/AtmosphereQuality.h"

namespace jasperengine{

    // the look a preset sets (AtmospherePresets); CUSTOM: the fields as they are
    enum class CloudsPreset{
        CUSTOM,
        SPARSE,
        CLOUDY,
        OVERCAST,
        STORMY
    };

    // Raymarched volumetric clouds (Unity HDRP's Volumetric Clouds): a layer of cloud between
    // bottomAltitude and bottomAltitude + altitudeRange (meters), lit by the main directional
    // light and the scene ambient, its shapes from baked 3D noise placed by a weather map
    // (R coverage, G cloud type). A scene has one: the first VolumetricCloudsComponent is
    // drawn, as with the Skybox. Defaults are the Cloudy preset.
    struct JASPERENGINE_API VolumetricCloudsComponent{
        CloudsPreset preset = CloudsPreset::CLOUDY;
        CloudsRendering rendering = CloudsRendering::AUTO;

        // the share of the sky the weather map covers (0..1)
        float coverage = 0.5f;
        // the clouds' extinction (0 clear .. 1 dense)
        float density = 0.5f;
        // the base shapes: their tiling (higher: smaller clouds) and how much they carve the
        // covered area (0 flat sheets .. 1 rounded heaps)
        float shapeScale = 4.0f;
        float shapeFactor = 0.8f;
        // the detail noise that erodes the shapes' edges: its amount (0..1) and tiling
        float erosionFactor = 0.6f;
        float erosionScale = 100.0f;

        float bottomAltitude = 1200.0f;
        float altitudeRange = 2000.0f;
        // how much the layer bends down with the earth's curvature (0 flat .. 1 real)
        float earthCurvature = 1.0f;

        // the direction the wind blows towards (degrees about +Y, 0 = +Z) and its speed (m/s)
        float windDirection = 0.0f;
        float windSpeed = 10.0f;

        // R coverage, G cloud type (0 stratus .. 1 cumulonimbus); empty: a built-in procedural map
        Texture weatherMap;
        // the distance the weather map spans before it repeats (km)
        float weatherMapScale = 32.0f;

        float sunLightDimmer = 1.0f;
        float ambientLightDimmer = 1.0f;
        // clouds farther than this are not marched (km)
        float maxDistance = 40.0f;

        AtmosphereQualityOverride qualityOverride = AtmosphereQualityOverride::PROJECT;
    };

}

#endif // VOLUMETRIC_CLOUDS_COMPONENT_H
