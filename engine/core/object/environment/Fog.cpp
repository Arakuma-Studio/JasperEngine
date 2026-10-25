// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Fog.h"
#include "util/Color.h"
#include "render/AtmospherePresets.h"

using namespace jasperengine;

Fog::Fog(Scene* scene): EntityHandle(scene){
    addComponent<FogComponent>();
}

Fog::Fog(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

Fog::~Fog(){
}

Fog::Fog(const Fog& rhs) = default;
Fog& Fog::operator=(const Fog& rhs) = default;
Fog::Fog(Fog&& rhs) noexcept = default;
Fog& Fog::operator=(Fog&& rhs) noexcept = default;

FogType Fog::getType() const{
    FogComponent& fog = getComponent<FogComponent>();
    return fog.type;
}

Vector3 Fog::getColor() const{
    FogComponent& fog = getComponent<FogComponent>();
    return Color::linearTosRGB(fog.color);
}

float Fog::getDensity() const{
    FogComponent& fog = getComponent<FogComponent>();
    return fog.density;
}

float Fog::getLinearStart() const{
    FogComponent& fog = getComponent<FogComponent>();
    return fog.linearStart;
}

float Fog::getLinearEnd() const{
    FogComponent& fog = getComponent<FogComponent>();
    return fog.linearEnd;
}

void Fog::setType(FogType type){
    FogComponent& fog = getComponent<FogComponent>();
    fog.type = type;
}

void Fog::setColor(Vector3 color){
    FogComponent& fog = getComponent<FogComponent>();
    fog.color = Color::sRGBToLinear(color);
}

void Fog::setColor(float red, float green, float blue){
    setColor(Vector3(red, green, blue));
}

void Fog::setDensity(float density){
    FogComponent& fog = getComponent<FogComponent>();
    fog.density = density;
}

void Fog::setLinearStart(float start){
    FogComponent& fog = getComponent<FogComponent>();
    fog.linearStart = start;
}

void Fog::setLinearEnd(float end){
    FogComponent& fog = getComponent<FogComponent>();
    fog.linearEnd = end;
}

void Fog::setLinearStartEnd(float start, float end){
    FogComponent& fog = getComponent<FogComponent>();
    fog.linearStart = start;
    fog.linearEnd = end;
}

FogColorMode Fog::getColorMode() const{
    return getComponent<FogComponent>().colorMode;
}

float Fog::getAttenuationDistance() const{
    return getComponent<FogComponent>().attenuationDistance;
}

float Fog::getBaseHeight() const{
    return getComponent<FogComponent>().baseHeight;
}

float Fog::getMaximumHeight() const{
    return getComponent<FogComponent>().maximumHeight;
}

float Fog::getStartDistance() const{
    return getComponent<FogComponent>().startDistance;
}

float Fog::getMaxFogDistance() const{
    return getComponent<FogComponent>().maxFogDistance;
}

float Fog::getMaxOpacity() const{
    return getComponent<FogComponent>().maxOpacity;
}

float Fog::getDirectionalInscattering() const{
    return getComponent<FogComponent>().directionalInscattering;
}

float Fog::getAnisotropy() const{
    return getComponent<FogComponent>().anisotropy;
}

bool Fog::isSkyFill() const{
    return getComponent<FogComponent>().skyFill;
}

float Fog::getSkyFillAngle() const{
    return getComponent<FogComponent>().skyFillAngle;
}

bool Fog::isVolumetric() const{
    return getComponent<FogComponent>().volumetric;
}

float Fog::getVolumetricDistance() const{
    return getComponent<FogComponent>().volumetricDistance;
}

bool Fog::isLightShafts() const{
    return getComponent<FogComponent>().lightShafts;
}

float Fog::getNoiseIntensity() const{
    return getComponent<FogComponent>().noiseIntensity;
}

float Fog::getNoiseScale() const{
    return getComponent<FogComponent>().noiseScale;
}

Vector3 Fog::getWind() const{
    return getComponent<FogComponent>().wind;
}

AtmosphereQualityOverride Fog::getQualityOverride() const{
    return getComponent<FogComponent>().qualityOverride;
}

void Fog::setColorMode(FogColorMode colorMode){
    getComponent<FogComponent>().colorMode = colorMode;
}

void Fog::setAttenuationDistance(float attenuationDistance){
    getComponent<FogComponent>().attenuationDistance = attenuationDistance;
}

void Fog::setBaseHeight(float baseHeight){
    getComponent<FogComponent>().baseHeight = baseHeight;
}

void Fog::setMaximumHeight(float maximumHeight){
    getComponent<FogComponent>().maximumHeight = maximumHeight;
}

void Fog::setStartDistance(float startDistance){
    getComponent<FogComponent>().startDistance = startDistance;
}

void Fog::setMaxFogDistance(float maxFogDistance){
    getComponent<FogComponent>().maxFogDistance = maxFogDistance;
}

void Fog::setMaxOpacity(float maxOpacity){
    getComponent<FogComponent>().maxOpacity = maxOpacity;
}

void Fog::setDirectionalInscattering(float directionalInscattering){
    getComponent<FogComponent>().directionalInscattering = directionalInscattering;
}

void Fog::setAnisotropy(float anisotropy){
    getComponent<FogComponent>().anisotropy = anisotropy;
}

void Fog::setSkyFill(bool skyFill){
    getComponent<FogComponent>().skyFill = skyFill;
}

void Fog::setSkyFillAngle(float skyFillAngle){
    getComponent<FogComponent>().skyFillAngle = skyFillAngle;
}

void Fog::setVolumetric(bool volumetric){
    getComponent<FogComponent>().volumetric = volumetric;
}

void Fog::setVolumetricDistance(float volumetricDistance){
    getComponent<FogComponent>().volumetricDistance = volumetricDistance;
}

void Fog::setLightShafts(bool lightShafts){
    getComponent<FogComponent>().lightShafts = lightShafts;
}

void Fog::setNoiseIntensity(float noiseIntensity){
    getComponent<FogComponent>().noiseIntensity = noiseIntensity;
}

void Fog::setNoiseScale(float noiseScale){
    getComponent<FogComponent>().noiseScale = noiseScale;
}

void Fog::setWind(Vector3 wind){
    getComponent<FogComponent>().wind = wind;
}

void Fog::setQualityOverride(AtmosphereQualityOverride qualityOverride){
    getComponent<FogComponent>().qualityOverride = qualityOverride;
}

void Fog::setWind(float x, float y, float z){
    setWind(Vector3(x, y, z));
}

bool Fog::applyPreset(const std::string& preset){
    return AtmospherePresets::applyFogPreset(preset, getComponent<FogComponent>());
}
