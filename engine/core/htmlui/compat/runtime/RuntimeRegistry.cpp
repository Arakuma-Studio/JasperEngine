// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The compat layer's runtime emulations (phase-2 §3.11), registered after every
// Rml::Initialise (HtmlUI::acquire) and dropped after Rml::Shutdown (HtmlUI::release).

#include "htmlui/compat/runtime/RuntimeRegistry.h"

#include "htmlui/compat/runtime/CompatProperties.h"
#include "htmlui/compat/runtime/SvgElementHook.h"

using namespace jasperengine::htmlui::compat;

namespace {

    bool registered = false;

}

void jasperengine::htmlui::compat::registerRuntime(){
    if (registered)
        return;
    // properties first: the instancers and decorators read them
    CompatProperties::registerAll();
    SvgElementHook::install();
    registered = true;
}

void jasperengine::htmlui::compat::releaseRuntime(){
    SvgElementHook::uninstall();
    CompatProperties::reset();
    registered = false;
}

void jasperengine::htmlui::compat::updateRuntime(Rml::Context*){
}
