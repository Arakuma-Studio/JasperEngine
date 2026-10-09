// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUIContext.h"

#include "HtmlUI.h"
#include "RmlRenderer.h"
#include "RmlTextInputHandler.h"
#include "compat/runtime/RuntimeRegistry.h"
#include "compat/CompatOptions.h"
#include "compat/DocumentLoader.h"

#include "Engine.h"
#include "subsystem/UIDocumentSystem.h"
#include "Log.h"
#include "System.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Factory.h>

#include <atomic>
#include <cmath>

using namespace jasperengine;

namespace {

    // the element is still part of a loaded document (an unloaded one is only released by
    // the next Context::Update)
    bool isAttached(Rml::Element* element){
        Rml::ElementDocument* document = element ? element->GetOwnerDocument() : nullptr;
        return document && document->GetParentNode();
    }

    // An element that takes the pointer (phase-1 spec §3.6.2): not the context root, and
    // either an element inside a document or a document that paints its background (a
    // background colour with alpha, or a decorator), as a browser canvas would. A transparent
    // document lets the pointer through to the game. The <body> of an HTML document (RML
    // htmlbody, phase 2) is part of that canvas: transparent, it lets the pointer through too.
    bool isContent(Rml::Context* context, Rml::Element* element){
        if (!element || element == context->GetRootElement())
            return false;
        if (rmlui_dynamic_cast<Rml::ElementDocument*>(element) ||
            (element->GetTagName() == "htmlbody" && htmlui::compat::DocumentLoader::isCompatDocument(element))){
            const Rml::Style::ComputedValues& values = element->GetComputedValues();
            if (values.background_color().alpha == 0 && !values.has_decorator())
                return false;
        }
        return isAttached(element);
    }

    // htmlnavigate reaching the context root unprevented (phase-2 §3.11.2, hook H4)
    class NavigateListener : public Rml::EventListener {
    public:
        explicit NavigateListener(std::function<void(Rml::ElementDocument*, const std::string&)>* handler): handler(handler) {}
        void ProcessEvent(Rml::Event& event) override{
            if (event.IsDefaultPrevented() || !handler || !*handler)
                return;
            Rml::Element* target = event.GetTargetElement();
            Rml::ElementDocument* document = target ? target->GetOwnerDocument() : nullptr;
            const std::string href = event.GetParameter<Rml::String>("href", "");
            if (document && !href.empty())
                (*handler)(document, href);
        }
        void OnDetach(Rml::Element*) override {}

    private:
        std::function<void(Rml::ElementDocument*, const std::string&)>* handler;
    };

#if defined(__EMSCRIPTEN__)
    // the Web layer reports about 4 engine units per wheel notch (JasperEngineWeb wheel_callback)
    constexpr float defaultWheelScale = 0.25f;
#else
    constexpr float defaultWheelScale = 1.0f;
#endif

}

HtmlUIContext::HtmlUIContext(const std::string& name): name(name), renderer(new RmlRenderer()), renderTarget(new RmlRenderTarget()),
    textInputHandler(new RmlTextInputHandler()), wheelScale(defaultWheelScale){
}

HtmlUIContext::~HtmlUIContext(){
    destroy();
}

bool HtmlUIContext::create(Rml::Vector2i dimensions){
    if (context)
        return true;

    if (!HtmlUI::isInitialized()){
        Log::error("[HTML UI] cannot create context '%s': RmlUi is not initialised", name.c_str());
        return false;
    }

    // the renderer is this context's own (Rml::SetRenderInterface is never called)
    context = Rml::CreateContext(name, dimensions, renderer.get(), textInputHandler.get());
    if (!context){
        Log::error("[HTML UI] could not create context '%s'", name.c_str());
        return false;
    }
    textInputHandler->setContext(context);
    // the caret area: context pixels to canvas pixels (texture contexts set their own mapper)
    textInputHandler->setCanvasMapper([this](const Rect& area){
        const Rml::Vector2i size = this->dimensions;
        const float scaleX = (size.x > 0) ? (float)Engine::getCanvasWidth() / (float)size.x : 1.0f;
        const float scaleY = (size.y > 0) ? (float)Engine::getCanvasHeight() / (float)size.y : 1.0f;
        return Rect(area.getX() * scaleX, area.getY() * scaleY, area.getWidth() * scaleX, area.getHeight() * scaleY);
    });

    this->dimensions = dimensions;
    densityRatio = context->GetDensityIndependentPixelRatio();
    HtmlUI::registerContext(this);
    applyThemes();

    return true;
}

void HtmlUIContext::destroy(){
    if (!context)
        return;

    // the debugger first: it unloads its own documents and stops inspecting ours
    if (HtmlUI::getDebuggerContext() == this){
        HtmlUI::setDebuggerContext(nullptr, false);
    }
    context->UnloadAllDocuments();
    activeElement.reset();
    touches.clear();

    // geometry goes back through RmlRenderer::ReleaseGeometry
    Rml::RemoveContext(name);
    context = nullptr;
    navigateListener.reset();   // detached with the root element

    // file textures, then font textures and this renderer's render manager; in this order,
    // or a callback texture of a destroyed render manager would release through it
    Rml::ReleaseTextures(renderer.get());
    Rml::ReleaseRenderManagers();

    releaseGpuResources();
    HtmlUI::unregisterContext(this);
}

Rml::Context* HtmlUIContext::getRmlContext() const{
    return context;
}

void HtmlUIContext::setDimensions(Rml::Vector2i dimensions){
    if (dimensions.x <= 0 || dimensions.y <= 0)
        return;

    if (this->dimensions != dimensions){
        this->dimensions = dimensions;
        if (context){
            context->SetDimensions(dimensions);
            restyleForViewport();
        }
    }
}

Rml::Vector2i HtmlUIContext::getDimensions() const{
    return dimensions;
}

void HtmlUIContext::setDensityRatio(float ratio){
    if (!(ratio > 0.0f))
        return;

    if (densityRatio != ratio){
        densityRatio = ratio;
        if (context)
            context->SetDensityIndependentPixelRatio(ratio);
    }
}

Rml::ElementDocument* HtmlUIContext::loadDocument(const std::string& path){
    if (!context){
        Log::error("[HTML UI] could not load document '%s': no HTML UI context", path.c_str());
        return nullptr;
    }

    // A load (or reload) reads the files as they are now. RmlUi caches linked stylesheets and
    // templates by path until Rml::Shutdown, which never comes while any scene holds a
    // document (the editor's edit-mode scenes do), so an edited .css would never show.
    // Loaded documents keep their own merged copy of their sheets, so this only drops parsing.
    Rml::Factory::ClearStyleSheetCache();
    Rml::Factory::ClearTemplateCache();

    // loaded hidden: UIDocumentSystem shows it
    Rml::ElementDocument* document = nullptr;
    if (isCompatPath(path)){
        // HTML through the compat layer (phase-2 hook H1b): CSS px are this context's px
        htmlui::compat::CompatOptions options;
        options.viewport = htmlui::compat::CompatSize{dimensions.x, dimensions.y};
        options.dpRatio = densityRatio;
        options.touchDevice = System::instance().isTouchDevice();
        options.colorScheme = colorScheme;
        options.reducedMotion = reducedMotion;
        options.verboseDiagnostics = UIDocumentSystem::getVerboseDiagnostics();   // AppSettings::htmlUiVerboseDiagnostics
        document = htmlui::compat::DocumentLoader::load(context, path, options);
        if (document && !navigateListener){
            navigateListener.reset(new NavigateListener(&navigateHandler));
            context->GetRootElement()->AddEventListener("htmlnavigate", navigateListener.get(), false);
        }
    }else{
        document = context->LoadDocument(path);
    }
    if (!document){
        Log::error("[HTML UI] could not load document '%s'", path.c_str());
        return nullptr;
    }

    return document;
}

void HtmlUIContext::unloadDocument(Rml::ElementDocument* document){
    if (!context || !document)
        return;

    if (activeElement && activeElement->GetOwnerDocument() == document){
        activeElement.reset();
    }

    context->UnloadDocument(document);
}

void HtmlUIContext::update(){
    if (context){
        // focus tracking, animation fill modes (phase-2 hook H1)
        htmlui::compat::updateRuntime(context);
        context->Update();
    }
    markUpdated();
}

bool HtmlUIContext::processMouseMove(Rml::Vector2f position, int modifiers){
    if (!context)
        return false;

    // the pixel the point is in
    context->ProcessMouseMove((int)std::floor(position.x), (int)std::floor(position.y), modifiers);

    return isPointerOverContent();
}

bool HtmlUIContext::processMouseButtonDown(int button, int modifiers){
    if (!context)
        return false;

    context->ProcessMouseButtonDown(button, modifiers);

    // RmlUi makes the hovered element active on a primary press
    if (button == 0){
        Rml::Element* hover = context->GetHoverElement();
        if (isContent(context, hover)){
            activeElement = hover->GetObserverPtr();
        }else{
            activeElement.reset();
        }
    }

    return isPointerOverContent();
}

bool HtmlUIContext::processMouseButtonUp(int button, int modifiers){
    if (!context)
        return false;

    const bool overContent = isPointerOverContent();

    context->ProcessMouseButtonUp(button, modifiers);

    if (button == 0){
        activeElement.reset();
    }

    return overContent;
}

bool HtmlUIContext::processMouseLeave(){
    if (!context)
        return false;

    context->ProcessMouseLeave();

    return isPointerOverContent();
}

bool HtmlUIContext::isPointerOverContent() const{
    if (!context)
        return false;

    // Context::IsMouseInteracting counts a document body as interactive, so a transparent
    // full-screen body would swallow every click meant for the game
    if (isContent(context, context->GetHoverElement()))
        return true;

    return isContent(context, activeElement.get());
}

void HtmlUIContext::setDebuggerVisible(bool visible){
    if (!visible && HtmlUI::getDebuggerContext() != this)
        return;

    HtmlUI::setDebuggerContext(this, visible);
}

// ---- phase 2 (p2-html) ----

bool HtmlUIContext::isCompatPath(const std::string& path){
    std::string extension;
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return false;
    extension = path.substr(dot);
    for (char& c : extension){
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    }
    return extension == ".html" || extension == ".htm";
}

void HtmlUIContext::setColorScheme(const std::string& scheme){
    colorScheme = (scheme == "dark") ? "dark" : "light";
    applyThemes();
}

const std::string& HtmlUIContext::getColorScheme() const{
    return colorScheme;
}

void HtmlUIContext::setReducedMotion(bool reduced){
    reducedMotion = reduced;
    applyThemes();
}

bool HtmlUIContext::getReducedMotion() const{
    return reducedMotion;
}

void HtmlUIContext::setNavigateHandler(std::function<void(Rml::ElementDocument*, const std::string&)> handler){
    navigateHandler = std::move(handler);
}

void HtmlUIContext::applyThemes(){
    if (!context)
        return;
    // the compat layer writes prefers-color-scheme as (theme: light|dark) and
    // prefers-reduced-motion as (theme: reduced-motion) (phase-2 §3.9.4)
    context->ActivateTheme("light", colorScheme != "dark");
    context->ActivateTheme("dark", colorScheme == "dark");
    context->ActivateTheme("reduced-motion", reducedMotion);
}

void HtmlUIContext::restyleForViewport(){
    if (!context)
        return;
    for (int i = 0; i < context->GetNumDocuments(); i++){
        Rml::ElementDocument* document = context->GetDocument(i);
        std::shared_ptr<const htmlui::compat::CompatDocument> compiled = htmlui::compat::DocumentLoader::compiledOf(document);
        if (!compiled || !compiled->dependsOnViewport)
            continue;
        // the options it was compiled with, at the new size
        htmlui::compat::CompatOptions resized = compiled->options;
        resized.viewport = htmlui::compat::CompatSize{dimensions.x, dimensions.y};
        htmlui::compat::DocumentLoader::restyleForViewport(document, compiled, resized);
    }
}
