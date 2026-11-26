// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/HtmlTextEffects.h"
#include "htmlui/HtmlL10n.h"
#include "HtmlUIContext.h"
#include "audio/AudioMarkers.h"
#include "anim/AnimText.h"

#include "HtmlUI.h"
#include "RmlRenderer.h"
#include "RmlTextInputHandler.h"
#include "compat/runtime/RuntimeRegistry.h"
#include "widgets/WidgetsRuntime.h"
#include "effects/EffectRuntime.h"
#include "compat/CompatOptions.h"
#include "compat/CompatApi.h"
#include "compat/DiagnosticCodes.h"
#include "compat/Diagnostics.h"
#include "compat/DocumentLoader.h"
#include "object/ui/HtmlUIEventListener.h"

#include "Engine.h"
#include "subsystem/UIDocumentSystem.h"
#include "Log.h"
#include "System.h"
#include "input/InputScheme.h"
#include "jobs/JobSystem.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Factory.h>

#include <atomic>
#include <cmath>
#include <iterator>
#include <optional>
#include <set>
#include <unordered_map>

using namespace jasperengine;

namespace {

    // the element is still part of a loaded document (an unloaded one is only released by
    // the next Context::Update)
    bool isAttached(Rml::Element* element){
        Rml::ElementDocument* document = element ? element->GetOwnerDocument() : nullptr;
        return document && document->GetParentNode();
    }

    // An element that takes the pointer (phase-1 spec §3.6.2): not the context root, attached,
    // and not a page the pointer passes through (HtmlUIContext::passesPointerThrough).
    bool isContent(Rml::Context* context, Rml::Element* element){
        if (!element || element == context->GetRootElement())
            return false;
        if (HtmlUIContext::passesPointerThrough(element))
            return false;
        return isAttached(element);
    }

    // S93 R4: the DOM events whose listener on a page makes it take presses on its empty parts
    bool isPressEvent(const std::string& type){
        static const char* const types[] = {"click", "dblclick", "contextmenu", "mousedown", "pointerdown", "touchstart"};
        for (const char* name : types){
            if (type == name)
                return true;
        }
        return false;
    }

    bool isClickListener(const HtmlUIEventListener& listener){
        const std::string& type = listener.getDomType();
        return type == "click" || type == "pointerdown" || type == "mousedown";
    }

    // ... and the wheel only (the RmlUi name of the DOM wheel event is mousescroll)
    bool isWheelEvent(const std::string& type){
        return type == "wheel" || type == "mousescroll";
    }

    // the type of the element's first listener of a kind: addEventListener (Lua or C++) or a
    // data-event-* binding (the engine's inline handlers: on* attributes are JavaScript, dropped
    // by the compat layer); "" for none
    std::string pageListener(Rml::Element* element, bool (*kind)(const std::string&)){
        if (!element)
            return std::string();
        for (HtmlUIEventListener* listener : HtmlUIEventListener::listenersOf(element)){
            if (kind(listener->getDomType()))
                return listener->getDomType();
            if (kind(listener->getRmlType()))
                return listener->getRmlType();
        }
        for (const auto& attribute : element->GetAttributes()){
            if (attribute.first.compare(0, 11, "data-event-") == 0 && kind(attribute.first.substr(11)))
                return attribute.first.substr(11);
        }
        return std::string();
    }

    // HTMLUI-INPUT-0001 (S93 R5), once per document and event type: a press listener makes the
    // page take every press on its empty parts. Called from the hit tests, so it also covers
    // data-event-* bindings, reloads and listeners that scripts add at any time
    void reportPressListener(Rml::ElementDocument* document, Rml::Element* page, const std::string& type){
        struct Reported {
            Rml::ObserverPtr<Rml::Element> document;   // a new document at a freed address starts over
            std::set<std::string> types;
        };
        static std::unordered_map<const Rml::Element*, Reported> reported;
        auto found = reported.find(document);
        if (found == reported.end()){
            // the documents that went are forgotten
            if (reported.size() >= 64){
                for (auto it = reported.begin(); it != reported.end();)
                    it = it->second.document ? std::next(it) : reported.erase(it);
            }
            found = reported.emplace(document, Reported()).first;
        }
        Reported& entry = found->second;
        if (entry.document.get() != document){
            entry.document = document->GetObserverPtr();
            entry.types.clear();
        }
        if (!entry.types.insert(type).second)
            return;

        std::shared_ptr<const htmlui::compat::CompatDocument> compiled = htmlui::compat::DocumentLoader::compiledOf(document);
        htmlui::compat::Diagnostic diagnostic;
        if (compiled){
            if (std::optional<htmlui::compat::Diagnostic> located = htmlui::compat::locateElement(*compiled, page))
                diagnostic = *located;
        }else{
            diagnostic.file = document->GetSourceURL();
        }
        std::string name = page->GetTagName();
        if (compiled)
            name = (page == document) ? "html" : (name == "htmlbody" ? "body" : name);
        diagnostic.code = htmlui::compat::codes::PagePressListener;
        diagnostic.severity = htmlui::compat::Severity::Info;
        diagnostic.message = "a '" + type + "' listener on <" + name + "> makes the page take presses: the document now takes every "
                             "press on its empty parts, which no longer reach the documents below or the game";
        diagnostic.suggestion = "listen on the elements that take the press; move, up, over and out listeners on the page take "
                                "nothing, a wheel listener only the wheel";
        diagnostic.subject = type;
        htmlui::compat::DocumentLoader::addRuntimeDiagnostic(document, diagnostic);
        Log::print("[HTML UI] %s", htmlui::compat::toText(diagnostic).c_str());
    }

    bool listensForClick(Rml::Element* element){
        if (HtmlUIEventListener::anyListenerOf(element, &isClickListener))
            return true;
        // the names past the small-string buffer, built once
        static const Rml::String clickAttribute = "data-event-click";
        static const Rml::String pointerdownAttribute = "data-event-pointerdown";
        static const Rml::String mousedownAttribute = "data-event-mousedown";
        return element->HasAttribute(clickAttribute) || element->HasAttribute(pointerdownAttribute) || element->HasAttribute(mousedownAttribute);
    }

    // a background colour with alpha, a decorator or a background image layer
    bool paintsBackground(const Rml::Element* element){
        const Rml::Style::ComputedValues& values = element->GetComputedValues();
        return values.background_color().alpha != 0 || values.has_decorator() || values.has_background_image();
    }

#if JASPERENGINE_AUDIO_CAPTURE
    // an element's click for a capture, in the capture phase: a page's stopPropagation does not hide it
    class ClickMarkerListener : public Rml::EventListener {
    public:
        void ProcessEvent(Rml::Event& event) override{
            if (!AudioMarkers::isEnabled())
                return;
            Rml::Element* target = event.GetTargetElement();
            if (!target)
                return;
            const std::string id = target->GetId();
            AudioMarkers::htmlClick(nullptr, id.empty() ? target->GetAddress(false, false) : "#" + id);
        }
    };
#endif

    // htmlnavigate reaching the context root unprevented (phase-2 §3.11.2, hook H4)
    class NavigateListener : public Rml::EventListener {
    public:
        explicit NavigateListener(std::function<void(Rml::ElementDocument*, const std::string&, const std::string&)>* handler): handler(handler) {}
        void ProcessEvent(Rml::Event& event) override{
            if (event.IsDefaultPrevented() || !handler || !*handler)
                return;
            Rml::Element* target = event.GetTargetElement();
            Rml::ElementDocument* document = target ? target->GetOwnerDocument() : nullptr;
            if (event.GetParameter<bool>("external", false))
                return;
            // the anchor (p2-runtime) resolved it: `url` is the linked file when `document` is set
            const std::string href = event.GetParameter<Rml::String>("href", "");
            const std::string url = event.GetParameter<bool>("document", false) ? event.GetParameter<Rml::String>("url", "") : std::string();
            if (document && (!href.empty() || !url.empty()))
                (*handler)(document, href, url);
        }
        void OnDetach(Rml::Element*) override {}

    private:
        std::function<void(Rml::ElementDocument*, const std::string&, const std::string&)>* handler;
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
    RmlRenderer::bindContext(context, renderer.get());
    if (!context){
        Log::error("[HTML UI] could not create context '%s'", name.c_str());
        return false;
    }
    textInputHandler->setContext(context);
    // a transparent page passes the pointer on to the documents below it, then to the game
    context->SetPointerPassThrough([this](Rml::Element* element){
        return passesPointerThrough(element, wheelHitTest ? PointerHit::Wheel : PointerHit::Press);
    });
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

void HtmlUIContext::releaseFocus(){
    // a primary press no document takes goes to the game: the focus leaves the element that had
    // it (as a press on an empty page focused that page before transparent pages passed the
    // pointer through), so keys and text are the game's again
    Rml::Element* focus = context ? context->GetFocusElement() : nullptr;
    if (!focus || focus == context->GetRootElement())
        return;
    Rml::ElementDocument* document = focus->GetOwnerDocument();
    if (document && focus != document)
        document->Focus();
}

bool HtmlUIContext::takesPointerInput(Rml::Element* element){
    for (Rml::Element* node = element; node; node = node->GetParentNode()){
        if (rmlui_dynamic_cast<Rml::ElementDocument*>(node))
            return false;
        const Rml::String& tag = node->GetTagName();
        if (tag == "button" || tag == "input" || tag == "select" || tag == "textarea" || (tag == "a" && node->HasAttribute("href")) ||
            node->HasAttribute("onclick") || listensForClick(node))
            return true;
    }
    return false;
}

bool HtmlUIContext::passesPointerThrough(Rml::Element* element, PointerHit hit){
    if (!element)
        return false;
    Rml::ElementDocument* document = rmlui_dynamic_cast<Rml::ElementDocument*>(element);
    const bool page = document || (element->GetTagName() == "htmlbody" && htmlui::compat::DocumentLoader::isCompatDocument(element));
    if (!page || paintsBackground(element))
        return false;
    if (!document)
        document = element->GetOwnerDocument();
    // the document and its <body> (the HTML body, or the canvas body of the fork's canvas background)
    Rml::Element* body = nullptr;
    if (document){
        const std::string bodyTag = document->HasCanvasBackground() && !document->GetCanvasBodyTag().empty() ? document->GetCanvasBodyTag()
                                                                                                           : std::string("htmlbody");
        for (int i = 0; i < document->GetNumChildren() && !body; i++){
            if (document->GetChild(i)->GetTagName() == bodyTag)
                body = document->GetChild(i);
        }
    }
    // the body's background painted over the whole canvas (the fork's canvas background, phase 3)
    if (element == document && body && document->HasCanvasBackground() && paintsBackground(body))
        return false;
    // A page that listens for presses takes the pointer on its empty parts, as a browser page
    // does ("click outside closes the menu"); per document: the context has one hover chain,
    // which moves, presses, clicks and drags follow. Move, up, over and out listeners take
    // nothing, so a drag tracker on <body> does not block the game (S93 R4)
    Rml::Element* const pages[] = {document, body};
    for (Rml::Element* listening : pages){
        const std::string press = pageListener(listening, isPressEvent);
        if (!press.empty()){
            reportPressListener(document, listening, press);
            return false;
        }
    }
    if (hit == PointerHit::Wheel){
        for (Rml::Element* listening : pages){
            if (!pageListener(listening, isWheelEvent).empty())
                return false;
        }
    }
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
    touchPresses.clear();

    // geometry goes back through RmlRenderer::ReleaseGeometry
    RmlRenderer::unbindContext(context);
    htmlui::HtmlTextEffects::get().forget(context);
    Rml::RemoveContext(name);
    context = nullptr;
    navigateListener.reset();   // detached with the root element

    // file textures, then this renderer's render manager, which releases its textures of the
    // glyph pages first; the glyphs and pages stay for the next context (S93 R13)
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
        options.coarsePointer = InputSchemeTracker::getPointer() == InputScheme::Touch;
        options.colorScheme = colorScheme;
        options.reducedMotion = reducedMotion;
        options.verboseDiagnostics = UIDocumentSystem::getVerboseDiagnostics();   // AppSettings::htmlUiVerboseDiagnostics
        std::shared_ptr<const htmlui::compat::CompatDocument> compiled;
        document = htmlui::compat::DocumentLoader::load(context, path, options, &compiled);
        // a touch device's first switch to the mouse (or back) is a cache hit: the other pointer's
        // variant of a document that reads hover or pointer is compiled in the background
        if (document && compiled && compiled->dependsOnPointer && options.touchDevice){
            htmlui::compat::CompatOptions other = options;
            other.coarsePointer = !options.coarsePointer;
            jobs::enqueueDetached([path, other](){ htmlui::compat::DocumentLoader::compile(path, other); }, jobs::Lane::Background,
                                  "htmlui-pointer-variant");
            // and both variants' sheets parsed for the document once that compile is in (update)
            pointerPrewarms.push_back(PointerPrewarm{document->GetObserverPtr(), path, other});
        }
        if (document && !navigateListener){
            navigateListener.reset(new NavigateListener(&navigateHandler));
            context->GetRootElement()->AddEventListener("htmlnavigate", navigateListener.get(), false);
#if JASPERENGINE_AUDIO_CAPTURE
            clickMarkerListener.reset(new ClickMarkerListener());
            context->GetRootElement()->AddEventListener("click", clickMarkerListener.get(), true);
#endif
        }
    }else{
        document = context->LoadDocument(path);
        htmlui::compat::DocumentLoader::applyInputScheme(document);
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
    for (auto& press : touchPresses){
        if (press.second && press.second->GetOwnerDocument() == document)
            press.second.reset();
    }

    context->UnloadDocument(document);
}

void HtmlUIContext::applyInputScheme(){
    const uint32_t revision = InputSchemeTracker::getRevision();
    if (!context || revision == inputSchemeRevision)
        return;
    inputSchemeRevision = revision;
    const bool coarse = InputSchemeTracker::getPointer() == InputScheme::Touch;
    for (int i = 0; i < context->GetNumDocuments(); i++){
        Rml::ElementDocument* document = context->GetDocument(i);
        // RmlUi restyles what the attribute selectors match
        htmlui::compat::DocumentLoader::applyInputScheme(document);
        // hover and pointer: the other variant (prewarmed on a touch device); documents that never
        // read them keep their compile
        std::shared_ptr<const htmlui::compat::CompatDocument> compiled = htmlui::compat::DocumentLoader::compiledOf(document);
        if (compiled && compiled->dependsOnPointer && compiled->options.coarsePointer != coarse){
            htmlui::compat::CompatOptions options = compiled->options;
            options.coarsePointer = coarse;
            htmlui::compat::DocumentLoader::restyleWith(document, compiled, options);
        }
    }
}

void HtmlUIContext::prewarmPointerVariants(){
    for (auto it = pointerPrewarms.begin(); it != pointerPrewarms.end();){
        Rml::ElementDocument* document = it->document ? it->document->GetOwnerDocument() : nullptr;
        if (!document){
            it = pointerPrewarms.erase(it);
            continue;
        }
        std::shared_ptr<const htmlui::compat::CompatDocument> other = htmlui::compat::DocumentLoader::findCompiled(it->path, it->options);
        if (!other){
            // the background compile is not in yet; one that never comes (the file went) is let go:
            // the switch then compiles, as on a desktop
            if (++it->frames > kPointerPrewarmFrames)
                it = pointerPrewarms.erase(it);
            else
                ++it;
            continue;
        }
        // the document moves to its own kept sheets now: the ones RmlUi parsed at the load go with
        // this frame, not with the switch's (freeing them is most of what a first swap cost)
        std::shared_ptr<const htmlui::compat::CompatDocument> current = htmlui::compat::DocumentLoader::compiledOf(document);
        htmlui::compat::DocumentLoader::prewarmSheets(document, current);
        if (current)
            htmlui::compat::DocumentLoader::restyleSheets(document, *current);
        // the other variant under the scheme that comes with its pointer (a mouse: Keyboard&Mouse)
        htmlui::compat::DocumentLoader::prewarmSheets(document, other,
            InputSchemeTracker::htmlName(it->options.coarsePointer ? InputScheme::Touch : InputScheme::KeyboardMouse));
        it = pointerPrewarms.erase(it);
    }
}

void HtmlUIContext::update(){
    if (context){
        // a touch device's documents that read hover or pointer: both variants parsed ahead (R23)
        if (!pointerPrewarms.empty())
            prewarmPointerVariants();
        // the input scheme of the frame (InputSchemeTracker::latch): root attributes, hover and pointer
        applyInputScheme();
        // text-effect: the texts that animate draw again (geometry only)
        htmlui::HtmlTextEffects::get().update(context);
        // focus tracking, animation fill modes (phase-2 hook H1)
        htmlui::compat::updateRuntime(context);
        // S17: tooltips, drags, runtime diagnostics
        htmlui::WidgetsRuntime::update(context);
        // the keyed elements made since (inserted fragments) and by the update (virtual rows)
        htmlui::HtmlL10n::update();
        context->Update();
        htmlui::HtmlL10n::update();
        if (AnimText::updateLayouts(context)) context->Update();
    }
}

bool HtmlUIContext::processMouseMove(Rml::Vector2f position, int modifiers){
    if (!context)
        return false;

    // the pixel the point is in
    mousePosition = Rml::Vector2i((int)std::floor(position.x), (int)std::floor(position.y));
    mouseInside = true;
    context->ProcessMouseMove(mousePosition.x, mousePosition.y, modifiers);
    // the effects' pointer (phase 5): the exact position
    htmlui::EffectRuntime::get().onPointerMove(context, position);

    return isPointerOverContent();
}

bool HtmlUIContext::processMouseButtonDown(int button, int modifiers){
    if (!context)
        return false;

    context->ProcessMouseButtonDown(button, modifiers);
    if (button >= 0 && button < 31)
        mouseButtons |= 1 << button;
    if (button == 0)
        htmlui::EffectRuntime::get().onPointerButton(context, true);

    // RmlUi makes the hovered element active on a primary press
    if (button == 0){
        Rml::Element* hover = context->GetHoverElement();
        if (isContent(context, hover)){
            activeElement = hover->GetObserverPtr();
        }else{
            activeElement.reset();
            releaseFocus();
        }
    }

    return isPointerOverContent();
}

bool HtmlUIContext::processMouseButtonUp(int button, int modifiers){
    if (!context)
        return false;

    const bool overContent = isPointerOverContent();

    context->ProcessMouseButtonUp(button, modifiers);
    if (button >= 0 && button < 31)
        mouseButtons &= ~(1 << button);

    if (button == 0){
        activeElement.reset();
        htmlui::EffectRuntime::get().onPointerButton(context, false);
    }

    return overContent;
}

bool HtmlUIContext::processMouseLeave(){
    if (!context)
        return false;

    context->ProcessMouseLeave();
    mouseInside = false;
    htmlui::EffectRuntime::get().onPointerLeave(context);

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
    if (context)
        htmlui::HtmlTextEffects::get().setReducedMotion(context, reduced);
    applyThemes();
}

bool HtmlUIContext::getReducedMotion() const{
    return reducedMotion;
}

void HtmlUIContext::setNavigateHandler(std::function<void(Rml::ElementDocument*, const std::string&, const std::string&)> handler){
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
