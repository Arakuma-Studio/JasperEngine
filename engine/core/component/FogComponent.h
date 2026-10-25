// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FOG_COMPONENT_H
#define FOG_COMPONENT_H

#include "util/Color.h"
#include "render/AtmosphereQuality.h"

namespace jasperengine{

    enum class FogType{
        LINEAR,
		EXPONENTIAL,
		EXPONENTIALSQUARED,
		// physically based height fog (HDRP's Fog): homogeneous below the base height,
		// thinning exponentially above it, lit by the sun; optionally volumetric
		HEIGHT
    };

    // where the height fog's colour comes from: its Color, or the sky's horizon
    enum class FogColorMode{
        COLOR,
        SKY
    };

    // The scene's fog: the first FogComponent of the scene is used (RenderSystem). The
    // distance types (linear, exponential, exponential squared) read color, density and the
    // linear range; HEIGHT reads the height fog fields below, which the others ignore.
    struct JASPERENGINE_API FogComponent{
        FogType type = FogType::LINEAR;
		Vector3 color = Color::sRGBToLinear(Vector3(0.8f, 0.8f, 0.8f)); // linear color
		float density = 0.01f; // for exponential fog
		float linearStart = 10;
		float linearEnd = 60;

		// Height fog (type HEIGHT). The medium: extinction 1 / attenuationDistance up to
		// baseHeight, falling exponentially above it to 1/1000 of that at maximumHeight,
		// over the view ray's part from startDistance to maxFogDistance (meters).
		FogColorMode colorMode = FogColorMode::COLOR;
		float attenuationDistance = 400.0f;
		float baseHeight = 0.0f;
		float maximumHeight = 50.0f;
		float startDistance = 0.0f;
		float maxFogDistance = 2000.0f;
		float maxOpacity = 1.0f;
		// the sun's light the fog scatters towards the camera (Henyey-Greenstein, anisotropy
		// -1 back .. 1 forward)
		float directionalInscattering = 0.2f;
		float anisotropy = 0.6f;
		// the sky's pixels take the fog colour up to skyFillAngle degrees above the horizon
		bool skyFill = true;
		float skyFillAngle = 3.0f;
		// Volumetric fog: the medium raymarched up to volumetricDistance from the camera, with
		// the sun's shadows (light shafts), Local Volumetric Fog volumes and a density noise;
		// the analytic fog beyond. The main camera only.
		bool volumetric = false;
		float volumetricDistance = 64.0f;
		bool lightShafts = true;
		float noiseIntensity = 0.0f;
		float noiseScale = 20.0f;
		Vector3 wind = Vector3(0.0f, 0.0f, 0.0f); // m/s, scrolls the noise
		AtmosphereQualityOverride qualityOverride = AtmosphereQualityOverride::PROJECT;
    };

}

#endif //FOG_COMPONENT_H
