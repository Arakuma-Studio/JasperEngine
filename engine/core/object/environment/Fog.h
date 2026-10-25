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

        // a look of AtmospherePresets::getFogPresets (by id: "morning_haze", ...); false
        // for an unknown one
        bool applyPreset(const std::string& preset);
    };
}

#endif //FOG_H