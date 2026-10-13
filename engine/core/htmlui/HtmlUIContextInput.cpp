// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// HtmlUIContext input beyond phase 0's mouse helpers (phase-1 spec §3.6, p1-input): touch
// pointers with ids, the wheel and the context's text input handler.

#include "HtmlUIContext.h"

#include "RmlTextInputHandler.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>

using namespace jasperengine;

namespace {

    Rml::Touch makeTouch(int pointer, Rml::Vector2f position){
        Rml::Touch touch;
        // ids are small non-negative integers on every platform; keep negative ones distinct
        touch.identifier = (Rml::TouchId)(intptr_t)pointer;
        touch.position = position;
        return touch;
    }

}

bool HtmlUIContext::hasTouch(int pointer) const{
    return std::find(touches.begin(), touches.end(), pointer) != touches.end();
}

bool HtmlUIContext::processTouchStart(int pointer, Rml::Vector2f position, int modifiers){
    if (!context)
        return false;

    // a start for a pointer RmlUi still tracks (a lost end): finish the old one first
    if (hasTouch(pointer))
        processTouchCancel(pointer);

    context->ProcessTouchStart(Rml::TouchList{makeTouch(pointer, position)}, modifiers);
    touches.push_back(pointer);

    // the touched element is pressed like a primary button (Context::ProcessTouchStart)
    Rml::Element* hover = context->GetHoverElement();
    if (hover && hover != context->GetRootElement() && hover->GetOwnerDocument() && hover->GetOwnerDocument()->GetParentNode()){
        activeElement = hover->GetObserverPtr();
    }else{
        activeElement.reset();
        releaseFocus();
    }

    return isPointerOverContent();
}

bool HtmlUIContext::processTouchMove(int pointer, Rml::Vector2f position, int modifiers){
    if (!context || !hasTouch(pointer))
        return false;

    context->ProcessTouchMove(Rml::TouchList{makeTouch(pointer, position)}, modifiers);

    return isPointerOverContent();
}

bool HtmlUIContext::processTouchEnd(int pointer, Rml::Vector2f position, int modifiers){
    if (!context || !hasTouch(pointer))
        return false;

    const bool overContent = isPointerOverContent();

    touches.erase(std::find(touches.begin(), touches.end(), pointer));
    context->ProcessTouchEnd(Rml::TouchList{makeTouch(pointer, position)}, modifiers);

    // RmlUi has one active chain: the release of any finger resets it
    activeElement.reset();

    return overContent;
}

bool HtmlUIContext::processTouchCancel(int pointer){
    if (!context || !hasTouch(pointer))
        return false;

    touches.erase(std::find(touches.begin(), touches.end(), pointer));
    // a cancel releases without clicking: Context::ProcessTouchCancel releases the primary
    // button over the last position, which clicks the hovered element if it is the pressed
    // one; moving the hover off it first (a leave) keeps that from happening
    context->ProcessMouseLeave();
    Rml::Touch touch;
    touch.identifier = (Rml::TouchId)(intptr_t)pointer;
    context->ProcessTouchCancel(Rml::TouchList{touch});
    activeElement.reset();

    return false;
}

bool HtmlUIContext::processMouseWheel(Rml::Vector2f offset, int modifiers){
    if (!context)
        return true;

    return context->ProcessMouseWheel(Rml::Vector2f(-offset.x, -offset.y) * wheelScale, modifiers);
}

void HtmlUIContext::setWheelScale(float scale){
    if (scale > 0.0f)
        wheelScale = scale;
}

float HtmlUIContext::getWheelScale() const{
    return wheelScale;
}

RmlTextInputHandler& HtmlUIContext::getTextInputHandler(){
    return *textInputHandler;
}
