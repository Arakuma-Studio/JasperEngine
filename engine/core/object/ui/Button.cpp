// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Button.h"
#include "component/ButtonComponent.h"
#include "util/Color.h"

#include "component/TextComponent.h"
#include "component/UIComponent.h"
#include "subsystem/UISystem.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

Button::Button(Scene* scene): Image(scene){
    addComponent<ButtonComponent>();
}

Button::Button(Scene* scene, Entity entity): Image(scene, entity){
}

Button::~Button(){
}

Button::Button(const Button& rhs) = default;
Button& Button::operator=(const Button& rhs) = default;
Button::Button(Button&& rhs) noexcept = default;
Button& Button::operator=(Button&& rhs) noexcept = default;

bool Button::hasLabel() const{
    const ButtonComponent& btcomp = getComponent<ButtonComponent>();
    if (btcomp.label == NULL_ENTITY){
        return false;
    }

    Signature signature = scene->getSignature(btcomp.label);
    return signature.test(scene->getComponentId<TextComponent>());
}

void Button::ensureLabel(){
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    if (!hasLabel()){
        scene->getSystem<UISystem>()->createButtonObjects(getEntity(), btcomp);
    }
}

Text Button::getLabelObject() const{
    const ButtonComponent& btcomp = getComponent<ButtonComponent>();
    if (!hasLabel()){
        return Text(scene, NULL_ENTITY);
    }

    return Text(scene, btcomp.label);
}

void Button::setLabel(const std::string& text){
    ensureLabel();
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    TextComponent& textcomp = scene->getComponent<TextComponent>(btcomp.label);

    if (textcomp.text != text){
        textcomp.text = text;
        textcomp.needUpdateText = true;
    }
}

std::string Button::getLabel() const{
    if (!hasLabel()){
        return "";
    }

    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    TextComponent& textcomp = scene->getComponent<TextComponent>(btcomp.label);

    return textcomp.text;
}

void Button::setLabelColor(Vector4 color){
    ensureLabel();
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    UIComponent& uilabel = scene->getComponent<UIComponent>(btcomp.label);

    uilabel.color = Color::sRGBToLinear(color);
}

void Button::setLabelColor(const float red, const float green, const float blue, const float alpha){
    setLabelColor(Vector4(red, green, blue, alpha));
}

Vector4 Button::getLabelColor() const{
    if (!hasLabel()){
        return Vector4(0.0, 0.0, 0.0, 1.0);
    }

    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    UIComponent& uilabel = scene->getComponent<UIComponent>(btcomp.label);

    return Color::linearTosRGB(uilabel.color);
}

void Button::setLabelFont(const std::string& font){
    ensureLabel();
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    getLabelObject().setFont(font);
    btcomp.needUpdateButton = true;
    markChanged<ButtonComponent>();
}

std::string Button::getLabelFont() const{
    if (!hasLabel()){
        return "";
    }

    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    TextComponent& textcomp = scene->getComponent<TextComponent>(btcomp.label);

    return textcomp.font[0];
}

void Button::setLabelFontSize(unsigned int fontSize){
    ensureLabel();
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    getLabelObject().setFontSize(fontSize);
    btcomp.needUpdateButton = true;
    markChanged<ButtonComponent>();
}

unsigned int Button::getLabelFontSize() const{
    if (!hasLabel()){
        return 0;
    }

    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    TextComponent& textcomp = scene->getComponent<TextComponent>(btcomp.label);

    return textcomp.fontSize;
}

void Button::setTextureNormal(const std::string& path){
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    btcomp.textureNormal.setPath(path);
    btcomp.needUpdateButton = true;
    markChanged<ButtonComponent>();

    Image::setTexture(path);
}

void Button::setTextureHovered(const std::string& path){
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    btcomp.textureHovered.setPath(path);
    btcomp.needUpdateButton = true;
    markChanged<ButtonComponent>();
}

void Button::setTexturePressed(const std::string& path){
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    btcomp.texturePressed.setPath(path);
    btcomp.needUpdateButton = true;
    markChanged<ButtonComponent>();
}

void Button::setTextureDisabled(const std::string& path){
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    btcomp.textureDisabled.setPath(path);
    btcomp.needUpdateButton = true;
    markChanged<ButtonComponent>();
}

void Button::setColorNormal(Vector4 color){
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    Vector4 oldColorNormal = btcomp.colorNormal;
    btcomp.colorNormal = Color::sRGBToLinear(color);
    if (btcomp.colorHovered == oldColorNormal){
        btcomp.colorHovered = btcomp.colorNormal;
    }
    btcomp.needUpdateButton = true;
    markChanged<ButtonComponent>();

    Image::setColor(color);
}

void Button::setColorNormal(const float red, const float green, const float blue, const float alpha){
    setColorNormal(Vector4(red, green, blue, alpha));
}

void Button::setColorNormal(const float red, const float green, const float blue){
    setColorNormal(Vector4(red, green, blue, getColorNormal().w));
}

Vector4 Button::getColorNormal() const{
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    return Color::linearTosRGB(btcomp.colorNormal);
}

void Button::setColorHovered(Vector4 color){
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    btcomp.colorHovered = Color::sRGBToLinear(color);
    btcomp.needUpdateButton = true;
    markChanged<ButtonComponent>();
}

void Button::setColorHovered(const float red, const float green, const float blue, const float alpha){
    setColorHovered(Vector4(red, green, blue, alpha));
}

void Button::setColorHovered(const float red, const float green, const float blue){
    setColorHovered(Vector4(red, green, blue, getColorHovered().w));
}

Vector4 Button::getColorHovered() const{
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    return Color::linearTosRGB(btcomp.colorHovered);
}

void Button::setColorPressed(Vector4 color){
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    btcomp.colorPressed = Color::sRGBToLinear(color);
    btcomp.needUpdateButton = true;
    markChanged<ButtonComponent>();
}

void Button::setColorPressed(const float red, const float green, const float blue, const float alpha){
    setColorPressed(Vector4(red, green, blue, alpha));
}

void Button::setColorPressed(const float red, const float green, const float blue){
    setColorPressed(Vector4(red, green, blue, getColorPressed().w));
}

Vector4 Button::getColorPressed() const{
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    return Color::linearTosRGB(btcomp.colorPressed);
}

void Button::setColorDisabled(Vector4 color){
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    btcomp.colorDisabled = Color::sRGBToLinear(color);
    btcomp.needUpdateButton = true;
    markChanged<ButtonComponent>();
}

void Button::setColorDisabled(const float red, const float green, const float blue, const float alpha){
    setColorDisabled(Vector4(red, green, blue, alpha));
}

void Button::setColorDisabled(const float red, const float green, const float blue){
    setColorDisabled(Vector4(red, green, blue, getColorDisabled().w));
}

Vector4 Button::getColorDisabled() const{
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    return Color::linearTosRGB(btcomp.colorDisabled);
}

void Button::setDisabled(bool disabled){
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    btcomp.disabled = disabled;
    btcomp.needUpdateButton = true;
    markChanged<ButtonComponent>();
}

bool Button::getDisabled() const{
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    return btcomp.disabled;
}
void Button::setInteractable(bool interactable){
    setDisabled(!interactable);
}

bool Button::isInteractable() const{
    return !getDisabled();
}

void Button::setColor(Vector4 color){
    if (UISystem::buttonOwnsUIColor(getComponent<ButtonComponent>()))
        setColorNormal(color);
    else
        Image::setColor(color);
}

void Button::setColor(const float red, const float green, const float blue, const float alpha){
    setColor(Vector4(red, green, blue, alpha));
}

void Button::setColor(const float red, const float green, const float blue){
    setColor(Vector4(red, green, blue, getColor().w));
}

void Button::setTexture(const std::string& path){
    if (UISystem::buttonOwnsUITexture(getComponent<ButtonComponent>()))
        setTextureNormal(path);
    else
        Image::setTexture(path);
}

void Button::setTextureSelected(const std::string& path){
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    btcomp.textureSelected.setPath(path);
    btcomp.needUpdateButton = true;
    markChanged<ButtonComponent>();
}

void Button::setColorSelected(Vector4 color){
    ButtonComponent& btcomp = getComponent<ButtonComponent>();
    btcomp.colorSelected = Color::sRGBToLinear(color);
    btcomp.needUpdateButton = true;
    markChanged<ButtonComponent>();
}

void Button::setColorSelected(const float red, const float green, const float blue, const float alpha){
    setColorSelected(Vector4(red, green, blue, alpha));
}

void Button::setColorSelected(const float red, const float green, const float blue){
    setColorSelected(Vector4(red, green, blue, getColorSelected().w));
}

Vector4 Button::getColorSelected() const{
    return Color::linearTosRGB(getComponent<ButtonComponent>().colorSelected);
}

namespace {
    // a Button field written from a script: the button redraws
    template<typename T>
    void setButtonField(ButtonComponent& button, T ButtonComponent::* field, T value){
        button.*field = value;
        button.needUpdateButton = true;
    }
}

void Button::setTransition(ButtonTransition transition){
    setButtonField(getComponent<ButtonComponent>(), &ButtonComponent::transition, transition);
    markChanged<ButtonComponent>();
}

ButtonTransition Button::getTransition() const{
    return getComponent<ButtonComponent>().transition;
}

void Button::setFadeDuration(float seconds){
    setButtonField(getComponent<ButtonComponent>(), &ButtonComponent::fadeDuration, std::isfinite(seconds) ? std::max(0.0f, seconds) : 0.0f);
    markChanged<ButtonComponent>();
}

float Button::getFadeDuration() const{
    return getComponent<ButtonComponent>().fadeDuration;
}

void Button::setScaleHovered(float scale){
    setButtonField(getComponent<ButtonComponent>(), &ButtonComponent::scaleHovered, scale);
    markChanged<ButtonComponent>();
}

float Button::getScaleHovered() const{
    return getComponent<ButtonComponent>().scaleHovered;
}

void Button::setScalePressed(float scale){
    setButtonField(getComponent<ButtonComponent>(), &ButtonComponent::scalePressed, scale);
    markChanged<ButtonComponent>();
}

float Button::getScalePressed() const{
    return getComponent<ButtonComponent>().scalePressed;
}

void Button::setScaleSelected(float scale){
    setButtonField(getComponent<ButtonComponent>(), &ButtonComponent::scaleSelected, scale);
    markChanged<ButtonComponent>();
}

float Button::getScaleSelected() const{
    return getComponent<ButtonComponent>().scaleSelected;
}

void Button::setNavigationMode(NavigationMode mode){
    getComponent<ButtonComponent>().navigationMode = mode;
    markChanged<ButtonComponent>();
}

NavigationMode Button::getNavigationMode() const{
    return getComponent<ButtonComponent>().navigationMode;
}

void Button::setWrapAround(bool wrapAround){
    getComponent<ButtonComponent>().wrapAround = wrapAround;
    markChanged<ButtonComponent>();
}

bool Button::getWrapAround() const{
    return getComponent<ButtonComponent>().wrapAround;
}

void Button::setSelectOnUp(Entity entity){
    getComponent<ButtonComponent>().selectOnUp = entity;
    markChanged<ButtonComponent>();
}

Entity Button::getSelectOnUp() const{
    return getComponent<ButtonComponent>().selectOnUp;
}

void Button::setSelectOnDown(Entity entity){
    getComponent<ButtonComponent>().selectOnDown = entity;
    markChanged<ButtonComponent>();
}

Entity Button::getSelectOnDown() const{
    return getComponent<ButtonComponent>().selectOnDown;
}

void Button::setSelectOnLeft(Entity entity){
    getComponent<ButtonComponent>().selectOnLeft = entity;
    markChanged<ButtonComponent>();
}

Entity Button::getSelectOnLeft() const{
    return getComponent<ButtonComponent>().selectOnLeft;
}

void Button::setSelectOnRight(Entity entity){
    getComponent<ButtonComponent>().selectOnRight = entity;
    markChanged<ButtonComponent>();
}

Entity Button::getSelectOnRight() const{
    return getComponent<ButtonComponent>().selectOnRight;
}

void Button::setSelectOnEnable(bool selectOnEnable){
    getComponent<ButtonComponent>().selectOnEnable = selectOnEnable;
    markChanged<ButtonComponent>();
}

bool Button::getSelectOnEnable() const{
    return getComponent<ButtonComponent>().selectOnEnable;
}

bool Button::select(){
    return scene->getSystem<UISystem>()->setSelected(entity);
}

bool Button::isSelected() const{
    return scene->getSystem<UISystem>()->getSelected() == entity;
}

SelectionState Button::getState() const{
    return scene->getSystem<UISystem>()->getButtonState(entity);
}

Entity Button::findSelectableOnUp() const{
    return scene->getSystem<UISystem>()->findSelectableOnDirection(entity, MoveDirection::UP);
}

Entity Button::findSelectableOnDown() const{
    return scene->getSystem<UISystem>()->findSelectableOnDirection(entity, MoveDirection::DOWN);
}

Entity Button::findSelectableOnLeft() const{
    return scene->getSystem<UISystem>()->findSelectableOnDirection(entity, MoveDirection::LEFT);
}

Entity Button::findSelectableOnRight() const{
    return scene->getSystem<UISystem>()->findSelectableOnDirection(entity, MoveDirection::RIGHT);
}
