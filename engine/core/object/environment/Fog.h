// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FOG_H
#define FOG_H

#include "object/EntityHandle.h"
#include "component/FogComponent.h"

#include <string>

namespace jasperengine{

    class JASPERENGINE_API Fog: public EntityHandle {

    public:
        Fog(Scene* scene);
        Fog(Scene* scene, Entity entity);
        virtual ~Fog();

        Fog(const Fog& rhs);
        Fog& operator=(const Fog& rhs);
        Fog(Fog&& rhs) noexcept;
        Fog& operator=(Fog&& rhs) noexcept;

        FogType getType() const;
        Vector3 getColor() const;
        float getDensity() const;
        float getLinearStart() const;
        float getLinearEnd() const;

        void setType(FogType type);
        void setColor(Vector3 color);
        void setColor(float red, float green, float blue);
        void setDensity(float density);
        void setLinearStart(float start);
        void setLinearEnd(float end);
        void setLinearStartEnd(float start, float end);

        // Height fog (FogType::HEIGHT, see FogComponent)
        FogColorMode getColorMode() const;
        float getAttenuationDistance() const;
        float getBaseHeight() const;
        float getMaximumHeight() const;
        float getStartDistance() const;
        float getMaxFogDistance() const;
        float getMaxOpacity() const;
        float getDirectionalInscattering() const;
        float getAnisotropy() const;
        bool isSkyFill() const;
        float getSkyFillAngle() const;
        // colour mode LIT
        Vector3 getAlbedo() const;
        float getAmbientMultiplier() const;
        Vector3 getEmissive() const;

        void setColorMode(FogColorMode colorMode);
        void setAttenuationDistance(float attenuationDistance);
        void setBaseHeight(float baseHeight);
        void setMaximumHeight(float maximumHeight);
        void setStartDistance(float startDistance);
        void setMaxFogDistance(float maxFogDistance);
        void setMaxOpacity(float maxOpacity);
        void setDirectionalInscattering(float directionalInscattering);
        void setAnisotropy(float anisotropy);
        void setSkyFill(bool skyFill);
        void setSkyFillAngle(float skyFillAngle);
        void setAlbedo(Vector3 albedo);
        void setAmbientMultiplier(float ambientMultiplier);
        void setEmissive(Vector3 emissive);

        // Volumetric fog (a height fog's option)
        bool isVolumetric() const;
        float getVolumetricDistance() const;
        bool isLightShafts() const;
        float getNoiseIntensity() const;
        float getNoiseScale() const;
        Vector3 getWind() const;
        AtmosphereQualityOverride getQualityOverride() const;

        void setVolumetric(bool volumetric);
        void setVolumetricDistance(float volumetricDistance);
        void setLightShafts(bool lightShafts);
        void setNoiseIntensity(float noiseIntensity);
        void setNoiseScale(float noiseScale);
        void setWind(Vector3 wind);
        void setWind(float x, float y, float z);
        void setQualityOverride(AtmosphereQualityOverride qualityOverride);

        // a look of AtmospherePresets::getFogPresets (by id: "morning_haze", ...) written into
        // the Fog's own fields; false for an unknown one. Profiles are the shared looks.
        bool applyPreset(const std::string& preset);

        // The Fog Profile (docs/specs/S101-vapor.md R7, R10): a .fogprofile path (assets-relative),
        // "builtin:<id>", a runtime key ("runtime:<n>"), or "" for none. While the game runs,
        // setting it applies a runtime profile at once (never saved) and cancels a blend;
        // otherwise it sets the saved reference. The getter names the one in use.
        std::string getProfile() const;
        void setProfile(const std::string& profile);
        // cross-fades the look to this Fog's own fields under `profile` ("" none) over `seconds`
        // of game time (<= 0: at once); a blend running is re-targeted from where it is
        void blendTo(const std::string& profile, float seconds);
        bool isBlending() const;
        float getBlendProgress() const;
        std::string getBlendTarget() const;
        // the look at the scene's camera with every layer applied (colours linear)
        FogComponent getResolved() const;
        // drop the runtime writes of a field (false: not a look field), or of all
        bool clearOverride(const std::string& field);
        void clearOverrides();
    };
}

#endif //FOG_H