// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUILocalization.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Factory.h>

using namespace jasperengine;

namespace {

    // the element's text as one text node, unless it already is exactly that
    void htmlLocSetText(Rml::Element* element, const std::string& text){
        if (element->GetNumChildren() == 1){
            if (Rml::ElementText* only = rmlui_dynamic_cast<Rml::ElementText*>(element->GetFirstChild()); only && only->GetText() == text) return;
        }
        if (element->GetNumChildren() == 0 && text.empty()) return;
        while (element->GetNumChildren() > 0) element->RemoveChild(element->GetFirstChild());
        if (text.empty()) return;
        Rml::ElementPtr node = Rml::Factory::InstanceElement(nullptr, "#text", "#text", Rml::XMLAttributes());
        if (Rml::ElementText* textNode = rmlui_dynamic_cast<Rml::ElementText*>(node.get())){
            textNode->SetText(text);
            element->AppendChild(std::move(node));
        }
    }

    void htmlLocApplyOne(Rml::Element* element){
        const std::string key = element->GetAttribute<Rml::String>(HtmlUILocalization::KEY_ATTRIBUTE, "");
        if (key.empty()) return;
        const LocalizationArgs args = LocalizationArgs::fromJson(element->GetAttribute<Rml::String>(HtmlUILocalization::ARGS_ATTRIBUTE, ""));
        htmlLocSetText(element, Localization::get(key, args));
    }

    int htmlLocWalk(Rml::Element* element){
        int count = 0;
        if (element->HasAttribute(HtmlUILocalization::KEY_ATTRIBUTE)){
            htmlLocApplyOne(element);
            count++;
            // its children are its text now
            return count;
        }
        for (int i = 0; i < element->GetNumChildren(); i++) count += htmlLocWalk(element->GetChild(i));
        return count;
    }

}

int HtmlUILocalization::apply(Rml::Element* root){
    return root ? htmlLocWalk(root) : 0;
}

void HtmlUILocalization::setLocalized(Rml::Element* element, const std::string& key, const LocalizationArgs& args){
    if (!element) return;
    element->SetAttribute(KEY_ATTRIBUTE, key);
    const std::string json = args.toJson();
    if (json.empty()) element->RemoveAttribute(ARGS_ATTRIBUTE);
    else element->SetAttribute(ARGS_ATTRIBUTE, json);
    htmlLocApplyOne(element);
}
