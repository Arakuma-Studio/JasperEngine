// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FOG_COMPONENT_H
#define FOG_COMPONENT_H

#include "util/Color.h"
#include "asset/AssetRef.h"
#include "render/AtmosphereQuality.h"

#include <memory>

namespace jasperengine{

    enum class FogType{
        LINEAR,
		EXPONENTIAL,
		EXPONENTIALSQUARED,
		// physically based height fog (HDRP's Fog): homogeneous below the base height,
		// thinning exponentially above it, lit by the sun; optionally volumetric
		HEIGHT
    };

    // where the height fog's colour comes from: its Color, the sky's horizon, or the scene's
    // light scattered by the fog (LIT: albedo times the sun and the scene ambient, plus emissive)
    enum class FogColorMode{
        COLOR,
        SKY,
        LIT
    };

    struct FogRuntime;

    // What a running game layers on a Fog (render/FogResolve.h: blendTo, a runtime profile, the
    // script and animation writes). Never saved, and never copied: a copy of the component
    // starts without it and an assignment keeps the target's (it belongs to the entity, not
    // the value); a move carries it (the component arrays move their elements).
    class JASPERENGINE_API FogRuntimeSlot{
    public:
        FogRuntimeSlot();
        ~FogRuntimeSlot();
        FogRuntimeSlot(const FogRuntimeSlot&);
        FogRuntimeSlot& operator=(const FogRuntimeSlot&);
        FogRuntimeSlot(FogRuntimeSlot&& other) noexcept;
        FogRuntimeSlot& operator=(FogRuntimeSlot&& other) noexcept;

        FogRuntime* get() const { return state.get(); }
        FogRuntime& ensure();
        void reset();

    private:
        std::unique_ptr<FogRuntime> state;
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
		// colour mode LIT: the share of the light the fog scatters (linear, per channel), the
		// scene ambient's weight in that light, and light the fog gives off itself (linear, HDR)
		Vector3 albedo = Vector3(1.0f, 1.0f, 1.0f);
		float ambientMultiplier = 1.0f;
		Vector3 emissive = Vector3(0.0f, 0.0f, 0.0f);
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

		// a Fog Profile (.fogprofile, assets-relative, or builtin:<id>): the look fields it sets
		// come from it, the others from this Fog (docs/specs/S101-vapor.md R7)
		AssetRef profile;

		FogRuntimeSlot runtime;
    };

}

#endif //FOG_COMPONENT_H
