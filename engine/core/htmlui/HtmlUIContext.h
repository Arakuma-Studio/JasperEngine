// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUICONTEXT_H
#define HTMLUICONTEXT_H

#include "Export.h"

#include <RmlUi/Core/ObserverPtr.h>
#include <RmlUi/Core/Types.h>

#include "math/Rect.h"
#include "render/CameraRender.h"
#include "render/FramebufferRender.h"
#include "render/Render.h"

#include <memory>
#include <string>
#include <vector>

namespace Rml {
    class Context;
    class Element;
    class ElementDocument;
}

namespace jasperengine {

    class RmlRenderer;
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
        // true when the hovered/active element blocks the pointer: an element inside a
        // document, or a document with a background colour or a decorator (phase 1)
        bool isPointerOverContent() const;
        void setDebuggerVisible(bool visible);

        // --- p1-input (HtmlUIContextInput.cpp, phase-1 spec §3.6) ---
        // touch pointers with their ids (RmlUi treats each as the primary button)
        bool processTouchStart(int pointer, Rml::Vector2f position, int modifiers);
        bool processTouchMove(int pointer, Rml::Vector2f position, int modifiers);
        bool processTouchEnd(int pointer, Rml::Vector2f position, int modifiers);
        bool processTouchCancel(int pointer);
        bool hasTouch(int pointer) const;
        // engine wheel offsets (positive y = up, positive x = left, as GLFW) to RmlUi's
        // (positive = down/right), times the wheel scale. Returns Context::ProcessMouseWheel
        bool processMouseWheel(Rml::Vector2f offset, int modifiers);
        // RmlUi scroll units per platform wheel step (default 1; 0.25 on Web, whose layer
        // reports about 4 per notch)
        void setWheelScale(float scale);
        float getWheelScale() const;
        RmlTextInputHandler& getTextInputHandler();

        // --- HtmlUIContextRender.cpp (p0-render) ---
        // Offscreen: pixel ratio from viewRect, target (re)creation, font raster scale,
        // renderer->beginFrame, Context::Render, renderer->endFrame. False when not drawn.
        bool render(const Rect& viewRect);
        // inside the camera pass; nothing when the last render() did not draw
        void composite(PipelineType colorPipeline, bool flipY);
        void releaseGpuResources();      // target + renderer->releaseGpuResources()
        bool hasRenderedFrame() const;   // at least one successful render() since (re)creation
        float getPixelRatio() const;
        FramebufferRender* getTarget();  // tests (readback of the document layer)
        RmlRenderer& getRenderer();

    private:
        std::string name;
        Rml::Context* context = nullptr;
        std::unique_ptr<RmlRenderer> renderer;
        FramebufferRender target;
        CameraRender pass;
        Rml::Vector2i dimensions{0, 0};
        Rml::Vector2i targetSize{0, 0};
        float pixelRatio = 1.0f;
        float densityRatio = 1.0f;
        bool renderedThisFrame = false;
        bool renderedOnce = false;
        // the element a primary button press started on, until the release (RmlUi keeps it private)
        Rml::ObserverPtr<Rml::Element> activeElement;

        // --- p1-input ---
        // outlives the RmlUi context (its text fields call OnDestroy when they go)
        std::unique_ptr<RmlTextInputHandler> textInputHandler;
        std::vector<int> touches;   // touch pointers down in RmlUi
        float wheelScale;
    };

}

#endif //HTMLUICONTEXT_H
