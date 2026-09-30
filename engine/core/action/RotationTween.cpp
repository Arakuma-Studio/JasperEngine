// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RotationTween.h"

using namespace jasperengine;

RotationTween::RotationTween(Scene* scene): Tween(scene){
    addComponent<RotationTweenComponent>();
}

RotationTween::RotationTween(Scene* scene, Entity entity): Tween(scene, entity){

}

void RotationTween::setAction(Quaternion startRotation, Quaternion endRotation, float duration, bool loop){
    Tween::setAction(duration, loop);

    RotationTweenComponent& rotAction = getComponent<RotationTweenComponent>();

    rotAction.startRotation = startRotation;
    rotAction.endRotation = endRotation;
}