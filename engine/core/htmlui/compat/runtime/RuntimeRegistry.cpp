// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The compat layer's runtime emulations (phase-2 §3.11), registered after every
// Rml::Initialise (HtmlUI::acquire) and dropped after Rml::Shutdown (HtmlUI::release).
// RmlUi forgets instancers at shutdown and its parsers die with it, so everything is made
// again for each initialisation.

#include "htmlui/compat/runtime/RuntimeRegistry.h"

#include "htmlui/compat/runtime/CompatProperties.h"
#include "htmlui/compat/runtime/HtmlBorderImageDecorator.h"
#include "htmlui/compat/runtime/HtmlLayerDecorator.h"
#include "htmlui/compat/runtime/HtmlShapeDecorator.h"
#include "htmlui/compat/runtime/SvgElementHook.h"

#include <RmlUi/Core/Factory.h>

#include <memory>

using namespace jasperengine::htmlui::compat;

namespace {

    struct Runtime {
        bool registered = false;
        std::unique_ptr<HtmlLayerDecoratorInstancer> layer;
        std::unique_ptr<HtmlShapeDecoratorInstancer> shape;
        std::unique_ptr<HtmlBorderImageDecoratorInstancer> borderImage;
    };

    Runtime& runtime(){
        static Runtime value;
        return value;
    }

}

void jasperengine::htmlui::compat::registerRuntime(){
    Runtime& r = runtime();
    if (r.registered)
        return;

    // properties first: the instancers and decorators read them
    CompatProperties::registerAll();
    SvgElementHook::install();

    // decorators (§3.11.3); the names p2-css writes
    r.layer = std::make_unique<HtmlLayerDecoratorInstancer>();
    r.shape = std::make_unique<HtmlShapeDecoratorInstancer>();
    r.borderImage = std::make_unique<HtmlBorderImageDecoratorInstancer>();
    Rml::Factory::RegisterDecoratorInstancer("html-layer", r.layer.get());
    Rml::Factory::RegisterDecoratorInstancer("html-shape", r.shape.get());
    Rml::Factory::RegisterDecoratorInstancer("html-border-image", r.borderImage.get());

    r.registered = true;
}

void jasperengine::htmlui::compat::releaseRuntime(){
    Runtime& r = runtime();
    SvgElementHook::uninstall();
    CompatProperties::reset();
    r.layer.reset();
    r.shape.reset();
    r.borderImage.reset();
    r.registered = false;
}

void jasperengine::htmlui::compat::updateRuntime(Rml::Context*){
}
