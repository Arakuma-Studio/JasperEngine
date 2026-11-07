// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUICONTEXT_H
#define HTMLUICONTEXT_H

#include "Export.h"

#include <RmlUi/Core/Types.h>
#include <RmlUi/Core/Matrix4.h>
#include <RmlUi/Core/ObserverPtr.h>

#include "math/Rect.h"
#include "render/CameraRender.h"
#include "render/FramebufferRender.h"
#include "render/Render.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <map>
#include <vector>

namespace Rml {
    class Context;
    class Element;
    class ElementDocument;
    class EventListener;
}

namespace jasperengine {

    class RmlRenderer;
    class RmlRenderTarget;
    class RmlTextInputHandler;

    // One RmlUi context (the screen-space documents of a scene), its renderer, its
    // offscreen target and the composite into the camera pass.
    class JASPERENGINE_INTERNAL HtmlUIContext {
    public:
        explicit HtmlUIContext(const std::string& name);   // e.g. "scene-0x55d0c3a0"
        ~HtmlUIContext();                                    // destroy()

        // --- HtmlUIContext.cpp (p0-ecs) ---
        bool create(Rml::Vector2i dimensions);   // HtmlUI::registerContext, Rml::CreateContext(name, dims, renderer)
        void destroy();
        Rml::Context* getRmlContext() const;
        void setDimensions(Rml::Vector2i dimensions);   // Context::SetDimensions when changed
        Rml::Vector2i getDimensions() const;
        void setDensityRatio(float ratio);              // Context::SetDensityIndependentPixelRatio when changed
        Rml::ElementDocument* loadDocument(const std::string& path);  // nullptr + Log::error on failure
        void unloadDocument(Rml::ElementDocument* document);
        void update();                                  // Context::Update
        bool processMouseMove(Rml::Vector2f position, int modifiers);
        bool processMouseButtonDown(int button, int modifiers);
        bool processMouseButtonUp(int button, int modifiers);
        bool processMouseLeave();
        // ui-multitouch D6: the mouse's held buttons let go without a click or drop (pointercancel)
        void processMouseCancel();
        // true when the hovered/active element blocks the pointer: an element inside a
        // document, or a document with a background colour or a decorator (phase 1)
        bool isPointerOverContent() const;
        // The pointer routing rule (phase-1 spec §3.6.2, as implemented by p4-inspector): a
        // document, or the <body> of an HTML document (RML htmlbody), that paints no background
        // (no colour with alpha, decorator or image layer; for a document, nor its canvas body's)
        // and whose document listens for no pointer event (on the document or its <body>:
        // addEventListener from Lua or C++, or data-event-*) is not hit itself. Its elements still are, and the point goes on to the documents
        // below in z-order, then to the game when none takes it. Every hit test of the context
        // uses it (Rml::Context::SetPointerPassThrough): hover, presses, touches, wheel, drags.
        static bool passesPointerThrough(Rml::Element* element);
        void setDebuggerVisible(bool visible);

        // --- phase 2 (p2-html, hooks H1/H1b/H4) ---
        // .html/.htm documents load through the compat layer (DocumentLoader::load, with
        // options from this context: its size, density ratio, themes); .rml through RmlUi
        static bool isCompatPath(const std::string& path);
        // color scheme ("light"/"dark") and reduced motion: RmlUi themes of the whole context
        // (media queries prefers-color-scheme / prefers-reduced-motion), kept for later loads
        void setColorScheme(const std::string& scheme);
        const std::string& getColorScheme() const;
        void setReducedMotion(bool reduced);
        bool getReducedMotion() const;
        // the default action of the htmlnavigate event (an <a href> click that no listener
        // prevented): called with the document, the href and the linked document's path when the
        // anchor resolved one (`url` of the event; UIDocumentSystem loads it)
        void setNavigateHandler(std::function<void(Rml::ElementDocument*, const std::string&, const std::string&)> handler);

        // --- p1-input (HtmlUIContextInput.cpp, phase-1 spec §3.6) ---
        // touch pointers with their Input finger ids (ui-multitouch): each its own DOM pointer
        // (pointerId = fingerId + 2, touchPointerId) with its own hover, :active, capture, drag and
        // click (the fork's Context::ProcessPointer*). True when the finger is over or pressed
        // content (isTouchOverContent).
        bool processTouchStart(int pointer, Rml::Vector2f position, int modifiers);
        bool processTouchMove(int pointer, Rml::Vector2f position, int modifiers);
        bool processTouchEnd(int pointer, Rml::Vector2f position, int modifiers);
        bool processTouchCancel(int pointer);
        bool hasTouch(int pointer) const;
        const std::vector<int>& getTouches() const { return touches; }
        bool isTouchOverContent(int pointer) const;
        // D1: the DOM pointerId of an Input finger id (the mouse is pointer 1)
        static int touchPointerId(int fingerId) { return fingerId + 2; }
        // engine wheel offsets (positive y = up, positive x = left, as GLFW) to RmlUi's
        // (positive = down/right), times the wheel scale. Returns Context::ProcessMouseWheel
        bool processMouseWheel(Rml::Vector2f offset, int modifiers);
        // RmlUi scroll units per platform wheel step (default 1; 0.25 on Web, whose layer
        // reports about 4 per notch)
        void setWheelScale(float scale);
        float getWheelScale() const;
        RmlTextInputHandler& getTextInputHandler();

        // --- HtmlUIContextRender.cpp (p0-render, p1-render) ---
        // Offscreen: pixel ratio from viewRect, target (re)creation, font raster scale,
        // renderer->beginFrame, Context::Render, renderer->endFrame. Once per sokol frame, and
        // only when the context updated since its last render or the target was recreated
        // (otherwise the previous content stays). False when there is nothing to composite.
        bool render(const Rect& viewRect);
        // inside the camera pass; nothing when there is no rendered content. clipFromQuad maps
        // the quad (-1,1) top-left .. (1,-1) bottom-right (a world-space canvas); without it
        // the quad fills the viewport (flipped on Y for offscreen GL passes)
        void composite(PipelineType colorPipeline, bool flipY);
        // msaaSamples: the sample count of the camera pass when colorPipeline is PIP_MSAA* (the
        // post-processed camera's multisampled scene target)
        void composite(PipelineType colorPipeline, const Rml::Matrix4f& clipFromQuad, int msaaSamples = 1);
        void releaseGpuResources();      // target + renderer->releaseGpuResources()
        bool hasRenderedFrame() const;   // at least one successful render() since (re)creation
        float getPixelRatio() const;
        FramebufferRender* getTarget();  // tests: layer 0, the composited image (resolved under MSAA)
        RmlRenderTarget* getRenderTarget();
        RmlRenderer& getRenderer();
        // the context changed (Context::Update ran): the next render() draws again
        void markUpdated();
        // MSAA samples of every UI target: the preferred count (1/2/4/8, default 4), 1 when
        // the device cannot multisample RGBA8 and DEPTH_STENCIL attachments
        static void setPreferredSampleCount(int samples);
        static int getPreferredSampleCount();
        int getSampleCount() const;

    private:
        std::string name;
        Rml::Context* context = nullptr;
        std::unique_ptr<RmlRenderer> renderer;
        std::unique_ptr<RmlRenderTarget> renderTarget;
        // the target of the other size when two views of different sizes draw the scene in
        // turn (the editor's Scene and Game views): swapped instead of recreated every frame
        std::unique_ptr<RmlRenderTarget> spareTarget;
        bool updatedSinceRender = true;
        bool hasContent = false;          // the target holds a rendered frame
        uint64_t lastRenderFrame = UINT64_MAX;
        Rml::Vector2i dimensions{0, 0};
        Rml::Vector2i targetDimensions{0, 0};   // context dimensions of the target's content
        float pixelRatio = 1.0f;
        float densityRatio = 1.0f;
        bool renderedOnce = false;
        // the element a primary button press started on, until the release (RmlUi keeps it private)
        Rml::ObserverPtr<Rml::Element> activeElement;
        int mouseButtons = 0;   // the RmlUi mouse buttons held (bit per button), for processMouseCancel

        // --- p1-input ---
        // outlives the RmlUi context (its text fields call OnDestroy when they go)
        std::unique_ptr<RmlTextInputHandler> textInputHandler;
        std::vector<int> touches;   // touch pointers down in RmlUi (finger ids)
        // the content element each finger pressed, until its release (the touch's activeElement)
        std::map<int, Rml::ObserverPtr<Rml::Element>> touchPresses;
        float wheelScale;

        // --- phase 2 (p2-html) ---
        std::string colorScheme = "light";
        bool reducedMotion = false;
        std::function<void(Rml::ElementDocument*, const std::string&, const std::string&)> navigateHandler;
        std::unique_ptr<Rml::EventListener> navigateListener;   // on the context root (bubble phase)
#if JASPERENGINE_AUDIO_CAPTURE
        std::unique_ptr<Rml::EventListener> clickMarkerListener;   // on the context root (capture phase)
#endif
        void applyThemes();
        // a primary press or touch no document took: the focused element loses the focus
        void releaseFocus();
        // documents laid out with viewport-dependent values follow a size change (§3.12.1)
        void restyleForViewport();
    };

}

#endif //HTMLUICONTEXT_H
