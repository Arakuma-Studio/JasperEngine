// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LocalVolumetricFog.h"
#include "util/Color.h"

using namespace jasperengine;

LocalVolumetricFog::LocalVolumetricFog(Scene* scene): GameObject(scene){
    addComponent<LocalVolumetricFogComponent>();
}

LocalVolumetricFog::LocalVolumetricFog(Scene* scene, Entity entity): GameObject(scene, entity){
}

LocalVolumetricFog::~LocalVolumetricFog(){
}

LocalVolumetricFog::LocalVolumetricFog(const LocalVolumetricFog& rhs) = default;
LocalVolumetricFog& LocalVolumetricFog::operator=(const LocalVolumetricFog& rhs) = default;
LocalVolumetricFog::LocalVolumetricFog(LocalVolumetricFog&& rhs) noexcept = default;
LocalVolumetricFog& LocalVolumetricFog::operator=(LocalVolumetricFog&& rhs) noexcept = default;

void LocalVolumetricFog::setShape(FogVolumeShape shape){
    getComponent<LocalVolumetricFogComponent>().shape = shape;
}

FogVolumeShape LocalVolumetricFog::getShape() const{
    return getComponent<LocalVolumetricFogComponent>().shape;
}

void LocalVolumetricFog::setSize(Vector3 size){
    getComponent<LocalVolumetricFogComponent>().size = size;
}

void LocalVolumetricFog::setSize(const float x, const float y, const float z){
    setSize(Vector3(x, y, z));
}

Vector3 LocalVolumetricFog::getSize() const{
    return getComponent<LocalVolumetricFogComponent>().size;
}

void LocalVolumetricFog::setAttenuationDistance(float attenuationDistance){
    getComponent<LocalVolumetricFogComponent>().attenuationDistance = attenuationDistance;
}

float LocalVolumetricFog::getAttenuationDistance() const{
    return getComponent<LocalVolumetricFogComponent>().attenuationDistance;
}

void LocalVolumetricFog::setAlbedo(Vector3 albedo){
    getComponent<LocalVolumetricFogComponent>().albedo = Color::sRGBToLinear(albedo);
}

void LocalVolumetricFog::setAlbedo(const float red, const float green, const float blue){
    setAlbedo(Vector3(red, green, blue));
}

Vector3 LocalVolumetricFog::getAlbedo() const{
    return Color::linearTosRGB(getComponent<LocalVolumetricFogComponent>().albedo);
}

void LocalVolumetricFog::setBlendDistance(float blendDistance){
    getComponent<LocalVolumetricFogComponent>().blendDistance = blendDistance;
}

float LocalVolumetricFog::getBlendDistance() const{
    return getComponent<LocalVolumetricFogComponent>().blendDistance;
}

void LocalVolumetricFog::setFalloff(FogVolumeFalloff falloff){
    getComponent<LocalVolumetricFogComponent>().falloff = falloff;
}

FogVolumeFalloff LocalVolumetricFog::getFalloff() const{
    return getComponent<LocalVolumetricFogComponent>().falloff;
}

void LocalVolumetricFog::setNoiseIntensity(float noiseIntensity){
    getComponent<LocalVolumetricFogComponent>().noiseIntensity = noiseIntensity;
}

float LocalVolumetricFog::getNoiseIntensity() const{
    return getComponent<LocalVolumetricFogComponent>().noiseIntensity;
}

void LocalVolumetricFog::setNoiseScale(float noiseScale){
    getComponent<LocalVolumetricFogComponent>().noiseScale = noiseScale;
}

float LocalVolumetricFog::getNoiseScale() const{
    return getComponent<LocalVolumetricFogComponent>().noiseScale;
}

void LocalVolumetricFog::setScrollSpeed(Vector3 scrollSpeed){
    getComponent<LocalVolumetricFogComponent>().scrollSpeed = scrollSpeed;
}

void LocalVolumetricFog::setScrollSpeed(const float x, const float y, const float z){
    setScrollSpeed(Vector3(x, y, z));
}

Vector3 LocalVolumetricFog::getScrollSpeed() const{
    return getComponent<LocalVolumetricFogComponent>().scrollSpeed;
}
