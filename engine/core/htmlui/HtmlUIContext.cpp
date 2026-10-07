// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUIContext.h"

#include "HtmlUI.h"
#include "RmlRenderer.h"

#include "Log.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>

#include <cmath>

using namespace jasperengine;

namespace {

    // the element is still part of a loaded document (an unloaded one is only released by
    // the next Context::Update)
    bool isAttached(Rml::Element* element){
        Rml::ElementDocument* document = element ? element->GetOwnerDocument() : nullptr;
        return document && document->GetParentNode();
    }

    // an element inside a document body: not the context root, not a document itself
    bool isContent(Rml::Context* context, Rml::Element* element){
        if (!element || element == context->GetRootElement())
            return false;
        if (rmlui_dynamic_cast<Rml::ElementDocument*>(element))
            return false;
        return isAttached(element);
    }

}

HtmlUIContext::HtmlUIContext(const std::string& name): name(name), renderer(new RmlRenderer()){
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
    context = Rml::CreateContext(name, dimensions, renderer.get());
    if (!context){
        Log::error("[HTML UI] could not create context '%s'", name.c_str());
        return false;
    }

    this->dimensions = dimensions;
    densityRatio = context->GetDensityIndependentPixelRatio();
    HtmlUI::registerContext(this);

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

    // geometry goes back through RmlRenderer::ReleaseGeometry
    Rml::RemoveContext(name);
    context = nullptr;

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
        if (context)
            context->SetDimensions(dimensions);
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
    Rml::ElementDocument* document = context->LoadDocument(path);
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
    if (context)
        context->Update();
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
