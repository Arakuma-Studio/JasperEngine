// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PoseAnimator.h"

#include "subsystem/PoseAnimatorSystem.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

PoseAnimator::PoseAnimator(Scene* scene, Entity entity): EntityHandle(scene, entity){
    if (!scene->findComponent<PoseAnimatorComponent>(entity)){
        scene->addComponent<PoseAnimatorComponent>(entity, {});
    }
}

PoseAnimator::~PoseAnimator(){
}

PoseAnimator::PoseAnimator(const PoseAnimator& rhs) = default;
PoseAnimator& PoseAnimator::operator=(const PoseAnimator& rhs) = default;
PoseAnimator::PoseAnimator(PoseAnimator&& rhs) noexcept = default;
PoseAnimator& PoseAnimator::operator=(PoseAnimator&& rhs) noexcept = default;

void PoseAnimator::setAnimation(const std::string& path){
    PoseAnimatorComponent& animator = getComponent<PoseAnimatorComponent>();
    animator.animation = path;
    animator.needsLoad = true;
    // a new load even for the same path: the file may have changed (no goToPose carried over)
    if (animator.runtime){
        animator.runtime->loading = false;
        animator.runtime->pending = pose::PoseGoTo();
        animator.runtime->transitionActive = false;
    }
    animator.time = 0;
}

std::string PoseAnimator::getAnimation() const{
    return getComponent<PoseAnimatorComponent>().animation;
}

void PoseAnimator::play(){
    PoseAnimatorComponent& animator = getComponent<PoseAnimatorComponent>();
    animator.started = true;
    // a one-shot that ended plays again from its start
    if (animator.runtime && animator.runtime->isLoaded() && !animator.runtime->data.settings.loop &&
            !animator.playing && animator.runtime->length > 0.0f){
        if (animator.speed >= 0.0f && animator.time >= animator.runtime->length) animator.time = 0;
        if (animator.speed < 0.0f && animator.time <= 0.0f) animator.time = animator.runtime->length;
    }
    animator.playing = true;
    if (animator.runtime){
        animator.runtime->transitionActive = false;
        animator.runtime->pending = pose::PoseGoTo();
    }
}

void PoseAnimator::pause(){
    PoseAnimatorComponent& animator = getComponent<PoseAnimatorComponent>();
    animator.started = true;
    animator.playing = false;
    // a live transition freezes where it is (play() goes back to playback, goToPose starts anew)
    if (animator.runtime && animator.runtime->transitionActive){
        animator.runtime->transitionPaused = true;
    }
}

void PoseAnimator::stop(){
    PoseAnimatorComponent& animator = getComponent<PoseAnimatorComponent>();
    animator.started = true;
    animator.playing = false;
    animator.time = 0;
    if (animator.runtime){
        pose::PoseAnimatorRuntime& runtime = *animator.runtime;
        runtime.pending = pose::PoseGoTo();
        // key pose 0 itself: in a loop, time 0 still has the tail of the move back to it
        runtime.transitionActive = runtime.isLoaded();
        runtime.transitionArrived = true;
        runtime.transitionPaused = false;
        runtime.transitionKey = 0;
    }
}

bool PoseAnimator::isPlaying() const{
    return getComponent<PoseAnimatorComponent>().playing;
}

void PoseAnimator::setTime(float time){
    // NaN or infinity from a script: ignored (the clock would never recover)
    if (!std::isfinite(time)) return;
    PoseAnimatorComponent& animator = getComponent<PoseAnimatorComponent>();
    animator.time = std::max(time, 0.0f);
    if (animator.runtime && animator.runtime->isLoaded()){
        animator.time = pose::playbackTime(time, animator.runtime->length, animator.runtime->data.settings.loop);
    }
    // the playback pose at that time, not a live transition
    if (animator.runtime){
        animator.runtime->transitionActive = false;
        animator.runtime->pending = pose::PoseGoTo();
    }
}

float PoseAnimator::getTime() const{
    return getComponent<PoseAnimatorComponent>().time;
}

float PoseAnimator::getLength() const{
    const PoseAnimatorComponent& animator = getComponent<PoseAnimatorComponent>();
    return (animator.runtime && animator.runtime->isLoaded()) ? animator.runtime->length : 0.0f;
}

void PoseAnimator::setSpeed(float speed){
    if (!std::isfinite(speed)) return;
    getComponent<PoseAnimatorComponent>().speed = speed;
}

float PoseAnimator::getSpeed() const{
    return getComponent<PoseAnimatorComponent>().speed;
}

void PoseAnimator::setPoseFrameRate(float fps){
    getComponent<PoseAnimatorComponent>().poseFrameRate = std::isfinite(fps) ? std::max(0.0f, fps) : 0.0f;
}

float PoseAnimator::getPoseFrameRate() const{
    return getComponent<PoseAnimatorComponent>().poseFrameRate;
}

void PoseAnimator::setWeight(float weight){
    if (std::isnan(weight)) return;
    getComponent<PoseAnimatorComponent>().weight = std::clamp(weight, 0.0f, 1.0f);
}

float PoseAnimator::getWeight() const{
    return getComponent<PoseAnimatorComponent>().weight;
}

void PoseAnimator::setAutoplay(bool autoplay){
    getComponent<PoseAnimatorComponent>().autoplay = autoplay;
}

bool PoseAnimator::isAutoplay() const{
    return getComponent<PoseAnimatorComponent>().autoplay;
}

int PoseAnimator::getPoseCount() const{
    PoseAnimatorComponent& animator = getComponent<PoseAnimatorComponent>();
    if (!scene->getSystem<PoseAnimatorSystem>()->loadData(animator)) return 0;
    return animator.runtime->poseCount();
}

std::string PoseAnimator::getPoseName(int index) const{
    PoseAnimatorComponent& animator = getComponent<PoseAnimatorComponent>();
    if (!scene->getSystem<PoseAnimatorSystem>()->loadData(animator)) return "";
    if (index < 0 || index >= animator.runtime->poseCount()) return "";
    return animator.runtime->data.poses[index].name;
}

int PoseAnimator::findPose(const std::string& name) const{
    PoseAnimatorComponent& animator = getComponent<PoseAnimatorComponent>();
    if (!scene->getSystem<PoseAnimatorSystem>()->loadData(animator)) return -1;
    return animator.runtime->findPose(name);
}

bool PoseAnimator::goToPose(int index){
    return goToPose(index, -1.0f, -1.0f, -1.0f);
}

bool PoseAnimator::goToPose(int index, float duration){
    return goToPose(index, duration, -1.0f, -1.0f);
}

bool PoseAnimator::goToPose(int index, float duration, float overshoot){
    return goToPose(index, duration, overshoot, -1.0f);
}

bool PoseAnimator::goToPose(int index, float duration, float overshoot, float anticipation){
    pose::PoseGoTo request;
    request.key = index;
    request.duration = duration;
    request.overshoot = overshoot;
    request.anticipation = anticipation;
    return scene->getSystem<PoseAnimatorSystem>()->goToPose(getComponent<PoseAnimatorComponent>(), request);
}

bool PoseAnimator::goToPose(const std::string& name){
    return goToPose(name, -1.0f, -1.0f, -1.0f);
}

bool PoseAnimator::goToPose(const std::string& name, float duration){
    return goToPose(name, duration, -1.0f, -1.0f);
}

bool PoseAnimator::goToPose(const std::string& name, float duration, float overshoot){
    return goToPose(name, duration, overshoot, -1.0f);
}

bool PoseAnimator::goToPose(const std::string& name, float duration, float overshoot, float anticipation){
    const int index = findPose(name);
    if (index < 0) return false;
    return goToPose(index, duration, overshoot, anticipation);
}

bool PoseAnimator::isLoaded() const{
    const PoseAnimatorComponent& animator = getComponent<PoseAnimatorComponent>();
    return animator.runtime && animator.runtime->isLoaded();
}

FunctionSubscribe<void()>& PoseAnimator::getOnFinish(){
    return getComponent<PoseAnimatorComponent>().onFinish;
}
