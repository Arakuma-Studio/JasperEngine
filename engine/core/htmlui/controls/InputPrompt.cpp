// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "InputPrompt.h"
#include "input/InputActions.h"
#include "input/InputScheme.h"
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Factory.h>
#include <memory>
#include <string>

using namespace jasperengine;

namespace {

    std::unique_ptr<Rml::ElementInstancerGeneric<UIInputPrompt>>& promptInstancer(){
        static std::unique_ptr<Rml::ElementInstancerGeneric<UIInputPrompt>> value;
        return value;
    }

    bool whitespaceOnly(const Rml::String& text){
        return text.find_first_not_of(" \t\n\r\f") == Rml::String::npos;
    }

}

RMLUI_RTTI_Define(UIInputPrompt)

UIInputPrompt::UIInputPrompt(const Rml::String& tag): Rml::Element(tag){
}

void UIInputPrompt::registerInstancer(){
    if (!promptInstancer())
        promptInstancer() = std::make_unique<Rml::ElementInstancerGeneric<UIInputPrompt>>();
    Rml::Factory::RegisterElementInstancer("input-prompt", promptInstancer().get());
}

void UIInputPrompt::releaseInstancer(){
    promptInstancer().reset();
}

void UIInputPrompt::OnAttributeChange(const Rml::ElementAttributes& changed_attributes){
    Rml::Element::OnAttributeChange(changed_attributes);
    if (changed_attributes.count("action") || changed_attributes.count("scheme"))
        dirty = true;
}

void UIInputPrompt::OnUpdate(){
    Rml::Element::OnUpdate();
    // the scheme, the pad's family and the bindings as of this frame: rebuilt only on a change
    const uint32_t scheme = InputSchemeTracker::getRevision();
    const uint32_t bindings = InputActions::getBindingRevision();
    if (!dirty && scheme == schemeRevision && bindings == bindingRevision)
        return;
    dirty = false;
    schemeRevision = scheme;
    bindingRevision = bindings;
    rebuild();
}

void UIInputPrompt::rebuild(){
    if (!checkedChildren){
        // what the author put in it (whitespace aside) replaces the text
        checkedChildren = true;
        for (int i = 0; i < GetNumChildren() && !authorChildren; i++){
            Rml::ElementText* child = rmlui_dynamic_cast<Rml::ElementText*>(GetChild(i));
            authorChildren = !child || !whitespaceOnly(child->GetText());
        }
    }

    const std::string action = GetAttribute<Rml::String>("action", "");
    const std::string wanted = GetAttribute<Rml::String>("scheme", "current");
    InputScheme scheme = InputSchemeTracker::getCurrent();
    if (wanted != "current")
        InputSchemeTracker::parseScheme(wanted, scheme);
    const std::string group = InputSchemeTracker::groupName(scheme);
    const std::string name = InputActions::getActionDisplayName(action, group);

    SetAttribute("data-glyph", Rml::String(InputActions::getActionGlyph(action, group)));
    SetAttribute("data-scheme", Rml::String(InputSchemeTracker::htmlName(scheme)));
    SetAttribute("data-empty", Rml::String(name.empty() ? "true" : "false"));
    if (authorChildren)
        return;
    if (text){
        text->SetText(name);
        return;
    }
    Rml::ElementDocument* document = GetOwnerDocument();
    if (!document)
        return;
    text = rmlui_dynamic_cast<Rml::ElementText*>(AppendChild(document->CreateTextNode(name)));
}
