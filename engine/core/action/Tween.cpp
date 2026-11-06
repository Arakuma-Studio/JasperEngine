// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Tween.h"
#include "component/ActionComponent.h"
#include "component/TweenComponent.h"

using namespace jasperengine;

Tween::Tween(Scene* scene): Action(scene){
    addComponent<TweenComponent>();
}

Tween::Tween(Scene* scene, Entity entity): Action(scene, entity){

}

void Tween::setAction(float duration, bool loop){
    ActionComponent& action = getComponent<ActionComponent>();
    TweenComponent& tween = getComponent<TweenComponent>();

    action.timecount = 0;
    tween.duration = duration;
    tween.loop = loop;
    tween.time = 0;
    tween.value = 0;
}

void Tween::setFunctionType(EaseType functionType){
    TweenComponent& tween = getComponent<TweenComponent>();

    tween.function.setType(functionType);
}

void Tween::setCurve(const Curve& curve){
    TweenComponent& tween = getComponent<TweenComponent>();

    tween.curve = curve;
    tween.function.setType(EaseType::CURVE);
}

Curve Tween::getCurve() const{
    return getComponent<TweenComponent>().curve;
}

float Tween::getValue() const{
    TweenComponent& tween = getComponent<TweenComponent>();

    return tween.value;
}

float Tween::getTime() const{
    TweenComponent& tween = getComponent<TweenComponent>();

    return tween.time;
}

void Tween::setDuration(float duration){
    TweenComponent& tween = getComponent<TweenComponent>();

    tween.duration = duration;
}

float Tween::getDuration() const{
    TweenComponent& tween = getComponent<TweenComponent>();

    return tween.duration;
}

void Tween::setLoop(bool loop){
    TweenComponent& tween = getComponent<TweenComponent>();

    tween.loop = loop;
}

bool Tween::isLoop() const{
    TweenComponent& tween = getComponent<TweenComponent>();

    return tween.loop;
}

void Tween::setPingPong(bool pingPong){
    TweenComponent& tween = getComponent<TweenComponent>();

    tween.pingPong = pingPong;
}

bool Tween::isPingPong() const{
    TweenComponent& tween = getComponent<TweenComponent>();

    return tween.pingPong;
}
void Tween::setUseUnscaledTime(bool useUnscaledTime){
    TweenComponent& tween = getComponent<TweenComponent>();

    tween.useUnscaledTime = useUnscaledTime;
}

bool Tween::isUseUnscaledTime() const{
    TweenComponent& tween = getComponent<TweenComponent>();

    return tween.useUnscaledTime;
}
