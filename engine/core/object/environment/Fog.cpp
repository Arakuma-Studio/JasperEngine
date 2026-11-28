// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Fog.h"
#include "util/Color.h"
#include "render/AtmospherePresets.h"
#include "render/FogResolve.h"

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

// The look fields: while the game runs, a write goes to the runtime layer above the profile and
// the zones (docs/specs/S101-vapor.md R43) and a read shows it; otherwise they are the Fog's own.

Vector3 Fog::getColor() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return Color::linearTosRGB(FogScene::readSource(fog, FogField::Color).color);
}

void Fog::setColor(Vector3 color){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::Color).color = Color::sRGBToLinear(color);
}

float Fog::getDensity() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return FogScene::readSource(fog, FogField::Density).density;
}

void Fog::setDensity(float density){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::Density).density = density;
}

float Fog::getLinearStart() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return FogScene::readSource(fog, FogField::LinearStart).linearStart;
}

void Fog::setLinearStart(float linearStart){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::LinearStart).linearStart = linearStart;
}

float Fog::getLinearEnd() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return FogScene::readSource(fog, FogField::LinearEnd).linearEnd;
}

void Fog::setLinearEnd(float linearEnd){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::LinearEnd).linearEnd = linearEnd;
}

FogColorMode Fog::getColorMode() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return FogScene::readSource(fog, FogField::ColorMode).colorMode;
}

void Fog::setColorMode(FogColorMode colorMode){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::ColorMode).colorMode = colorMode;
}

float Fog::getAttenuationDistance() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return FogScene::readSource(fog, FogField::AttenuationDistance).attenuationDistance;
}

void Fog::setAttenuationDistance(float attenuationDistance){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::AttenuationDistance).attenuationDistance = attenuationDistance;
}

float Fog::getBaseHeight() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return FogScene::readSource(fog, FogField::BaseHeight).baseHeight;
}

void Fog::setBaseHeight(float baseHeight){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::BaseHeight).baseHeight = baseHeight;
}

float Fog::getMaximumHeight() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return FogScene::readSource(fog, FogField::MaximumHeight).maximumHeight;
}

void Fog::setMaximumHeight(float maximumHeight){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::MaximumHeight).maximumHeight = maximumHeight;
}

float Fog::getStartDistance() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return FogScene::readSource(fog, FogField::StartDistance).startDistance;
}

void Fog::setStartDistance(float startDistance){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::StartDistance).startDistance = startDistance;
}

float Fog::getMaxFogDistance() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return FogScene::readSource(fog, FogField::MaxFogDistance).maxFogDistance;
}

void Fog::setMaxFogDistance(float maxFogDistance){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::MaxFogDistance).maxFogDistance = maxFogDistance;
}

float Fog::getMaxOpacity() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return FogScene::readSource(fog, FogField::MaxOpacity).maxOpacity;
}

void Fog::setMaxOpacity(float maxOpacity){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::MaxOpacity).maxOpacity = maxOpacity;
}

float Fog::getDirectionalInscattering() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return FogScene::readSource(fog, FogField::DirectionalInscattering).directionalInscattering;
}

void Fog::setDirectionalInscattering(float directionalInscattering){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::DirectionalInscattering).directionalInscattering = directionalInscattering;
}

float Fog::getAnisotropy() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return FogScene::readSource(fog, FogField::Anisotropy).anisotropy;
}

void Fog::setAnisotropy(float anisotropy){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::Anisotropy).anisotropy = anisotropy;
}

bool Fog::isSkyFill() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return FogScene::readSource(fog, FogField::SkyFill).skyFill;
}

void Fog::setSkyFill(bool skyFill){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::SkyFill).skyFill = skyFill;
}

float Fog::getSkyFillAngle() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return FogScene::readSource(fog, FogField::SkyFillAngle).skyFillAngle;
}

void Fog::setSkyFillAngle(float skyFillAngle){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::SkyFillAngle).skyFillAngle = skyFillAngle;
}

float Fog::getNoiseIntensity() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return FogScene::readSource(fog, FogField::NoiseIntensity).noiseIntensity;
}

void Fog::setNoiseIntensity(float noiseIntensity){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::NoiseIntensity).noiseIntensity = noiseIntensity;
}

float Fog::getNoiseScale() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return FogScene::readSource(fog, FogField::NoiseScale).noiseScale;
}

void Fog::setNoiseScale(float noiseScale){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::NoiseScale).noiseScale = noiseScale;
}

Vector3 Fog::getWind() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return FogScene::readSource(fog, FogField::Wind).wind;
}

void Fog::setWind(Vector3 wind){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::Wind).wind = wind;
}

Vector3 Fog::getAlbedo() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return Color::linearTosRGB(FogScene::readSource(fog, FogField::Albedo).albedo);
}

void Fog::setAlbedo(Vector3 albedo){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::Albedo).albedo = Color::sRGBToLinear(albedo);
}

float Fog::getAmbientMultiplier() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return FogScene::readSource(fog, FogField::AmbientMultiplier).ambientMultiplier;
}

void Fog::setAmbientMultiplier(float ambientMultiplier){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::AmbientMultiplier).ambientMultiplier = ambientMultiplier;
}

Vector3 Fog::getEmissive() const{
    const FogComponent& fog = getComponent<FogComponent>();
    return Color::linearTosRGB(FogScene::readSource(fog, FogField::Emissive).emissive);
}

void Fog::setEmissive(Vector3 emissive){
    FogScene::writeTarget(scene, getComponent<FogComponent>(), FogField::Emissive).emissive = Color::sRGBToLinear(emissive);
}

void Fog::setColor(float red, float green, float blue){
    setColor(Vector3(red, green, blue));
}

void Fog::setLinearStartEnd(float start, float end){
    setLinearStart(start);
    setLinearEnd(end);
}

void Fog::setWind(float x, float y, float z){
    setWind(Vector3(x, y, z));
}

// what sets cost or technique is never layered: always the Fog's own

FogType Fog::getType() const{
    return getComponent<FogComponent>().type;
}

void Fog::setType(FogType type){
    getComponent<FogComponent>().type = type;
}

bool Fog::isVolumetric() const{
    return getComponent<FogComponent>().volumetric;
}

void Fog::setVolumetric(bool volumetric){
    getComponent<FogComponent>().volumetric = volumetric;
}

float Fog::getVolumetricDistance() const{
    return getComponent<FogComponent>().volumetricDistance;
}

void Fog::setVolumetricDistance(float volumetricDistance){
    getComponent<FogComponent>().volumetricDistance = volumetricDistance;
}

bool Fog::isLightShafts() const{
    return getComponent<FogComponent>().lightShafts;
}

void Fog::setLightShafts(bool lightShafts){
    getComponent<FogComponent>().lightShafts = lightShafts;
}

AtmosphereQualityOverride Fog::getQualityOverride() const{
    return getComponent<FogComponent>().qualityOverride;
}

void Fog::setQualityOverride(AtmosphereQualityOverride qualityOverride){
    getComponent<FogComponent>().qualityOverride = qualityOverride;
}

bool Fog::applyPreset(const std::string& preset){
    return AtmospherePresets::applyFogPreset(preset, getComponent<FogComponent>());
}

std::string Fog::getProfile() const{
    return FogScene::profileKey(getComponent<FogComponent>());
}

void Fog::setProfile(const std::string& profile){
    FogComponent& fog = getComponent<FogComponent>();
    if (FogScene::isRunning(scene)){
        FogScene::setRuntimeProfile(fog, profile);
    }else{
        if (FogRuntime* runtime = fog.runtime.get()){
            runtime->profileOverridden = false;
            runtime->blending = false;
        }
        fog.profile = profile;
    }
}

void Fog::blendTo(const std::string& profile, float seconds){
    FogScene::blendTo(getComponent<FogComponent>(), profile, seconds);
}

bool Fog::isBlending() const{
    return FogScene::blendState(getComponent<FogComponent>()).active;
}

float Fog::getBlendProgress() const{
    return FogScene::blendState(getComponent<FogComponent>()).progress;
}

std::string Fog::getBlendTarget() const{
    return FogScene::blendState(getComponent<FogComponent>()).targetKey;
}

FogComponent Fog::getResolved() const{
    FogResolve resolve;
    FogScene::resolveAtMainCamera(scene, getComponent<FogComponent>(), resolve);
    return resolve.resolved;
}

bool Fog::clearOverride(const std::string& field){
    FogField id;
    if (!FogFields::find(field, id))
        return false;
    FogScene::clearOverride(getComponent<FogComponent>(), id);
    return true;
}

void Fog::clearOverrides(){
    FogScene::clearOverrides(getComponent<FogComponent>());
}
