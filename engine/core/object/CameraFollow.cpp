// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CameraFollow.h"

#include "Log.h"
#include "manager/SceneManager.h"
#include "subsystem/CameraControlSystem.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace{

    float nonNegative(float value, float fallback){
        return std::isfinite(value) ? std::max(0.0f, value) : fallback;
    }

}

CameraFollow::CameraFollow(Scene* scene, Entity entity): EntityHandle(scene, entity){
    if (!scene->findComponent<CameraFollowComponent>(entity)){
        scene->addComponent<CameraFollowComponent>(entity, {});
    }
}

CameraFollow::~CameraFollow(){
}

CameraFollow::CameraFollow(const CameraFollow& rhs) = default;
CameraFollow& CameraFollow::operator=(const CameraFollow& rhs) = default;
CameraFollow::CameraFollow(CameraFollow&& rhs) noexcept = default;
CameraFollow& CameraFollow::operator=(CameraFollow&& rhs) noexcept = default;

void CameraFollow::snap(){
    CameraControlSystem::snap(getComponent<CameraFollowComponent>());
}

void CameraFollow::setTarget(Entity target){
    CameraFollowComponent& follow = getComponent<CameraFollowComponent>();
    follow.target = target;
    follow.targetPersistent = false;
}

bool CameraFollow::setTarget(Scene* targetScene, Entity target){
    if (target == NULL_ENTITY || targetScene == scene){
        setTarget(target);
        return true;
    }
    if (targetScene && targetScene == SceneManager::getPersistentScene()){
        CameraFollowComponent& follow = getComponent<CameraFollowComponent>();
        follow.target = target;
        follow.targetPersistent = true;
        return true;
    }
    Log::warn("CameraFollow.target: '%s' is in another scene; a Camera Follow follows a GameObject of its own scene "
              "or of the persistent (DontDestroyOnLoad) scene", targetScene ? targetScene->getEntityName(target).c_str() : "?");
    return false;
}

Scene* CameraFollow::getTargetScene() const{
    const CameraFollowComponent& follow = getComponent<CameraFollowComponent>();
    return follow.targetPersistent ? SceneManager::getPersistentScene() : scene;
}

Entity CameraFollow::getTarget() const{
    return getComponent<CameraFollowComponent>().target;
}

void CameraFollow::setOffset(Vector3 offset){
    getComponent<CameraFollowComponent>().offset = offset;
}

Vector3 CameraFollow::getOffset() const{
    return getComponent<CameraFollowComponent>().offset;
}

std::string CameraFollow::offsetSpaceToString(FollowOffsetSpace space){
    switch (space){
        case FollowOffsetSpace::Target: return "target";
        case FollowOffsetSpace::TargetYaw: return "target_yaw";
        default: return "world";
    }
}

bool CameraFollow::offsetSpaceFromString(const std::string& text, FollowOffsetSpace& out){
    if (text == "world"){ out = FollowOffsetSpace::World; return true; }
    if (text == "target"){ out = FollowOffsetSpace::Target; return true; }
    if (text == "target_yaw"){ out = FollowOffsetSpace::TargetYaw; return true; }
    return false;
}

void CameraFollow::setOffsetSpace(const std::string& space){
    FollowOffsetSpace value;
    if (!offsetSpaceFromString(space, value)){
        Log::warn("CameraFollow.offsetSpace: '%s' is not \"world\", \"target\" or \"target_yaw\"", space.c_str());
        return;
    }
    getComponent<CameraFollowComponent>().offsetSpace = value;
}

std::string CameraFollow::getOffsetSpace() const{
    return offsetSpaceToString(getComponent<CameraFollowComponent>().offsetSpace);
}

void CameraFollow::setDamping(Vector3 damping){
    getComponent<CameraFollowComponent>().damping = Vector3(nonNegative(damping.x, 0.2f), nonNegative(damping.y, 0.2f), nonNegative(damping.z, 0.2f));
}

Vector3 CameraFollow::getDamping() const{
    return getComponent<CameraFollowComponent>().damping;
}

void CameraFollow::setMaxSpeed(float maxSpeed){
    getComponent<CameraFollowComponent>().maxSpeed = nonNegative(maxSpeed, 0.0f);
}

float CameraFollow::getMaxSpeed() const{
    return getComponent<CameraFollowComponent>().maxSpeed;
}

void CameraFollow::setLookAt(bool lookAt){
    getComponent<CameraFollowComponent>().lookAt = lookAt;
}

bool CameraFollow::isLookAt() const{
    return getComponent<CameraFollowComponent>().lookAt;
}

void CameraFollow::setLookAtOffset(Vector3 offset){
    getComponent<CameraFollowComponent>().lookAtOffset = offset;
}

Vector3 CameraFollow::getLookAtOffset() const{
    return getComponent<CameraFollowComponent>().lookAtOffset;
}

void CameraFollow::setRotationDamping(float damping){
    getComponent<CameraFollowComponent>().rotationDamping = nonNegative(damping, 0.1f);
}

float CameraFollow::getRotationDamping() const{
    return getComponent<CameraFollowComponent>().rotationDamping;
}

void CameraFollow::setDeadZone(Vector2 deadZone){
    getComponent<CameraFollowComponent>().deadZone = Vector2(nonNegative(deadZone.x, 0.0f), nonNegative(deadZone.y, 0.0f));
}

Vector2 CameraFollow::getDeadZone() const{
    return getComponent<CameraFollowComponent>().deadZone;
}

void CameraFollow::setUseBounds(bool useBounds){
    getComponent<CameraFollowComponent>().useBounds = useBounds;
}

bool CameraFollow::isUseBounds() const{
    return getComponent<CameraFollowComponent>().useBounds;
}

void CameraFollow::setBoundsMin(Vector3 boundsMin){
    getComponent<CameraFollowComponent>().boundsMin = boundsMin;
}

Vector3 CameraFollow::getBoundsMin() const{
    return getComponent<CameraFollowComponent>().boundsMin;
}

void CameraFollow::setBoundsMax(Vector3 boundsMax){
    getComponent<CameraFollowComponent>().boundsMax = boundsMax;
}

Vector3 CameraFollow::getBoundsMax() const{
    return getComponent<CameraFollowComponent>().boundsMax;
}

void CameraFollow::setUnscaledTime(bool unscaled){
    getComponent<CameraFollowComponent>().unscaledTime = unscaled;
}

bool CameraFollow::isUnscaledTime() const{
    return getComponent<CameraFollowComponent>().unscaledTime;
}

void CameraFollow::setEnabled(bool enabled){
    getComponent<CameraFollowComponent>().enabled = enabled;
}

bool CameraFollow::isEnabled() const{
    return getComponent<CameraFollowComponent>().enabled;
}
