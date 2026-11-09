// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CameraShaker.h"

#include "Log.h"
#include "camera/ShakerRuntime.h"
#include "subsystem/CameraControlSystem.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

CameraShaker::CameraShaker(Scene* scene, Entity entity): EntityHandle(scene, entity){
    if (!scene->findComponent<CameraShakerComponent>(entity)){
        scene->addComponent<CameraShakerComponent>(entity, {});
    }
}

CameraShaker::~CameraShaker(){
}

CameraShaker::CameraShaker(const CameraShaker& rhs) = default;
CameraShaker& CameraShaker::operator=(const CameraShaker& rhs) = default;
CameraShaker::CameraShaker(CameraShaker&& rhs) noexcept = default;
CameraShaker& CameraShaker::operator=(CameraShaker&& rhs) noexcept = default;

ShakeInstanceHandle CameraShaker::play(const ShakeSource& source, const ShakeOptions& options, bool fromPoint,
                                       const Vector3& point, float radius){
    ShakeInstanceHandle handle;
    CameraShakerComponent* shaker = scene->findComponent<CameraShakerComponent>(entity);
    if (!shaker) return handle;
    camera::ShakePreset preset;
    if (!CameraShake::resolveSource(source, preset)) return handle;
    auto system = scene->getSystem<CameraControlSystem>();
    const float scale = fromPoint ? system->distanceScale(entity, *shaker, point, radius) : 1.0f;
    const uint64_t id = system->play(entity, *shaker, preset, source.name, options, scale);
    if (id != 0) handle.add(shaker->runtime, id);
    return handle;
}

ShakeInstanceHandle CameraShaker::shake(const ShakeSource& preset){
    return play(preset, ShakeOptions(), false, Vector3(0, 0, 0), 0.0f);
}

ShakeInstanceHandle CameraShaker::shake(const ShakeSource& preset, const ShakeOptions& options){
    return play(preset, options, false, Vector3(0, 0, 0), 0.0f);
}

ShakeInstanceHandle CameraShaker::shakeAt(const Vector3& position, float radius, const ShakeSource& preset){
    return play(preset, ShakeOptions(), true, position, radius);
}

ShakeInstanceHandle CameraShaker::shakeAt(const Vector3& position, float radius, const ShakeSource& preset, const ShakeOptions& options){
    return play(preset, options, true, position, radius);
}

void CameraShaker::addTrauma(float amount){
    CameraControlSystem::addTrauma(getComponent<CameraShakerComponent>(), amount);
}

void CameraShaker::stopAll(){
    stopAll(-1.0f);
}

void CameraShaker::stopAll(float fade){
    CameraControlSystem::stopAll(getComponent<CameraShakerComponent>(), fade);
}

int CameraShaker::getInstanceCount() const{
    const CameraShakerComponent& shaker = getComponent<CameraShakerComponent>();
    return shaker.runtime ? (int)shaker.runtime->instances.size() : 0;
}

void CameraShaker::setTrauma(float trauma){
    if (!std::isfinite(trauma)) return;
    camera::ShakerRuntime& runtime = CameraControlSystem::ensureRuntime(getComponent<CameraShakerComponent>());
    runtime.trauma.trauma = std::max(0.0f, std::min(1.0f, trauma));
}

float CameraShaker::getTrauma() const{
    const CameraShakerComponent& shaker = getComponent<CameraShakerComponent>();
    return shaker.runtime ? shaker.runtime->trauma.trauma : 0.0f;
}

Vector3 CameraShaker::getOffsetPosition() const{
    const CameraShakerComponent& shaker = getComponent<CameraShakerComponent>();
    return shaker.runtime ? shaker.runtime->offsetPosition : Vector3(0, 0, 0);
}

Vector3 CameraShaker::getOffsetRotation() const{
    const CameraShakerComponent& shaker = getComponent<CameraShakerComponent>();
    return shaker.runtime ? shaker.runtime->offsetRotation : Vector3(0, 0, 0);
}

void CameraShaker::setEnabled(bool enabled){
    getComponent<CameraShakerComponent>().enabled = enabled;
}

bool CameraShaker::isEnabled() const{
    return getComponent<CameraShakerComponent>().enabled;
}

void CameraShaker::setGlobal(bool global){
    getComponent<CameraShakerComponent>().global = global;
}

bool CameraShaker::isGlobal() const{
    return getComponent<CameraShakerComponent>().global;
}

void CameraShaker::setUnscaledTime(bool unscaled){
    getComponent<CameraShakerComponent>().unscaledTime = unscaled;
}

bool CameraShaker::isUnscaledTime() const{
    return getComponent<CameraShakerComponent>().unscaledTime;
}

std::string CameraShaker::modeToString(CameraShakeMode mode){
    switch (mode){
        case CameraShakeMode::Mode3D: return "3d";
        case CameraShakeMode::Mode2D: return "2d";
        default: return "auto";
    }
}

bool CameraShaker::modeFromString(const std::string& text, CameraShakeMode& out){
    if (text == "auto"){ out = CameraShakeMode::Auto; return true; }
    if (text == "3d"){ out = CameraShakeMode::Mode3D; return true; }
    if (text == "2d"){ out = CameraShakeMode::Mode2D; return true; }
    return false;
}

void CameraShaker::setMode(const std::string& mode){
    CameraShakeMode value;
    if (!modeFromString(mode, value)){
        Log::warn("CameraShaker.mode: '%s' is not \"auto\", \"3d\" or \"2d\"", mode.c_str());
        return;
    }
    getComponent<CameraShakerComponent>().mode = value;
}

std::string CameraShaker::getMode() const{
    return modeToString(getComponent<CameraShakerComponent>().mode);
}

void CameraShaker::setPositionScale(float scale){
    getComponent<CameraShakerComponent>().positionScale = std::isfinite(scale) ? std::max(0.0f, scale) : 1.0f;
}

float CameraShaker::getPositionScale() const{
    return getComponent<CameraShakerComponent>().positionScale;
}

void CameraShaker::setRotationScale(float scale){
    getComponent<CameraShakerComponent>().rotationScale = std::isfinite(scale) ? std::max(0.0f, scale) : 1.0f;
}

float CameraShaker::getRotationScale() const{
    return getComponent<CameraShakerComponent>().rotationScale;
}

void CameraShaker::setStartPreset(const std::string& preset){
    getComponent<CameraShakerComponent>().startPreset = preset;
}

std::string CameraShaker::getStartPreset() const{
    return getComponent<CameraShakerComponent>().startPreset;
}

void CameraShaker::setTraumaDecay(float decay){
    getComponent<CameraShakerComponent>().traumaDecay = std::isfinite(decay) ? std::max(0.0f, decay) : 1.0f;
}

float CameraShaker::getTraumaDecay() const{
    return getComponent<CameraShakerComponent>().traumaDecay;
}

void CameraShaker::setTraumaPower(float power){
    getComponent<CameraShakerComponent>().traumaPower = std::isfinite(power) ? std::max(1.0f, std::min(4.0f, power)) : 2.0f;
}

float CameraShaker::getTraumaPower() const{
    return getComponent<CameraShakerComponent>().traumaPower;
}

void CameraShaker::setTraumaMaxOffset(Vector3 offset){
    getComponent<CameraShakerComponent>().traumaMaxOffset = offset;
}

Vector3 CameraShaker::getTraumaMaxOffset() const{
    return getComponent<CameraShakerComponent>().traumaMaxOffset;
}

void CameraShaker::setTraumaMaxAngle(Vector3 angle){
    getComponent<CameraShakerComponent>().traumaMaxAngle = angle;
}

Vector3 CameraShaker::getTraumaMaxAngle() const{
    return getComponent<CameraShakerComponent>().traumaMaxAngle;
}

void CameraShaker::setTraumaFrequency(float frequency){
    getComponent<CameraShakerComponent>().traumaFrequency = std::isfinite(frequency) ? std::max(0.0f, frequency) : 15.0f;
}

float CameraShaker::getTraumaFrequency() const{
    return getComponent<CameraShakerComponent>().traumaFrequency;
}

void CameraShaker::setPixelSnap(bool snap){
    getComponent<CameraShakerComponent>().pixelSnap = snap;
}

bool CameraShaker::isPixelSnap() const{
    return getComponent<CameraShakerComponent>().pixelSnap;
}

void CameraShaker::setTickRate(float hz){
    getComponent<CameraShakerComponent>().tickRate = std::isfinite(hz) ? std::max(0.0f, hz) : 0.0f;
}

float CameraShaker::getTickRate() const{
    return getComponent<CameraShakerComponent>().tickRate;
}
