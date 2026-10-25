// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LOCALVOLUMETRICFOG_H
#define LOCALVOLUMETRICFOG_H

#include "object/GameObject.h"
#include "component/LocalVolumetricFogComponent.h"

namespace jasperengine{

    // A GameObject holding a Local Volumetric Fog volume (see LocalVolumetricFogComponent).
    // Colours are sRGB here, linear in the component.
    class JASPERENGINE_API LocalVolumetricFog: public GameObject{

    public:
        LocalVolumetricFog(Scene* scene);
        LocalVolumetricFog(Scene* scene, Entity entity);
        virtual ~LocalVolumetricFog();

        LocalVolumetricFog(const LocalVolumetricFog& rhs);
        LocalVolumetricFog& operator=(const LocalVolumetricFog& rhs);
        LocalVolumetricFog(LocalVolumetricFog&& rhs) noexcept;
        LocalVolumetricFog& operator=(LocalVolumetricFog&& rhs) noexcept;

        void setShape(FogVolumeShape shape);
        FogVolumeShape getShape() const;

        void setSize(Vector3 size);
        void setSize(const float x, const float y, const float z);
        Vector3 getSize() const;

        void setAttenuationDistance(float attenuationDistance);
        float getAttenuationDistance() const;

        void setAlbedo(Vector3 albedo);
        void setAlbedo(const float red, const float green, const float blue);
        Vector3 getAlbedo() const;

        void setBlendDistance(float blendDistance);
        float getBlendDistance() const;

        void setFalloff(FogVolumeFalloff falloff);
        FogVolumeFalloff getFalloff() const;

        void setNoiseIntensity(float noiseIntensity);
        float getNoiseIntensity() const;

        void setNoiseScale(float noiseScale);
        float getNoiseScale() const;

        void setScrollSpeed(Vector3 scrollSpeed);
        void setScrollSpeed(const float x, const float y, const float z);
        Vector3 getScrollSpeed() const;
    };
}

#endif //LOCALVOLUMETRICFOG_H
