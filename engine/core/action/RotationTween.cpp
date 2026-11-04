// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RotationTween.h"
#include "component/RotationTweenComponent.h"

#include "util/Angle.h"

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
    rotAction.spin = false;
}

void RotationTween::setSpinAction(Quaternion startRotation, Vector3 axis, float angle, float duration, bool loop){
    Tween::setAction(duration, loop);

    RotationTweenComponent& rotAction = getComponent<RotationTweenComponent>();

    rotAction.startRotation = startRotation;
    rotAction.spin = true;
    rotAction.axis = axis;
    rotAction.angle = Angle::defaultToDeg(angle);
}