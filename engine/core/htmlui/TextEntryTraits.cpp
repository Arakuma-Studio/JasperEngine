// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/TextEntryTraits.h"

#include "htmlui/compat/runtime/HtmlInputElement.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ScrollTypes.h>
#include <RmlUi/Core/Variant.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

using namespace jasperengine;

namespace {

    const EnterKey* actionKey = nullptr;

    // an attribute's text without a copy (names stay within the small-string buffer); empty when absent
    bool attribute(Rml::Element* element, const char* name, std::string_view& out){
        const Rml::Variant* value = element->GetAttribute(name);
        if (!value)
            return false;
        if (value->GetType() == Rml::Variant::STRING){
            out = value->GetReference<Rml::String>();
            return true;
        }
        // a number set from a script or a data binding: rare, read through a copy
        static thread_local Rml::String converted;
        converted = value->Get<Rml::String>();
        out = converted;
        return true;
    }

    bool equalsLower(std::string_view text, std::string_view lower){
        if (text.size() != lower.size())
            return false;
        for (size_t i = 0; i < text.size(); i++){
            if ((char)std::tolower((unsigned char)text[i]) != lower[i])
                return false;
        }
        return true;
    }

    // a decimal number of the whole text (no allocation: the attribute is copied to the stack)
    bool parseNumber(std::string_view text, double& value){
        char buffer[64];
        if (text.empty() || text.size() >= sizeof(buffer))
            return false;
        text.copy(buffer, text.size());
        buffer[text.size()] = '\0';
        char* end = nullptr;
        const double parsed = std::strtod(buffer, &end);
        if (end != buffer + text.size() || !std::isfinite(parsed))
            return false;
        value = parsed;
        return true;
    }

    bool isTextInputType(std::string_view type){
        return type.empty() || equalsLower(type, "text") || equalsLower(type, "password") || equalsLower(type, "search") ||
               equalsLower(type, "email") || equalsLower(type, "url") || equalsLower(type, "tel") || equalsLower(type, "number");
    }

    // the next (previous) text field after `field` in tab order, without wrapping around
    Rml::Element* adjacentTextField(Rml::Element* field, bool forward){
        Rml::ElementDocument* document = field->GetOwnerDocument();
        if (!document)
            return nullptr;
        Rml::Element* current = field;
        while (Rml::Element* next = document->FindNextTabElement(current, forward, false)){
            if (next == field || next == current)
                return nullptr;
            if (htmlui::isTextEntryField(next))
                return next;
            current = next;
        }
        return nullptr;
    }

}

bool htmlui::isTextEntryField(Rml::Element* element){
    if (!element)
        return false;
    const Rml::String& tag = element->GetTagName();
    if (tag == "textarea")
        return true;
    if (tag != "input")
        return false;
    std::string_view type;
    attribute(element, "type", type);
    return isTextInputType(type);
}

TextEntryTraits htmlui::textEntryTraitsOf(Rml::Element* field){
    TextEntryTraits traits;
    if (!field)
        return traits;

    std::string_view value;
    if (field->GetTagName() == "textarea"){
        traits.multiline = true;
        traits.enterKey = EnterKey::Enter;
    }else{
        std::string_view type;
        attribute(field, "type", type);
        if (equalsLower(type, "email")){
            traits.keyboard = KeyboardType::Email;
        }else if (equalsLower(type, "url")){
            traits.keyboard = KeyboardType::Url;
        }else if (equalsLower(type, "tel")){
            traits.keyboard = KeyboardType::Phone;
        }else if (equalsLower(type, "search")){
            traits.keyboard = KeyboardType::Search;
            traits.enterKey = EnterKey::Search;
        }else if (equalsLower(type, "password")){
            traits.secure = true;
        }else if (equalsLower(type, "number")){
            // whole steps from a minimum of 0 or more: digits only; else a decimal point, and a
            // sign while negative numbers are allowed
            double step = 1.0;
            if (attribute(field, "step", value) && !parseNumber(value, step))
                step = 0.5;   // "any" or a typo: not whole
            double minimum = 0.0;
            const bool hasMin = attribute(field, "min", value) && parseNumber(value, minimum);
            if (step > 0.0 && std::floor(step) == step && hasMin && minimum >= 0.0){
                traits.keyboard = KeyboardType::Number;
            }else{
                traits.keyboard = KeyboardType::Decimal;
                traits.signedNumber = !hasMin || minimum < 0.0;
            }
        }
    }

    KeyboardType keyboard;
    if (attribute(field, "inputmode", value) && keyboardTypeFromName(value, keyboard) && !equalsLower(value, "number"))
        traits.keyboard = keyboard;
    EnterKey enterKey;
    if (explicitEnterKeyHint(field, enterKey))
        traits.enterKey = enterKey;
    AutoCapitalize capitalize;
    if (attribute(field, "autocapitalize", value) && autoCapitalizeFromName(value, capitalize))
        traits.capitalize = capitalize;
    if (attribute(field, "autocorrect", value)){
        if (value.empty() || equalsLower(value, "on"))
            traits.autocorrect = true;
        else if (equalsLower(value, "off"))
            traits.autocorrect = false;
    }
    double maxLength = 0.0;
    if (attribute(field, "maxlength", value) && parseNumber(value, maxLength) && maxLength >= 0.0 && maxLength < 2147483647.0)
        traits.maxLength = (int)maxLength;
    return traits;
}

bool htmlui::explicitEnterKeyHint(Rml::Element* field, EnterKey& key){
    std::string_view value;
    return attribute(field, "enterkeyhint", value) && !value.empty() && enterKeyFromName(value, key) && key != EnterKey::Default;
}

const EnterKey* htmlui::actionKeyInFlight(){
    return actionKey;
}

htmlui::ActionKeyScope::ActionKeyScope(EnterKey key): previous(actionKey), key(key){
    actionKey = &this->key;
}

htmlui::ActionKeyScope::~ActionKeyScope(){
    actionKey = previous;
}

void htmlui::runEnterKeyAction(Rml::Element* field, EnterKey key){
    if (!field)
        return;
    Rml::ObserverPtr<Rml::Element> alive = field->GetObserverPtr();

    if (key == EnterKey::Next || key == EnterKey::Previous){
        if (Rml::Element* next = adjacentTextField(field, key == EnterKey::Next)){
            if (next->Focus(true)){
                next->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest));
                return;
            }
        }
        field->Blur();
        return;
    }
    if (key == EnterKey::Done){
        field->Blur();
        return;
    }

    if (field->GetTagName() == "textarea"){
        // through the field's own input path: its maxlength and listeners apply
        Rml::Dictionary parameters;
        parameters["text"] = Rml::String("\n");
        field->DispatchEvent(Rml::EventId::Textinput, parameters);
        return;
    }

    // implicit submission, as a hardware Enter, then the keyboard closes
    if (auto* input = rmlui_dynamic_cast<htmlui::compat::HtmlInputElement*>(field)){
        if (input->getHtmlType() == "number")
            input->clampToRange();
        input->implicitSubmit();
    }
    if (Rml::Element* still = alive.get())
        still->Blur();
}
