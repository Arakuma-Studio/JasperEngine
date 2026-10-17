// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LIGHT_H
#define LIGHT_H

#include "GameObject.h"

namespace jasperengine{

    class JASPERENGINE_API Light: public GameObject{
    public:

        // component checkbox (Unity Behaviour.enabled): a disabled Light is skipped by its system
        void setEnabled(bool enabled);
        bool isEnabled() const;

        // Unity Light.cullingMask: layers this light lights and takes shadows from
        void setCullingMask(uint32_t cullingMask);
        uint32_t getCullingMask() const;

        // stage evaluating the light on MIXED-lighting materials (PIXEL and VERTEX
        // materials evaluate every light their own way); BOTH goes per pixel there
        void setShading(LightShading shading);
        LightShading getShading() const;

        Light(Scene* scene);
        Light(Scene* scene, Entity entity);
        virtual ~Light();

        Light(const Light& rhs);
        Light& operator=(const Light& rhs);
        Light(Light&& rhs) noexcept;
        Light& operator=(Light&& rhs) noexcept;

        void setType(LightType type);
        LightType getType() const;

        void setDirection(Vector3 direction);
        void setDirection(const float x, const float y, const float z);
        Vector3 getDirection() const;

        void setColor(Vector3 color);
        void setColor(const float r, const float g, const float b);
        Vector3 getColor() const;

        void setRange(float range);
        float getRange() const;

        void setIntensity(float intensity);
        float getIntensity() const;

        void setConeAngle(float inner, float outer);
        void setInnerConeAngle(float inner);
        float getInnerConeAngle() const;
        void setOuterConeAngle(float outer);
        float getOuterConeAngle() const;

        void setSpotMask(const std::string& texture);
        void setSpotMask(const std::string& id, TextureData data);
        void clearSpotMask();
        Texture getSpotMask() const;

        void setShadows(bool shadows);
        bool isShadows() const;

        void setBias(float bias);
        float getBias() const;

        void setShadowMapSize(unsigned int size);
        unsigned int getShadowMapSize() const;

        void setShadowCameraNearFar(float nearValue, float farValue);
        void setCameraNear(float nearValue);
        float getCameraNear() const;
        void setCameraFar(float farValue);
        float getCameraFar() const;

        void setAutomaticShadowCamera(bool automatic);
        bool isAutomaticShadowCamera() const;

        void setNumCascades(unsigned int numCascades);
        float getNumCascades() const;
    };
}

#endif //LIGHT_H
