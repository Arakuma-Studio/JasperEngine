// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// HtmlUIContext input beyond phase 0's mouse helpers (phase-1 spec §3.6, p1-input): touch
// pointers with ids (ui-multitouch: each a DOM pointer of its own), the wheel and the context's
// text input handler.

#include "HtmlUIContext.h"

#include "RmlTextInputHandler.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>

using namespace jasperengine;

namespace {

    Rml::PointerInput touchInput(int pointer, Rml::Vector2f position, int modifiers){
        Rml::PointerInput input;
        input.id = HtmlUIContext::touchPointerId(pointer);
        input.type = Rml::PointerType::Touch;
        input.position = position;
        input.button = 0;
        input.key_modifier_state = modifiers;
        return input;
    }

    // an element that takes the pointer: not the context root, attached, not passed through
    bool isTouchContent(Rml::Context* context, Rml::Element* element){
        if (!element || element == context->GetRootElement() || HtmlUIContext::passesPointerThrough(element))
            return false;
        Rml::ElementDocument* document = element->GetOwnerDocument();
        return document && document->GetParentNode();
    }

}

bool HtmlUIContext::hasTouch(int pointer) const{
    return std::find(touches.begin(), touches.end(), pointer) != touches.end();
}

bool HtmlUIContext::isTouchOverContent(int pointer) const{
    if (!context || !hasTouch(pointer))
        return false;
    if (isTouchContent(context, context->GetHoverElement(touchPointerId(pointer))))
        return true;
    auto it = touchPresses.find(pointer);
    return it != touchPresses.end() && isTouchContent(context, it->second.get());
}

bool HtmlUIContext::processTouchStart(int pointer, Rml::Vector2f position, int modifiers){
    if (!context)
        return false;

    // a start for a pointer RmlUi still tracks (a lost end): finish the old one first
    if (hasTouch(pointer))
        processTouchCancel(pointer);

    context->ProcessPointerDown(touchInput(pointer, position, modifiers));
    touches.push_back(pointer);

    // the touched element is pressed (its own :active, capture and click: other fingers keep theirs)
    Rml::Element* hover = context->GetHoverElement(touchPointerId(pointer));
    if (isTouchContent(context, hover)){
        touchPresses[pointer] = hover->GetObserverPtr();
    }else{
        touchPresses.erase(pointer);
        releaseFocus();
    }

    return isTouchOverContent(pointer);
}

bool HtmlUIContext::processTouchMove(int pointer, Rml::Vector2f position, int modifiers){
    if (!context || !hasTouch(pointer))
        return false;

    context->ProcessPointerMove(touchInput(pointer, position, modifiers));

    return isTouchOverContent(pointer);
}

bool HtmlUIContext::processTouchEnd(int pointer, Rml::Vector2f position, int modifiers){
    if (!context || !hasTouch(pointer))
        return false;

    const bool overContent = isTouchOverContent(pointer);

    context->ProcessPointerUp(touchInput(pointer, position, modifiers));
    touches.erase(std::find(touches.begin(), touches.end(), pointer));
    touchPresses.erase(pointer);

    return overContent;
}

bool HtmlUIContext::processTouchCancel(int pointer){
    if (!context || !hasTouch(pointer))
        return false;

    // pointercancel: no click, no drop; the other fingers keep their presses
    context->ProcessPointerCancel(touchPointerId(pointer));
    touches.erase(std::find(touches.begin(), touches.end(), pointer));
    touchPresses.erase(pointer);

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
