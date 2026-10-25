// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIDOCUMENTSYSTEM_H
#define UIDOCUMENTSYSTEM_H

#include "SubSystem.h"
#include "math/Rect.h"
#include "render/Render.h"
#include "component/UIDocumentComponent.h"
#include "System.h"
#include "htmlui/HtmlUIRenderStats.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <functional>
#include <vector>

typedef struct lua_State lua_State;

// RmlUi stays out of the engine API (swappable core): forward declarations only
namespace Rml {
    class Element;
    class ElementDocument;
}

namespace jasperengine::htmlui::compat {
    struct CompatDocument;
    struct Diagnostic;
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
    struct HtmlUICompatState;   // p2-html    (UIDocumentSystemCompat.cpp)

    // phase of a pointer event handed to texture-mode documents (dispatchWorldPointer)
    enum class PointerPhase { DOWN, MOVE, UP, CANCEL };

    // What a gamepad gives documents (phase-4 p4-input; Engine::systemGamepad*): its buttons, the four
    // directions of the left stick and the two triggers, each a key of keydown/keyup events (DOM `code`
    // "GamepadDPadUp", "GamepadA", ...; RmlKeyMap::gamepadDomCode)
    enum class GamepadUIKey {
        DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT,
        LEFT_STICK_UP, LEFT_STICK_DOWN, LEFT_STICK_LEFT, LEFT_STICK_RIGHT,
        A, B, X, Y, LEFT_SHOULDER, RIGHT_SHOULDER, LEFT_TRIGGER, RIGHT_TRIGGER,
        VIEW, MENU, GUIDE, LEFT_THUMB, RIGHT_THUMB,
        COUNT   // none
    };

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

        bool loadDocumentNow(Entity entity);           // UIDocument::load, getters in init: synchronous, also before the view loads
        bool isDocumentLoaded(Entity entity) const;
        UIElement getElementById(Entity entity, const std::string& id) const;
        UIElement getDocumentBody(Entity entity) const;

        bool hasDocuments() const;      // any UIDocumentComponent with a filename
        bool isAllLoaded() const;       // every enabled document loaded or failed, and (if any visible) a frame rendered
        HtmlUIContext* getContext() const;   // internal/tests

        // ---- phase 1. Declared by the wave-0 commit, defined by each package in its own .cpp ----

        // --- p1-render (UIDocumentSystemRender.cpp, §3.5) ---
        // camera-aware composite: UI and orthographic cameras draw the canvas rectangle in world
        // space (the editor's Scene view shows it in place), perspective ones fill the viewport
        // like drawComposite(PipelineType, bool). msaaSamples: the camera pass's sample count when
        // colorPipeline is PIP_MSAA* (the post-processed camera's multisampled scene target)
        void drawComposite(CameraComponent& camera, Transform& cameraTransform, PipelineType colorPipeline, bool flipY,
                           int msaaSamples = 1);
        const HtmlUIRenderStats& getRenderStats() const;    // last frame, summed over the scene's contexts
        // the last frame of the context that draws the entity's document (its own in texture mode,
        // else the screen context; empty without one)
        HtmlUIRenderStats getRenderStats(Entity entity) const;
        // MSAA samples of every UI target (1/2/4/8, default 4): the exported entry to
        // HtmlUIContext::setPreferredSampleCount for the editor's project setting and the
        // generated game init code (neither can reach the internal HtmlUIContext)
        static void setPreferredSampleCount(int samples);
        static int getPreferredSampleCount();
        // S16 (D1, R13): the glyph atlas budget in MiB (0: the platform's default), SDF primitives
        // and transform-aware text; the exported entry for the project's htmlUiGlyphAtlasMiB,
        // htmlUiSdfPrimitives and htmlUiTransformAwareText. Documents already drawn generate
        // their backgrounds and text again
        static void setHtmlUiRenderOptions(int glyphAtlasMiB, bool sdfPrimitives, bool transformAwareText);

        // --- p1-input (UIDocumentSystemInput.cpp, §3.6.2); the phase-0 pointer methods above become wrappers ---
        // world = false: the screen documents only (and a pointer a texture-mode document
        // holds); the engine offers the event to the documents on meshes after the component
        // UI, which is drawn over the 3D world (dispatchWorldPointer)
        bool eventOnPointerDown(int pointer, int button, float x, float y, int mods, bool touch, bool world = true);
        bool eventOnPointerUp(int pointer, int button, float x, float y, int mods, bool touch, bool world = true);
        bool eventOnPointerMove(int pointer, float x, float y, int mods, bool touch, bool world = true);
        bool eventOnTouchCancel(int pointer);
        bool eventOnWheel(float dx, float dy, int mods);
        void eventOnMouseLeave();
        bool eventOnKeyDown(int key, bool repeat, int mods);
        bool eventOnKeyUp(int key, int mods);
        bool eventOnTextInput(const std::string& utf8);
        bool eventOnImeComposition(const std::string& utf8, int cursor);
        void eventOnImeCancel();
        bool hasFocusedTextInput() const;
        // a document took this pointer's down (touch: pointer id; mouse: button) and keeps it until the up
        bool hasPointerCapture(int pointer, int button, bool touch) const;
        // the CSS cursor of the element under the pointer, ARROW when the pointer is not over content
        CursorType getPointerCursor() const;
        // p4-input: a gamepad key (Engine's routing turns buttons, the left stick and the triggers into
        // keys and repeats held directions) as a keydown/keyup at the focused element of the context keys
        // go to, with the keyboard's default actions where the key stands for one (d-pad and stick:
        // the arrows, A: Enter / Space, B: Escape) and spatial navigation for the directions. Consumed:
        // with an element focused, the directions and A always; the other keys when a listener or a
        // control stopped or prevented them. A key up is consumed when its down was
        bool eventOnGamepadKeyDown(int gamepad, GamepadUIKey key, bool repeat);
        bool eventOnGamepadKeyUp(int gamepad, GamepadUIKey key);
        // an element of a shown document has the focus of the context keys go to: the documents take the
        // gamepad's navigation (the d-pad, the left stick, A)
        bool hasGamepadFocus() const;

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
        // (mtime, size) differs from the one recorded at that document's last (re)load.
        // settleSeconds > 0 (the editor's poller; p5-tooling): a document with a changed file
        // modified less than that long ago is left for a later call, its stamps untouched, so
        // files written back to back (one edit) reload once, not once per file the poll saw
        std::vector<Reloaded> reloadChangedFiles(const std::vector<std::string>& files, double settleSeconds = 0.0);
        // The editor's hot reload knows these files changed since the documents read them (a
        // save can keep the time and size of the one before, in the same timestamp tick): the
        // next reloadChangedFiles naming them reloads the documents reading them
        void forgetFileStamps(const std::vector<std::string>& files);
        // Tests: called after a document's files were read by a (re)load and before the load
        // records what they were (p5-tooling: a file changed in that window is still seen as
        // changed by the next reloadChangedFiles); nullptr removes it
        static void setLoadedHookForTesting(std::function<void(Entity)> hook);
        bool setDebuggerVisible(bool visible);   // false when the debugger is not compiled in
        bool isDebuggerVisible() const;
        // seconds until a document of this scene needs the next update to animate (CSS
        // transitions, @keyframes): 0 while animating, infinity when nothing is scheduled.
        // The editor keeps the Scene view redrawing by it in edit mode
        double getNextUpdateDelay() const;

        // --- p1-lua (UIDocumentSystemScript.cpp, §3.9.5) ---
        std::vector<std::string> getDataModelNames(Entity entity) const;
        bool pushDataModel(lua_State* L, Entity entity, const std::string& name);   // pushes the model's Lua proxy
        // engine-internal (HtmlUIScriptInternal.h): data models and notifications of this scene
        HtmlUIScriptState& getScriptState();
        // the scene's scripts ended (LuaBinding::cleanupLuaScripts, play stop): their data models
        // are removed, and the documents unload (listeners, detached elements and DOM changes go)
        // and load again at the next update
        void releaseScriptState();

        // --- p1-rtt (UIDocumentSystemTexture.cpp, §3.12) ---
        // A pointer event the screen context did not consume, offered to the texture-mode
        // documents shown on meshes: a ray from the scene camera through the canvas point,
        // hit-tested against each interactive document's inputSurface (or own entity), nearest
        // first, at the document point of the hit's uv. A document that took a down keeps the
        // pointer until the up (capture). True = consumed. covered: screen-space UI in front of
        // the world (a screen document, the component UI) took the event, so the documents on
        // meshes only lose the pointer (hover, and key focus on a press)
        bool dispatchWorldPointer(int pointer, int button, float canvasX, float canvasY, int mods, PointerPhase phase, bool touch, bool covered = false);
        Framebuffer* getTargetTexture(Entity entity) const;        // TEXTURE mode target; nullptr otherwise
        HtmlUIContext* getTextureContext(Entity entity) const;     // internal/tests: a texture-mode document's own context
        // the texture-mode document that took the last pointer-down (it gets keys and text);
        // NULL_ENTITY when that was the screen context, the game or nothing
        Entity getTextureFocus() const;

        // --- p2-html (UIDocumentSystemCompat.cpp, phase-2 §3.12, hooks H1b/H4) ---
        // the compile of an .html document (the compat layer's RML, source map, diagnostics);
        // null for .rml documents and before the load
        std::shared_ptr<const htmlui::compat::CompatDocument> getCompiledDocument(Entity entity) const;
        // compile-time + runtime diagnostics (RmlUi's messages mapped back, Lua script
        // errors): the list UIDocument::getDiagnostics() returns
        const std::vector<htmlui::compat::Diagnostic>& getDiagnostics(Entity entity) const;
        // prefers-color-scheme ("light", "dark") and prefers-reduced-motion of the context the
        // entity's document is in (RmlUi themes are per context)
        void setColorScheme(Entity entity, const std::string& scheme);
        void setReducedMotion(Entity entity, bool reduced);
        // the scene's Lua scripts started (LuaBinding::initializeLuaScripts): the documents'
        // <script type="text/lua"> run now and after every later (re)load, until the play stops
        void startDocumentScripts();
        // ---- phase 2 ----
        // --- p2-importers: the editor's AppSettings::htmlUiVerboseDiagnostics; document loads
        // build CompatOptions::verboseDiagnostics from it (Info diagnostics logged one by one) ---
        static void setVerboseDiagnostics(bool verbose);
        static bool getVerboseDiagnostics();

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
        bool debuggerVisible = false;   // asked before the context existed

        // ---- phase 1: per-package state and private helpers (each package adds its own in its block) ----
        // shared_ptr: the deleter is bound where the state is created, so ~UIDocumentSystem
        // (UIDocumentSystem.cpp) never needs the complete types
        friend struct HtmlUIRenderState;
        friend struct HtmlUIInputState;
        friend struct HtmlUIScriptState;
        friend struct HtmlUIReloadState;
        friend struct HtmlUITextureState;
        friend struct HtmlUICompatState;

        // --- p1-render ---
        std::shared_ptr<HtmlUIRenderState> renderState;

        // --- p1-input ---
        std::shared_ptr<HtmlUIInputState> inputState;
        HtmlUIInputState& getInputState();
        // once per update(): control characters that came without their key down (§3.6.2)
        void updateInput();

        // --- p1-lua ---
        std::shared_ptr<HtmlUIScriptState> scriptState;
        // p1-editor's full reload with state preservation (§3.8.6 step 4) calls it after the new
        // document loaded, while the old one is still alive (RmlUi destroys unloaded documents
        // at the next context update): oldToNew maps the old document and its id-ed elements to
        // their counterparts in the new one. p1-lua moves those elements' listeners and warns
        // about the ones it drops
        void scriptDocumentReloaded(Entity entity, const std::unordered_map<Rml::Element*, Rml::Element*>& oldToNew);
        // one-line hooks of the phase-0 UIDocumentSystem.cpp (a reload path that bypasses
        // applyComponent/unloadEntityDocument calls scriptDocumentLoaded/scriptDocumentUnloading itself)
        void scriptLoadDocuments();                 // load(): every enabled document, so a script's init can query them
        void scriptContextCreated();                // ensureContext(): data models into the new context, DOM change events
        void scriptContextDestroying();             // destroyContext(), first: detached pools, RmlUi side of the data models
        // applyComponent, before a load: placeholder data models (in target, p1-rtt's texture
        // contexts; the screen context when null)
        void scriptDocumentLoading(const std::string& filename, HtmlUIContext* target = nullptr);
        void scriptDocumentLoaded(Entity entity);   // applyComponent, after a load: onLoad (fired after the context update)
        void scriptDocumentUnloading(Entity entity, Rml::ElementDocument* document);   // unloadEntityDocument: pool, onUnload
        void scriptComponentRemoved(Entity entity); // onComponentRemoved: the data models the document created
        void scriptUpdate();                        // end of update(): detached pool, onChange, onUnload/onLoad

        // --- p1-editor ---
        std::shared_ptr<HtmlUIReloadState> reloadState;
        HtmlUIReloadState& getReloadState();
        // context->loadDocument inside the runtime file log scope; records the document's
        // files, their stamps, its messages and the load count
        Rml::ElementDocument* loadEntityDocument(Entity entity, const std::string& filename);
        void reloadContextDestroyed();           // the context's lazy file log, the debugger owner

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
        std::vector<HtmlUIContext*> getTextureContexts() const;   // renderOffscreen(): statistics
        // the context data models of owner live in (its texture context, else the screen one; p1-lua)
        HtmlUIContext* modelContext(Entity owner) const;
        // the context an entity's document lives in: its texture context, else the screen one (p1-editor's reload)
        HtmlUIContext* documentContext(Entity entity) const;
        // p1-input's calls: keys/text/IME go to the context that took the last press (a texture
        // document's, or the screen's when it has a shown document); null = no document's
        HtmlUIContext* keyContext() const;
        void textureFocusScreen();                   // the screen context took a press
        bool isWorldPointerCaptured(int pointer, int button, bool touch) const;   // button < 0: any mouse button
        bool dispatchWorldWheel(float dx, float dy, int mods);   // the hovered texture document
        void textureMouseLeave();

        // --- p2-html ---
        std::shared_ptr<HtmlUICompatState> compatState;
        HtmlUICompatState& getCompatState();
        // scriptDocumentLoaded (every load and reload): navigation, themes, its Lua scripts queued
        void compatDocumentLoaded(Entity entity);
        // applyComponent, a freshly loaded document shown: autofocus
        void compatDocumentShown(Rml::ElementDocument* document);
        // the queued document scripts: update() before the context update, the end of a synchronous load
        void compatRunScripts();
        // releaseScriptState (play stop): no more document scripts
        void compatScriptsStopped();
        // hook H2: a stylesheet-only change of an .html document: the new compile's sheets swap
        // in place when its RML (and scripts, templates) did not change; false = needs a full reload
        bool compatRestyle(Entity entity);
        // hook H2: the compat dependencies of a loaded .html document (CSS, imports, Lua src, manifest)
        std::vector<std::string> compatDependencies(Rml::ElementDocument* document) const;
    };

}

#endif //UIDOCUMENTSYSTEM_H
