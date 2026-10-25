// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOLUMETRICCLOUDS_H
#define VOLUMETRICCLOUDS_H

#include "object/EntityHandle.h"
#include "component/VolumetricCloudsComponent.h"

#include <string>

namespace jasperengine{

    // The scene's volumetric clouds (see VolumetricCloudsComponent): like Fog and Skybox an
    // environment object without a Transform. Setting a preset other than CUSTOM sets the
    // fields of its look (AtmospherePresets).
    class JASPERENGINE_API VolumetricClouds: public EntityHandle {

    public:
        VolumetricClouds(Scene* scene);
        VolumetricClouds(Scene* scene, Entity entity);
        virtual ~VolumetricClouds();

        VolumetricClouds(const VolumetricClouds& rhs);
        VolumetricClouds& operator=(const VolumetricClouds& rhs);
        VolumetricClouds(VolumetricClouds&& rhs) noexcept;
        VolumetricClouds& operator=(VolumetricClouds&& rhs) noexcept;

        void setPreset(CloudsPreset preset);
        CloudsPreset getPreset() const;

        void setRendering(CloudsRendering rendering);
        CloudsRendering getRendering() const;

        void setCoverage(float coverage);
        float getCoverage() const;
        void setDensity(float density);
        float getDensity() const;
        void setShapeScale(float shapeScale);
        float getShapeScale() const;
        void setShapeFactor(float shapeFactor);
        float getShapeFactor() const;
        void setErosionFactor(float erosionFactor);
        float getErosionFactor() const;
        void setErosionScale(float erosionScale);
        float getErosionScale() const;

        void setBottomAltitude(float bottomAltitude);
        float getBottomAltitude() const;
        void setAltitudeRange(float altitudeRange);
        float getAltitudeRange() const;
        void setEarthCurvature(float earthCurvature);
        float getEarthCurvature() const;

        void setWindDirection(float windDirection);
        float getWindDirection() const;
        void setWindSpeed(float windSpeed);
        float getWindSpeed() const;

        // an image file (R coverage, G cloud type); "" for the built-in procedural map
        void setWeatherMap(const std::string& path);
        std::string getWeatherMap() const;
        void setWeatherMapScale(float weatherMapScale);
        float getWeatherMapScale() const;

        void setSunLightDimmer(float sunLightDimmer);
        float getSunLightDimmer() const;
        void setAmbientLightDimmer(float ambientLightDimmer);
        float getAmbientLightDimmer() const;
        void setMaxDistance(float maxDistance);
        float getMaxDistance() const;

        void setQualityOverride(AtmosphereQualityOverride qualityOverride);
        AtmosphereQualityOverride getQualityOverride() const;
    };
}

#endif //VOLUMETRICCLOUDS_H
