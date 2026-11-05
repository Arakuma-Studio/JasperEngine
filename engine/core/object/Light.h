// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LIGHT_H
#define LIGHT_H

#include "GameObject.h"
#include "render/shadow/ShadowTypes.h"

namespace jasperengine{

    enum class LightType;
    enum class LightShading;
    enum class LightUnit;

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

        // how much of this point or spot light the volumetric fog scatters (0: none)
        void setVolumetricIntensity(float volumetricIntensity);
        float getVolumetricIntensity() const;

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

        // Photometric units (util/LightPhotometry.h): what the intensity is in. ENGINE, LUX
        // (directional), CANDELA or LUMEN (point, spot); a unit the type cannot take draws as
        // its neighbour. setIntensityUnit(unit, true) converts the intensity so the light stays
        // as bright (through the scene's Physical Light Exposure); the property sets the unit
        // alone (the number then means the new unit).
        void setIntensityUnit(LightUnit unit);
        void setIntensityUnit(LightUnit unit, bool preserveBrightness);
        LightUnit getIntensityUnit() const;

        // A black body's colour at colorTemperature (Kelvin, 1000-20000; 6500 is white), with
        // the light's colour as its filter
        void setUseColorTemperature(bool use);
        bool isUseColorTemperature() const;
        void setColorTemperature(float kelvin);
        float getColorTemperature() const;

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

        // Shadows (docs/rendering/shadows.md): Unity's Shadow Type (NONE, HARD, SOFT)
        void setShadowType(ShadowType type);
        ShadowType getShadowType() const;
        void setShadowStrength(float strength);
        float getShadowStrength() const;
        // in shadow texels (1 by default)
        void setShadowNormalBias(float bias);
        float getShadowNormalBias() const;
        void setCascadeSplitMode(CascadeSplitMode mode);
        CascadeSplitMode getCascadeSplitMode() const;
        // MANUAL: the first three splits as fractions of the shadow distance
        void setCascadeSplits(Vector3 splits);
        Vector3 getCascadeSplits() const;
        // metres of directional shadows; 0: the scene's, else the quality tier's
        void setShadowDistance(float distance);
        float getShadowDistance() const;
        // degrees of a directional light's disc
        void setShadowAngularDiameter(float degrees);
        float getShadowAngularDiameter() const;
        // metres, spot and point lights
        void setShadowSourceRadius(float radius);
        float getShadowSourceRadius() const;
        void setShadowUpdateMode(ShadowUpdateMode mode);
        ShadowUpdateMode getShadowUpdateMode() const;
        // redraws the light's shadow maps (On Demand lights)
        void requestShadowUpdate();

        // contact shadows (directional and spot lights with shadows): the march's reach and the
        // surfaces' thickness in metres, its samples (8..16)
        void setContactShadows(bool contactShadows);
        bool isContactShadows() const;
        void setContactShadowLength(float length);
        float getContactShadowLength() const;
        void setContactShadowThickness(float thickness);
        float getContactShadowThickness() const;
        void setContactShadowSampleCount(int samples);
        int getContactShadowSampleCount() const;
    };
}

#endif //LIGHT_H
