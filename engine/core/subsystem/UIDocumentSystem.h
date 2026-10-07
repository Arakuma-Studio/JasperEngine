// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIDOCUMENTSYSTEM_H
#define UIDOCUMENTSYSTEM_H

#include "SubSystem.h"
#include "math/Rect.h"
#include "render/Render.h"
#include "component/UIDocumentComponent.h"
#include "htmlui/HtmlUIRenderStats.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

typedef struct lua_State lua_State;

// RmlUi stays out of the engine API (swappable core): forward declarations only
namespace Rml {
    class Element;
    class ElementDocument;
}

namespace jasperengine{

    class Framebuffer;
    class HtmlUIContext;
    class UIElement;
    struct UIDocumentContext;
    struct CameraComponent;
    struct Transform;

    // Per-package state of the phase-1 work (phase-1 spec §3.0): each struct is defined, and
    // created lazily, in its package's .cpp, so no package edits another's private members
    struct HtmlUIRenderState;   // p1-render  (UIDocumentSystemRender.cpp)
    struct HtmlUIInputState;    // p1-input   (UIDocumentSystemInput.cpp)
    struct HtmlUIScriptState;   // p1-lua     (UIDocumentSystemScript.cpp)
    struct HtmlUIReloadState;   // p1-editor  (UIDocumentSystemReload.cpp)
    struct HtmlUITextureState;  // p1-rtt     (UIDocumentSystemTexture.cpp)

    // phase of a pointer event handed to texture-mode documents (dispatchWorldPointer)
    enum class PointerPhase { DOWN, MOVE, UP, CANCEL };

    enum class HtmlUIMessageLevel { Info, Warning, Error };

    // an RmlUi message attributed to a document (Inspector "Messages", getDocumentMessages)
    struct HtmlUIMessage {
        HtmlUIMessageLevel level = HtmlUIMessageLevel::Info;
        std::string text;
        std::string file;   // asset-relative; empty when RmlUi gave none
        int line = 0;       // 0 = unknown
    };

    // The HTML/CSS documents of a scene (UIDocumentComponent): one RmlUi context, created
    // the first time the scene has a document, drawn offscreen and composited in front of
    // the component UI by RenderSystem.
    class JASPERENGINE_API UIDocumentSystem : public SubSystem {
    public:
        UIDocumentSystem(Scene* scene);
        virtual ~UIDocumentSystem();

        void load() override;                 // nothing heavy: the context is created lazily in update()
        void destroy() override;              // destroy context, HtmlUI::release() if acquired
        void update(double dt) override;
        void draw() override;                 // empty: RenderSystem drives drawing
        void onComponentAdded(Entity entity, ComponentId componentId) override;    // mark needReload
        void onComponentRemoved(Entity entity, ComponentId componentId) override;  // unload that document

        // RenderSystem hooks; one-line delegations to the context
        void renderOffscreen(const Rect& viewRect);                 // context->render(viewRect)
        void drawComposite(PipelineType colorPipeline, bool flipY); // context->composite(...)

        // Engine::system* hooks, canvas coordinates (top-left origin, y down); true = consumed
        bool eventOnPointerDown(int button, float x, float y, int mods);
        bool eventOnPointerUp(int button, float x, float y, int mods);
        bool eventOnPointerMove(float x, float y, int mods);

        // the editor lays documents out at the project canvas whatever the view size
        // (mirrors UISystem::setAnchorReferenceSize)
        void setReferenceSize(int width, int height);
        void clearReferenceSize();

        bool loadDocumentNow(Entity entity);           // UIDocument::load: synchronous when the view is loaded
        bool isDocumentLoaded(Entity entity) const;
        UIElement getElementById(Entity entity, const std::string& id) const;
        UIElement getDocumentBody(Entity entity) const;

        bool hasDocuments() const;      // any UIDocumentComponent with a filename
        bool isAllLoaded() const;       // every enabled document loaded or failed, and (if any visible) a frame rendered
        HtmlUIContext* getContext() const;   // internal/tests

        // ---- phase 1. Declared by the wave-0 commit, defined by each package in its own .cpp ----

        // --- p1-render (UIDocumentSystemRender.cpp, §3.5) ---
        // camera-aware composite; drawComposite(PipelineType, bool) becomes a thin wrapper of it
        void drawComposite(CameraComponent& camera, Transform& cameraTransform, PipelineType colorPipeline, bool flipY);
        const HtmlUIRenderStats& getRenderStats() const;    // last frame, summed over the scene's contexts
        // MSAA samples of every UI target (1/2/4/8, default 4): the exported entry to
        // HtmlUIContext::setPreferredSampleCount for the editor's project setting and the
        // generated game init code (neither can reach the internal HtmlUIContext)
        static void setPreferredSampleCount(int samples);
        static int getPreferredSampleCount();

        // --- p1-input (UIDocumentSystemInput.cpp, §3.6.2); the phase-0 pointer methods above become wrappers ---
        bool eventOnPointerDown(int pointer, int button, float x, float y, int mods, bool touch);
        bool eventOnPointerUp(int pointer, int button, float x, float y, int mods, bool touch);
        bool eventOnPointerMove(int pointer, float x, float y, int mods, bool touch);
        bool eventOnTouchCancel(int pointer);
        bool eventOnWheel(float dx, float dy, int mods);
        void eventOnMouseLeave();
        bool eventOnKeyDown(int key, bool repeat, int mods);
        bool eventOnKeyUp(int key, int mods);
        bool eventOnTextInput(const std::string& utf8);
        bool eventOnImeComposition(const std::string& utf8, int cursor);
        void eventOnImeCancel();
        bool hasFocusedTextInput() const;

        // --- p1-editor (UIDocumentSystemReload.cpp, §3.8.6-§3.8.7); paths are asset-relative ---
        struct Reloaded { Entity entity; bool stylesheetOnly; };
        std::vector<std::string> getDocumentFiles(Entity entity) const;   // document + union(scanner, runtime log)
        void reloadDocument(Entity entity, bool preserveState = true);
        void reloadStyleSheets(Entity entity);
        void reloadTexture(const std::string& path);               // Rml::RenderManager::ReleaseTexture(path) on every context
        const std::vector<HtmlUIMessage>& getDocumentMessages(Entity entity) const;
        Rml::ElementDocument* getDocument(Entity entity) const;    // engine-internal; null until loaded
        uint32_t getLoadCount(Entity entity) const;                // +1 on every (re)load, a stylesheet-only refresh included
        // The one reload entry point for both the editor watcher and phase 4's McpAutoSync pick-up:
        // reloads each document of this scene that depends on one of `files` whose stamp
        // (mtime, size) differs from the one recorded at that document's last (re)load
        std::vector<Reloaded> reloadChangedFiles(const std::vector<std::string>& files);
        bool setDebuggerVisible(bool visible);   // false when the debugger is not compiled in
        bool isDebuggerVisible() const;

        // --- p1-lua (UIDocumentSystemScript.cpp, §3.9.5) ---
        std::vector<std::string> getDataModelNames(Entity entity) const;
        bool pushDataModel(lua_State* L, Entity entity, const std::string& name);   // pushes the model's Lua proxy

        // --- p1-rtt (UIDocumentSystemTexture.cpp, §3.12) ---
        // A pointer event the screen context did not consume, offered to the texture-mode
        // documents shown on meshes: a ray from the scene camera through the canvas point,
        // hit-tested against each interactive document's inputSurface (or own entity), nearest
        // first, at the document point of the hit's uv. A document that took a down keeps the
        // pointer until the up (capture). True = consumed.
        bool dispatchWorldPointer(int pointer, int button, float canvasX, float canvasY, int mods, PointerPhase phase, bool touch);
        Framebuffer* getTargetTexture(Entity entity) const;        // TEXTURE mode target; nullptr otherwise
        HtmlUIContext* getTextureContext(Entity entity) const;     // internal/tests: a texture-mode document's own context
        // the texture-mode document that took the last pointer-down (it gets keys and text);
        // NULL_ENTITY when that was the screen context, the game or nothing
        Entity getTextureFocus() const;

    private:
        // HtmlUI::acquire and the context, the first time the scene has a document
        bool ensureContext();
        // unloads every document, then drops the GPU objects, the RmlUi context and RmlUi
        void destroyContext();
        // context dimensions: the reference size where set, else the canvas
        void getLayoutSize(int& width, int& height) const;
        // load/unload, z-index and visibility of one component's document
        void applyComponent(Entity entity, UIDocumentComponent& document);
        void unloadEntityDocument(Entity entity);
        bool hasVisibleDocument() const;
        // canvas coordinates to context coordinates
        void toContext(float& x, float& y) const;

        // owned. The HtmlUIContext, through a type only UIDocumentSystem.cpp defines: a field
        // naming the hidden HtmlUIContext makes GCC warn (-Wattributes) wherever HtmlUIContext.h
        // is included before this header
        UIDocumentContext* context = nullptr;
        std::unordered_map<Entity, Rml::ElementDocument*> documents;
        bool acquired = false;
        int referenceWidth = 0, referenceHeight = 0;
        bool pointerCaptured = false;   // a consumed down is followed by a consumed up
        bool debuggerVisible = false;   // asked before the context existed

        // ---- phase 1: per-package state and private helpers (each package adds its own in its block) ----
        // shared_ptr: the deleter is bound where the state is created, so ~UIDocumentSystem
        // (UIDocumentSystem.cpp) never needs the complete types
        friend struct HtmlUIRenderState;
        friend struct HtmlUIInputState;
        friend struct HtmlUIScriptState;
        friend struct HtmlUIReloadState;
        friend struct HtmlUITextureState;

        // --- p1-render ---
        std::shared_ptr<HtmlUIRenderState> renderState;

        // --- p1-input ---
        std::shared_ptr<HtmlUIInputState> inputState;

        // --- p1-lua ---
        std::shared_ptr<HtmlUIScriptState> scriptState;
        // p1-editor's full reload with state preservation (§3.8.6 step 4) calls it after the new
        // document loaded, while the old one is still alive (RmlUi destroys unloaded documents
        // at the next context update): oldToNew maps the old document and its id-ed elements to
        // their counterparts in the new one. p1-lua moves those elements' listeners and warns
        // about the ones it drops
        void scriptDocumentReloaded(Entity entity, const std::unordered_map<Rml::Element*, Rml::Element*>& oldToNew);

        // --- p1-editor ---
        std::shared_ptr<HtmlUIReloadState> reloadState;

        // --- p1-rtt ---
        std::shared_ptr<HtmlUITextureState> textureState;
        HtmlUITextureState& getTextureState();
        // the phase-0 code's calls: a TEXTURE-mode document gets its own context (and target)
        Rml::ElementDocument* textureLoadDocument(Entity entity, UIDocumentComponent& document);   // applyComponent
        bool textureUnloadDocument(Entity entity);   // unloadEntityDocument: true when it was a texture document
        void textureRemoveEntity(Entity entity);     // onComponentRemoved: drops its context and target
        void textureUpdate();                        // update(): size/ratio sync, Context::Update
        void textureRenderOffscreen();               // renderOffscreen(): each texture context into its target
        bool textureAllRendered() const;             // isAllLoaded(): every shown texture document drawn once
        void textureDestroyAll();                    // destroyContext(): before RmlUi is released
    };

}

#endif //UIDOCUMENTSYSTEM_H
