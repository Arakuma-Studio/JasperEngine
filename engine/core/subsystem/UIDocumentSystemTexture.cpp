// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIDocumentSystem texture-mode documents (phase 1, p1-rtt, spec §3.12): HtmlUITextureState
// (one context and target per document) and world-space pointer input.
//
// A TEXTURE-mode document lives in its own HtmlUIContext ("tex:<scene>:<entity>"): its
// layout size is textureWidth x textureHeight divided by its pixel ratio (CSS px), and it
// renders at that ratio into its own RmlRenderTarget (MSAA, stencil, layers like the screen
// context), then one un-premultiplying pass (HTMLUI_POST Unp) copies it into the
// textureWidth x textureHeight target texture. The document stays in
// UIDocumentSystem::documents like a screen document, so every element API (getDocument,
// getElementById, UIDocument, Lua) reaches it; only loading, unloading, updating, rendering
// and input go through the hooks below. Data models created by a texture document's scripts
// live in its context.

#include "UIDocumentSystem.h"

#include "Engine.h"
#include "Input.h"
#include "Log.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "htmlui/HtmlUIContext.h"
#include "htmlui/HtmlUIDependencies.h"
#include "htmlui/RmlKeyMap.h"
#include "htmlui/RmlRenderer.h"
#include "htmlui/RmlTextInputHandler.h"
#include "htmlui/UIWorldRaycast.h"
#include "object/Camera.h"
#include "object/ui/HtmlUIEventListener.h"
#include "object/ui/HtmlUIScriptInternal.h"
#include "render/CameraRender.h"
#include "render/SystemRender.h"
#include "texture/Framebuffer.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <unordered_map>
#include <vector>

namespace jasperengine{

    // one TEXTURE-mode document's context and target
    struct JASPERENGINE_INTERNAL HtmlUITextureDocument {
        std::unique_ptr<HtmlUIContext> context;
        // what getTargetTexture returns: straight-alpha RGBA8, top-left origin (owned by the
        // state's targets, see there)
        Framebuffer* target = nullptr;
        int width = 0, height = 0;   // texture pixels
        float ratio = 1.0f;          // CSS px -> texture px
        bool hovered = false;        // the last pointer event was over it
        bool rendered = false;       // its target holds the current frame (or was cleared while hidden)
        bool showing = false;        // the target holds a frame of the shown document
        Vector2 lastUV;              // where a captured pointer was last over the surface
    };

    struct JASPERENGINE_INTERNAL HtmlUITextureState {
        std::unordered_map<Entity, HtmlUITextureDocument> entries;
        // Every target handed out, one per entity, kept as long as the system lives: Image,
        // Mesh and materials hold the pointer. A document that goes (view loss, mode switch,
        // removal) destroys only the GPU texture; when it comes back the same Framebuffer is
        // created again and its version bump makes the consumers rebind.
        std::unordered_map<Entity, std::unique_ptr<Framebuffer>> targets;
        // pointers whose down a texture document took: touch pointer ids, mouse buttons as -1 - button
        std::unordered_map<int, Entity> captured;
        Entity focus = NULL_ENTITY;                 // document that took the last pointer-down

        Framebuffer* targetFor(Entity entity){
            std::unique_ptr<Framebuffer>& target = targets[entity];
            if (!target)
                target.reset(new Framebuffer());
            return target.get();
        }

        void forget(Entity entity){
            for (auto it = captured.begin(); it != captured.end();){
                it = (it->second == entity) ? captured.erase(it) : std::next(it);
            }
            if (focus == entity)
                focus = NULL_ENTITY;
        }
    };

}

using namespace jasperengine;

namespace {

    constexpr int maxTextureSize = 8192;

    // engine mouse buttons to RmlUi's (0 left, 1 right, 2 middle); -1 for the others
    int toRmlButton(int button){
        switch (button){
            case D_MOUSE_BUTTON_LEFT: return 0;
            case D_MOUSE_BUTTON_RIGHT: return 1;
            case D_MOUSE_BUTTON_MIDDLE: return 2;
            default: return -1;
        }
    }

    int captureKey(int pointer, int button, bool touch){
        return touch ? pointer : -1 - button;
    }

    // the size a component asks for: texture pixels and the CSS px -> texture px ratio
    void textureSize(const UIDocumentComponent& document, int& width, int& height, float& ratio){
        width = std::min(std::max(document.textureWidth, 1), maxTextureSize);
        height = std::min(std::max(document.textureHeight, 1), maxTextureSize);
        ratio = (document.pixelRatio > 0.0f && std::isfinite(document.pixelRatio)) ? std::min(std::max(document.pixelRatio, 0.25f), 8.0f) : 1.0f;
    }

    Rml::Vector2i layoutSize(int width, int height, float ratio){
        return Rml::Vector2i(std::max(1, (int)std::lround(width / ratio)), std::max(1, (int)std::lround(height / ratio)));
    }

    bool isShown(Rml::ElementDocument* document){
        return document && document->IsVisible();
    }

    // (re)creates the target texture at the size: RGBA8, 1 sample, no depth, LINEAR, CLAMP
    bool ensureTarget(Framebuffer& target, int width, int height, Entity entity){
        if (target.isCreated() && (int)target.getWidth() == width && (int)target.getHeight() == height)
            return true;

        FramebufferDesc desc;
        desc.width = width;
        desc.height = height;
        desc.color = ColorFormat::RGBA;
        desc.depth = DepthStencilFormat::NONE;
        desc.sampleCount = 1;
        desc.minFilter = TextureFilter::LINEAR;
        desc.magFilter = TextureFilter::LINEAR;
        desc.wrapU = TextureWrap::CLAMP_TO_EDGE;
        desc.wrapV = TextureWrap::CLAMP_TO_EDGE;
        desc.label = "htmlui-texture-" + std::to_string((unsigned int)entity);
        if (!target.create(desc)){
            Log::error("[HTML UI] could not create the %d x %d target texture of a texture-mode document", width, height);
            return false;
        }
        return true;
    }

    // a target without a document frame: transparent
    void clearTarget(Framebuffer& target){
        CameraRender pass;
        pass.setColorActions(LoadAction::CLEAR, StoreAction::STORE, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        pass.setLabel("html ui texture");
        pass.startRenderPass(&target.getRender());
        pass.endRenderPass();
    }

}

HtmlUITextureState& UIDocumentSystem::getTextureState(){
    if (!textureState)
        textureState = std::make_shared<HtmlUITextureState>();
    return *textureState;
}

HtmlUIContext* UIDocumentSystem::documentContext(Entity entity) const{
    if (textureState){
        auto it = textureState->entries.find(entity);
        if (it != textureState->entries.end())
            return it->second.context.get();
    }
    return getContext();
}

HtmlUIContext* UIDocumentSystem::modelContext(Entity owner) const{
    return documentContext(owner);
}

Rml::ElementDocument* UIDocumentSystem::textureLoadDocument(Entity entity, UIDocumentComponent& document){
    HtmlUITextureState& state = getTextureState();

    int width, height;
    float ratio;
    textureSize(document, width, height, ratio);
    const Rml::Vector2i dimensions = layoutSize(width, height, ratio);

    HtmlUITextureDocument& entry = state.entries[entity];
    if (!entry.context){
        char name[64];
        std::snprintf(name, sizeof(name), "tex:%p:%u", (void*)scene, (unsigned int)entity);
        entry.context.reset(new HtmlUIContext(name));
        if (!entry.context->create(dimensions)){
            // the context logged why
            state.entries.erase(entity);
            return nullptr;
        }
        // p1-lua: DOM change events, and the data models this document's scripts made
        Rml::Context* rmlContext = entry.context->getRmlContext();
        HtmlUIEventNames::installChangeSynthesizer(rmlContext);
        if (scriptState){
            for (auto& pair : scriptState->models){
                if (pair.second->owner == entity)
                    pair.second->attach(rmlContext);
            }
        }
    }
    entry.width = width;
    entry.height = height;
    entry.ratio = ratio;
    entry.context->setDimensions(dimensions);
    entry.context->setDensityRatio(ratio);
    if (!entry.target)
        entry.target = state.targetFor(entity);

    // placeholders for the data models its scripts did not make yet, in its own context
    scriptDocumentLoading(document.filename, entry.context.get());
    return entry.context->loadDocument(document.filename);
}

bool UIDocumentSystem::textureUnloadDocument(Entity entity){
    if (!textureState)
        return false;

    auto it = textureState->entries.find(entity);
    if (it == textureState->entries.end())
        return false;
    HtmlUITextureDocument& entry = it->second;

    auto doc = documents.find(entity);
    if (doc != documents.end()){
        scriptDocumentUnloading(entity, doc->second);
        entry.context->unloadDocument(doc->second);
        documents.erase(doc);
    }
    entry.hovered = false;
    textureState->forget(entity);

    // back to SCREEN (a reload follows): the texture context goes, the screen one loads it
    const UIDocumentComponent* component = scene->findComponent<UIDocumentComponent>(entity);
    if (!component || component->renderMode != UIDocumentRenderMode::TEXTURE)
        textureRemoveEntity(entity);

    return true;
}

void UIDocumentSystem::textureRemoveEntity(Entity entity){
    if (!textureState)
        return;

    auto it = textureState->entries.find(entity);
    if (it == textureState->entries.end())
        return;
    HtmlUITextureDocument& entry = it->second;

    // unloaded documents are destroyed by the next update: now, while their data models exist
    entry.context->update();
    // the RmlUi side of what lives in this context goes before it
    Rml::Context* rmlContext = entry.context->getRmlContext();
    const UIDocumentComponent* component = scene->findComponent<UIDocumentComponent>(entity);
    const bool toScreen = component && component->renderMode == UIDocumentRenderMode::SCREEN;
    if (scriptState){
        for (auto& pair : scriptState->models){
            if (pair.second->context == rmlContext){
                pair.second->detach();
                // a document that switched to SCREEN takes its models along
                if (toScreen && getContext())
                    pair.second->attach(getContext()->getRmlContext());
            }
        }
    }
    HtmlUIElementRegistry::freeContext(rmlContext);
    HtmlUIFileLog::forgetRenderer(&entry.context->getRenderer());

    // the target object stays (consumers may still point at it), its GPU texture goes
    if (entry.target)
        entry.target->destroy();
    entry.context.reset();
    textureState->forget(entity);
    textureState->entries.erase(it);
}

void UIDocumentSystem::textureDestroyAll(){
    if (!textureState)
        return;

    std::vector<Entity> entities;
    for (const auto& pair : textureState->entries){
        entities.push_back(pair.first);
    }
    for (Entity entity : entities){
        auto doc = documents.find(entity);
        if (doc != documents.end()){
            HtmlUIElementRegistry::freeDocument(doc->second);
            textureState->entries[entity].context->unloadDocument(doc->second);
            documents.erase(doc);
        }
        textureRemoveEntity(entity);
    }
    textureState->captured.clear();
    textureState->focus = NULL_ENTITY;
}

void UIDocumentSystem::textureUpdate(){
    if (!textureState)
        return;

    for (auto& pair : textureState->entries){
        HtmlUITextureDocument& entry = pair.second;

        // size and ratio follow the component without a reload
        if (const UIDocumentComponent* document = scene->findComponent<UIDocumentComponent>(pair.first)){
            int width, height;
            float ratio;
            textureSize(*document, width, height, ratio);
            if (width != entry.width || height != entry.height || ratio != entry.ratio){
                entry.width = width;
                entry.height = height;
                entry.ratio = ratio;
                entry.context->setDimensions(layoutSize(width, height, ratio));
                entry.context->setDensityRatio(ratio);
            }
        }

        entry.context->getTextInputHandler().flushKeyboard();
        entry.context->update();
    }
}

void UIDocumentSystem::textureRenderOffscreen(){
    if (!textureState)
        return;

    const uint64_t frame = SystemRender::getFrameCount();
    for (auto& pair : textureState->entries){
        HtmlUITextureDocument& entry = pair.second;
        if (!entry.target)
            continue;

        auto doc = documents.find(pair.first);
        const bool shown = doc != documents.end() && isShown(doc->second);
        const bool resized = !entry.target->isCreated() || (int)entry.target->getWidth() != entry.width ||
                             (int)entry.target->getHeight() != entry.height;
        if (!ensureTarget(*entry.target, entry.width, entry.height, pair.first)){
            entry.rendered = false;
            continue;
        }

        if (!shown){
            // hidden: the texture shows nothing
            if (entry.showing || resized){
                clearTarget(*entry.target);
                entry.showing = false;
            }
            entry.rendered = true;
            continue;
        }

        // the context's own pixel ratio and font raster scale: texture pixels per CSS px
        if (!entry.context->render(Rect(0, 0, (float)entry.width, (float)entry.height))){
            // nothing drawn yet (shaders compiling): a transparent texture until then
            if (resized)
                clearTarget(*entry.target);
            continue;
        }

        // a new frame (or a new target): the straight-alpha copy; else the texture still holds it
        RmlRenderer& renderer = entry.context->getRenderer();
        RmlRenderTarget* source = entry.context->getRenderTarget();
        if (!source || (!renderer.hasFrame(frame) && !resized && entry.showing))
            continue;
        if (renderer.drawUnpremultiplied(*source, entry.target->getRender(), entry.width, entry.height)){
            entry.rendered = true;
            entry.showing = true;
        }
    }
}

bool UIDocumentSystem::textureAllRendered() const{
    if (!textureState)
        return true;

    for (const auto& pair : textureState->entries){
        auto doc = documents.find(pair.first);
        if (doc != documents.end() && isShown(doc->second) && !pair.second.rendered)
            return false;
    }
    return true;
}

std::vector<HtmlUIContext*> UIDocumentSystem::getTextureContexts() const{
    std::vector<HtmlUIContext*> contexts;
    if (textureState){
        for (const auto& pair : textureState->entries){
            contexts.push_back(pair.second.context.get());
        }
    }
    return contexts;
}

Framebuffer* UIDocumentSystem::getTargetTexture(Entity entity) const{
    if (!textureState)
        return nullptr;

    auto it = textureState->entries.find(entity);
    return (it != textureState->entries.end()) ? it->second.target : nullptr;
}

HtmlUIContext* UIDocumentSystem::getTextureContext(Entity entity) const{
    if (!textureState)
        return nullptr;

    auto it = textureState->entries.find(entity);
    return (it != textureState->entries.end()) ? it->second.context.get() : nullptr;
}

Entity UIDocumentSystem::getTextureFocus() const{
    return textureState ? textureState->focus : NULL_ENTITY;
}

HtmlUIContext* UIDocumentSystem::keyContext() const{
    if (textureState && textureState->focus != NULL_ENTITY){
        auto it = textureState->entries.find(textureState->focus);
        auto doc = documents.find(textureState->focus);
        const UIDocumentComponent* component = scene->findComponent<UIDocumentComponent>(textureState->focus);
        if (it != textureState->entries.end() && doc != documents.end() && isShown(doc->second) && component && component->interactive)
            return it->second.context.get();
        // focus on a document that went or hid: nobody's (the screen gets keys again after a press)
        return nullptr;
    }
    return hasVisibleDocument() ? getContext() : nullptr;
}

void UIDocumentSystem::textureFocusScreen(){
    if (textureState)
        textureState->focus = NULL_ENTITY;
}

bool UIDocumentSystem::isWorldPointerCaptured(int pointer, int button, bool touch) const{
    if (!textureState || textureState->captured.empty())
        return false;
    if (touch)
        return textureState->captured.count(pointer) > 0;
    // a mouse move has no button: any captured button keeps the mouse
    if (button < 0){
        for (const auto& pair : textureState->captured){
            if (pair.first < 0)
                return true;
        }
        return false;
    }
    return textureState->captured.count(captureKey(pointer, button, false)) > 0;
}

bool UIDocumentSystem::dispatchWorldWheel(float dx, float dy, int mods){
    if (!textureState)
        return false;

    // the hovered texture document under the mouse
    for (auto& pair : textureState->entries){
        HtmlUITextureDocument& entry = pair.second;
        if (!entry.hovered)
            continue;
        const bool notScrolled = entry.context->processMouseWheel(Rml::Vector2f(dx, dy), RmlKeyMap::toRmlModifiers(mods));
        return !notScrolled || entry.context->isPointerOverContent();
    }
    return false;
}

void UIDocumentSystem::textureMouseLeave(){
    if (!textureState)
        return;

    for (auto& pair : textureState->entries){
        if (pair.second.hovered){
            pair.second.context->processMouseLeave();
            pair.second.hovered = false;
        }
    }
}

void UIDocumentSystem::textureCancelAll(){
    if (!textureState)
        return;

    // ui-multitouch D6: every finger and the mouse of every texture document end without a click
    for (auto& pair : textureState->entries){
        HtmlUIContext& context = *pair.second.context;
        const std::vector<int> touches = context.getTouches();
        for (int pointer : touches)
            context.processTouchCancel(pointer);
        context.processMouseCancel();
    }
    textureState->captured.clear();
}

bool UIDocumentSystem::dispatchWorldPointer(int pointer, int button, float canvasX, float canvasY, int mods, PointerPhase phase, bool touch, bool covered){
    if (!textureState || textureState->entries.empty())
        return false;
    HtmlUITextureState& state = *textureState;

    const int rmlMods = RmlKeyMap::toRmlModifiers(mods);
    const int rmlButton = touch ? 0 : toRmlButton(button);
    if (!touch && (phase == PointerPhase::DOWN || phase == PointerPhase::UP) && rmlButton < 0)
        return false;

    // the ray of the scene camera through the canvas point
    Ray ray;
    const Entity cameraEntity = scene->getCamera();
    const bool hasCamera = cameraEntity != NULL_ENTITY && scene->findComponent<CameraComponent>(cameraEntity);
    if (hasCamera){
        Camera camera(scene, cameraEntity);
        ray = camera.screenToRay(canvasX, canvasY);
    }

    // the point of a uv in a document's layout (CSS px)
    auto toDocument = [](const HtmlUITextureDocument& entry, Vector2 uv){
        const Rml::Vector2i dimensions = entry.context->getDimensions();
        return Rml::Vector2f(uv.x * dimensions.x, uv.y * dimensions.y);
    };

    // sends the event; true when the document consumed it (§3.6.2 rules: the hovered element
    // blocks, a release after a taken press is taken)
    auto send = [&](HtmlUITextureDocument& entry, Vector2 uv) -> bool {
        HtmlUIContext& context = *entry.context;
        const Rml::Vector2f point = toDocument(entry, uv);
        bool consumed = false;
        if (touch){
            switch (phase){
                case PointerPhase::DOWN: consumed = context.processTouchStart(pointer, point, rmlMods); break;
                case PointerPhase::MOVE: consumed = context.processTouchMove(pointer, point, rmlMods); break;
                case PointerPhase::UP: consumed = context.processTouchEnd(pointer, point, rmlMods); break;
                case PointerPhase::CANCEL: context.processTouchCancel(pointer); break;
            }
        }else if (phase == PointerPhase::CANCEL){
            // ui-multitouch D6: the held buttons let go without a click
            context.processMouseCancel();
        }else{
            consumed = context.processMouseMove(point, rmlMods);
            if (phase == PointerPhase::DOWN){
                consumed = context.processMouseButtonDown(rmlButton, rmlMods);
            }else if (phase == PointerPhase::UP){
                consumed = context.processMouseButtonUp(rmlButton, rmlMods) || consumed;
            }
        }
        entry.hovered = (phase != PointerPhase::CANCEL) && !(touch && phase == PointerPhase::UP);
        return consumed;
    };

    // a captured pointer: its document gets everything until the release, uv clamped
    const int key = captureKey(pointer, button, touch);
    auto capturedIt = state.captured.end();
    if (touch || phase == PointerPhase::DOWN || phase == PointerPhase::UP){
        capturedIt = state.captured.find(key);
    }else{
        for (auto it = state.captured.begin(); it != state.captured.end(); ++it){
            if (it->first < 0){
                capturedIt = it;
                break;
            }
        }
    }
    if (capturedIt != state.captured.end()){
        const Entity entity = capturedIt->second;
        auto entryIt = state.entries.find(entity);
        if (entryIt == state.entries.end() || documents.find(entity) == documents.end()){
            state.captured.erase(capturedIt);
            return false;
        }
        HtmlUITextureDocument& entry = entryIt->second;

        Vector2 uv = entry.lastUV;
        if (hasCamera){
            const UIDocumentComponent* document = scene->findComponent<UIDocumentComponent>(entity);
            const Entity surface = (document && document->inputSurface != NULL_ENTITY) ? document->inputSurface : entity;
            const UIWorldHit hit = raycastMeshUV(scene, surface, ray);
            if (hit.hit)
                uv = hit.uv;
        }
        uv = Vector2(std::min(std::max(uv.x, 0.0f), 1.0f), std::min(std::max(uv.y, 0.0f), 1.0f));
        entry.lastUV = uv;

        send(entry, uv);
        if (phase == PointerPhase::UP || phase == PointerPhase::CANCEL)
            state.captured.erase(capturedIt);
        return true;
    }
    if (phase == PointerPhase::CANCEL){
        // a pointer nobody captured: drop a touch a document may still track
        for (auto& pair : state.entries){
            if (touch && pair.second.context->hasTouch(pointer))
                pair.second.context->processTouchCancel(pointer);
        }
        return false;
    }

    // every interactive, shown document whose surface the ray hits, nearest first; none
    // under screen-space UI that took the event (it is drawn over the world)
    struct Candidate { Entity entity; HtmlUITextureDocument* entry; UIWorldHit hit; };
    std::vector<Candidate> candidates;
    if (hasCamera && !covered){
        for (auto& pair : state.entries){
            const UIDocumentComponent* document = scene->findComponent<UIDocumentComponent>(pair.first);
            auto doc = documents.find(pair.first);
            if (!document || !document->interactive || doc == documents.end() || !isShown(doc->second))
                continue;

            const Entity surface = (document->inputSurface != NULL_ENTITY) ? document->inputSurface : pair.first;
            const UIWorldHit hit = raycastMeshUV(scene, surface, ray);
            if (hit.hit)
                candidates.push_back({pair.first, &pair.second, hit});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b){
        return a.hit.distance < b.hit.distance;
    });

    // offered nearest first, until one consumes (a transparent body passes it on)
    Entity receiver = NULL_ENTITY;
    std::vector<Entity> reached;
    for (Candidate& candidate : candidates){
        const Vector2 uv(std::min(std::max(candidate.hit.uv.x, 0.0f), 1.0f), std::min(std::max(candidate.hit.uv.y, 0.0f), 1.0f));
        candidate.entry->lastUV = uv;
        reached.push_back(candidate.entity);
        if (send(*candidate.entry, uv)){
            receiver = candidate.entity;
            break;
        }
        // a touch it did not take is not its touch
        if (touch && phase == PointerPhase::DOWN)
            candidate.entry->context->processTouchCancel(pointer);
    }

    // documents the pointer left
    for (auto& pair : state.entries){
        if (pair.second.hovered && std::find(reached.begin(), reached.end(), pair.first) == reached.end()){
            pair.second.context->processMouseLeave();
            pair.second.hovered = false;
        }
    }

    if (phase == PointerPhase::DOWN){
        // keys follow the last press: this document, or (none consumed) the game
        state.focus = receiver;
        if (receiver != NULL_ENTITY){
            state.captured[key] = receiver;
            // a press on a text field (re)opens the virtual keyboard
            if (hasFocusedTextInput())
                state.entries[receiver].context->getTextInputHandler().requestKeyboard();
        }
    }

    return receiver != NULL_ENTITY;
}
