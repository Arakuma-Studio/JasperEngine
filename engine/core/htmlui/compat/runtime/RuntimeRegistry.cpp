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

#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/runtime/DecoratorCommon.h"

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/ImageSource.h>

#include <memory>
#include <vector>

using namespace jasperengine::htmlui::compat;

namespace {

    // The url() images of the fork's background and mask layers (phase 3, p3-paint): loaded as
    // compat's own images (html-layer's), so SVG files are rasterized at the size of a tile,
    // monochrome currentColor SVGs tinted, and image-rendering chooses the sampling
    class LayerImageSource : public Rml::ImageSourceHandler {
    public:
        bool GetImage(Rml::Element* element, const Rml::String& url, const Rml::String& /*base_path*/, Rml::Vector2f draw_size,
                      bool repeatX, bool repeatY, Image& out) override{
            // the paths compat writes are resolved for the document (resolveUrlIn); others are
            // RmlUi's own (a .rml document): the fork loads them
            Rml::ElementDocument* document = element ? element->GetOwnerDocument() : nullptr;
            if (!document || !DocumentLoader::isCompatDocument(document))
                return false;
            // a raster image repeated along an axis: its texture sampled with repeat wrap there
            // (RmlRenderer::LoadTexture reads rmlui-wrap); SVG images are rasterized per tile
            const bool wrap = (repeatX || repeatY) && !decorators::isSvgPath(url);
            std::string path = url;
            if (wrap)
                path += std::string(url.find('?') == std::string::npos ? "?" : "&") + "rmlui-wrap=" +
                        (repeatX && repeatY ? "repeat" : (repeatX ? "repeat-x" : "repeat-y"));
            auto image = std::make_shared<decorators::Image>();
            if (!image->load(element, path))
                return true;   // reported once by load; nothing drawn
            out.repeats = wrap;
            out.intrinsic_size = image->intrinsicSize();
            if (draw_size.x > 0.0f && draw_size.y > 0.0f){
                out.texture = image->texture(element, draw_size);
                out.colour = image->colour(element);
                out.owner = image;
            }
            return true;
        }
    };

    struct Runtime {
        bool registered = false;
        std::unique_ptr<HtmlLayerDecoratorInstancer> layer;
        std::unique_ptr<HtmlShapeDecoratorInstancer> shape;
        std::unique_ptr<HtmlBorderImageDecoratorInstancer> borderImage;
        std::vector<std::unique_ptr<Rml::ElementInstancer>> elements;
        LayerImageSource layerImages;
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
    Rml::SetImageSourceHandler(&r.layerImages);

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
    SvgElementHook::uninstall();
    Rml::SetImageSourceHandler(nullptr);
    CompatProperties::reset();
    r.layer.reset();
    r.shape.reset();
    r.borderImage.reset();
    r.elements.clear();
    r.registered = false;
}

void jasperengine::htmlui::compat::updateRuntime(Rml::Context* context){
    // phase 3's FocusPseudoClasses and AnimationLonghands turn these off (§3.6): the gates
    // go here when the features exist
    FocusTracker::update(context);
    // the fork keeps an animation's end values itself (animation-fill-mode, phase 3)
    static const bool fillEmulated = !ForkFeatures::present().has(ForkFeature::AnimationLonghands);
    if (fillEmulated)
        AnimationFill::update(context);
}
