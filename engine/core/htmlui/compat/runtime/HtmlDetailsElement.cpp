// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/HtmlDetailsElement.h"

#include "htmlui/compat/runtime/HtmlFormSupport.h"
#include "htmlui/compat/runtime/ListMarkers.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/StyleTypes.h>

using namespace jasperengine::htmlui::compat;

RMLUI_RTTI_Define(HtmlDetailsElement)

HtmlDetailsElement::HtmlDetailsElement(const Rml::String& tag): Rml::Element(tag) {}

HtmlDetailsElement::~HtmlDetailsElement() = default;

bool HtmlDetailsElement::isOpen() const{
    return HasAttribute("open");
}

void HtmlDetailsElement::setOpen(bool value){
    if (value == isOpen())
        return;
    if (value)
        SetAttribute("open", "");
    else
        RemoveAttribute("open");
}

void HtmlDetailsElement::toggle(){
    setOpen(!isOpen());
}

void HtmlDetailsElement::OnAttributeChange(const Rml::ElementAttributes& changed_attributes){
    Rml::Element::OnAttributeChange(changed_attributes);
    if (!changed_attributes.count("open"))
        return;

    // every change queues a toggle, the initial open state too (as browsers do)
    const bool now = HasAttribute("open");
    if (now == open)
        return;
    if (!pending){
        pending = true;
        pendingOld = open;
    }
    open = now;
    if (Rml::Context* context = GetContext())
        context->RequestNextUpdate(0);

    // an exclusive accordion: the other open details of the same name close
    const std::string name = GetAttribute<Rml::String>("name", "");
    Rml::ElementDocument* document = GetOwnerDocument();
    if (now && !name.empty() && document){
        Rml::ElementList all;
        Rml::ElementUtilities::GetElementsByTagName(all, document, "details");
        for (Rml::Element* other : all){
            if (other != this && other->HasAttribute("open") && other->GetAttribute<Rml::String>("name", "") == name)
                other->RemoveAttribute("open");
        }
    }
}

void HtmlDetailsElement::OnUpdate(){
    Rml::Element::OnUpdate();
    // the content of a closed details is not rendered: its elements through the user-agent sheet
    // (details:not([open]) > :not(summary)), its bare text here (no selector reaches text)
    const bool closed = !HasAttribute("open");
    for (int i = 0; i < GetNumChildren(); i++){
        Rml::Element* child = GetChild(i);
        if (child->GetTagName() != "#text")
            continue;
        const bool hidden = child->GetLocalProperty(Rml::PropertyId::Display) != nullptr;
        if (closed && !hidden)
            child->SetProperty(Rml::PropertyId::Display, Rml::Property(Rml::Style::Display::None));
        else if (!closed && hidden)
            child->RemoveProperty(Rml::PropertyId::Display);
    }
    if (!pending)
        return;
    pending = false;
    // toggled back before the event went out: nothing happened
    if (pendingOld == open)
        return;
    Rml::Dictionary parameters;
    parameters["newState"] = Rml::Variant(Rml::String(open ? "open" : "closed"));
    parameters["oldState"] = Rml::Variant(Rml::String(pendingOld ? "open" : "closed"));
    DispatchEvent("toggle", parameters, false, false);
}

// ---------------------------------------------------------------- summary

RMLUI_RTTI_Define(HtmlSummaryElement)

HtmlSummaryElement::HtmlSummaryElement(const Rml::String& tag): HtmlListItemElement(tag) {}

HtmlSummaryElement::~HtmlSummaryElement() = default;

HtmlDetailsElement* HtmlSummaryElement::getDetails(){
    auto* details = rmlui_dynamic_cast<HtmlDetailsElement*>(GetParentNode());
    if (!details)
        return nullptr;
    // only the first summary child is the details' summary
    for (int i = 0; i < details->GetNumChildren(); i++){
        Rml::Element* child = details->GetChild(i);
        if (child->GetTagName() == "summary")
            return child == this ? details : nullptr;
    }
    return nullptr;
}

std::string HtmlSummaryElement::markerTextFor(const std::string& listStyleType){
    // no counter: the disclosure triangle (or what the author set)
    return ListMarkers::text(listStyleType.empty() ? std::string("disclosure-closed") : listStyleType, 1);
}

void HtmlSummaryElement::ProcessDefaultAction(Rml::Event& event){
    HtmlListItemElement::ProcessDefaultAction(event);
    HtmlDetailsElement* details = getDetails();
    if (!details)
        return;
    if (event == Rml::EventId::Keydown || event == Rml::EventId::Keyup){
        forms::activateFromKey(this, event);
        return;
    }
    if (event == Rml::EventId::Click)
        details->toggle();
}
