// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_HTMLFORMSUPPORT_H
#define COMPAT_RUNTIME_HTMLFORMSUPPORT_H

// What the HTML form-associated elements of the compat runtime share (phase-2 §3.11.2):
// the form a control belongs to, submit and reset with HTML semantics on top of RmlUi's
// ElementForm, the default values a reset restores, keyboard activation.

#include "Export.h"

#include <string>

namespace Rml {
    class Element;
    class ElementForm;
    class Event;
}

namespace jasperengine::htmlui::compat::forms {

    // The control's form: the element named by its form="id" attribute, else the nearest
    // ancestor <form> (nullptr when none).
    Rml::ElementForm* formOf(Rml::Element* control);

    // ElementForm::Submit with the submitter's name and value (RmlUi's `submit` event).
    void submit(Rml::Element* submitter);

    // HTML form reset: a cancelable `reset` event on the form, then every control of the form
    // (descendants, and controls elsewhere naming it with form="id") back to its default:
    // inputs to their default value/checked state, textareas and selects to the values they had
    // when the form was first seen (rememberDefaults).
    void reset(Rml::ElementForm* form);

    // Records the current values of the form's textareas and selects as their defaults, for
    // those not recorded yet (reset controls call it on their first update, before any edit).
    void rememberDefaults(Rml::ElementForm* form);

    // HTML button activation: Enter on key down, Space on key up -> Click() (true when handled).
    bool activateFromKey(Rml::Element* element, Rml::Event& event);

    bool isDisabled(Rml::Element* element);
    std::string lowerAttribute(Rml::Element* element, const char* name, const char* fallback = "");

}

#endif //COMPAT_RUNTIME_HTMLFORMSUPPORT_H
