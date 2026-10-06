// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UIElement.h"

using namespace jasperengine;

UIElement::UIElement(){
    // HTMLUI_SKELETON: p0-ecs
}

bool UIElement::isValid() const{
    // HTMLUI_SKELETON: p0-ecs
    return false;
}

std::string UIElement::getId() const{
    // HTMLUI_SKELETON: p0-ecs
    return "";
}

std::string UIElement::getTagName() const{
    // HTMLUI_SKELETON: p0-ecs
    return "";
}

std::string UIElement::getTextContent() const{
    // HTMLUI_SKELETON: p0-ecs
    return "";
}

void UIElement::setTextContent(const std::string& text){
    // HTMLUI_SKELETON: p0-ecs
    (void)text;
}

std::string UIElement::getInnerHTML() const{
    // HTMLUI_SKELETON: p0-ecs
    return "";
}

void UIElement::setInnerHTML(const std::string& markup){
    // HTMLUI_SKELETON: p0-ecs
    (void)markup;
}

bool UIElement::hasAttribute(const std::string& name) const{
    // HTMLUI_SKELETON: p0-ecs
    (void)name;
    return false;
}

std::string UIElement::getAttribute(const std::string& name) const{
    // HTMLUI_SKELETON: p0-ecs
    (void)name;
    return "";
}

void UIElement::setAttribute(const std::string& name, const std::string& value){
    // HTMLUI_SKELETON: p0-ecs
    (void)name;
    (void)value;
}

void UIElement::removeAttribute(const std::string& name){
    // HTMLUI_SKELETON: p0-ecs
    (void)name;
}

bool UIElement::hasClass(const std::string& name) const{
    // HTMLUI_SKELETON: p0-ecs
    (void)name;
    return false;
}

void UIElement::setClass(const std::string& name, bool active){
    // HTMLUI_SKELETON: p0-ecs
    (void)name;
    (void)active;
}

bool UIElement::setStyleProperty(const std::string& name, const std::string& value){
    // HTMLUI_SKELETON: p0-ecs
    (void)name;
    (void)value;
    return false;
}

void UIElement::removeStyleProperty(const std::string& name){
    // HTMLUI_SKELETON: p0-ecs
    (void)name;
}

Rect UIElement::getBoundingBox() const{
    // HTMLUI_SKELETON: p0-ecs
    return Rect(0, 0, 0, 0);
}

bool UIElement::isPseudoClassSet(const std::string& pseudoClass) const{
    // HTMLUI_SKELETON: p0-ecs
    (void)pseudoClass;
    return false;
}
