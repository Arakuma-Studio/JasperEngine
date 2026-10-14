// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/HtmlButtonElement.h"

#include "htmlui/compat/runtime/HtmlFormSupport.h"

#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>

using namespace jasperengine::htmlui::compat;

// A disabled button gets no click: stopped at the button before any listener of the button or
// a later bubble phase sees it. At the target RmlUi runs the non-capture listeners first, in
// the order they were added, so this one (added by the constructor) is the first.
class HtmlButtonElement::DisabledGuard : public Rml::EventListener {
public:
    explicit DisabledGuard(Rml::Element* button): button(button) {}

    void ProcessEvent(Rml::Event& event) override{
        if (!forms::isDisabled(button))
            return;
        event.StopImmediatePropagation();
        event.PreventDefault();
    }

private:
    Rml::Element* button;
};

RMLUI_RTTI_Define(HtmlButtonElement)

HtmlButtonElement::HtmlButtonElement(const Rml::String& tag): Rml::Element(tag){
    guard = std::make_unique<DisabledGuard>(this);
    for (Rml::EventId id : {Rml::EventId::Click, Rml::EventId::Dblclick})
        AddEventListener(id, guard.get(), false);
}

HtmlButtonElement::~HtmlButtonElement(){
    for (Rml::EventId id : {Rml::EventId::Click, Rml::EventId::Dblclick})
        RemoveEventListener(id, guard.get(), false);
}

bool HtmlButtonElement::HasButtonLayout() const{
    return true;
}

bool HtmlButtonElement::HasNativeAppearance() const{
    return forms::hasNativeAppearance(this);
}

std::string HtmlButtonElement::getType() const{
    std::string type = forms::lowerAttribute(const_cast<HtmlButtonElement*>(this), "type", "submit");
    if (type != "reset" && type != "button")
        type = "submit";
    return type;
}

void HtmlButtonElement::OnAttributeChange(const Rml::ElementAttributes& changed_attributes){
    Rml::Element::OnAttributeChange(changed_attributes);
    if (changed_attributes.count("disabled"))
        SetPseudoClass("disabled", HasAttribute("disabled"));
}

void HtmlButtonElement::OnUpdate(){
    Rml::Element::OnUpdate();
    if (updated)
        return;
    updated = true;
    // before any edit: what a reset goes back to
    if (getType() == "reset")
        forms::rememberDefaults(forms::formOf(this));
    if (HasAttribute("autofocus") && !forms::isDisabled(this))
        Focus(true);
}

void HtmlButtonElement::ProcessDefaultAction(Rml::Event& event){
    Rml::Element::ProcessDefaultAction(event);
    if (forms::isDisabled(this))
        return;

    if (event == Rml::EventId::Keydown || event == Rml::EventId::Keyup){
        forms::activateFromKey(this, event);
        return;
    }
    if (event.GetId() != Rml::EventId::Click)
        return;

    const std::string type = getType();
    if (type == "submit")
        forms::submit(this);
    else if (type == "reset")
        forms::reset(forms::formOf(this));
}
