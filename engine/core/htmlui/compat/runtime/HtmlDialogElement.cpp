// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/HtmlDialogElement.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

using namespace jasperengine::htmlui::compat;

namespace {

    bool focusable(Rml::Element* element){
        const std::string tag = element->GetTagName();
        if (element->HasAttribute("disabled"))
            return false;
        if (tag == "input" || tag == "button" || tag == "select" || tag == "textarea" || tag == "summary")
            return true;
        if (tag == "a" && element->HasAttribute("href"))
            return true;
        return element->GetComputedValues().tab_index() == Rml::Style::TabIndex::Auto;
    }

    // the autofocus descendant, else the first focusable one (document order)
    Rml::Element* initialFocus(Rml::Element* root, bool autofocusOnly){
        for (int i = 0; i < root->GetNumChildren(); i++){
            Rml::Element* child = root->GetChild(i);
            if (autofocusOnly ? child->HasAttribute("autofocus") : focusable(child))
                return child;
            if (Rml::Element* found = initialFocus(child, autofocusOnly))
                return found;
        }
        return nullptr;
    }

}

RMLUI_RTTI_Define(HtmlDialogElement)

HtmlDialogElement::HtmlDialogElement(const Rml::String& tag): Rml::Element(tag) {}

HtmlDialogElement::~HtmlDialogElement() = default;

bool HtmlDialogElement::isOpen() const{
    return HasAttribute("open");
}

void HtmlDialogElement::show(){
    if (isOpen())
        return;
    modal = false;
    SetAttribute("open", "");
}

void HtmlDialogElement::showModal(){
    if (isOpen())
        return;
    modal = true;
    SetAttribute("open", "");
    // the focus goes into the dialog (it becomes visible with the next update's styles)
    Rml::Element* target = initialFocus(this, true);
    if (!target)
        target = initialFocus(this, false);
    if (target)
        target->Focus(true);
    else
        Focus();
}

void HtmlDialogElement::close(const std::string& value){
    if (!isOpen())
        return;
    if (!value.empty())
        returnValue = value;
    RemoveAttribute("open");
}

void HtmlDialogElement::OnAttributeChange(const Rml::ElementAttributes& changed_attributes){
    Rml::Element::OnAttributeChange(changed_attributes);
    if (!changed_attributes.count("open"))
        return;
    if (!HasAttribute("open")){
        // closed (by close() or by removing the attribute): `close` follows
        modal = false;
        closePending = true;
        if (Rml::Context* context = GetContext())
            context->RequestNextUpdate(0);
    }
}

void HtmlDialogElement::OnUpdate(){
    Rml::Element::OnUpdate();
    if (!closePending)
        return;
    closePending = false;
    Rml::Dictionary parameters;
    parameters["returnValue"] = Rml::Variant(returnValue);
    DispatchEvent("close", parameters, false, false);
}

void HtmlDialogElement::ProcessDefaultAction(Rml::Event& event){
    Rml::Element::ProcessDefaultAction(event);
    if (event != Rml::EventId::Keydown || !modal || !isOpen())
        return;
    const auto key = (Rml::Input::KeyIdentifier)event.GetParameter<int>("key_identifier", 0);
    if (key != Rml::Input::KI_ESCAPE)
        return;
    // Escape inside a modal dialog: a cancelable `cancel`, then close
    if (DispatchEvent("cancel", Rml::Dictionary(), true, false))
        close();
}
