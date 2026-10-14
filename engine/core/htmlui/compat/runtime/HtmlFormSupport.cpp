// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/HtmlFormSupport.h"

#include "htmlui/compat/runtime/HtmlInputElement.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Elements/ElementForm.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/ObserverPtr.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/StyleSheetSpecification.h>

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <vector>

using namespace jasperengine::htmlui::compat;

namespace {

    // the default values of textareas (text) and selects (index) by element
    struct Default {
        Rml::ObserverPtr<Rml::Element> element;
        std::string value;
        int selection = -1;
    };

    std::unordered_map<Rml::Element*, Default>& defaults(){
        static std::unordered_map<Rml::Element*, Default> map;
        return map;
    }

    // every element of the form: its descendants, then the elements naming it with form="id"
    std::vector<Rml::Element*> controlsOf(Rml::ElementForm* form){
        std::vector<Rml::Element*> controls;
        Rml::ElementList list;
        for (const char* tag : {"input", "textarea", "select"}){
            list.clear();
            Rml::ElementUtilities::GetElementsByTagName(list, form, tag);
            controls.insert(controls.end(), list.begin(), list.end());
        }
        const std::string id = form->GetId();
        Rml::ElementDocument* document = form->GetOwnerDocument();
        if (!id.empty() && document){
            for (const char* tag : {"input", "textarea", "select"}){
                list.clear();
                Rml::ElementUtilities::GetElementsByTagName(list, document, tag);
                for (Rml::Element* element : list){
                    if (element->GetAttribute<Rml::String>("form", "") == id &&
                        std::find(controls.begin(), controls.end(), element) == controls.end())
                        controls.push_back(element);
                }
            }
        }
        return controls;
    }

    void remember(Rml::Element* element){
        auto& map = defaults();
        auto it = map.find(element);
        if (it != map.end() && it->second.element.get() == element)
            return;
        Default entry;
        entry.element = element->GetObserverPtr();
        if (auto* select = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(element))
            entry.selection = select->GetSelection();
        else if (auto* area = rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(element))
            entry.value = area->GetValue();
        map[element] = entry;
        // drop the entries of elements that are gone
        for (auto i = map.begin(); i != map.end();){
            if (!i->second.element)
                i = map.erase(i);
            else
                ++i;
        }
    }

}

std::string forms::lowerAttribute(Rml::Element* element, const char* name, const char* fallback){
    std::string value = element->GetAttribute<Rml::String>(name, fallback);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    const size_t first = value.find_first_not_of(" \t\r\n");
    const size_t last = value.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : value.substr(first, last - first + 1);
}

bool forms::isDisabled(Rml::Element* element){
    return element && element->HasAttribute("disabled");
}

bool forms::hasNativeAppearance(const Rml::Element* element){
    if (!element)
        return false;
    const Rml::Property* appearance = const_cast<Rml::Element*>(element)->GetProperty("--html-appearance");
    return !appearance || Rml::StringUtilities::StripWhitespace(appearance->Get<Rml::String>()) != "none";
}

bool forms::themedComputedValue(Rml::Element* element, Rml::PropertyId id, std::string& text){
    using Rml::PropertyId;
    if (!element)
        return false;
    if (id == PropertyId::Display && element->HasButtonLayout()){
        // with the look Chromium's theme computes it; without, the declared value stays (laid out as the fork computes)
        if (!element->HasNativeAppearance())
            return false;
        using Rml::Style::Display;
        switch (element->GetComputedValues().display()){
            case Display::Block: text = "block"; return true;
            case Display::InlineBlock: text = "inline-block"; return true;
            default: return false;   // as declared (flex, grid, flow-root, ...)
        }
    }
    if (!element->HasThemedBox())
        return false;
    switch (id){
        case PropertyId::BorderTopWidth:
        case PropertyId::BorderRightWidth:
        case PropertyId::BorderBottomWidth:
        case PropertyId::BorderLeftWidth:
        case PropertyId::PaddingTop:
        case PropertyId::PaddingRight:
        case PropertyId::PaddingBottom:
        case PropertyId::PaddingLeft:
        case PropertyId::RmlUi_BorderTopLeftRadiusX:
        case PropertyId::RmlUi_BorderTopLeftRadiusY:
        case PropertyId::RmlUi_BorderTopRightRadiusX:
        case PropertyId::RmlUi_BorderTopRightRadiusY:
        case PropertyId::RmlUi_BorderBottomRightRadiusX:
        case PropertyId::RmlUi_BorderBottomRightRadiusY:
        case PropertyId::RmlUi_BorderBottomLeftRadiusX:
        case PropertyId::RmlUi_BorderBottomLeftRadiusY:
            text = "0px";
            return true;
        case PropertyId::BorderTopStyle:
        case PropertyId::BorderRightStyle:
        case PropertyId::BorderBottomStyle:
        case PropertyId::BorderLeftStyle:
            text = "none";
            return true;
        default:
            return false;
    }
}

Rml::ElementForm* forms::formOf(Rml::Element* control){
    if (!control)
        return nullptr;
    const std::string id = control->GetAttribute<Rml::String>("form", "");
    if (!id.empty()){
        Rml::ElementDocument* document = control->GetOwnerDocument();
        Rml::Element* named = document ? document->GetElementById(id) : nullptr;
        return rmlui_dynamic_cast<Rml::ElementForm*>(named);
    }
    for (Rml::Element* parent = control->GetParentNode(); parent; parent = parent->GetParentNode()){
        if (auto* form = rmlui_dynamic_cast<Rml::ElementForm*>(parent))
            return form;
    }
    return nullptr;
}

void forms::submit(Rml::Element* submitter){
    if (Rml::ElementForm* form = formOf(submitter))
        form->Submit(submitter->GetAttribute<Rml::String>("name", ""), submitter->GetAttribute<Rml::String>("value", ""));
}

void forms::rememberDefaults(Rml::ElementForm* form){
    if (!form)
        return;
    for (Rml::Element* control : controlsOf(form)){
        if (rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(control) || rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(control))
            remember(control);
    }
}

void forms::reset(Rml::ElementForm* form){
    if (!form)
        return;
    // cancelable, before anything changes (HTML); RmlUi reports a stopped event like a
    // prevented one, so stopping it cancels the reset too
    if (!form->DispatchEvent("reset", Rml::Dictionary(), true, true))
        return;
    for (Rml::Element* control : controlsOf(form)){
        if (auto* input = rmlui_dynamic_cast<HtmlInputElement*>(control)){
            input->resetToDefault();
            continue;
        }
        auto it = defaults().find(control);
        if (it == defaults().end() || it->second.element.get() != control)
            continue;
        if (auto* select = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(control))
            select->SetSelection(it->second.selection);
        else if (auto* area = rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(control))
            area->SetValue(it->second.value);
    }
}

bool forms::activateFromKey(Rml::Element* element, Rml::Event& event, Activation activation){
    if (event.GetTargetElement() != element)
        return false;
    // RmlUi's document clicks a focused tab-index:auto element on Enter and Space key down (its
    // default action, after this one): stopping the event keeps it to one activation, at the
    // time HTML has it
    const auto key = (Rml::Input::KeyIdentifier)event.GetParameter<int>("key_identifier", 0);
    const bool enter = key == Rml::Input::KI_RETURN || key == Rml::Input::KI_NUMPADENTER;
    const bool space = key == Rml::Input::KI_SPACE;
    if (event == Rml::EventId::Keydown && (enter || space)){
        if (enter && activation != Activation::SpaceOnly)
            element->Click();
        event.StopPropagation();
        return true;
    }
    if (event == Rml::EventId::Keyup && space && activation != Activation::EnterOnly){
        element->Click();
        return true;
    }
    return false;
}
