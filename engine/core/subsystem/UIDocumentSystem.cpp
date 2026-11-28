// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UIDocumentSystem.h"

#include <algorithm>
#include <cmath>

#include "Scene.h"
#include "component/Transform.h"
#include "Engine.h"
#include "Screen.h"
#include "Input.h"
#include "Log.h"
#include "object/ui/UIElement.h"
#include "htmlui/HtmlUI.h"
#include "htmlui/HtmlUIContext.h"
#include "htmlui/HtmlUIClock.h"
#include "htmlui/HtmlUIImageCache.h"
#include "htmlui/UIParticleSystemElement.h"
#include "htmlui/anim/AnimEngine.h"
#include "htmlui/HtmlUIFontDiagnostics.h"
#include "htmlui/HtmlUIPrewarm.h"

#include <atomic>

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Property.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace jasperengine{

    struct JASPERENGINE_INTERNAL UIDocumentContext : public HtmlUIContext {
        using HtmlUIContext::HtmlUIContext;
    };

}

using namespace jasperengine;

namespace {

    // the scene whose context shows the (global) RmlUi debugger
    UIDocumentSystem* debuggerOwner = nullptr;

    // UIDocumentSystem::getMenuRevision: Engine::isMenuShowing recounts only after a change
    uint32_t documentMenuRevision = 0;
    void bumpMenuRevision(){ documentMenuRevision++; }

}

uint32_t UIDocumentSystem::getMenuRevision(){
    return documentMenuRevision;
}

UIDocumentSystem::UIDocumentSystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<UIDocumentComponent>());
    // the type onComponentAdded/onComponentRemoved handle
    declareHookType(scene->getComponentId<UIDocumentComponent>());
}

UIDocumentSystem::~UIDocumentSystem(){
    // Scene::destroy already ran destroy(); this only covers a system dropped without it
    destroyContext();
    if (debuggerOwner == this) debuggerOwner = nullptr;
}

void UIDocumentSystem::load(){
    scriptLoadDocuments();
}

void UIDocumentSystem::destroyContext(){
    textureDestroyAll();
    scriptContextDestroying();
    reloadContextDestroyed();
    documents.clear();
    safeAreaApplied.clear();
    inputState.reset();
    // the context's documents go: the cursor overlay with them
    cursorOverlay = nullptr;
    cursorOverlayImage = nullptr;
    cursorOverlayGraphic.clear();
    cursorOverlayLook[0] = -1.0f;
    if (!menuEntities.empty() || !cursorEntities.empty()){
        menuEntities.clear();
        cursorEntities.clear();
        bumpMenuRevision();
    }
    if (context)
        contextScenes().erase(context->getRmlContext());
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
        document.appliedMenu = false;
        document.appliedCursor = false;
    }
}

void UIDocumentSystem::releaseGpu(){
    if (context)
        context->releaseGpuResources();
    textureReleaseGpu();
}

bool UIDocumentSystem::ensureContext(){
    if (context)
        return true;

    // no view needed: loading and layout use no GPU (a scene's scripts run init before the
    // platform loads the view in exported games; the textures follow once it exists)
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
    contextScenes()[created->getRmlContext()] = scene;
    scriptContextCreated();

    if (debuggerVisible){
        context->setDebuggerVisible(true);
    }

    return true;
}

std::unordered_map<const Rml::Context*, Scene*>& UIDocumentSystem::contextScenes(){
    static std::unordered_map<const Rml::Context*, Scene*> scenes;
    return scenes;
}

Scene* UIDocumentSystem::sceneOfContext(const Rml::Context* context){
    auto it = contextScenes().find(context);
    return it != contextScenes().end() ? it->second : nullptr;
}

void UIDocumentSystem::addRuntimeEntity(Entity entity){
    if (std::find(runtimeEntities.begin(), runtimeEntities.end(), entity) == runtimeEntities.end())
        runtimeEntities.push_back(entity);
}

void UIDocumentSystem::removeRuntimeEntity(Entity entity){
    runtimeEntities.erase(std::remove(runtimeEntities.begin(), runtimeEntities.end(), entity), runtimeEntities.end());
}

bool UIDocumentSystem::isRuntimeEntity(Entity entity) const{
    // the entity or one of its ancestors
    for (Entity e = entity; e != NULL_ENTITY;){
        if (std::find(runtimeEntities.begin(), runtimeEntities.end(), e) != runtimeEntities.end())
            return true;
        const Transform* transform = scene->findComponent<Transform>(e);
        e = transform ? transform->parent : NULL_ENTITY;
    }
    return false;
}

void UIDocumentSystem::getLayoutSize(int& width, int& height) const{
    width = Engine::getCanvasWidth();
    height = Engine::getCanvasHeight();
    // the Canvas Scaler's UI size, in whole CSS pixels
    if (scene->isDocumentScaled()){
        width = std::max(1, (int)std::lround(scene->getCanvasScale().uiWidth));
        height = std::max(1, (int)std::lround(scene->getCanvasScale().uiHeight));
    }
    if (referenceWidth != 0)
        width = referenceWidth;
    if (referenceHeight != 0)
        height = referenceHeight;
}

void UIDocumentSystem::getFrameSize(float& width, float& height) const{
    if (frameWidth > 0.0f && frameHeight > 0.0f){
        width = frameWidth;
        height = frameHeight;
        return;
    }
    int layoutWidth = 0, layoutHeight = 0;
    getLayoutSize(layoutWidth, layoutHeight);
    width = (float)layoutWidth;
    height = (float)layoutHeight;
    // a scaled document still covers the game canvas a 2D camera shows; a UI camera spans the UI size
    if (scene->isDocumentScaled() && referenceWidth == 0 && referenceHeight == 0){
        if (scene->isUIScaled()){
            width = scene->getCanvasScale().uiWidth;
            height = scene->getCanvasScale().uiHeight;
        }else{
            width = (float)Engine::getCanvasWidth();
            height = (float)Engine::getCanvasHeight();
        }
    }
}

void UIDocumentSystem::setFrameSize(float width, float height){
    frameWidth = width;
    frameHeight = height;
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
            if (!rendersToTexture(document.renderMode)) scriptDocumentLoading(document.filename);
            // the context logged why
            Rml::ElementDocument* loadedDocument = loadEntityDocument(entity, document.filename);
            if (loadedDocument){
                documents[entity] = loadedDocument;
                document.loaded = true;
                scriptDocumentLoaded(entity);
            }else{
                document.loadFailed = true;
                compatDocumentLoaded(entity);   // p2-html: why it failed (getDiagnostics)
            }
        }

        document.needReload = false;
        forceApply = true;
    }

    auto it = documents.find(entity);
    if (it == documents.end()){
        setInputFlagsApplied(entity, document, false, false);
        return;
    }
    Rml::ElementDocument* rmlDocument = it->second;

    if (forceApply || document.sortOrder != document.appliedSortOrder){
        // a numeric z-index also stops RmlUi pulling a focused document to the front
        rmlDocument->SetProperty(Rml::PropertyId::ZIndex, Rml::Property((float)document.sortOrder, Rml::Unit::NUMBER));
        document.appliedSortOrder = document.sortOrder;
    }

    // S93 R35: the clock its animations run on (a change keeps their progress)
    if (forceApply || document.updateMode != document.appliedUpdateMode){
        rmlDocument->SetAnimationClock(HtmlUIClock::animationClockOf(document.updateMode));
        document.appliedUpdateMode = document.updateMode;
    }

    if (forceApply || visible != document.appliedVisible){
        if (visible){
            rmlDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
            if (forceApply) compatDocumentShown(rmlDocument);   // p2-html: autofocus
        }else{
            rmlDocument->Hide();
        }
        document.appliedVisible = visible;
    }
    setInputFlagsApplied(entity, document, visible && document.interactive && document.isMenu,
                         visible && document.interactive && document.gamepadCursor && document.renderMode == UIDocumentRenderMode::SCREEN);
}

void UIDocumentSystem::setInputFlagsApplied(Entity entity, UIDocumentComponent& document, bool menu, bool cursor){
    bool changed = false;
    if (menu != document.appliedMenu || (menuEntities.count(entity) > 0) != menu){
        document.appliedMenu = menu;
        static uint64_t menuOrder = 0;
        if (menu)
            menuEntities[entity] = ++menuOrder;
        else
            menuEntities.erase(entity);
        changed = true;
    }
    if (cursor != document.appliedCursor || (cursorEntities.count(entity) > 0) != cursor){
        document.appliedCursor = cursor;
        if (cursor)
            cursorEntities[entity] = true;
        else
            cursorEntities.erase(entity);
        changed = true;
    }
    if (changed)
        bumpMenuRevision();
}

void UIDocumentSystem::update(double dt){
    (void)dt;

    // anime on the UI clock, before RmlUi's update (idempotent per frame: every scene calls it)
    AnimEngine::instance().tick(HtmlUIClock::now());
    // S93: prewarmed glyphs placed within their budget, fallback glyphs reported (once per frame)
    HtmlUIPrewarm::update();
    HtmlUIFontDiagnostics::update();

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
    // the live safe area, before the layout below
    updateSafeAreaProperties();

    // p2-html: <script type="text/lua"> of the documents loaded above, before the layout
    compatRunScripts();

    // control characters that came without their key down this frame, before the layout
    updateInput();
    context->update();
    textureUpdate();
    scriptUpdate();
    // <particle-system> elements a script detached (with an ancestor too) get no update: their
    // prefabs go now, and come back when the element is attached again
    UIParticleSystemElement::releaseDetached(scene);
}

void UIDocumentSystem::draw(){
}

void UIDocumentSystem::updateSafeAreaProperties(){
    const uint64_t revision = Screen::getSafeAreaRevision();
    int width, height;
    getLayoutSize(width, height);

    // documents gone since the last call
    for (auto it = safeAreaApplied.begin(); it != safeAreaApplied.end();){
        auto document = documents.find(it->first);
        it = (document == documents.end() || document->second != it->second.document) ? safeAreaApplied.erase(it) : std::next(it);
    }

    for (const auto& entry : documents){
        const Entity entity = entry.first;
        Rml::ElementDocument* rmlDocument = entry.second;
        SafeAreaApplied& applied = safeAreaApplied[entity];
        if (applied.document == rmlDocument && applied.revision == revision && applied.width == width && applied.height == height)
            continue;
        applied.document = rmlDocument;
        applied.revision = revision;
        applied.width = width;
        applied.height = height;

        // CSS px of the document: canvas units at the document's layout scale. A document on a
        // mesh (texture mode) is not on the screen: no insets
        const bool screenDocument = getTextureContext(entity) == nullptr;
        const float canvasWidth = Screen::getSafeAreaCanvasWidth();
        const float canvasHeight = Screen::getSafeAreaCanvasHeight();
        const float sx = (screenDocument && canvasWidth > 0.0f) ? (float)width / canvasWidth : 0.0f;
        const float sy = (screenDocument && canvasHeight > 0.0f) ? (float)height / canvasHeight : 0.0f;
        auto set = [rmlDocument](const char* prefix, const EdgeInsets& insets, float sx, float sy){
            const struct { const char* edge; float value; } edges[] = {
                {"top", insets.top * sy}, {"right", insets.right * sx}, {"bottom", insets.bottom * sy}, {"left", insets.left * sx}};
            for (const auto& edge : edges){
                char value[32];
                std::snprintf(value, sizeof(value), "%gpx", std::round(std::max(0.0f, edge.value) * 1000.0f) / 1000.0f);
                rmlDocument->SetProperty(std::string(prefix) + edge.edge, value);
            }
        };
        set("--jasper-safe-area-inset-", Screen::getSafeAreaInsets(), sx, sy);
        set("--jasper-device-safe-area-inset-", Screen::getDeviceSafeAreaInsets(), sx, sy);
    }
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
    styleSheetsComponentRemoved(entity);
    if (menuEntities.erase(entity) + cursorEntities.erase(entity) > 0)
        bumpMenuRevision();
}

bool UIDocumentSystem::hasVisibleDocument() const{
    if (!context)
        return false;

    for (const auto& pair : documents){
        const UIDocumentComponent* document = scene->findComponent<UIDocumentComponent>(pair.first);
        if (document && document->appliedVisible && !getTextureContext(pair.first))
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

void UIDocumentSystem::toCanvas(float& x, float& y) const{
    const Rml::Vector2i dimensions = context->getDimensions();
    if (dimensions.x > 0)
        x = x * (float)Engine::getCanvasWidth() / (float)dimensions.x;
    if (dimensions.y > 0)
        y = y * (float)Engine::getCanvasHeight() / (float)dimensions.y;
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
    frameWidth = 0.0f;
    frameHeight = 0.0f;
}

bool UIDocumentSystem::loadDocumentNow(Entity entity){
    UIDocumentComponent* document = scene->findComponent<UIDocumentComponent>(entity);
    if (!document)
        return false;

    // before the view too: a scene's scripts query their documents in init, which exported
    // games run before the platform loads the view
    if (document->filename.empty())
        return false;

    if (!ensureContext())
        return false;

    int width, height;
    getLayoutSize(width, height);
    context->setDimensions(Rml::Vector2i(width, height));

    applyComponent(entity, *document);
    updateSafeAreaProperties();
    // style and layout, so elements and their boxes can be queried right away
    context->update();
    textureUpdate();
    compatRunScripts();   // p2-html: its <script type="text/lua">

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

    // a shown document is loaded once it has been drawn, its images decoded (S93 R18: a decoded
    // image is uploaded by the next frame that draws it)
    return (!anyVisible || (context && context->hasRenderedFrame())) && textureAllRendered() &&
           HtmlUIImageCache::get().getDecodingCount() == 0;
}

bool UIDocumentSystem::setDebuggerVisible(bool visible){
    if (!HtmlUI::isDebuggerCompiled()){
        static bool warned = false;
        if (!warned){
            Log::warn("[HTML UI] the RmlUi debugger is not compiled into this build (JASPERENGINE_HTMLUI_DEBUGGER)");
            warned = true;
        }
        debuggerVisible = false;
        return false;
    }

    // the debugger is global: showing it here hides it in the scene that had it
    if (visible && debuggerOwner && debuggerOwner != this){
        debuggerOwner->debuggerVisible = false;
    }
    if (visible){
        debuggerOwner = this;
    }else if (debuggerOwner == this){
        debuggerOwner = nullptr;
    }

    debuggerVisible = visible;
    // without a context yet, ensureContext shows it
    if (context){
        context->setDebuggerVisible(visible);
    }
    return true;
}

bool UIDocumentSystem::isDebuggerVisible() const{
    if (!HtmlUI::isDebuggerCompiled()){
        static bool warned = false;
        if (!warned){
            Log::warn("[HTML UI] the RmlUi debugger is not compiled into this build (JASPERENGINE_HTMLUI_DEBUGGER)");
            warned = true;
        }
        return false;
    }

    if (!debuggerVisible || debuggerOwner != this)
        return false;
    // requested before the context existed: shown once it does
    return !context || (HtmlUI::getDebuggerContext() == context && HtmlUI::isDebuggerVisible());
}

HtmlUIContext* UIDocumentSystem::getContext() const{
    return context;
}

// p2-importers: process-wide, set by the editor's settings
static std::atomic<bool> htmlUiVerboseDiagnostics{false};

void UIDocumentSystem::setVerboseDiagnostics(bool verbose){
    htmlUiVerboseDiagnostics = verbose;
}

bool UIDocumentSystem::getVerboseDiagnostics(){
    return htmlUiVerboseDiagnostics;
}

void UIDocumentSystem::setHtmlUiImageCacheMiB(int mib){
    HtmlUIImageCache::setBudgetMiB(mib);
}
