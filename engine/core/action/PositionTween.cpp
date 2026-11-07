// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PositionTween.h"
#include "component/PositionTweenComponent.h"

using namespace jasperengine;

PositionTween::PositionTween(Scene* scene): Tween(scene){
    addComponent<PositionTweenComponent>();
}

PositionTween::PositionTween(Scene* scene, Entity entity): Tween(scene, entity){

}

void PositionTween::setAction(Vector3 startPosition, Vector3 endPosition, float duration, bool loop){
    Tween::setAction(duration, loop);

    PositionTweenComponent& posAction = getComponent<PositionTweenComponent>();

    posAction.startPosition = startPosition;
    posAction.endPosition = endPosition;
}