// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The compat layer's runtime emulations (phase-2 §3.11), registered after every
// Rml::Initialise (HtmlUI::acquire) and dropped after Rml::Shutdown (HtmlUI::release).
// RmlUi forgets instancers at shutdown and its parsers die with it, so everything is made
// again for each initialisation.

#include "htmlui/compat/runtime/RuntimeRegistry.h"

#include "htmlui/compat/ForkFeatures.h"
#include "htmlui/compat/runtime/AnimationFill.h"
#include "htmlui/compat/runtime/CompatProperties.h"
#include "htmlui/compat/runtime/FocusTracker.h"
#include "htmlui/compat/runtime/HtmlAnchorElement.h"
#include "htmlui/compat/runtime/HtmlBorderImageDecorator.h"
#include "htmlui/compat/runtime/HtmlButtonElement.h"
#include "htmlui/compat/runtime/HtmlDetailsElement.h"
#include "htmlui/compat/runtime/HtmlDialogElement.h"
#include "htmlui/compat/runtime/HtmlFieldsetElement.h"
#include "htmlui/compat/runtime/HtmlImageElement.h"
#include "htmlui/compat/runtime/HtmlInputElement.h"
#include "htmlui/compat/runtime/HtmlLabelBehavior.h"
#include "htmlui/compat/runtime/HtmlListItemElement.h"
#include "htmlui/compat/runtime/HtmlLayerDecorator.h"
#include "htmlui/compat/runtime/HtmlShapeDecorator.h"
#include "htmlui/compat/runtime/SvgElementHook.h"

#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>

#include <memory>
#include <vector>

using namespace jasperengine::htmlui::compat;

namespace {

    struct Runtime {
        bool registered = false;
        std::unique_ptr<HtmlLayerDecoratorInstancer> layer;
        std::unique_ptr<HtmlShapeDecoratorInstancer> shape;
        std::unique_ptr<HtmlBorderImageDecoratorInstancer> borderImage;
        std::vector<std::unique_ptr<Rml::ElementInstancer>> elements;
    };

    template<typename T>
    void registerElement(Runtime& r, const char* tag){
        r.elements.push_back(std::make_unique<Rml::ElementInstancerGeneric<T>>());
        Rml::Factory::RegisterElementInstancer(tag, r.elements.back().get());
    }

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

    // the pseudo-classes FocusTracker sets: the fork drops selectors with unknown ones (phase 3),
    // and compat writes them while FocusPseudoClasses is absent (forced off in tests)
    Rml::Factory::RegisterPseudoClass(FocusTracker::kFocus);
    Rml::Factory::RegisterPseudoClass(FocusTracker::kFocusVisible);

    // decorators (§3.11.3); the names p2-css writes
    r.layer = std::make_unique<HtmlLayerDecoratorInstancer>();
    r.shape = std::make_unique<HtmlShapeDecoratorInstancer>();
    r.borderImage = std::make_unique<HtmlBorderImageDecoratorInstancer>();
    Rml::Factory::RegisterDecoratorInstancer("html-layer", r.layer.get());
    Rml::Factory::RegisterDecoratorInstancer("html-shape", r.shape.get());
    Rml::Factory::RegisterDecoratorInstancer("html-border-image", r.borderImage.get());

    // element instancers (§3.11.2), replacing RmlUi's for the same tags (img, input, label)
    registerElement<HtmlButtonElement>(r, "button");
    registerElement<HtmlAnchorElement>(r, "a");
    registerElement<HtmlInputElement>(r, "input");
    registerElement<HtmlListItemElement>(r, "li");
    registerElement<HtmlSummaryElement>(r, "summary");
    registerElement<HtmlDetailsElement>(r, "details");
    registerElement<HtmlDialogElement>(r, "dialog");
    registerElement<HtmlImageElement>(r, "img");
    registerElement<HtmlLabelElement>(r, "label");
    registerElement<HtmlFieldsetElement>(r, "fieldset");

    r.registered = true;
}

void jasperengine::htmlui::compat::releaseRuntime(){
    Runtime& r = runtime();
    FocusTracker::reset();
    AnimationFill::reset();
    HtmlListItemElement::resetMarkerEmulation();
    SvgElementHook::uninstall();
    CompatProperties::reset();
    r.layer.reset();
    r.shape.reset();
    r.borderImage.reset();
    r.elements.clear();
    r.registered = false;
}

void jasperengine::htmlui::compat::updateRuntime(Rml::Context* context){
    // phase 3's FocusPseudoClasses and AnimationLonghands turn these off (§3.6): with the fork's
    // leaf-only :focus, :focus-within and :focus-visible the selectors pass through and nothing
    // tracks the focus
    static const bool trackFocus = !ForkFeatures::present().has(ForkFeature::FocusPseudoClasses);
    if (trackFocus)
        FocusTracker::update(context);
    // the fork keeps an animation's end values itself (animation-fill-mode, phase 3)
    static const bool fillEmulated = !ForkFeatures::present().has(ForkFeature::AnimationLonghands);
    if (fillEmulated)
        AnimationFill::update(context);
}
