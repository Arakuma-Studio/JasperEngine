// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Animator.h"

#include "Log.h"
#include "Scene.h"
#include "animation/AnimatorInstance.h"
#include "animation/CompiledController.h"
#include "component/Transform.h"
#include "subsystem/AnimatorSystem.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

Animator::Animator(Scene* scene, Entity entity): EntityHandle(scene, entity){
    if (!scene->findComponent<AnimatorComponent>(entity)){
        scene->addComponent<AnimatorComponent>(entity, {});
    }
}

Animator::~Animator(){
}

Animator::Animator(const Animator& rhs) = default;
Animator& Animator::operator=(const Animator& rhs) = default;
Animator::Animator(Animator&& rhs) noexcept = default;
Animator& Animator::operator=(Animator&& rhs) noexcept = default;

anim::AnimatorInstance* Animator::runtime() const{
    return scene->getSystem<AnimatorSystem>()->instance(entity);
}

void Animator::unknown(const std::string& what, const std::string& name) const{
    anim::AnimatorInstance* instance = runtime();
    const std::string message = what + " \"" + name + "\"";
    if (!instance || instance->warnOnce(message)){
        Log::warn("Animator '%s': unknown %s", scene->getEntityName(entity).c_str(), message.c_str());
    }
}

void Animator::setController(const std::string& path){
    AnimatorComponent& component = getComponent<AnimatorComponent>();
    component.controller = path;
    component.needsLoad = true;
}

std::string Animator::getController() const{
    return getComponent<AnimatorComponent>().controller.path;
}

bool Animator::isLoaded() const{
    anim::AnimatorInstance* instance = runtime();
    return instance && instance->controller && instance->bound;
}

namespace{

    const char* parameterKindName(AnimatorParameterType type){
        switch (type){
            case AnimatorParameterType::Float: return "float";
            case AnimatorParameterType::Int: return "int";
            case AnimatorParameterType::Bool: return "bool";
            case AnimatorParameterType::Trigger: return "trigger";
        }
        return "";
    }

}

void Animator::mismatch(const std::string& name, AnimatorParameterType as) const{
    anim::AnimatorInstance* instance = runtime();
    if (!instance || !instance->controller) return;
    const int index = instance->controller->findParameter(name);
    if (index < 0) return;
    const AnimatorParameterType type = instance->controller->parameters[(size_t)index].type;
    // a float set as an int (and back) is a number either way; a bool or trigger is not
    const bool numbers = (type == AnimatorParameterType::Float || type == AnimatorParameterType::Int) &&
                         (as == AnimatorParameterType::Float || as == AnimatorParameterType::Int);
    if (type == as || numbers) return;
    const std::string message = "'" + name + "' is a " + parameterKindName(type) + ", set as a " + parameterKindName(as);
    if (instance->warnOnce(message)){
        Log::warn("Animator '%s': %s (converted)", scene->getEntityName(entity).c_str(), message.c_str());
    }
}

void Animator::setFloat(const std::string& name, float value){
    anim::AnimatorInstance* instance = runtime();
    if (!instance || !instance->setFloat(name, value)) unknown("parameter", name);
    else mismatch(name, AnimatorParameterType::Float);
}

void Animator::setFloat(const std::string& name, float value, float dampTime, float deltaTime){
    anim::AnimatorInstance* instance = runtime();
    if (!instance || !instance->setFloat(name, value, dampTime, deltaTime)) unknown("parameter", name);
}

float Animator::getFloat(const std::string& name) const{
    anim::AnimatorInstance* instance = runtime();
    if (!instance || instance->findParameter(name) < 0){
        unknown("parameter", name);
        return 0.0f;
    }
    return instance->getParameter(name);
}

void Animator::setInteger(const std::string& name, int value){
    anim::AnimatorInstance* instance = runtime();
    if (!instance || !instance->setInteger(name, value)) unknown("parameter", name);
    else mismatch(name, AnimatorParameterType::Int);
}

int Animator::getInteger(const std::string& name) const{
    return (int)std::lround(getFloat(name));
}

void Animator::setBool(const std::string& name, bool value){
    anim::AnimatorInstance* instance = runtime();
    if (!instance || !instance->setBool(name, value)) unknown("parameter", name);
    else mismatch(name, AnimatorParameterType::Bool);
}

bool Animator::getBool(const std::string& name) const{
    return getFloat(name) != 0.0f;
}

void Animator::setTrigger(const std::string& name){
    anim::AnimatorInstance* instance = runtime();
    if (!instance || !instance->setTrigger(name)) unknown("parameter", name);
    else mismatch(name, AnimatorParameterType::Trigger);
}

void Animator::resetTrigger(const std::string& name){
    anim::AnimatorInstance* instance = runtime();
    if (!instance || !instance->resetTrigger(name)) unknown("parameter", name);
}

int Animator::getParameterIndex(const std::string& name) const{
    anim::AnimatorInstance* instance = runtime();
    return instance ? instance->findParameter(name) : -1;
}

int Animator::getParameterCount() const{
    anim::AnimatorInstance* instance = runtime();
    return instance && instance->controller ? (int)instance->controller->parameters.size() : 0;
}

std::string Animator::getParameterName(int index) const{
    anim::AnimatorInstance* instance = runtime();
    if (!instance || !instance->controller || index < 0 || (size_t)index >= instance->controller->parameters.size()) return std::string();
    return instance->controller->parameters[(size_t)index].name;
}

void Animator::play(const std::string& state){
    play(state, -1, -1.0f);
}

void Animator::play(const std::string& state, int layer){
    play(state, layer, -1.0f);
}

void Animator::play(const std::string& state, int layer, float normalizedTime){
    anim::AnimatorInstance* instance = runtime();
    if (!instance || !instance->play(state, layer, normalizedTime)) unknown("state", state);
}

void Animator::crossFade(const std::string& state, float normalizedDuration){
    crossFade(state, normalizedDuration, -1, 0.0f);
}

void Animator::crossFade(const std::string& state, float normalizedDuration, int layer){
    crossFade(state, normalizedDuration, layer, 0.0f);
}

void Animator::crossFade(const std::string& state, float normalizedDuration, int layer, float normalizedOffset){
    anim::AnimatorInstance* instance = runtime();
    if (!instance || !instance->crossFade(state, normalizedDuration, false, layer, normalizedOffset)) unknown("state", state);
}

void Animator::crossFadeInFixedTime(const std::string& state, float seconds){
    crossFadeInFixedTime(state, seconds, -1, 0.0f);
}

void Animator::crossFadeInFixedTime(const std::string& state, float seconds, int layer){
    crossFadeInFixedTime(state, seconds, layer, 0.0f);
}

void Animator::crossFadeInFixedTime(const std::string& state, float seconds, int layer, float normalizedOffset){
    anim::AnimatorInstance* instance = runtime();
    if (!instance || !instance->crossFade(state, seconds, true, layer, normalizedOffset)) unknown("state", state);
}

namespace{

    AnimatorStateInfo toInfo(const anim::AnimatorInstance::StateInfo& info){
        AnimatorStateInfo out;
        out.valid = info.state >= 0;
        out.name = info.name;
        out.fullPath = info.fullPath;
        out.tag = info.tag;
        out.normalizedTime = info.normalizedTime;
        out.length = info.length;
        out.loop = info.loop;
        out.speed = info.speed;
        out.mirror = info.mirror;
        out.footIK = info.footIK;
        return out;
    }

}

AnimatorStateInfo Animator::getCurrentState(int layer) const{
    anim::AnimatorInstance* instance = runtime();
    return instance ? toInfo(instance->currentState(layer)) : AnimatorStateInfo();
}

AnimatorStateInfo Animator::getNextState(int layer) const{
    anim::AnimatorInstance* instance = runtime();
    return instance ? toInfo(instance->nextState(layer)) : AnimatorStateInfo();
}

bool Animator::isInTransition(int layer) const{
    anim::AnimatorInstance* instance = runtime();
    return instance && instance->isInTransition(layer);
}

int Animator::getLayerCount() const{
    anim::AnimatorInstance* instance = runtime();
    return instance ? (int)instance->layers.size() : 0;
}

std::string Animator::getLayerName(int layer) const{
    anim::AnimatorInstance* instance = runtime();
    if (!instance || !instance->controller || layer < 0 || (size_t)layer >= instance->controller->layers.size()) return std::string();
    return instance->controller->layers[(size_t)layer].name;
}

int Animator::getLayerIndex(const std::string& name) const{
    anim::AnimatorInstance* instance = runtime();
    return instance ? instance->getLayerIndex(name) : -1;
}

void Animator::setLayerWeight(int layer, float weight){
    anim::AnimatorInstance* instance = runtime();
    if (instance) instance->setLayerWeight(layer, weight);
}

void Animator::setLayerWeight(int layer, float weight, float fade){
    anim::AnimatorInstance* instance = runtime();
    if (instance) instance->setLayerWeight(layer, weight, fade);
}

float Animator::getLayerWeight(int layer) const{
    anim::AnimatorInstance* instance = runtime();
    return instance ? instance->getLayerWeight(layer) : 0.0f;
}

bool Animator::playMontage(const std::string& path){
    return playMontage(path, 1.0f, std::string());
}

bool Animator::playMontage(const std::string& path, float rate){
    return playMontage(path, rate, std::string());
}

bool Animator::playMontage(const std::string& path, float rate, const std::string& section){
    std::string error;
    if (!scene->getSystem<AnimatorSystem>()->playMontage(entity, path, rate, section, &error)){
        anim::AnimatorInstance* instance = runtime();
        if (!instance || instance->warnOnce("montage " + path)){
            Log::warn("Animator '%s': %s", scene->getEntityName(entity).c_str(), error.c_str());
        }
        return false;
    }
    return true;
}

void Animator::stopMontage(){
    stopMontage(-1.0f);
}

void Animator::stopMontage(float blendOut){
    anim::AnimatorInstance* instance = runtime();
    if (instance) instance->stopMontage(nullptr, blendOut);
}

bool Animator::isMontagePlaying() const{
    anim::AnimatorInstance* instance = runtime();
    return instance && instance->isMontagePlaying();
}

std::string Animator::getMontage() const{
    anim::AnimatorInstance* instance = runtime();
    const anim::AnimatorMontageRun* montage = instance ? instance->activeMontage() : nullptr;
    return montage ? montage->path : std::string();
}

std::string Animator::getMontageSection() const{
    anim::AnimatorInstance* instance = runtime();
    const anim::AnimatorMontageRun* montage = instance ? instance->activeMontage() : nullptr;
    if (!montage || montage->section < 0 || (size_t)montage->section >= montage->montage->sections.size()) return std::string();
    return montage->montage->sections[(size_t)montage->section].name;
}

bool Animator::montageJumpToSection(const std::string& section){
    anim::AnimatorInstance* instance = runtime();
    return instance && instance->montageJumpToSection(section);
}

bool Animator::montageSetNextSection(const std::string& from, const std::string& to){
    anim::AnimatorInstance* instance = runtime();
    return instance && instance->montageSetNextSection(from, to);
}

namespace{

    int findConstraint(const AnimatorComponent& component, const std::string& name){
        for (size_t i = 0; i < component.ik.size(); i++){
            if (component.ik[i].name == name) return (int)i;
        }
        return -1;
    }

}

void Animator::setIKTarget(const std::string& constraint, const Vector3& position){
    anim::AnimatorInstance* instance = runtime();
    const int index = findConstraint(getComponent<AnimatorComponent>(), constraint);
    if (!instance || index < 0 || (size_t)index >= instance->ik.size()){
        unknown("IK constraint", constraint);
        return;
    }
    instance->ik[(size_t)index].scriptTarget = true;
    instance->ik[(size_t)index].scriptTargetWorld = position;
}

void Animator::setIKTarget(const std::string& constraint, const Vector3& position, float weight){
    setIKTarget(constraint, position);
    setIKWeight(constraint, weight);
}

void Animator::clearIKTarget(const std::string& constraint){
    anim::AnimatorInstance* instance = runtime();
    const int index = findConstraint(getComponent<AnimatorComponent>(), constraint);
    if (!instance || index < 0 || (size_t)index >= instance->ik.size()){
        unknown("IK constraint", constraint);
        return;
    }
    instance->ik[(size_t)index].scriptTarget = false;
    instance->ik[(size_t)index].scriptWeight = -1.0f;
}

void Animator::setIKWeight(const std::string& constraint, float weight){
    anim::AnimatorInstance* instance = runtime();
    const int index = findConstraint(getComponent<AnimatorComponent>(), constraint);
    if (!instance || index < 0 || (size_t)index >= instance->ik.size()){
        unknown("IK constraint", constraint);
        return;
    }
    instance->ik[(size_t)index].scriptWeight = std::isfinite(weight) ? std::min(std::max(weight, 0.0f), 1.0f) : 0.0f;
}

bool Animator::isIKReached(const std::string& constraint) const{
    const anim::AnimatorInstance* instance = runtime();
    const int index = findConstraint(getComponent<AnimatorComponent>(), constraint);
    if (!instance || index < 0 || (size_t)index >= instance->ik.size()){
        unknown("IK constraint", constraint);
        return false;
    }
    return instance->ik[(size_t)index].reached;
}

void Animator::setIKSoften(const std::string& constraint, float soften){
    AnimatorComponent& component = getComponent<AnimatorComponent>();
    const int index = findConstraint(component, constraint);
    if (index < 0){
        unknown("IK constraint", constraint);
        return;
    }
    component.ik[(size_t)index].soften = std::isfinite(soften) ? std::min(std::max(soften, 0.0f), 1.0f) : 0.0f;
}

void Animator::setIKHingeAxis(const std::string& constraint, const Vector3& axis){
    AnimatorComponent& component = getComponent<AnimatorComponent>();
    const int index = findConstraint(component, constraint);
    if (index < 0){
        unknown("IK constraint", constraint);
        return;
    }
    component.ik[(size_t)index].hingeAxis = axis;
}

void Animator::setIKTwist(const std::string& constraint, float degrees){
    AnimatorComponent& component = getComponent<AnimatorComponent>();
    const int index = findConstraint(component, constraint);
    if (index < 0){
        unknown("IK constraint", constraint);
        return;
    }
    component.ik[(size_t)index].twist = std::isfinite(degrees) ? degrees : 0.0f;
}

void Animator::setIKAimOffset(const std::string& constraint, const Vector3& offset){
    AnimatorComponent& component = getComponent<AnimatorComponent>();
    const int index = findConstraint(component, constraint);
    if (index < 0){
        unknown("IK constraint", constraint);
        return;
    }
    component.ik[(size_t)index].aimOffset = offset;
}

void Animator::setLookAt(const Vector3& position){
    const AnimatorComponent& component = getComponent<AnimatorComponent>();
    for (const AnimatorIKConstraint& c : component.ik){
        if (c.type == AnimatorIKType::LookAt){
            setIKTarget(c.name, position);
            return;
        }
    }
    unknown("IK constraint", "(a LookAt)");
}

void Animator::setLookAt(const Vector3& position, float weight){
    const AnimatorComponent& component = getComponent<AnimatorComponent>();
    for (const AnimatorIKConstraint& c : component.ik){
        if (c.type == AnimatorIKType::LookAt){
            setIKTarget(c.name, position, weight);
            return;
        }
    }
    unknown("IK constraint", "(a LookAt)");
}

bool Animator::hasBone(const std::string& bone) const{
    anim::AnimatorInstance* instance = runtime();
    return instance && instance->bound && instance->resolveBone(bone) >= 0;
}

Vector3 Animator::getBonePosition(const std::string& bone) const{
    anim::AnimatorInstance* instance = runtime();
    if (!instance || !instance->bound) return Vector3(0, 0, 0);
    const int index = instance->resolveBone(bone);
    if (index < 0 || (size_t)index >= instance->rigPositions().size()){
        unknown("bone", bone);
        return Vector3(0, 0, 0);
    }
    const Transform& root = getComponent<Transform>();
    return root.modelMatrix * instance->rigPositions()[(size_t)index];
}

Quaternion Animator::getBoneRotation(const std::string& bone) const{
    anim::AnimatorInstance* instance = runtime();
    if (!instance || !instance->bound) return Quaternion();
    const int index = instance->resolveBone(bone);
    if (index < 0 || (size_t)index >= instance->rigRotations().size()){
        unknown("bone", bone);
        return Quaternion();
    }
    const Transform& root = getComponent<Transform>();
    return root.worldRotation * instance->rigRotations()[(size_t)index];
}

void Animator::setSpeed(float speed){
    getComponent<AnimatorComponent>().speed = speed;
}

float Animator::getSpeed() const{
    return getComponent<AnimatorComponent>().speed;
}

void Animator::setApplyRootMotion(bool apply){
    getComponent<AnimatorComponent>().applyRootMotion = apply;
}

bool Animator::isApplyRootMotion() const{
    return getComponent<AnimatorComponent>().applyRootMotion;
}

Vector3 Animator::getDeltaPosition() const{
    anim::AnimatorInstance* instance = runtime();
    if (!instance || !instance->output.hasRootMotion) return Vector3(0, 0, 0);
    const Transform& root = getComponent<Transform>();
    return root.worldRotation * (root.worldScale * instance->output.rootDeltaPosition);
}

Quaternion Animator::getDeltaRotation() const{
    anim::AnimatorInstance* instance = runtime();
    if (!instance || !instance->output.hasRootMotion) return Quaternion();
    const float yaw = instance->output.rootDeltaYaw;
    const Transform& root = getComponent<Transform>();
    const Quaternion local(std::cos(0.5f * yaw), 0.0f, std::sin(0.5f * yaw), 0.0f);
    return root.worldRotation * local * root.worldRotation.inverse();
}

bool Animator::isHumanoid() const{
    anim::AnimatorInstance* instance = runtime();
    return instance && instance->bound && instance->humanoid.valid;
}

FunctionSubscribe<void(const AnimationEvent&)>& Animator::getOnEvent(){
    return getComponent<AnimatorComponent>().onEvent;
}

FunctionSubscribe<void(const std::string&, const std::string&)>& Animator::getOnStateEnter(){
    return getComponent<AnimatorComponent>().onStateEnter;
}

FunctionSubscribe<void(const std::string&, const std::string&)>& Animator::getOnStateExit(){
    return getComponent<AnimatorComponent>().onStateExit;
}

FunctionSubscribe<void(const std::string&, bool)>& Animator::getOnMontageEnded(){
    return getComponent<AnimatorComponent>().onMontageEnded;
}

FunctionSubscribe<void(Vector3, Quaternion)>& Animator::getOnAnimatorMove(){
    return getComponent<AnimatorComponent>().onAnimatorMove;
}
