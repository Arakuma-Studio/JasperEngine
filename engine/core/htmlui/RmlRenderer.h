// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RMLRENDERER_H
#define RMLRENDERER_H

#include <RmlUi/Core/RenderInterface.h>

#include "math/Rect.h"
#include "render/Render.h"

#include <cstddef>

namespace jasperengine {

    class FramebufferRender;
    class CameraRender;
    class TextureRender;

    // RmlUi's render interface on the engine render abstraction. One per HtmlUIContext:
    // records a frame's draws, then replays them in one offscreen pass into the context's
    // target, which is composited into the camera pass.
    class RmlRenderer : public Rml::RenderInterface {
    public:
        RmlRenderer();
        ~RmlRenderer() override;                    // releaseGpuResources()

        // Rml::RenderInterface, required set + SetTransform
        Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
        void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
        void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
        Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
        Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
        void ReleaseTexture(Rml::TextureHandle texture) override;
        void EnableScissorRegion(bool enable) override;
        void SetScissorRegion(Rml::Rectanglei region) override;
        void SetTransform(const Rml::Matrix4f* transform) override;

        // engine side, called by HtmlUIContext (HtmlUIContextRender.cpp)
        // Pipelines exist once both HTMLUI variants are compiled; false while they compile
        // (ShaderPool::get may hand back a shader that is not created yet).
        bool loadPipelines();
        bool isReady() const;
        // contextSize in document px, targetSize in target pixels
        void beginFrame(Rml::Vector2i contextSize, Rml::Vector2i targetSize);
        // uploads this frame's vertices/indices, runs one offscreen pass into target and
        // replays the recorded draws; then destroys what was released during the frame
        bool endFrame(FramebufferRender& target, CameraRender& pass);
        // the document target as a quad inside the current (camera) pass
        bool drawComposite(TextureRender& source, PipelineType compositePipeline, bool flipY);
        // view lost / context destroyed: every GPU object, handles stay valid as "dead"
        void releaseGpuResources();

        // pure function, unit-tested: RmlUi scissor (document px, top-left origin) to a
        // CameraRender::applyScissor rect (target px, bottom-left origin)
        static Rect toTargetScissor(Rml::Rectanglei region, Rml::Vector2i contextSize,
                                    Rml::Vector2i targetSize, bool openGLFlipped);

        // test statistics
        size_t getLiveGeometryCount() const;
        size_t getLiveTextureCount() const;
        size_t getLastFrameDrawCount() const;
        size_t getVertexCapacity() const;
    };

}

#endif //RMLRENDERER_H
