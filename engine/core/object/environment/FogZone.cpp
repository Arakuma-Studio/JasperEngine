// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FogZone.h"
#include "render/FogResolve.h"

using namespace jasperengine;

FogZone::FogZone(Scene* scene): GameObject(scene){
    addComponent<FogZoneComponent>();
}

FogZone::FogZone(Scene* scene, Entity entity): GameObject(scene, entity){
}

FogZone::~FogZone(){
}

FogZone::FogZone(const FogZone& rhs) = default;
FogZone& FogZone::operator=(const FogZone& rhs) = default;
FogZone::FogZone(FogZone&& rhs) noexcept = default;
FogZone& FogZone::operator=(FogZone&& rhs) noexcept = default;

void FogZone::setShape(FogZoneShape shape){
    getComponent<FogZoneComponent>().shape = shape;
}

FogZoneShape FogZone::getShape() const{
    return getComponent<FogZoneComponent>().shape;
}

void FogZone::setSize(Vector3 size){
    getComponent<FogZoneComponent>().size = size;
}

void FogZone::setSize(const float x, const float y, const float z){
    setSize(Vector3(x, y, z));
}

Vector3 FogZone::getSize() const{
    return getComponent<FogZoneComponent>().size;
}

void FogZone::setBlendDistance(float blendDistance){
    getComponent<FogZoneComponent>().blendDistance = blendDistance;
}

float FogZone::getBlendDistance() const{
    return getComponent<FogZoneComponent>().blendDistance;
}

void FogZone::setPriority(int priority){
    getComponent<FogZoneComponent>().priority = priority;
}

int FogZone::getPriority() const{
    return getComponent<FogZoneComponent>().priority;
}

void FogZone::setWeight(float weight){
    getComponent<FogZoneComponent>().weight = weight;
}

float FogZone::getWeight() const{
    return getComponent<FogZoneComponent>().weight;
}

void FogZone::setProfile(const std::string& profile){
    getComponent<FogZoneComponent>().profile = profile;
}

std::string FogZone::getProfile() const{
    const AssetRef& profile = getComponent<FogZoneComponent>().profile;
    return !profile.path.empty() || profile.guid.isNull() ? profile.path : profile.guid.str();
}

float FogZone::getCurrentWeight() const{
    for (const FogZoneState& state : FogScene::zoneStates(scene, FogScene::mainCameraPosition(scene))){
        if (state.entity == entity)
            return state.weight;
    }
    return 0.0f;
}
