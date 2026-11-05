// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "InputField.h"
#include "component/InputFieldComponent.h"
#include "util/Color.h"

#include "component/PolygonComponent.h"
#include "component/TextComponent.h"
#include "component/UIComponent.h"
#include "subsystem/UISystem.h"
#include "util/StringUtils.h"

using namespace jasperengine;

InputField::InputField(Scene* scene): Image(scene){
    addComponent<InputFieldComponent>();
}

InputField::InputField(Scene* scene, Entity entity): Image(scene, entity){
}

InputField::~InputField(){
}

InputField::InputField(const InputField& rhs) = default;
InputField& InputField::operator=(const InputField& rhs) = default;
InputField::InputField(InputField&& rhs) noexcept = default;
InputField& InputField::operator=(InputField&& rhs) noexcept = default;

bool InputField::hasText() const{
    const InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    if (tecomp.text == NULL_ENTITY){
        return false;
    }

    Signature signature = scene->getSignature(tecomp.text);
    return signature.test(scene->getComponentId<TextComponent>());
}

bool InputField::hasCursor() const{
    const InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    if (tecomp.cursor == NULL_ENTITY){
        return false;
    }

    Signature signature = scene->getSignature(tecomp.cursor);
    return signature.test(scene->getComponentId<PolygonComponent>());
}

bool InputField::hasSelection() const{
    const InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    if (tecomp.selection == NULL_ENTITY){
        return false;
    }

    Signature signature = scene->getSignature(tecomp.selection);
    return signature.test(scene->getComponentId<PolygonComponent>());
}

void InputField::ensureText(){
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    if (!hasText() || !hasCursor() || !hasSelection()){
        scene->getSystem<UISystem>()->createInputFieldObjects(getEntity(), tecomp);
    }
}

Text InputField::getTextObject() const{
    const InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    if (!hasText()){
        return Text(scene, NULL_ENTITY);
    }

    return Text(scene, tecomp.text);
}

// Fully qualify Polygon: UISystem.h pulls in wingdi.h on Windows, whose GDI Polygon()
// function clashes with jasperengine::Polygon when used as an unqualified return type here.
jasperengine::Polygon InputField::getCursorObject() const{
    const InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    if (!hasCursor()){
        return Polygon(scene, NULL_ENTITY);
    }

    return Polygon(scene, tecomp.cursor);
}

jasperengine::Polygon InputField::getSelectionObject() const{
    const InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    if (!hasSelection()){
        return Polygon(scene, NULL_ENTITY);
    }

    return Polygon(scene, tecomp.selection);
}

void InputField::setDisabled(bool disabled){
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();

    tecomp.disabled = disabled;

    tecomp.needUpdateInputField = true;
    markChanged<InputFieldComponent>();
}

bool InputField::getDisabled() const{
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();

    return  tecomp.disabled;
}

void InputField::setText(const std::string& text){
    ensureText();
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    TextComponent& textcomp = scene->getComponent<TextComponent>(tecomp.text);

    if (textcomp.text != text){
        textcomp.text = text;
        textcomp.needUpdateText = true;
        tecomp.cursorIndex = static_cast<int>(StringUtils::countCodepoints(text));
        tecomp.selectionAnchor = tecomp.cursorIndex;
        tecomp.scrollOffset = 0;
        tecomp.needUpdateInputField = true;
        markChanged<InputFieldComponent>();
    }
}

std::string InputField::getText() const{
    if (!hasText()){
        return "";
    }

    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    TextComponent& textcomp = scene->getComponent<TextComponent>(tecomp.text);

    return textcomp.text;
}

void InputField::setTextColor(Vector4 color){
    ensureText();
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    UIComponent& uitext = scene->getComponent<UIComponent>(tecomp.text);

    uitext.color = Color::sRGBToLinear(color);
}

void InputField::setTextColor(const float red, const float green, const float blue, const float alpha){
    setTextColor(Vector4(red, green, blue, alpha));
}

void InputField::setTextColor(const float red, const float green, const float blue){
    setTextColor(Vector4(red, green, blue, getTextColor().w));
}

Vector4 InputField::getTextColor() const{
    if (!hasText()){
        return Vector4(0.0, 0.0, 0.0, 1.0);
    }

    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    UIComponent& uitext = scene->getComponent<UIComponent>(tecomp.text);

    return Color::linearTosRGB(uitext.color);
}

void InputField::setTextFont(const std::string& font){
    ensureText();
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();

    getTextObject().setFont(font);

    tecomp.needUpdateInputField = true;
    markChanged<InputFieldComponent>();
}

std::string InputField::getTextFont() const{
    if (!hasText()){
        return "";
    }

    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    TextComponent& textcomp = scene->getComponent<TextComponent>(tecomp.text);

    return textcomp.font[0];
}

void InputField::setFontSize(unsigned int fontSize){
    ensureText();
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();

    getTextObject().setFontSize(fontSize);

    tecomp.needUpdateInputField = true;
    markChanged<InputFieldComponent>();
}

unsigned int InputField::getFontSize() const{
    if (!hasText()){
        return 0;
    }

    return getTextObject().getFontSize();
}

void InputField::setMaxTextSize(unsigned int maxTextSize){
    ensureText();
    getTextObject().setMaxTextSize(maxTextSize);
}

unsigned int InputField::getMaxTextSize() const{
    if (!hasText()){
        return 0;
    }

    return getTextObject().getMaxTextSize();
}

void InputField::setPlaceholder(const std::string& placeholder){
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    if (tecomp.placeholder != placeholder){
        tecomp.placeholder = placeholder;
        tecomp.needUpdateInputField = true;
        markChanged<InputFieldComponent>();
    }
}

std::string InputField::getPlaceholder() const{
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    return tecomp.placeholder;
}

void InputField::setPlaceholderColor(Vector4 color){
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    tecomp.placeholderColor = Color::sRGBToLinear(color);
    tecomp.needUpdateInputField = true;
    markChanged<InputFieldComponent>();
}

void InputField::setPlaceholderColor(const float red, const float green, const float blue, const float alpha){
    setPlaceholderColor(Vector4(red, green, blue, alpha));
}

void InputField::setPlaceholderColor(const float red, const float green, const float blue){
    setPlaceholderColor(Vector4(red, green, blue, getPlaceholderColor().w));
}

Vector4 InputField::getPlaceholderColor() const{
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    return Color::linearTosRGB(tecomp.placeholderColor);
}

void InputField::setPassword(bool password){
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    if (tecomp.password != password){
        tecomp.password = password;
        tecomp.needUpdateInputField = true;
        markChanged<InputFieldComponent>();
    }
}

bool InputField::getPassword() const{
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    return tecomp.password;
}

void InputField::setPasswordChar(char passwordChar){
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    if (tecomp.passwordChar != passwordChar){
        tecomp.passwordChar = passwordChar;
        tecomp.needUpdateInputField = true;
        markChanged<InputFieldComponent>();
    }
}

char InputField::getPasswordChar() const{
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    return tecomp.passwordChar;
}

void InputField::setCursorIndex(int cursorIndex){
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    tecomp.cursorIndex = cursorIndex;
    tecomp.selectionAnchor = cursorIndex;
    tecomp.needUpdateInputField = true;
    markChanged<InputFieldComponent>();
}

int InputField::getCursorIndex() const{
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    return tecomp.cursorIndex;
}

void InputField::setSelection(int anchor, int focus){
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    tecomp.selectionAnchor = anchor;
    tecomp.cursorIndex = focus;
    tecomp.needUpdateInputField = true;
    markChanged<InputFieldComponent>();
}

int InputField::getSelectionAnchor() const{
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    return tecomp.selectionAnchor;
}

int InputField::getSelectionFocus() const{
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    return tecomp.cursorIndex;
}

void InputField::setCursorBlink(float cursorBlink){
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();

    tecomp.cursorBlink = cursorBlink;
}

float InputField::getCursorBlink() const{
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();

    return tecomp.cursorBlink;
}

void InputField::setCursorWidth(float cursorWidth){
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    if (tecomp.cursorWidth != cursorWidth){
        tecomp.cursorWidth = cursorWidth;
        tecomp.needUpdateInputField = true;
        markChanged<InputFieldComponent>();
    }
}

float InputField::getCursorWidth() const{
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();

    return tecomp.cursorWidth;
}

void InputField::setCursorColor(Vector4 color){
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    tecomp.needUpdateInputField = true;
    markChanged<InputFieldComponent>();
    tecomp.cursorColor = Color::sRGBToLinear(color);
}

void InputField::setCursorColor(const float red, const float green, const float blue, const float alpha){
    setCursorColor(Vector4(red, green, blue, alpha));
}

void InputField::setCursorColor(const float red, const float green, const float blue){
    setCursorColor(Vector4(red, green, blue, getCursorColor().w));
}

Vector4 InputField::getCursorColor() const{
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    return Color::linearTosRGB(tecomp.cursorColor);
}

void InputField::setSelectionColor(Vector4 color){
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    tecomp.selectionColor = Color::sRGBToLinear(color);
    tecomp.needUpdateInputField = true;
    markChanged<InputFieldComponent>();
}

void InputField::setSelectionColor(const float red, const float green, const float blue, const float alpha){
    setSelectionColor(Vector4(red, green, blue, alpha));
}

void InputField::setSelectionColor(const float red, const float green, const float blue){
    setSelectionColor(Vector4(red, green, blue, getSelectionColor().w));
}

Vector4 InputField::getSelectionColor() const{
    InputFieldComponent& tecomp = getComponent<InputFieldComponent>();
    return Color::linearTosRGB(tecomp.selectionColor);
}
