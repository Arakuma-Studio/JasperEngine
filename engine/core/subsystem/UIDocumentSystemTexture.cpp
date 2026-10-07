// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIDocumentSystem texture-mode documents (phase 1, p1-rtt, spec §3.12): HtmlUITextureState
// (one context and target per document) and world-space pointer input.
//
// A TEXTURE-mode document lives in its own HtmlUIContext ("tex:<scene>:<entity>"): its
// layout size is textureWidth x textureHeight divided by its pixel ratio (CSS px), and it
// renders at that ratio into a textureWidth x textureHeight target. The document stays in
// UIDocumentSystem::documents like a screen document, so every element API (getDocument,
// getElementById, UIDocument) reaches it; only loading, unloading, updating and rendering go
// through the hooks below.

#include "UIDocumentSystem.h"

#include "Engine.h"
#include "Input.h"
#include "Log.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "htmlui/HtmlUIContext.h"
#include "htmlui/UIWorldRaycast.h"
#include "object/Camera.h"
#include "texture/Framebuffer.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>

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
        // what getTargetTexture returns: straight-alpha RGBA8, top-left origin. Kept at a stable
        // address for as long as the system lives (Image/Mesh/materials hold the pointer)
        Framebuffer* target = nullptr;
        int width = 0, height = 0;   // texture pixels
        float ratio = 1.0f;          // CSS px -> texture px
        bool hovered = false;        // the last pointer event was over it
        bool rendered = false;       // its target holds a frame
        Vector2 lastUV;              // where a captured pointer was last over the surface
    };

    struct JASPERENGINE_INTERNAL HtmlUITextureState {
        std::unordered_map<Entity, HtmlUITextureDocument> entries;
        // every target ever handed out (see HtmlUITextureDocument::target)
        std::vector<std::unique_ptr<Framebuffer>> targets;
        std::unordered_map<int, Entity> captured;   // pointer id -> document that took the down
        Entity focus = NULL_ENTITY;                 // document that took the last pointer-down

        Framebuffer* newTarget(){
            targets.emplace_back(new Framebuffer());
            return targets.back().get();
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

    // engine modifier bits (Input.h) to RmlUi's
    int toRmlModifiers(int mods){
        int result = 0;
        if (mods & D_MODIFIER_SHIFT) result |= Rml::Input::KM_SHIFT;
        if (mods & D_MODIFIER_CONTROL) result |= Rml::Input::KM_CTRL;
        if (mods & D_MODIFIER_ALT) result |= Rml::Input::KM_ALT;
        if (mods & D_MODIFIER_SUPER) result |= Rml::Input::KM_META;
        if (mods & D_MODIFIER_CAPS_LOCK) result |= Rml::Input::KM_CAPSLOCK;
        if (mods & D_MODIFIER_NUM_LOCK) result |= Rml::Input::KM_NUMLOCK;
        return result;
    }

    // engine mouse buttons to RmlUi's (0 left, 1 right, 2 middle); touches press the primary
    int toRmlButton(int button, bool touch){
        if (touch)
            return 0;
        switch (button){
            case D_MOUSE_BUTTON_LEFT: return 0;
            case D_MOUSE_BUTTON_RIGHT: return 1;
            case D_MOUSE_BUTTON_MIDDLE: return 2;
            default: return -1;
        }
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

}

HtmlUITextureState& UIDocumentSystem::getTextureState(){
    if (!textureState)
        textureState = std::make_shared<HtmlUITextureState>();
    return *textureState;
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
    }
    entry.width = width;
    entry.height = height;
    entry.ratio = ratio;
    entry.context->setDimensions(dimensions);
    entry.context->setDensityRatio(ratio);
    if (!entry.target)
        entry.target = state.newTarget();

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

    // the target object stays (consumers may still point at it), its GPU texture goes
    if (it->second.target)
        it->second.target->destroy();
    it->second.context.reset();
    textureState->forget(entity);
    textureState->entries.erase(it);
}

void UIDocumentSystem::textureDestroyAll(){
    if (!textureState)
        return;

    for (auto& pair : textureState->entries){
        auto doc = documents.find(pair.first);
        if (doc != documents.end()){
            pair.second.context->unloadDocument(doc->second);
            documents.erase(doc);
        }
        if (pair.second.target)
            pair.second.target->destroy();
        pair.second.context.reset();
    }
    textureState->entries.clear();
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

        entry.context->update();
    }
}

void UIDocumentSystem::textureRenderOffscreen(){
    if (!textureState)
        return;

    for (auto& pair : textureState->entries){
        HtmlUITextureDocument& entry = pair.second;
        auto doc = documents.find(pair.first);
        if (doc == documents.end() || !isShown(doc->second))
            continue;

        // the context's own pixel ratio and font raster scale: texture pixels per CSS px
        if (!entry.context->render(Rect(0, 0, (float)entry.width, (float)entry.height)))
            continue;

        // HTMLUI_P1_PENDING (p1-render merge): un-premultiplying copy of the context's layer 0
        // into entry.target with the HTMLUI_POST Unp variant (RmlRenderer render-to-Framebuffer
        // entry point)
        entry.rendered = true;
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

bool UIDocumentSystem::dispatchWorldPointer(int pointer, int button, float canvasX, float canvasY, int mods, PointerPhase phase, bool touch){
    if (!textureState || textureState->entries.empty())
        return false;
    HtmlUITextureState& state = *textureState;

    const int rmlMods = toRmlModifiers(mods);
    const int rmlButton = toRmlButton(button, touch);
    if ((phase == PointerPhase::DOWN || phase == PointerPhase::UP) && rmlButton < 0)
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

    // sends the event; true when the document consumed it
    auto send = [&](HtmlUITextureDocument& entry, Vector2 uv) -> bool {
        HtmlUIContext& context = *entry.context;
        bool consumed = context.processMouseMove(toDocument(entry, uv), rmlMods);
        if (phase == PointerPhase::DOWN){
            consumed = context.processMouseButtonDown(rmlButton, rmlMods);
        }else if (phase == PointerPhase::UP){
            consumed = context.processMouseButtonUp(rmlButton, rmlMods) || consumed;
        }else if (phase == PointerPhase::CANCEL){
            context.processMouseLeave();
            consumed = false;
        }
        entry.hovered = (phase != PointerPhase::CANCEL);
        return consumed;
    };

    // a captured pointer: its document gets everything until the release, uv clamped
    auto capturedIt = state.captured.find(pointer);
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
            state.captured.erase(pointer);
        return true;
    }

    // every interactive, shown document whose surface the ray hits, nearest first
    struct Candidate { Entity entity; HtmlUITextureDocument* entry; UIWorldHit hit; };
    std::vector<Candidate> candidates;
    if (hasCamera && phase != PointerPhase::CANCEL){
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
    }

    // documents the pointer left
    for (auto& pair : state.entries){
        if (pair.second.hovered && std::find(reached.begin(), reached.end(), pair.first) == reached.end()){
            pair.second.context->processMouseLeave();
            pair.second.hovered = false;
        }
    }

    if (phase == PointerPhase::DOWN){
        // keys follow the last press: this document, or (none consumed) the screen or the game
        state.focus = receiver;
        if (receiver != NULL_ENTITY)
            state.captured[pointer] = receiver;
    }

    return receiver != NULL_ENTITY;
}
