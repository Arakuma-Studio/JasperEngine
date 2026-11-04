// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ColorTween.h"
#include "component/ColorTweenComponent.h"

using namespace jasperengine;

ColorTween::ColorTween(Scene* scene): Tween(scene){
    addComponent<ColorTweenComponent>();
}

ColorTween::ColorTween(Scene* scene, Entity entity): Tween(scene, entity){

}

void ColorTween::setAction(Vector3 startColor, Vector3 endColor, float duration, bool loop){
    Tween::setAction(duration, loop);

    ColorTweenComponent& colorTween = getComponent<ColorTweenComponent>();

    colorTween.startColor = startColor;
    colorTween.endColor = endColor;
}

void ColorTween::setAction(Vector4 startColor, Vector4 endColor, float duration, bool loop){
    setAction(Vector3(startColor.x, startColor.y, startColor.z), Vector3(endColor.x, endColor.y, endColor.z), duration, loop);
}
