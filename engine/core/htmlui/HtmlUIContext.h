// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUICONTEXT_H
#define HTMLUICONTEXT_H

#include "Export.h"

#include <RmlUi/Core/Types.h>

#include "math/Rect.h"
#include "render/CameraRender.h"
#include "render/FramebufferRender.h"
#include "render/Render.h"

#include <memory>
#include <string>

namespace Rml {
    class Context;
    class ElementDocument;
}

namespace jasperengine {

    class RmlRenderer;

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
        // true when the hovered/active element is inside a document body
        bool isPointerOverContent() const;
        void setDebuggerVisible(bool visible);

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
    };

}

#endif //HTMLUICONTEXT_H
