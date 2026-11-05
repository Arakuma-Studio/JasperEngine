// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CapsuleShadowCaster.h"

#include "Log.h"
#include "component/Transform.h"
#include "render/CapsuleShadows.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

using namespace jasperengine;

CapsuleShadowCaster::CapsuleShadowCaster(Scene* scene): GameObject(scene){
    addComponent<CapsuleShadowCasterComponent>();
}

CapsuleShadowCaster::CapsuleShadowCaster(Scene* scene, Entity entity): GameObject(scene, entity){
}

CapsuleShadowCaster::~CapsuleShadowCaster(){
}

CapsuleShadowCaster::CapsuleShadowCaster(const CapsuleShadowCaster& rhs) = default;
CapsuleShadowCaster& CapsuleShadowCaster::operator=(const CapsuleShadowCaster& rhs) = default;
CapsuleShadowCaster::CapsuleShadowCaster(CapsuleShadowCaster&& rhs) noexcept = default;
CapsuleShadowCaster& CapsuleShadowCaster::operator=(CapsuleShadowCaster&& rhs) noexcept = default;

void CapsuleShadowCaster::setEnabled(bool enabled){
    getComponent<CapsuleShadowCasterComponent>().enabled = enabled;
}

bool CapsuleShadowCaster::isEnabled() const{
    return getComponent<CapsuleShadowCasterComponent>().enabled;
}

void CapsuleShadowCaster::setCastShadows(CapsuleCastShadows castShadows){
    getComponent<CapsuleShadowCasterComponent>().castShadows = castShadows;
}

CapsuleCastShadows CapsuleShadowCaster::getCastShadows() const{
    return getComponent<CapsuleShadowCasterComponent>().castShadows;
}

int CapsuleShadowCaster::addCapsule(const std::string& bone, float radius, float length){
    CapsuleShadowCasterComponent& caster = getComponent<CapsuleShadowCasterComponent>();
    if ((int)caster.capsules.size() >= CapsuleShadowCasterComponent::MAX_CAPSULES){
        Log::warn("A Capsule Shadow Caster holds at most %i capsules", CapsuleShadowCasterComponent::MAX_CAPSULES);
        return -1;
    }
    CapsuleShadowShape shape;
    shape.bone = bone;
    shape.radius = std::isfinite(radius) ? std::max(radius, 0.0f) : shape.radius;
    shape.length = std::isfinite(length) ? std::max(length, 0.0f) : shape.length;
    caster.capsules.push_back(shape);
    return (int)caster.capsules.size() - 1;
}

void CapsuleShadowCaster::removeCapsule(int index){
    CapsuleShadowCasterComponent& caster = getComponent<CapsuleShadowCasterComponent>();
    if (index >= 0 && index < (int)caster.capsules.size())
        caster.capsules.erase(caster.capsules.begin() + index);
}

void CapsuleShadowCaster::clearCapsules(){
    getComponent<CapsuleShadowCasterComponent>().capsules.clear();
}

int CapsuleShadowCaster::getCapsuleCount() const{
    return (int)getComponent<CapsuleShadowCasterComponent>().capsules.size();
}

namespace {
    CapsuleShadowShape* capsuleAt(CapsuleShadowCasterComponent& caster, int index){
        if (index < 0 || index >= (int)caster.capsules.size()){
            Log::error("No capsule %i (the caster has %i)", index, (int)caster.capsules.size());
            return nullptr;
        }
        return &caster.capsules[(size_t)index];
    }
}

std::string CapsuleShadowCaster::getCapsuleBone(int index) const{
    const CapsuleShadowShape* shape = capsuleAt(getComponent<CapsuleShadowCasterComponent>(), index);
    return shape ? shape->bone : std::string();
}

void CapsuleShadowCaster::setCapsuleBone(int index, const std::string& bone){
    if (CapsuleShadowShape* shape = capsuleAt(getComponent<CapsuleShadowCasterComponent>(), index))
        shape->bone = bone;
}

float CapsuleShadowCaster::getCapsuleRadius(int index) const{
    const CapsuleShadowShape* shape = capsuleAt(getComponent<CapsuleShadowCasterComponent>(), index);
    return shape ? shape->radius : 0.0f;
}

void CapsuleShadowCaster::setCapsuleRadius(int index, float radius){
    if (CapsuleShadowShape* shape = capsuleAt(getComponent<CapsuleShadowCasterComponent>(), index)){
        if (std::isfinite(radius))
            shape->radius = std::max(radius, 0.0f);
    }
}

float CapsuleShadowCaster::getCapsuleLength(int index) const{
    const CapsuleShadowShape* shape = capsuleAt(getComponent<CapsuleShadowCasterComponent>(), index);
    return shape ? shape->length : 0.0f;
}

void CapsuleShadowCaster::setCapsuleLength(int index, float length){
    if (CapsuleShadowShape* shape = capsuleAt(getComponent<CapsuleShadowCasterComponent>(), index)){
        if (std::isfinite(length))
            shape->length = std::max(length, 0.0f);
    }
}

Vector3 CapsuleShadowCaster::getCapsuleOffset(int index) const{
    const CapsuleShadowShape* shape = capsuleAt(getComponent<CapsuleShadowCasterComponent>(), index);
    return shape ? shape->offset : Vector3(0.0f, 0.0f, 0.0f);
}

void CapsuleShadowCaster::setCapsuleOffset(int index, Vector3 offset){
    if (CapsuleShadowShape* shape = capsuleAt(getComponent<CapsuleShadowCasterComponent>(), index))
        shape->offset = offset;
}

CapsuleDirection CapsuleShadowCaster::getCapsuleDirection(int index) const{
    const CapsuleShadowShape* shape = capsuleAt(getComponent<CapsuleShadowCasterComponent>(), index);
    return shape ? shape->direction : CapsuleDirection::Y_AXIS;
}

void CapsuleShadowCaster::setCapsuleDirection(int index, CapsuleDirection direction){
    if (CapsuleShadowShape* shape = capsuleAt(getComponent<CapsuleShadowCasterComponent>(), index))
        shape->direction = direction;
}

bool CapsuleShadowCaster::fitToSkeleton(){
    // the bones: this GameObject's branch, in hierarchy order, by name
    auto transforms = scene->getComponentArray<Transform>();
    if (!scene->findComponent<Transform>(entity))
        return false;
    const size_t first = transforms->getIndex(entity);
    const size_t last = scene->findBranchLastIndex(entity);
    std::vector<CapsuleShadows::Bone> bones;
    std::unordered_map<Entity, int> indexOf;
    for (size_t k = first; k <= last && k < transforms->size(); k++){
        const Entity boneEntity = transforms->getEntity(k);
        const Transform& transform = transforms->getComponentFromIndex(k);
        CapsuleShadows::Bone bone;
        bone.name = scene->getEntityName(boneEntity);
        auto parent = indexOf.find(transform.parent);
        bone.parent = (parent != indexOf.end()) ? parent->second : -1;
        bone.position = transform.worldPosition;
        bone.rotation = transform.worldRotation;
        bone.scale = transform.worldScale;
        indexOf[boneEntity] = (int)bones.size();
        bones.push_back(bone);
    }
    std::vector<CapsuleShadowShape> capsules;
    std::string error;
    if (!CapsuleShadows::fitToSkeleton(bones, capsules, &error)){
        Log::warn("Fit To Skeleton: %s", error.c_str());
        return false;
    }
    getComponent<CapsuleShadowCasterComponent>().capsules = capsules;
    return true;
}
