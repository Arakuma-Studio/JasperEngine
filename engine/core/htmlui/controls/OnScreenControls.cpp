// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "OnScreenControls.h"

#include "System.h"
#include "input/InputActions.h"
#include "object/ui/HtmlUIEventListener.h"
#include "object/ui/UIValue.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/ID.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include "input/InputSourceShim.h"   // SYNC4-ABSORB(input)

using namespace jasperengine;

namespace {

    std::unique_ptr<Rml::ElementInstancerGeneric<UIOnScreenControl>>& controlInstancer(){
        static std::unique_ptr<Rml::ElementInstancerGeneric<UIOnScreenControl>> value;
        return value;
    }

    // Joystick Pack's SnapFloat (as UISystem's stick): the angle to up decides with both axes
    float snapValue(float value, float upX, float upY, bool horizontal, bool bothAxes){
        if (value == 0.0f)
            return value;
        if (bothAxes){
            const float length = std::sqrt(upX * upX + upY * upY);
            float angle = 0.0f;
            if (length > 0.0f)
                angle = std::acos(std::max(-1.0f, std::min(1.0f, upY / length))) * 180.0f / 3.14159265358979f;
            if (horizontal)
                return (angle < 22.5f || angle > 157.5f) ? 0.0f : (value > 0.0f ? 1.0f : -1.0f);
            return (angle > 67.5f && angle < 112.5f) ? 0.0f : (value > 0.0f ? 1.0f : -1.0f);
        }
        return value > 0.0f ? 1.0f : -1.0f;
    }

    // the action has a mouse-movement binding: the mouse already moves it
    bool mouseBound(const std::string& action){
        if (sync4input::mouseBound(action))   // SYNC4-ABSORB(input)
            return true;
        return false;
    }

    std::string formatValue(float value){
        char text[32];
        std::snprintf(text, sizeof(text), "%.3f", value);
        return text;
    }

}

RMLUI_RTTI_Define(UIOnScreenControl)

UIOnScreenControl::UIOnScreenControl(const Rml::String& tag): Rml::Element(tag){
    if (tag == "onscreen-stick")
        kind = Kind::STICK;
    else if (tag == "onscreen-button")
        kind = Kind::BUTTON;
    else
        kind = Kind::AREA;
    for (Rml::EventId id : {Rml::EventId::Pointerdown, Rml::EventId::Pointermove, Rml::EventId::Pointerup,
                            Rml::EventId::Pointercancel, Rml::EventId::Pointerover, Rml::EventId::Pointerout})
        AddEventListener(id, this);
    listening = true;
}

UIOnScreenControl::~UIOnScreenControl(){
    // removed or unloaded: nothing stays held
    sync4input::clearSource(owner());
    if (listening){
        for (Rml::EventId id : {Rml::EventId::Pointerdown, Rml::EventId::Pointermove, Rml::EventId::Pointerup,
                                Rml::EventId::Pointercancel, Rml::EventId::Pointerover, Rml::EventId::Pointerout})
            RemoveEventListener(id, this);
    }
}

void UIOnScreenControl::registerInstancers(){
    if (!controlInstancer())
        controlInstancer() = std::make_unique<Rml::ElementInstancerGeneric<UIOnScreenControl>>();
    for (const char* tag : {"onscreen-stick", "onscreen-button", "touch-area"})
        Rml::Factory::RegisterElementInstancer(tag, controlInstancer().get());
}

void UIOnScreenControl::releaseInstancers(){
    controlInstancer().reset();
}

float UIOnScreenControl::attributeFloat(const char* name, float fallback) const{
    const Rml::Variant* value = GetAttribute(name);
    if (!value)
        return fallback;
    const Rml::String text = value->Get<Rml::String>();
    char* end = nullptr;
    const float number = std::strtof(text.c_str(), &end);
    return (end && end != text.c_str() && std::isfinite(number)) ? number : fallback;
}

std::string UIOnScreenControl::attributeString(const char* name, const char* fallback) const{
    const Rml::Variant* value = GetAttribute(name);
    return value ? value->Get<Rml::String>() : std::string(fallback);
}

bool UIOnScreenControl::attributeFlag(const char* name) const{
    const Rml::Variant* value = GetAttribute(name);
    if (!value)
        return false;
    const Rml::String text = Rml::StringUtilities::ToLower(value->Get<Rml::String>());
    return text != "false" && text != "0";
}

Rml::Element* UIOnScreenControl::part(const char* className){
    Rml::ElementList found;
    GetElementsByClassName(found, className);
    return found.empty() ? nullptr : found.front();
}

// a stick without children gets its base and knob (authors may supply their own with these classes)
void UIOnScreenControl::ensureParts(){
    if (kind != Kind::STICK || part("stick-base"))
        return;
    Rml::ElementPtr base = Rml::Factory::InstanceElement(this, "div", "div", Rml::XMLAttributes());
    Rml::ElementPtr knob = Rml::Factory::InstanceElement(this, "div", "div", Rml::XMLAttributes());
    if (!base || !knob)
        return;
    base->SetClass("stick-base", true);
    knob->SetClass("stick-knob", true);
    base->AppendChild(std::move(knob));
    AppendChild(std::move(base));
}

void UIOnScreenControl::OnUpdate(){
    Rml::Element::OnUpdate();
    ensureParts();

    // visibility="touch-devices-only": hidden and inert where the device has no touch screen
    const bool touchOnly = attributeString("visibility", "always") == "touch-devices-only";
    const bool hide = touchOnly && !System::instance().isTouchDevice();
    if (hide != hiddenForDevice){
        hiddenForDevice = hide;
        if (hide)
            SetProperty(Rml::PropertyId::Visibility, Rml::Property(Rml::Style::Visibility::Hidden));
        else
            RemoveProperty(Rml::PropertyId::Visibility);
    }

    // display: none, a hidden document or hidden for the device: released
    if (!pointers.empty()){
        Rml::ElementDocument* document = GetOwnerDocument();
        if (hide || !IsVisible(true) || !document || !document->IsVisible())
            releaseAll(true);
    }
}

void UIOnScreenControl::OnChildRemove(Rml::Element* child){
    Rml::Element::OnChildRemove(child);
    // RmlUi calls it on the removed element itself too: a control taken out of its document
    // (UIElement::remove keeps it for a later insert) lets go of what it holds
    if (child == this)
        releaseAll(false);
}

void UIOnScreenControl::ProcessEvent(Rml::Event& event){
    const int pointerId = event.GetParameter<int>("pointer_id", 0);
    if (pointerId == 0 || hiddenForDevice)
        return;
    switch (event.GetId()){
        case Rml::EventId::Pointerdown:
            press(event, pointerId);
            break;
        case Rml::EventId::Pointermove:
            move(event, pointerId);
            break;
        case Rml::EventId::Pointerup:
        case Rml::EventId::Pointercancel:
            release(pointerId, true);
            break;
        case Rml::EventId::Pointerover:
            // a finger already down sliding onto a button (press-on-enter)
            if (kind == Kind::BUTTON && attributeFlag("press-on-enter") && event.GetParameter<int>("buttons", 0) != 0 &&
                event.GetParameter<Rml::String>("pointer_type", "") != "mouse")
                press(event, pointerId);
            break;
        case Rml::EventId::Pointerout:
            if (std::find(pointers.begin(), pointers.end(), pointerId) == pointers.end())
                break;
            if (event.GetParameter<int>("buttons", 0) == 0){
                // the pointer ended (up or cancel) away from its capture target: a finger that
                // pressed this button by sliding onto it (press-on-enter) is captured by the
                // element it went down on, which gets its up; the leave of the lifted finger
                // (no sticky hover) is the button's last event of it
                release(pointerId, true);
            }else if (kind == Kind::BUTTON && attributeFlag("release-on-leave")){
                // still over the button or a child: not a leave
                Rml::Context* context = GetContext();
                Rml::Element* hover = context ? context->GetHoverElement(pointerId) : nullptr;
                bool inside = false;
                for (Rml::Element* e = hover; e && !inside; e = e->GetParentNode())
                    inside = (e == this);
                if (!inside)
                    release(pointerId, true);
            }
            break;
        default:
            break;
    }
}

void UIOnScreenControl::press(Rml::Event& event, int pointerId){
    if (std::find(pointers.begin(), pointers.end(), pointerId) != pointers.end())
        return;
    const bool mouse = event.GetParameter<Rml::String>("pointer_type", "") == "mouse";
    if (mouse && event.GetParameter<int>("button", 0) != 0)
        return;   // the left button only, as the component UI
    // the first pointer wins (a button takes several)
    if (kind != Kind::BUTTON && !pointers.empty())
        return;

    pointers.push_back(pointerId);
    pointerIsMouse = mouse;
    // the control keeps the pointer: a finger already is (implicit capture of its target), the mouse is not
    if (mouse)
        SetPointerCapture(pointerId);
    SetPseudoClass("active", true);
    SetAttribute("data-pressed", "true");

    const float px = event.GetParameter<float>("mouse_x", 0.0f);
    const float py = event.GetParameter<float>("mouse_y", 0.0f);
    if (kind == Kind::STICK){
        updateStick(px, py, true);
        dispatchControlEvent("stickstart", valueX, valueY);
    }else if (kind == Kind::BUTTON){
        if (pointers.size() == 1){
            const std::string action = attributeString("action", "Fire1");
            const float value = std::max(-1.0f, std::min(1.0f, attributeFloat("value", 1.0f)));
            if (!action.empty())
                sync4input::setSourceValue(owner(), action, value);
        }
    }else{
        lastX = px;
        lastY = py;
    }
}

void UIOnScreenControl::move(Rml::Event& event, int pointerId){
    if (pointers.empty() || pointers.front() != pointerId || kind == Kind::BUTTON)
        return;
    const float px = event.GetParameter<float>("mouse_x", 0.0f);
    const float py = event.GetParameter<float>("mouse_y", 0.0f);
    if (kind == Kind::STICK){
        updateStick(px, py, false);
        dispatchControlEvent("stickmove", valueX, valueY);
        return;
    }
    const float dx = px - lastX;
    const float dy = py - lastY;
    lastX = px;
    lastY = py;
    if (dx == 0.0f && dy == 0.0f)
        return;
    const float sensitivity = attributeFloat("sensitivity", 0.1f);
    const float sx = attributeFlag("invert-x") ? -sensitivity : sensitivity;
    const float sy = attributeFlag("invert-y") ? -sensitivity : sensitivity;
    const std::string horizontal = attributeString("horizontal-action", "Mouse X");
    const std::string vertical = attributeString("vertical-action", "Mouse Y");
    if (!horizontal.empty() && !(pointerIsMouse && mouseBound(horizontal)))
        sync4input::addSourceDelta(owner(), horizontal, dx * sx);
    if (!vertical.empty() && !(pointerIsMouse && mouseBound(vertical)))
        sync4input::addSourceDelta(owner(), vertical, -dy * sy);
    valueX = dx;
    valueY = dy;
    dispatchControlEvent("areadrag", dx, dy);
}

void UIOnScreenControl::release(int pointerId, bool dispatch){
    auto it = std::find(pointers.begin(), pointers.end(), pointerId);
    if (it == pointers.end())
        return;
    pointers.erase(it);
    if (!pointers.empty())
        return;   // a button another finger holds

    SetPseudoClass("active", false);
    SetAttribute("data-pressed", "false");
    if (kind == Kind::AREA){
        // this frame's deltas stay (a quick flick counts)
        valueX = valueY = 0;
        return;
    }
    sync4input::clearSource(owner());
    if (kind == Kind::STICK){
        valueX = valueY = 0;
        SetAttribute("data-x", "0");
        SetAttribute("data-y", "0");
        placePart(part("stick-knob"), 0, 0);
        if (attributeString("mode", "fixed") != "fixed")
            placePart(part("stick-base"), 0, 0);
        if (dispatch)
            dispatchControlEvent("stickend", 0, 0);
    }
}

void UIOnScreenControl::releaseAll(bool dispatch){
    const std::vector<int> held = pointers;
    for (int pointerId : held)
        release(pointerId, dispatch);
    sync4input::clearSource(owner());
}

// The Joystick Pack maths of UISystem's stick in document px: the base's half width is the radius
void UIOnScreenControl::updateStick(float px, float py, bool first){
    Rml::Element* base = part("stick-base");
    Rml::Element* knob = part("stick-knob");
    const std::string mode = attributeString("mode", "fixed");
    const std::string axes = attributeString("axes", "both");

    float radius = 0.0f;
    // the base's centre at rest: its layout box (the transform placePart moves it by is not part
    // of the layout offset), measured at the press and kept for the drag
    float restX = restCenterX, restY = restCenterY;
    if (base){
        const Rml::Vector2f size = base->GetBox().GetSize(Rml::BoxArea::Border);
        radius = size.x / 2.0f;
        if (first){
            const Rml::Vector2f at = base->GetAbsoluteOffset(Rml::BoxArea::Border);
            restX = restCenterX = at.x + size.x / 2.0f;
            restY = restCenterY = at.y + size.y / 2.0f;
        }
    }else{
        const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Border);
        const Rml::Vector2f at = GetAbsoluteOffset(Rml::BoxArea::Border);
        radius = std::min(size.x, size.y) / 2.0f;
        restX = at.x + size.x / 2.0f;
        restY = at.y + size.y / 2.0f;
    }
    if (radius <= 0.0f)
        return;

    if (first){
        centerX = restX;
        centerY = restY;
        if (mode != "fixed" && base){
            // the base under the finger, kept inside the control's box
            const Rml::Vector2f areaAt = GetAbsoluteOffset(Rml::BoxArea::Border);
            const Rml::Vector2f areaSize = GetBox().GetSize(Rml::BoxArea::Border);
            const Rml::Vector2f baseSize = base->GetBox().GetSize(Rml::BoxArea::Border);
            centerX = px;
            centerY = py;
            const float left = areaAt.x + baseSize.x / 2.0f, right = areaAt.x + areaSize.x - baseSize.x / 2.0f;
            const float top = areaAt.y + baseSize.y / 2.0f, bottom = areaAt.y + areaSize.y - baseSize.y / 2.0f;
            centerX = right >= left ? std::max(left, std::min(right, centerX)) : (left + right) / 2.0f;
            centerY = bottom >= top ? std::max(top, std::min(bottom, centerY)) : (top + bottom) / 2.0f;
            placePart(base, centerX - restX, centerY - restY);
        }
    }

    float inX = (px - centerX) / radius;
    float inY = (py - centerY) / radius;
    if (axes == "horizontal") inY = 0;
    else if (axes == "vertical") inX = 0;

    // Dynamic: the base follows past move-threshold
    const float rawMagnitude = std::sqrt(inX * inX + inY * inY);
    const float moveThreshold = std::max(0.0f, attributeFloat("move-threshold", 1.0f));
    if (mode == "dynamic" && base && rawMagnitude > moveThreshold && rawMagnitude > 0.0f){
        const float scale = (rawMagnitude - moveThreshold) * radius / rawMagnitude;
        centerX += inX * scale;
        centerY += inY * scale;
        placePart(base, centerX - restX, centerY - restY);
    }

    // dead zone, normalised past 1
    const float deadZone = std::max(0.0f, std::min(1.0f, attributeFloat("dead-zone", 0.0f)));
    float magnitude = std::sqrt(inX * inX + inY * inY);
    if (magnitude <= deadZone || magnitude == 0.0f){
        inX = inY = 0;
    }else if (magnitude > 1.0f){
        inX /= magnitude;
        inY /= magnitude;
    }

    const float handleRange = std::max(0.0f, attributeFloat("handle-range", 1.0f));
    placePart(knob, inX * radius * handleRange, inY * radius * handleRange);

    // snap, y up
    float vx = inX, vy = -inY;
    const bool both = axes == "both";
    if (attributeFlag("snap-x")) vx = snapValue(inX, inX, -inY, true, both);
    if (attributeFlag("snap-y")) vy = snapValue(-inY, inX, -inY, false, both);
    valueX = vx;
    valueY = vy;

    const std::string horizontal = attributeString("horizontal-action", "Horizontal");
    const std::string vertical = attributeString("vertical-action", "Vertical");
    if (!horizontal.empty())
        sync4input::setSourceValue(owner(), horizontal, vx);
    if (!vertical.empty())
        sync4input::setSourceValue(owner(), vertical, vy);
    SetAttribute("data-x", formatValue(vx));
    SetAttribute("data-y", formatValue(vy));
}

void UIOnScreenControl::placePart(Rml::Element* element, float x, float y){
    if (!element)
        return;
    if (x == 0.0f && y == 0.0f){
        element->RemoveProperty(Rml::PropertyId::Transform);
        return;
    }
    element->SetProperty("transform", "translate(" + formatValue(x) + "px, " + formatValue(y) + "px)");
}

void UIOnScreenControl::dispatchControlEvent(const char* type, float x, float y){
    UIValue detail = UIValue::object();
    if (kind == Kind::AREA){
        detail["dx"] = UIValue((double)x);
        detail["dy"] = UIValue((double)y);
    }else{
        detail["x"] = UIValue((double)x);
        detail["y"] = UIValue((double)y);
    }
    Rml::Dictionary parameters;
    parameters[HtmlUIEventNames::detailParameter] = (void*)&detail;
    parameters["pointer_id"] = pointers.empty() ? 0 : pointers.front();
    DispatchEvent(type, parameters, false, true);
}
