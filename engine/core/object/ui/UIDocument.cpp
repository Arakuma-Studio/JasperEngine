// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UIDocument.h"
#include "HtmlUIEventListener.h"
#include "HtmlUIScriptInternal.h"

#include "Engine.h"
#include "Log.h"
#include "subsystem/UIDocumentSystem.h"
#include "htmlui/compat/LuaHooks.h"

#include <RmlUi/Core/ElementDocument.h>

using namespace jasperengine;

namespace {

    // the loaded RmlUi document, loading it first when it is due (a script's init can run
    // before the system's first update)
    Rml::ElementDocument* loadedDocument(Scene* scene, Entity entity){
        UIDocumentSystem* system = scene->getSystem<UIDocumentSystem>().get();
        Rml::ElementDocument* document = system->getDocument(entity);
        if (!document){
            const UIDocumentComponent* component = scene->findComponent<UIDocumentComponent>(entity);
            if (component && component->needReload && !component->filename.empty() && system->loadDocumentNow(entity)){
                document = system->getDocument(entity);
                system->getScriptState().fireNotifications();
            }
        }
        return document;
    }

}

UIDocument::UIDocument(Scene* scene): GameObject(scene){
    addComponent<UIDocumentComponent>();
}

UIDocument::UIDocument(Scene* scene, Entity entity): GameObject(scene, entity){
}

UIDocument::~UIDocument(){
}

UIDocument::UIDocument(const UIDocument& rhs) = default;
UIDocument& UIDocument::operator=(const UIDocument& rhs) = default;
UIDocument::UIDocument(UIDocument&& rhs) noexcept = default;
UIDocument& UIDocument::operator=(UIDocument&& rhs) noexcept = default;

void UIDocument::setFilename(const std::string& filename){
    UIDocumentComponent& document = getComponent<UIDocumentComponent>();

    // the same file again only retries a load that failed (reload() reloads a loaded one)
    if (document.filename != filename || document.loadFailed){
        document.filename = filename;
        document.needReload = true;
    }
}

const std::string& UIDocument::getFilename() const{
    return getComponent<UIDocumentComponent>().filename;
}

bool UIDocument::load(const std::string& filename){
    UIDocumentComponent& document = getComponent<UIDocumentComponent>();
    document.filename = filename;
    document.needReload = true;

    UIDocumentSystem* system = scene->getSystem<UIDocumentSystem>().get();
    const bool loaded = system->loadDocumentNow(entity);
    // onUnload/onLoad of this load run now, as the load event of a browser
    system->getScriptState().fireNotifications();
    return loaded;
}

void UIDocument::reload(){
    // keeps scroll, focus, form values and id-ed listeners (§3.8.6); without a view the next
    // update after systemViewLoaded loads it
    scene->getSystem<UIDocumentSystem>()->reloadDocument(entity, true);
}

bool UIDocument::isLoaded() const{
    return getComponent<UIDocumentComponent>().loaded;
}

void UIDocument::show(){
    setVisible(true);
}

void UIDocument::hide(){
    setVisible(false);
}

void UIDocument::setVisible(bool visible){
    getComponent<UIDocumentComponent>().enabled = visible;
}

bool UIDocument::isVisible() const{
    return getComponent<UIDocumentComponent>().enabled;
}

void UIDocument::setSortOrder(int sortOrder){
    getComponent<UIDocumentComponent>().sortOrder = sortOrder;
}

int UIDocument::getSortOrder() const{
    return getComponent<UIDocumentComponent>().sortOrder;
}

UIElement UIDocument::getElementById(const std::string& id) const{
    loadedDocument(scene, entity);
    return scene->getSystem<UIDocumentSystem>()->getElementById(entity, id);
}

UIElement UIDocument::getBody() const{
    // an HTML document's <body> (RML htmlbody); RmlUi's document element otherwise
    if (Rml::Element* body = htmlui::compat::LuaHooks::body(loadedDocument(scene, entity)))
        return UIElementAccess::wrap(body);
    return scene->getSystem<UIDocumentSystem>()->getDocumentBody(entity);
}

// ---- phase 1 ----

UIElement UIDocument::querySelector(const std::string& selector) const{
    Rml::ElementDocument* document = loadedDocument(scene, entity);
    if (!document)
        return UIElement();
    // HTML documents: CSS names (body, template, :focus, ...) -> RML (p2-html, hook H5)
    const bool compat = htmlui::compat::LuaHooks::isCompat(document);
    return UIElementAccess::wrap(document->QuerySelector(compat ? htmlui::compat::LuaHooks::selector(selector) : selector));
}

std::vector<UIElement> UIDocument::querySelectorAll(const std::string& selector) const{
    std::vector<UIElement> result;
    Rml::ElementDocument* document = loadedDocument(scene, entity);
    if (!document)
        return result;
    Rml::ElementList elements;
    const bool compat = htmlui::compat::LuaHooks::isCompat(document);
    document->QuerySelectorAll(elements, compat ? htmlui::compat::LuaHooks::selector(selector) : selector);
    result.reserve(elements.size());
    for (Rml::Element* element : elements){
        result.push_back(UIElementAccess::wrap(element));
    }
    return result;
}

UIElement UIDocument::createElement(const std::string& tag) const{
    Rml::ElementDocument* document = loadedDocument(scene, entity);
    if (!document){
        Log::error("[HTML UI] UIDocument::createElement('%s'): the document is not loaded", tag.c_str());
        return UIElement();
    }
    // detached until it is inserted: the pool keeps it while a handle does
    return UIElementAccess::wrap(HtmlUIElementRegistry::adopt(document->CreateElement(tag), document));
}

UIElement UIDocument::createTextNode(const std::string& text) const{
    Rml::ElementDocument* document = loadedDocument(scene, entity);
    if (!document){
        Log::error("[HTML UI] UIDocument::createTextNode: the document is not loaded");
        return UIElement();
    }
    return UIElementAccess::wrap(HtmlUIElementRegistry::adopt(document->CreateTextNode(text), document));
}

std::string UIDocument::getTitle() const{
    Rml::ElementDocument* document = loadedDocument(scene, entity);
    return document ? document->GetTitle() : std::string();
}

void UIDocument::setTitle(const std::string& title){
    if (Rml::ElementDocument* document = loadedDocument(scene, entity))
        document->SetTitle(title);
}

uint64_t UIDocument::addEventListener(const std::string& type, std::function<void(UIEvent&)> fn, bool capture){
    Rml::ElementDocument* document = loadedDocument(scene, entity);
    if (!document){
        Log::error("[HTML UI] UIDocument::addEventListener('%s'): the document is not loaded", type.c_str());
        return 0;
    }
    return UIElementAccess::wrap(document).addEventListener(type, std::move(fn), capture);
}

void UIDocument::removeEventListener(uint64_t listenerId){
    if (Rml::ElementDocument* document = scene->getSystem<UIDocumentSystem>()->getDocument(entity))
        UIElementAccess::wrap(document).removeEventListener(listenerId);
}

UIDataModel UIDocument::createDataModel(const std::string& name, const UIValue& initial){
    UIDocumentSystem* system = scene->getSystem<UIDocumentSystem>().get();
    std::string error;
    HtmlUIDataModelRecord* record = system->getScriptState().createModel(entity, name, initial, error);
    if (!record){
        Log::error("[HTML UI] UIDocument::createDataModel: %s", error.c_str());
        return UIDataModel();
    }
    UIDataModel model = HtmlUIScriptState::makeHandle(record);
    // RmlUi binds data-model while parsing: a loaded document that uses it reloads now
    system->getScriptState().reloadUsersOf(entity, name);
    return model;
}

UIDataModel UIDocument::getDataModel(const std::string& name) const{
    UIDocumentSystem* system = scene->getSystem<UIDocumentSystem>().get();
    HtmlUIDataModelRecord* record = system->getScriptState().findModel(name);
    return (record && record->owner == entity) ? HtmlUIScriptState::makeHandle(record) : UIDataModel();
}

void UIDocument::removeDataModel(const std::string& name){
    UIDocumentSystem* system = scene->getSystem<UIDocumentSystem>().get();
    HtmlUIDataModelRecord* record = system->getScriptState().findModel(name);
    if (record && record->owner == entity)
        system->getScriptState().removeModel(name);
}

void UIDocument::setDebuggerVisible(bool visible){
    if (Scene* main = Engine::getScene())
        main->getSystem<UIDocumentSystem>()->setDebuggerVisible(visible);
}

bool UIDocument::isDebuggerVisible(){
    Scene* main = Engine::getScene();
    return main && main->getSystem<UIDocumentSystem>()->isDebuggerVisible();
}

Framebuffer* UIDocument::getTargetTexture() const{
    return scene->getSystem<UIDocumentSystem>()->getTargetTexture(entity);
}

// ---- phase 2 (p2-html, hook H4) ----

UIElement UIDocument::getDocumentElement() const{
    loadedDocument(scene, entity);
    return scene->getSystem<UIDocumentSystem>()->getDocumentBody(entity);
}

std::vector<htmlui::compat::Diagnostic> UIDocument::getDiagnostics() const{
    loadedDocument(scene, entity);
    return scene->getSystem<UIDocumentSystem>()->getDiagnostics(entity);
}

void UIDocument::setColorScheme(const std::string& scheme){
    scene->getSystem<UIDocumentSystem>()->setColorScheme(entity, scheme);
}

void UIDocument::setReducedMotion(bool reduced){
    scene->getSystem<UIDocumentSystem>()->setReducedMotion(entity, reduced);
}
