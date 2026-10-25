// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VolumetricClouds.h"
#include "render/AtmospherePresets.h"

using namespace jasperengine;

VolumetricClouds::VolumetricClouds(Scene* scene): EntityHandle(scene){
    addComponent<VolumetricCloudsComponent>();
}

VolumetricClouds::VolumetricClouds(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

VolumetricClouds::~VolumetricClouds(){
}

VolumetricClouds::VolumetricClouds(const VolumetricClouds& rhs) = default;
VolumetricClouds& VolumetricClouds::operator=(const VolumetricClouds& rhs) = default;
VolumetricClouds::VolumetricClouds(VolumetricClouds&& rhs) noexcept = default;
VolumetricClouds& VolumetricClouds::operator=(VolumetricClouds&& rhs) noexcept = default;

void VolumetricClouds::setPreset(CloudsPreset preset){
    AtmospherePresets::applyCloudsPreset(preset, getComponent<VolumetricCloudsComponent>());
}

CloudsPreset VolumetricClouds::getPreset() const{
    return getComponent<VolumetricCloudsComponent>().preset;
}

void VolumetricClouds::setRendering(CloudsRendering rendering){
    getComponent<VolumetricCloudsComponent>().rendering = rendering;
}

CloudsRendering VolumetricClouds::getRendering() const{
    return getComponent<VolumetricCloudsComponent>().rendering;
}

void VolumetricClouds::setCoverage(float coverage){
    getComponent<VolumetricCloudsComponent>().coverage = coverage;
}

float VolumetricClouds::getCoverage() const{
    return getComponent<VolumetricCloudsComponent>().coverage;
}

void VolumetricClouds::setDensity(float density){
    getComponent<VolumetricCloudsComponent>().density = density;
}

float VolumetricClouds::getDensity() const{
    return getComponent<VolumetricCloudsComponent>().density;
}

void VolumetricClouds::setShapeScale(float shapeScale){
    getComponent<VolumetricCloudsComponent>().shapeScale = shapeScale;
}

float VolumetricClouds::getShapeScale() const{
    return getComponent<VolumetricCloudsComponent>().shapeScale;
}

void VolumetricClouds::setShapeFactor(float shapeFactor){
    getComponent<VolumetricCloudsComponent>().shapeFactor = shapeFactor;
}

float VolumetricClouds::getShapeFactor() const{
    return getComponent<VolumetricCloudsComponent>().shapeFactor;
}

void VolumetricClouds::setErosionFactor(float erosionFactor){
    getComponent<VolumetricCloudsComponent>().erosionFactor = erosionFactor;
}

float VolumetricClouds::getErosionFactor() const{
    return getComponent<VolumetricCloudsComponent>().erosionFactor;
}

void VolumetricClouds::setErosionScale(float erosionScale){
    getComponent<VolumetricCloudsComponent>().erosionScale = erosionScale;
}

float VolumetricClouds::getErosionScale() const{
    return getComponent<VolumetricCloudsComponent>().erosionScale;
}

void VolumetricClouds::setBottomAltitude(float bottomAltitude){
    getComponent<VolumetricCloudsComponent>().bottomAltitude = bottomAltitude;
}

float VolumetricClouds::getBottomAltitude() const{
    return getComponent<VolumetricCloudsComponent>().bottomAltitude;
}

void VolumetricClouds::setAltitudeRange(float altitudeRange){
    getComponent<VolumetricCloudsComponent>().altitudeRange = altitudeRange;
}

float VolumetricClouds::getAltitudeRange() const{
    return getComponent<VolumetricCloudsComponent>().altitudeRange;
}

void VolumetricClouds::setEarthCurvature(float earthCurvature){
    getComponent<VolumetricCloudsComponent>().earthCurvature = earthCurvature;
}

float VolumetricClouds::getEarthCurvature() const{
    return getComponent<VolumetricCloudsComponent>().earthCurvature;
}

void VolumetricClouds::setWindDirection(float windDirection){
    getComponent<VolumetricCloudsComponent>().windDirection = windDirection;
}

float VolumetricClouds::getWindDirection() const{
    return getComponent<VolumetricCloudsComponent>().windDirection;
}

void VolumetricClouds::setWindSpeed(float windSpeed){
    getComponent<VolumetricCloudsComponent>().windSpeed = windSpeed;
}

float VolumetricClouds::getWindSpeed() const{
    return getComponent<VolumetricCloudsComponent>().windSpeed;
}

void VolumetricClouds::setWeatherMapScale(float weatherMapScale){
    getComponent<VolumetricCloudsComponent>().weatherMapScale = weatherMapScale;
}

float VolumetricClouds::getWeatherMapScale() const{
    return getComponent<VolumetricCloudsComponent>().weatherMapScale;
}

void VolumetricClouds::setSunLightDimmer(float sunLightDimmer){
    getComponent<VolumetricCloudsComponent>().sunLightDimmer = sunLightDimmer;
}

float VolumetricClouds::getSunLightDimmer() const{
    return getComponent<VolumetricCloudsComponent>().sunLightDimmer;
}

void VolumetricClouds::setAmbientLightDimmer(float ambientLightDimmer){
    getComponent<VolumetricCloudsComponent>().ambientLightDimmer = ambientLightDimmer;
}

float VolumetricClouds::getAmbientLightDimmer() const{
    return getComponent<VolumetricCloudsComponent>().ambientLightDimmer;
}

void VolumetricClouds::setMaxDistance(float maxDistance){
    getComponent<VolumetricCloudsComponent>().maxDistance = maxDistance;
}

float VolumetricClouds::getMaxDistance() const{
    return getComponent<VolumetricCloudsComponent>().maxDistance;
}

void VolumetricClouds::setQualityOverride(AtmosphereQualityOverride qualityOverride){
    getComponent<VolumetricCloudsComponent>().qualityOverride = qualityOverride;
}

AtmosphereQualityOverride VolumetricClouds::getQualityOverride() const{
    return getComponent<VolumetricCloudsComponent>().qualityOverride;
}

void VolumetricClouds::setWeatherMap(const std::string& path){
    VolumetricCloudsComponent& clouds = getComponent<VolumetricCloudsComponent>();

    if (path.empty()){
        clouds.weatherMap.destroy();
        clouds.weatherMap = Texture();
    }else{
        clouds.weatherMap.setPath(path);
    }
}

std::string VolumetricClouds::getWeatherMap() const{
    return getComponent<VolumetricCloudsComponent>().weatherMap.getPath();
}
