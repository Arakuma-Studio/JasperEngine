// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "OnScreenControls.h"

#include "Scene.h"

using namespace jasperengine;

namespace {
    template<typename T>
    T& ensure(Scene* scene, Entity entity){
        if (!scene->findComponent<T>(entity))
            scene->addComponent<T>(entity, T{});
        return scene->getComponent<T>(entity);
    }
}

OnScreenStick::OnScreenStick(Scene* scene): Image(scene){
    addComponent<OnScreenStickComponent>();
}

OnScreenStick::OnScreenStick(Scene* scene, Entity entity): Image(scene, entity){
    ensure<OnScreenStickComponent>(scene, entity);
}

OnScreenStick::~OnScreenStick(){
}

OnScreenStickComponent& OnScreenStick::getStick() const{
    return ensure<OnScreenStickComponent>(scene, entity);
}

OnScreenButton::OnScreenButton(Scene* scene): Image(scene){
    addComponent<OnScreenButtonComponent>();
}

OnScreenButton::OnScreenButton(Scene* scene, Entity entity): Image(scene, entity){
    ensure<OnScreenButtonComponent>(scene, entity);
}

OnScreenButton::~OnScreenButton(){
}

OnScreenButtonComponent& OnScreenButton::getControl() const{
    return ensure<OnScreenButtonComponent>(scene, entity);
}

TouchArea::TouchArea(Scene* scene): Image(scene){
    addComponent<TouchAreaComponent>();
}

TouchArea::TouchArea(Scene* scene, Entity entity): Image(scene, entity){
    ensure<TouchAreaComponent>(scene, entity);
}

TouchArea::~TouchArea(){
}

TouchAreaComponent& TouchArea::getArea() const{
    return ensure<TouchAreaComponent>(scene, entity);
}
