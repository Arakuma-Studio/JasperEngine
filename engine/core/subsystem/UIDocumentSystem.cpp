// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UIDocumentSystem.h"

#include "Scene.h"
#include "Engine.h"
#include "Input.h"
#include "object/ui/UIElement.h"
#include "htmlui/HtmlUI.h"
#include "htmlui/HtmlUIContext.h"
#include "htmlui/HtmlUIClock.h"
#include "htmlui/anim/AnimEngine.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Property.h>

#include <cstdio>

namespace jasperengine{

    struct JASPERENGINE_INTERNAL UIDocumentContext : public HtmlUIContext {
        using HtmlUIContext::HtmlUIContext;
    };

}

using namespace jasperengine;

UIDocumentSystem::UIDocumentSystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<UIDocumentComponent>());
}

UIDocumentSystem::~UIDocumentSystem(){
    // Scene::destroy already ran destroy(); this only covers a system dropped without it
    destroyContext();
}

void UIDocumentSystem::load(){
    scriptLoadDocuments();
}

void UIDocumentSystem::destroyContext(){
    textureDestroyAll();
    scriptContextDestroying();
    documents.clear();
    inputState.reset();
    delete context;
    context = nullptr;
    if (acquired){
        HtmlUI::release();
        acquired = false;
    }
}

void UIDocumentSystem::destroy(){
    destroyContext();

    // a reloaded view (Scene::load after systemViewLoaded) loads everything again
    auto components = scene->getComponentArray<UIDocumentComponent>();
    for (size_t i = 0; i < components->size(); i++){
        UIDocumentComponent& document = components->getComponentFromIndex(i);
        document.needReload = true;
        document.loaded = false;
        document.loadFailed = false;
    }
}

bool UIDocumentSystem::ensureContext(){
    if (context)
        return true;

    if (!Engine::isViewLoaded())
        return false;

    if (!acquired){
        acquired = HtmlUI::acquire();
        if (!acquired)
            return false;
    }

    char name[48];
    std::snprintf(name, sizeof(name), "scene-%p", (void*)scene);

    int width, height;
    getLayoutSize(width, height);

    UIDocumentContext* created = new UIDocumentContext(name);
    if (!created->create(Rml::Vector2i(width, height))){
        delete created;
        HtmlUI::release();
        acquired = false;
        return false;
    }
    context = created;
    scriptContextCreated();

    if (debuggerVisible){
        context->setDebuggerVisible(true);
    }

    return true;
}

void UIDocumentSystem::getLayoutSize(int& width, int& height) const{
    width = (referenceWidth != 0) ? referenceWidth : Engine::getCanvasWidth();
    height = (referenceHeight != 0) ? referenceHeight : Engine::getCanvasHeight();
}

void UIDocumentSystem::unloadEntityDocument(Entity entity){
    if (textureUnloadDocument(entity)) return;
    auto it = documents.find(entity);
    if (it == documents.end())
        return;

    scriptDocumentUnloading(entity, it->second);
    if (context){
        context->unloadDocument(it->second);
    }
    documents.erase(it);
}

void UIDocumentSystem::applyComponent(Entity entity, UIDocumentComponent& document){
    const bool visible = document.enabled && scene->isEntityActiveInHierarchy(entity);

    // a component copied over a loaded one, or added with stale runtime fields
    if (document.loaded && documents.find(entity) == documents.end()){
        document.needReload = true;
    }

    bool forceApply = false;
    if (document.needReload){
        unloadEntityDocument(entity);
        document.loaded = false;
        document.loadFailed = false;

        if (!document.filename.empty()){
            scriptDocumentLoading(document.filename);
            // the context logged why
            Rml::ElementDocument* loadedDocument = (document.renderMode == UIDocumentRenderMode::TEXTURE) ? textureLoadDocument(entity, document) : context->loadDocument(document.filename);
            if (loadedDocument){
                documents[entity] = loadedDocument;
                document.loaded = true;
                scriptDocumentLoaded(entity);
            }else{
                document.loadFailed = true;
            }
        }

        document.needReload = false;
        forceApply = true;
    }

    auto it = documents.find(entity);
    if (it == documents.end())
        return;
    Rml::ElementDocument* rmlDocument = it->second;

    if (forceApply || document.sortOrder != document.appliedSortOrder){
        // a numeric z-index also stops RmlUi pulling a focused document to the front
        rmlDocument->SetProperty(Rml::PropertyId::ZIndex, Rml::Property((float)document.sortOrder, Rml::Unit::NUMBER));
        document.appliedSortOrder = document.sortOrder;
    }

    if (forceApply || visible != document.appliedVisible){
        if (visible){
            rmlDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        }else{
            rmlDocument->Hide();
        }
        document.appliedVisible = visible;
    }
}

void UIDocumentSystem::update(double dt){
    (void)dt;

    // anime on the UI clock, before RmlUi's update (idempotent per frame: every scene calls it)
    AnimEngine::instance().tick(HtmlUIClock::now());

    // runs paused too: the editor shows documents of a stopped scene
    if (!Engine::isViewLoaded())
        return;

    // scenes without documents (editor gizmo scenes too) never initialise RmlUi
    if (!context && !hasDocuments())
        return;

    if (!ensureContext())
        return;

    int width, height;
    getLayoutSize(width, height);
    context->setDimensions(Rml::Vector2i(width, height));
    context->setDensityRatio(context->getPixelRatio());

    auto components = scene->getComponentArray<UIDocumentComponent>();
    for (size_t i = 0; i < components->size(); i++){
        applyComponent(components->getEntity(i), components->getComponentFromIndex(i));
    }

    // control characters that came without their key down this frame, before the layout
    updateInput();
    context->update();
    textureUpdate();
    scriptUpdate();
}

void UIDocumentSystem::draw(){
}

void UIDocumentSystem::onComponentAdded(Entity entity, ComponentId componentId){
    if (componentId != scene->getComponentId<UIDocumentComponent>())
        return;

    UIDocumentComponent& document = scene->getComponent<UIDocumentComponent>(entity);
    document.needReload = true;
    document.loaded = false;
    document.loadFailed = false;
}

void UIDocumentSystem::onComponentRemoved(Entity entity, ComponentId componentId){
    if (componentId != scene->getComponentId<UIDocumentComponent>())
        return;

    unloadEntityDocument(entity);
    textureRemoveEntity(entity);
    scriptComponentRemoved(entity);
}

bool UIDocumentSystem::hasVisibleDocument() const{
    if (!context)
        return false;

    for (const auto& pair : documents){
        const UIDocumentComponent* document = scene->findComponent<UIDocumentComponent>(pair.first);
        if (document && document->appliedVisible)
            return true;
    }
    return false;
}

void UIDocumentSystem::toContext(float& x, float& y) const{
    const Rml::Vector2i dimensions = context->getDimensions();
    const int canvasWidth = Engine::getCanvasWidth();
    const int canvasHeight = Engine::getCanvasHeight();
    if (canvasWidth > 0)
        x = x * (float)dimensions.x / (float)canvasWidth;
    if (canvasHeight > 0)
        y = y * (float)dimensions.y / (float)canvasHeight;
}

// phase-0 entry points: the mouse (touch = false), phase-1 methods in UIDocumentSystemInput.cpp
bool UIDocumentSystem::eventOnPointerDown(int button, float x, float y, int mods){
    return eventOnPointerDown(0, button, x, y, mods, false);
}

bool UIDocumentSystem::eventOnPointerUp(int button, float x, float y, int mods){
    return eventOnPointerUp(0, button, x, y, mods, false);
}

bool UIDocumentSystem::eventOnPointerMove(float x, float y, int mods){
    return eventOnPointerMove(0, x, y, mods, false);
}

void UIDocumentSystem::setReferenceSize(int width, int height){
    referenceWidth = width;
    referenceHeight = height;
}

void UIDocumentSystem::clearReferenceSize(){
    referenceWidth = 0;
    referenceHeight = 0;
}

bool UIDocumentSystem::loadDocumentNow(Entity entity){
    UIDocumentComponent* document = scene->findComponent<UIDocumentComponent>(entity);
    if (!document)
        return false;

    // without a view the next update after systemViewLoaded loads it
    if (!Engine::isViewLoaded() || document->filename.empty())
        return false;

    if (!ensureContext())
        return false;

    int width, height;
    getLayoutSize(width, height);
    context->setDimensions(Rml::Vector2i(width, height));

    applyComponent(entity, *document);
    // style and layout, so elements and their boxes can be queried right away
    context->update();
    textureUpdate();

    return document->loaded;
}

bool UIDocumentSystem::isDocumentLoaded(Entity entity) const{
    return documents.find(entity) != documents.end();
}

UIElement UIDocumentSystem::getElementById(Entity entity, const std::string& id) const{
    auto it = documents.find(entity);
    if (it == documents.end())
        return UIElement();

    return UIElement::wrap(it->second->GetElementById(id));
}

UIElement UIDocumentSystem::getDocumentBody(Entity entity) const{
    auto it = documents.find(entity);
    if (it == documents.end())
        return UIElement();

    // RmlUi's document element is the body
    return UIElement::wrap(it->second);
}

bool UIDocumentSystem::hasDocuments() const{
    auto components = scene->getComponentArray<UIDocumentComponent>();
    for (size_t i = 0; i < components->size(); i++){
        if (!components->getComponentFromIndex(i).filename.empty())
            return true;
    }
    return false;
}

bool UIDocumentSystem::isAllLoaded() const{
    bool anyVisible = false;

    auto components = scene->getComponentArray<UIDocumentComponent>();
    for (size_t i = 0; i < components->size(); i++){
        const UIDocumentComponent& document = components->getComponentFromIndex(i);
        if (!document.enabled || document.filename.empty())
            continue;
        if (document.needReload || (!document.loaded && !document.loadFailed))
            return false;
        if (document.loaded && document.renderMode == UIDocumentRenderMode::SCREEN && scene->isEntityActiveInHierarchy(components->getEntity(i)))
            anyVisible = true;
    }

    // a shown document is loaded once it has been drawn
    return (!anyVisible || (context && context->hasRenderedFrame())) && textureAllRendered();
}

bool UIDocumentSystem::setDebuggerVisible(bool visible){
    debuggerVisible = visible;
    if (context){
        context->setDebuggerVisible(visible);
    }
    // HTMLUI_P1_STUB: p1-editor (false without JASPERENGINE_HTMLUI_DEBUGGER; phase 0 always compiles the debugger in)
    return true;
}

bool UIDocumentSystem::isDebuggerVisible() const{
    // HTMLUI_P1_STUB: p1-editor (the debugger is global: showing it in one scene hides it elsewhere)
    return debuggerVisible;
}

HtmlUIContext* UIDocumentSystem::getContext() const{
    return context;
}
