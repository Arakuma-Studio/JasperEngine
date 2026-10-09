// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/HtmlLabelBehavior.h"

#include "htmlui/compat/runtime/HtmlFormSupport.h"

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>

using namespace jasperengine::htmlui::compat;

namespace {

    Rml::Element* firstLabelable(Rml::Element* element){
        for (int i = 0; i < element->GetNumChildren(); i++){
            Rml::Element* child = element->GetChild(i);
            if (HtmlLabelElement::isLabelable(child))
                return child;
            if (Rml::Element* found = firstLabelable(child))
                return found;
        }
        return nullptr;
    }

}

RMLUI_RTTI_Define(HtmlLabelElement)

HtmlLabelElement::HtmlLabelElement(const Rml::String& tag): Rml::Element(tag) {}

HtmlLabelElement::~HtmlLabelElement() = default;

bool HtmlLabelElement::isLabelable(Rml::Element* element){
    if (!element)
        return false;
    const std::string tag = element->GetTagName();
    if (tag == "input")
        return forms::lowerAttribute(element, "type", "text") != "hidden";
    return tag == "button" || tag == "meter" || tag == "output" || tag == "progress" || tag == "select" || tag == "textarea";
}

Rml::Element* HtmlLabelElement::getControl(){
    const std::string id = GetAttribute<Rml::String>("for", "");
    if (!id.empty()){
        Rml::ElementDocument* document = GetOwnerDocument();
        Rml::Element* named = document ? document->GetElementById(id) : nullptr;
        return isLabelable(named) ? named : nullptr;
    }
    return firstLabelable(this);
}

void HtmlLabelElement::OnPseudoClassChange(const Rml::String& pseudo_class, bool activate){
    Rml::Element::OnPseudoClassChange(pseudo_class, activate);
    if (pseudo_class == "active" || pseudo_class == "hover"){
        if (Rml::Element* control = getControl())
            OverridePseudoClass(control, pseudo_class, activate);
    }
}

void HtmlLabelElement::ProcessDefaultAction(Rml::Event& event){
    Rml::Element::ProcessDefaultAction(event);
    if (event.GetId() != Rml::EventId::Click || forwarding)
        return;
    Rml::Element* control = getControl();
    // a click on the control itself (inside the label) is the control's
    if (!control || control->Contains(event.GetTargetElement()) || forms::isDisabled(control))
        return;
    forwarding = true;
    control->Focus();
    control->Click();
    forwarding = false;
}
