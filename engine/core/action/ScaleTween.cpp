// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScaleTween.h"
#include "component/ScaleTweenComponent.h"

using namespace jasperengine;

ScaleTween::ScaleTween(Scene* scene): Tween(scene){
    addComponent<ScaleTweenComponent>();
}

ScaleTween::ScaleTween(Scene* scene, Entity entity): Tween(scene, entity){

}

void ScaleTween::setAction(Vector3 startScale, Vector3 endScale, float duration, bool loop){
    Tween::setAction(duration, loop);

    ScaleTweenComponent& scaleTween = getComponent<ScaleTweenComponent>();

    scaleTween.startScale = startScale;
    scaleTween.endScale = endScale;
}