// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AlphaTween.h"
#include "component/AlphaTweenComponent.h"

using namespace jasperengine;

AlphaTween::AlphaTween(Scene* scene): Tween(scene){
    addComponent<AlphaTweenComponent>();
}

AlphaTween::AlphaTween(Scene* scene, Entity entity): Tween(scene, entity){

}

void AlphaTween::setAction(float startAlpha, float endAlpha, float duration, bool loop){
    Tween::setAction(duration, loop);

    AlphaTweenComponent& alphaTween = getComponent<AlphaTweenComponent>();

    alphaTween.startAlpha = startAlpha;
    alphaTween.endAlpha = endAlpha;
}

void AlphaTween::setAlphaTarget(AlphaTweenTarget alphaTarget){
    getComponent<AlphaTweenComponent>().alphaTarget = alphaTarget;
}

AlphaTweenTarget AlphaTween::getAlphaTarget() const{
    return getComponent<AlphaTweenComponent>().alphaTarget;
}
