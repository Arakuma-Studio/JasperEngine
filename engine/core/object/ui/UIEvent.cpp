// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UIEvent.h"
#include "HtmlUIEventListener.h"
#include "HtmlUIScriptInternal.h"
#include "htmlui/RmlKeyMap.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>

using namespace jasperengine;

namespace {

    Rml::Event* liveEvent(const std::shared_ptr<HtmlUIEventState>& state){
        return state ? state->event : nullptr;
    }

    int intParameter(const std::shared_ptr<HtmlUIEventState>& state, const char* name, int fallback = 0){
        Rml::Event* event = liveEvent(state);
        return event ? event->GetParameter<int>(name, fallback) : fallback;
    }

    float floatParameter(const std::shared_ptr<HtmlUIEventState>& state, const char* name){
        Rml::Event* event = liveEvent(state);
        return event ? event->GetParameter<float>(name, 0.0f) : 0.0f;
    }

}

UIEvent HtmlUIScriptAccess::makeEvent(const std::shared_ptr<HtmlUIEventState>& state){
    UIEvent event;
    event.state = state;
    return event;
}

HtmlUIEventState* HtmlUIScriptAccess::eventState(const UIEvent& event){
    return event.state.get();
}

bool UIEvent::isValid() const{
    return liveEvent(state) != nullptr;
}

std::string UIEvent::getType() const{
    return isValid() ? state->type : std::string();
}

UIElement UIEvent::getTarget() const{
    Rml::Event* event = liveEvent(state);
    return event ? UIElementAccess::wrap(event->GetTargetElement()) : UIElement();
}

UIElement UIEvent::getCurrentTarget() const{
    Rml::Event* event = liveEvent(state);
    return event ? UIElementAccess::wrap(event->GetCurrentElement()) : UIElement();
}

int UIEvent::getEventPhase() const{
    Rml::Event* event = liveEvent(state);
    if (!event)
        return 0;
    switch (event->GetPhase()){
        case Rml::EventPhase::Capture: return 1;
        case Rml::EventPhase::Target: return 2;
        case Rml::EventPhase::Bubble: return 3;
        default: return 0;
    }
}

void UIEvent::stopPropagation(){
    if (Rml::Event* event = liveEvent(state))
        event->StopPropagation();
}

void UIEvent::stopImmediatePropagation(){
    if (Rml::Event* event = liveEvent(state))
        event->StopImmediatePropagation();
}

void UIEvent::preventDefault(){
    Rml::Event* event = liveEvent(state);
    if (!event)
        return;
    event->PreventDefault();
    // dispatchEvent reports it through this flag (its result is false then)
    if (bool* prevented = static_cast<bool*>(event->GetParameter<void*>(HtmlUIEventNames::preventedParameter, nullptr)))
        *prevented = true;
}

bool UIEvent::isDefaultPrevented() const{
    Rml::Event* event = liveEvent(state);
    return event && event->IsDefaultPrevented();
}

namespace {

    // the engine key of a key event (keydown/keyup carry only the RmlUi key); -1 for other events
    int engineKey(Rml::Event* event){
        if (!event || !event->GetParameters().count("key_identifier"))
            return -1;
        return RmlKeyMap::fromRmlKey((Rml::Input::KeyIdentifier)event->GetParameter<int>("key_identifier", 0));
    }

    int engineModifiers(Rml::Event* event){
        static const char* const names[] = {"ctrl_key", "shift_key", "alt_key", "meta_key", "caps_lock_key", "num_lock_key", "scroll_lock_key"};
        int rmlMods = 0;
        for (int i = 0; i < 7; i++){
            if (event->GetParameter<int>(names[i], 0) != 0)
                rmlMods |= (1 << i);
        }
        return RmlKeyMap::fromRmlModifiers(rmlMods);
    }

}

std::string UIEvent::getKey() const{
    Rml::Event* event = liveEvent(state);
    const int key = engineKey(event);
    return (key >= 0) ? RmlKeyMap::domKey(key, engineModifiers(event)) : std::string();
}

std::string UIEvent::getCode() const{
    const int key = engineKey(liveEvent(state));
    return (key >= 0) ? RmlKeyMap::domCode(key) : std::string();
}

int UIEvent::getKeyCode() const{
    const int key = engineKey(liveEvent(state));
    return (key >= 0) ? key : 0;
}

bool UIEvent::isRepeat() const{
    return HtmlUIEventNames::isRepeat(liveEvent(state));
}

bool UIEvent::getCtrlKey() const{
    return intParameter(state, "ctrl_key") != 0;
}

bool UIEvent::getShiftKey() const{
    return intParameter(state, "shift_key") != 0;
}

bool UIEvent::getAltKey() const{
    return intParameter(state, "alt_key") != 0;
}

bool UIEvent::getMetaKey() const{
    return intParameter(state, "meta_key") != 0;
}

float UIEvent::getClientX() const{
    return floatParameter(state, "mouse_x");
}

float UIEvent::getClientY() const{
    return floatParameter(state, "mouse_y");
}

int UIEvent::getButton() const{
    // RmlUi: 0 left, 1 right, 2 middle; DOM: 0 left, 1 middle, 2 right
    const int button = intParameter(state, "button", 0);
    switch (button){
        case 1: return 2;
        case 2: return 1;
        default: return button;
    }
}

float UIEvent::getDeltaX() const{
    return floatParameter(state, "wheel_delta_x");
}

float UIEvent::getDeltaY() const{
    return floatParameter(state, "wheel_delta_y");
}

std::string UIEvent::getData() const{
    Rml::Event* event = liveEvent(state);
    return event ? event->GetParameter<Rml::String>("text", "") : std::string();
}

UIValue UIEvent::getDetail() const{
    if (!isValid() || !state->detail)
        return UIValue();
    return *state->detail;
}
