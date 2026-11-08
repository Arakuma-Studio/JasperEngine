// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Light.h"

#include <algorithm>

#include "util/Angle.h"
#include "util/Color.h"
#include "subsystem/RenderSystem.h"
#include "component/Transform.h"
#include "util/LightPhotometry.h"
#include <algorithm>
#include <cmath>
#include <math.h>

using namespace jasperengine;

Light::Light(Scene* scene): GameObject(scene){
    addComponent<LightComponent>();
}

Light::Light(Scene* scene, Entity entity): GameObject(scene, entity){
}

Light::~Light(){
}

Light::Light(const Light& rhs) = default;
Light& Light::operator=(const Light& rhs) = default;
Light::Light(Light&& rhs) noexcept = default;
Light& Light::operator=(Light&& rhs) noexcept = default;

void Light::setType(LightType type){
    LightComponent& lightcomp = getComponent<LightComponent>();

    if (lightcomp.type != type){
        lightcomp.type = type;
        // a unit the new type cannot take: its neighbour, the same number (lux is candela at 1 m)
        lightcomp.intensityUnit = LightPhotometry::validUnit(lightcomp.intensityUnit, type);

        lightcomp.needUpdateShadowCamera = true;
        lightcomp.needUpdateShadowMap = true;
        markChanged<LightComponent>();
    }
}

LightType Light::getType() const{
    LightComponent& lightcomp = getComponent<LightComponent>();

    return lightcomp.type;
}

void Light::setDirection(Vector3 direction){
    LightComponent& lightcomp = getComponent<LightComponent>();
    Transform& transform = getComponent<Transform>();

    if (lightcomp.direction != direction){
        lightcomp.direction = direction;

        transform.needUpdate = true;
        markChanged<Transform>();
    }
}

void Light::setDirection(const float x, const float y, const float z){
    setDirection(Vector3(x,y,z));
}

Vector3 Light::getDirection() const{
    LightComponent& lightcomp = getComponent<LightComponent>();

    return lightcomp.direction;
}

Vector3 Light::getWorldDirection() const{
    return getComponent<LightComponent>().worldDirection;
}

void Light::setColor(Vector3 color){
    LightComponent& lightcomp = getComponent<LightComponent>();

    lightcomp.color = Color::sRGBToLinear(color);
}

void Light::setColor(const float r, const float g, const float b){
    setColor(Vector3(r,g,b));
}

Vector3 Light::getColor() const{
    LightComponent& lightcomp = getComponent<LightComponent>();

    return Color::linearTosRGB(lightcomp.color);
}

void Light::setRange(float range){
    LightComponent& lightcomp = getComponent<LightComponent>();

    if (lightcomp.range != range){
        lightcomp.range = range;

        lightcomp.needUpdateShadowCamera = true;
        markChanged<LightComponent>();
    }
}

float Light::getRange() const{
    LightComponent& lightcomp = getComponent<LightComponent>();

    return lightcomp.range;
}

void Light::setIntensity(float intensity){
    LightComponent& lightcomp = getComponent<LightComponent>();

    if (intensity > 0 && lightcomp.intensity == 0){
        lightcomp.needUpdateShadowCamera = true;
        markChanged<LightComponent>();
    }

    lightcomp.intensity = intensity;
}

float  Light::getIntensity() const{
    LightComponent& lightcomp = getComponent<LightComponent>();

    return lightcomp.intensity;
}

void Light::setIntensityUnit(LightUnit unit){
    setIntensityUnit(unit, false);
}

void Light::setIntensityUnit(LightUnit unit, bool preserveBrightness){
    LightComponent& lightcomp = getComponent<LightComponent>();

    LightPhotometry::setIntensityUnit(lightcomp, unit, preserveBrightness, scene->getPhysicalLightEV100());
}

LightUnit Light::getIntensityUnit() const{
    LightComponent& lightcomp = getComponent<LightComponent>();

    return lightcomp.intensityUnit;
}

void Light::setUseColorTemperature(bool use){
    LightComponent& lightcomp = getComponent<LightComponent>();

    lightcomp.useColorTemperature = use;
}

bool Light::isUseColorTemperature() const{
    LightComponent& lightcomp = getComponent<LightComponent>();

    return lightcomp.useColorTemperature;
}

void Light::setColorTemperature(float kelvin){
    LightComponent& lightcomp = getComponent<LightComponent>();

    if (std::isfinite(kelvin))
        lightcomp.colorTemperature = std::clamp(kelvin, LightPhotometry::MIN_TEMPERATURE, LightPhotometry::MAX_TEMPERATURE);
}

float Light::getColorTemperature() const{
    LightComponent& lightcomp = getComponent<LightComponent>();

    return lightcomp.colorTemperature;
}

void Light::setConeAngle(float inner, float outer){
    LightComponent& lightcomp = getComponent<LightComponent>();

    float innerConeCos = cos(Angle::defaultToRad(inner / 2));
    float outerConeCos = cos(Angle::defaultToRad(outer / 2));

    if (lightcomp.innerConeCos != innerConeCos || lightcomp.outerConeCos != outerConeCos){
        lightcomp.innerConeCos = innerConeCos;
        lightcomp.outerConeCos = outerConeCos;

        lightcomp.needUpdateShadowCamera = true;
        markChanged<LightComponent>();
    }
}

void Light::setInnerConeAngle(float inner){
    LightComponent& lightcomp = getComponent<LightComponent>();

    float innerConeCos = cos(Angle::defaultToRad(inner / 2));

    if (lightcomp.innerConeCos != innerConeCos){
        lightcomp.innerConeCos = innerConeCos;

        lightcomp.needUpdateShadowCamera = true;
        markChanged<LightComponent>();
    }
}

float Light::getInnerConeAngle() const{
    LightComponent& lightcomp = getComponent<LightComponent>();

    return lightcomp.innerConeCos;
}

void Light::setOuterConeAngle(float outer){
    LightComponent& lightcomp = getComponent<LightComponent>();

    float outerConeCos = cos(Angle::defaultToRad(outer / 2));

    if (lightcomp.outerConeCos != outerConeCos){
        lightcomp.outerConeCos = outerConeCos;

        lightcomp.needUpdateShadowCamera = true;
        markChanged<LightComponent>();
    }
}

float Light::getOuterConeAngle() const{
    LightComponent& lightcomp = getComponent<LightComponent>();

    return lightcomp.outerConeCos;
}

void Light::setSpotMask(const std::string& texture){
    LightComponent& lightcomp = getComponent<LightComponent>();
    lightcomp.spotMask.setPath(texture);
    lightcomp.spotMaskReady = false;
    lightcomp.needUpdateShadowCamera = true;
    markChanged<LightComponent>();
}

void Light::setSpotMask(const std::string& id, TextureData data){
    LightComponent& lightcomp = getComponent<LightComponent>();
    lightcomp.spotMask.setData(id, data);
    lightcomp.spotMaskReady = false;
    lightcomp.needUpdateShadowCamera = true;
    markChanged<LightComponent>();
}

void Light::clearSpotMask(){
    LightComponent& lightcomp = getComponent<LightComponent>();
    lightcomp.spotMask = Texture();
    lightcomp.spotMaskReady = false;
    lightcomp.spotMaskAspect = 1.0f;
    lightcomp.needUpdateShadowCamera = true;
    markChanged<LightComponent>();
}

Texture Light::getSpotMask() const{
    return getComponent<LightComponent>().spotMask;
}

void Light::setShadows(bool shadows){
    LightComponent& lightcomp = getComponent<LightComponent>();

    if (lightcomp.shadows != shadows){
        lightcomp.shadows = shadows;

        lightcomp.needUpdateShadowCamera = true;
        markChanged<LightComponent>();
        scene->getSystem<RenderSystem>()->needReloadMeshVariants();
    }
}

bool Light::isShadows() const{
    LightComponent& lightcomp = getComponent<LightComponent>();

    return lightcomp.shadows;
}

void Light::setBias(float bias){
    LightComponent& lightcomp = getComponent<LightComponent>();

    lightcomp.shadowBias = bias;
}

float Light::getBias() const{
    LightComponent& lightcomp = getComponent<LightComponent>();

    return lightcomp.shadowBias;
}

void Light::setShadowMapSize(unsigned int size){
    LightComponent& lightcomp = getComponent<LightComponent>();

    if (lightcomp.mapResolution != size){
        lightcomp.mapResolution = size;

        lightcomp.needUpdateShadowMap = true;
        markChanged<LightComponent>();
    }
}

unsigned int Light::getShadowMapSize() const{
    LightComponent& lightcomp = getComponent<LightComponent>();

    return lightcomp.mapResolution;
}

void Light::setShadowCameraNearFar(float nearValue, float farValue){
    LightComponent& lightcomp = getComponent<LightComponent>();

    if (lightcomp.shadowCameraNearFar != Vector2(nearValue, farValue)){
        lightcomp.shadowCameraNearFar = Vector2(nearValue, farValue);

        lightcomp.automaticShadowCamera = false;
        lightcomp.needUpdateShadowCamera = true;
        markChanged<LightComponent>();
    }
}

void Light::setCameraNear(float nearValue){
    LightComponent& lightcomp = getComponent<LightComponent>();

    if (lightcomp.shadowCameraNearFar.x != nearValue){
        lightcomp.shadowCameraNearFar.x = nearValue;

        lightcomp.automaticShadowCamera = false;
        lightcomp.needUpdateShadowCamera = true;
        markChanged<LightComponent>();
    }
}

float Light::getCameraNear() const{
    LightComponent& lightcomp = getComponent<LightComponent>();

    return lightcomp.shadowCameraNearFar.x;
}

void Light::setCameraFar(float farValue){
    LightComponent& lightcomp = getComponent<LightComponent>();

    if (lightcomp.shadowCameraNearFar.y != farValue){
        lightcomp.shadowCameraNearFar.y = farValue;

        lightcomp.automaticShadowCamera = false;
        lightcomp.needUpdateShadowCamera = true;
        markChanged<LightComponent>();
    }
}

float Light::getCameraFar() const{
    LightComponent& lightcomp = getComponent<LightComponent>();

    return lightcomp.shadowCameraNearFar.y;
}

void Light::setAutomaticShadowCamera(bool automatic){
    LightComponent& lightcomp = getComponent<LightComponent>();

    if (lightcomp.automaticShadowCamera != automatic){
        lightcomp.automaticShadowCamera = automatic;

        lightcomp.needUpdateShadowCamera = true;
        markChanged<LightComponent>();
    }
}

bool Light::isAutomaticShadowCamera() const{
    LightComponent& lightcomp = getComponent<LightComponent>();

    return lightcomp.automaticShadowCamera;
}

void Light::setNumCascades(unsigned int numCascades){
    LightComponent& lightcomp = getComponent<LightComponent>();

    if (lightcomp.numShadowCascades != numCascades){
        lightcomp.numShadowCascades = numCascades;

        lightcomp.needUpdateShadowCamera = true;
        markChanged<LightComponent>();
    }
}

float Light::getNumCascades() const{
    LightComponent& lightcomp = getComponent<LightComponent>();

    return lightcomp.numShadowCascades;
}

void Light::setShadowType(ShadowType type){
    LightComponent& lightcomp = getComponent<LightComponent>();
    const bool hadShadows = lightcomp.shadows;
    lightcomp.setShadowType(type);
    if (hadShadows != lightcomp.shadows){
        lightcomp.needUpdateShadowCamera = true;
        scene->getSystem<RenderSystem>()->needReloadMeshVariants();
    }
}

ShadowType Light::getShadowType() const{
    return getComponent<LightComponent>().getShadowType();
}

void Light::setShadowStrength(float strength){
    getComponent<LightComponent>().shadowStrength = std::clamp(strength, 0.0f, 1.0f);
}

float Light::getShadowStrength() const{
    return getComponent<LightComponent>().shadowStrength;
}

void Light::setShadowNormalBias(float bias){
    getComponent<LightComponent>().shadowNormalBias = std::max(bias, 0.0f);
}

float Light::getShadowNormalBias() const{
    return getComponent<LightComponent>().shadowNormalBias;
}

void Light::setCascadeSplitMode(CascadeSplitMode mode){
    getComponent<LightComponent>().cascadeSplitMode = mode;
}

CascadeSplitMode Light::getCascadeSplitMode() const{
    return getComponent<LightComponent>().cascadeSplitMode;
}

void Light::setCascadeSplits(Vector3 splits){
    getComponent<LightComponent>().cascadeSplits = splits;
}

Vector3 Light::getCascadeSplits() const{
    return getComponent<LightComponent>().cascadeSplits;
}

void Light::setShadowDistance(float distance){
    getComponent<LightComponent>().shadowDistance = std::max(distance, 0.0f);
}

float Light::getShadowDistance() const{
    return getComponent<LightComponent>().shadowDistance;
}

void Light::setShadowAngularDiameter(float degrees){
    getComponent<LightComponent>().shadowAngularDiameter = std::clamp(degrees, 0.0f, 90.0f);
}

float Light::getShadowAngularDiameter() const{
    return getComponent<LightComponent>().shadowAngularDiameter;
}

void Light::setShadowSourceRadius(float radius){
    getComponent<LightComponent>().shadowSourceRadius = std::max(radius, 0.0f);
}

float Light::getShadowSourceRadius() const{
    return getComponent<LightComponent>().shadowSourceRadius;
}

void Light::setShadowUpdateMode(ShadowUpdateMode mode){
    getComponent<LightComponent>().shadowUpdateMode = mode;
}

ShadowUpdateMode Light::getShadowUpdateMode() const{
    return getComponent<LightComponent>().shadowUpdateMode;
}

void Light::requestShadowUpdate(){
    getComponent<LightComponent>().shadowUpdateRequested = true;
}

void Light::setContactShadows(bool contactShadows){
    getComponent<LightComponent>().contactShadows = contactShadows;
}

bool Light::isContactShadows() const{
    return getComponent<LightComponent>().contactShadows;
}

void Light::setContactShadowLength(float length){
    if (std::isfinite(length))
        getComponent<LightComponent>().contactShadowLength = std::max(length, 0.0f);
}

float Light::getContactShadowLength() const{
    return getComponent<LightComponent>().contactShadowLength;
}

void Light::setContactShadowThickness(float thickness){
    if (std::isfinite(thickness))
        getComponent<LightComponent>().contactShadowThickness = std::max(thickness, 0.0f);
}

float Light::getContactShadowThickness() const{
    return getComponent<LightComponent>().contactShadowThickness;
}

void Light::setContactShadowSampleCount(int samples){
    getComponent<LightComponent>().contactShadowSampleCount = std::clamp(samples, 8, 16);
}

int Light::getContactShadowSampleCount() const{
    return getComponent<LightComponent>().contactShadowSampleCount;
}

void Light::setEnabled(bool enabled){
    getComponent<LightComponent>().enabled = enabled;
}

bool Light::isEnabled() const{
    return getComponent<LightComponent>().enabled;
}

void Light::setCullingMask(uint32_t cullingMask){
    getComponent<LightComponent>().cullingMask = cullingMask;
}

uint32_t Light::getCullingMask() const{
    return getComponent<LightComponent>().cullingMask;
}

void Light::setShading(LightShading shading){
    getComponent<LightComponent>().shading = shading;
}

void Light::setVolumetricIntensity(float volumetricIntensity){
    getComponent<LightComponent>().volumetricIntensity = volumetricIntensity;
}

float Light::getVolumetricIntensity() const{
    return getComponent<LightComponent>().volumetricIntensity;
}

LightShading Light::getShading() const{
    return getComponent<LightComponent>().shading;
}
