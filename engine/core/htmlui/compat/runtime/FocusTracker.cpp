// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/FocusTracker.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <unordered_map>

using namespace jasperengine::htmlui::compat;

namespace {

    struct State {
        Rml::ObserverPtr<Rml::Element> element;
        bool visible = false;
    };

    std::unordered_map<Rml::Context*, State>& states(){
        static std::unordered_map<Rml::Context*, State> value;
        return value;
    }

}

void FocusTracker::update(Rml::Context* context){
    if (!context)
        return;

    Rml::Element* focus = context->GetFocusElement();
    // a document (or the context's root) is never "the focused element" for CSS
    if (focus && (focus == focus->GetOwnerDocument() || focus == context->GetRootElement()))
        focus = nullptr;
    const bool visible = focus && focus->IsPseudoClassSet("focus-visible");

    State& state = states()[context];
    Rml::Element* previous = state.element.get();
    if (previous == focus && state.visible == visible)
        return;

    if (previous){
        previous->SetPseudoClass(kFocus, false);
        previous->SetPseudoClass(kFocusVisible, false);
    }
    if (focus){
        focus->SetPseudoClass(kFocus, true);
        focus->SetPseudoClass(kFocusVisible, visible);
    }
    state.element = focus ? focus->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>();
    state.visible = visible;
}

Rml::Element* FocusTracker::focused(Rml::Context* context){
    const auto it = states().find(context);
    return it == states().end() ? nullptr : it->second.element.get();
}

void FocusTracker::reset(){
    states().clear();
}
