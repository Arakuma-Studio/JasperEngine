// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SplineAnimate.h"

#include "Scene.h"
#include "subsystem/SplineSystem.h"

using namespace jasperengine;

SplineAnimate::SplineAnimate(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

SplineAnimate::~SplineAnimate() = default;
SplineAnimate::SplineAnimate(const SplineAnimate& rhs) = default;
SplineAnimate& SplineAnimate::operator=(const SplineAnimate& rhs) = default;
SplineAnimate::SplineAnimate(SplineAnimate&& rhs) noexcept = default;
SplineAnimate& SplineAnimate::operator=(SplineAnimate&& rhs) noexcept = default;

SplineAnimateComponent& SplineAnimate::component() const{
    return getComponent<SplineAnimateComponent>();
}

bool SplineAnimate::isValid() const{
    return scene && scene->findComponent<SplineAnimateComponent>(entity) != nullptr;
}

void SplineAnimate::setEnabled(bool enabled){ component().enabled = enabled; }
bool SplineAnimate::isEnabled() const{ return component().enabled; }

void SplineAnimate::setSpline(Entity value){ component().spline = value; }
Entity SplineAnimate::getSpline() const{ return component().spline; }

void SplineAnimate::setMethod(SplineAnimateMethod value){ component().method = value; }
SplineAnimateMethod SplineAnimate::getMethod() const{ return component().method; }

void SplineAnimate::setDuration(float value){ component().duration = value; }
float SplineAnimate::getDuration() const{ return component().duration; }

void SplineAnimate::setSpeed(float value){ component().speed = value; }
float SplineAnimate::getSpeed() const{ return component().speed; }

void SplineAnimate::setLoop(SplineLoop value){ component().loop = value; }
SplineLoop SplineAnimate::getLoop() const{ return component().loop; }

void SplineAnimate::setEasing(SplineEasing value){ component().easing = value; }
SplineEasing SplineAnimate::getEasing() const{ return component().easing; }

void SplineAnimate::setAlignment(SplineAlignment value){ component().alignment = value; }
SplineAlignment SplineAnimate::getAlignment() const{ return component().alignment; }

void SplineAnimate::setStartOffset(float value){ component().startOffset = value; }
float SplineAnimate::getStartOffset() const{ return component().startOffset; }

void SplineAnimate::setLateralOffset(float value){ component().lateralOffset = value; }
float SplineAnimate::getLateralOffset() const{ return component().lateralOffset; }

void SplineAnimate::setVerticalOffset(float value){ component().verticalOffset = value; }
float SplineAnimate::getVerticalOffset() const{ return component().verticalOffset; }

void SplineAnimate::setPlayOnAwake(bool value){ component().playOnAwake = value; }
bool SplineAnimate::isPlayOnAwake() const{ return component().playOnAwake; }

static SplineSystem* splineSystemOf(Scene* scene){
    return scene ? scene->findSystem<SplineSystem>().get() : nullptr;
}

void SplineAnimate::play(){
    if (SplineSystem* system = splineSystemOf(scene)){
        system->play(entity);
    }
}

void SplineAnimate::pause(){
    if (SplineSystem* system = splineSystemOf(scene)){
        system->pause(entity);
    }
}

void SplineAnimate::restart(){
    if (SplineSystem* system = splineSystemOf(scene)){
        system->restart(entity);
    }
}

bool SplineAnimate::isPlaying() const{ return component().playing; }
float SplineAnimate::getNormalizedTime() const{ return component().normalizedTime; }
float SplineAnimate::getDistance() const{ return component().distance; }

void SplineAnimate::setDistance(float distance){
    if (SplineSystem* system = splineSystemOf(scene)){
        system->setFollowerDistance(entity, distance);
    }
}

bool SplineAnimate::isCompleted() const{ return component().completed; }
FunctionSubscribe<void()>& SplineAnimate::getOnComplete(){ return component().onComplete; }
