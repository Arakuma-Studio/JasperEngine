// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RMLRENDERER_H
#define RMLRENDERER_H

#include "Export.h"

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Matrix4.h>

#include "math/Rect.h"
#include "render/BufferRender.h"
#include "render/ObjectRender.h"
#include "render/Render.h"
#include "render/TextureRender.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

namespace jasperengine {

    class FramebufferRender;
    class CameraRender;
    class ShaderRender;

    struct RmlGeometry;
    struct RmlTexture;

    // RmlUi's render interface on the engine render abstraction. One per HtmlUIContext:
    // records a frame's draws, then replays them in one offscreen pass into the context's
    // target, which is composited into the camera pass.
    class JASPERENGINE_INTERNAL RmlRenderer : public Rml::RenderInterface {
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
        // beginFrame ran in that frame (SystemRender::getFrameCount)
        bool hasFrame(uint64_t frame) const;

        // pure function, unit-tested: RmlUi scissor (document px, top-left origin) to a
        // CameraRender::applyScissor rect (target px, bottom-left origin)
        static Rect toTargetScissor(Rml::Rectanglei region, Rml::Vector2i contextSize,
                                    Rml::Vector2i targetSize, bool openGLFlipped);

        // test statistics
        size_t getLiveGeometryCount() const;
        size_t getLiveTextureCount() const;
        size_t getLastFrameDrawCount() const;
        size_t getVertexCapacity() const;
        // the GPU texture of a handle (nullptr when unknown or dead)
        TextureRender* getTextureRender(Rml::TextureHandle texture) const;
        // sokol ids of every buffer and image the renderer owns right now
        void getGpuHandles(std::vector<uint32_t>& buffers, std::vector<uint32_t>& images) const;

    private:
        // one recorded RenderGeometry: a range of this frame's index stream
        struct DrawCommand {
            uint32_t firstIndex;
            uint32_t indexCount;
            RmlTexture* texture;          // nullptr = untextured variant
            Rml::Vector2f translation;
            uint32_t transformIndex;      // into the frame's matrix list (0 = projection only)
            bool scissorEnabled;
            Rect scissor;                 // already in target pixels, bottom-left origin
        };

        // the vertex layout of Rml::Vertex on an object of the HTMLUI shader
        bool loadObject(ObjectRender& object, ShaderRender* shader, BufferRender* vertexBuffer, BufferRender* indexBuffer, bool textured, uint16_t pipelines);
        bool createStreamBuffers(size_t vertexCapacity, size_t indexCapacity);
        void destroyStreamObjects();
        // pipelines, buffers, the placeholder and the shaders (textures stay)
        void destroyPipelineObjects();
        void destroyTexture(RmlTexture* texture);
        RmlTexture* createTexture(const uint8_t* rgba, int width, int height);
        void flushPendingReleases();
        void clearFrame();

        std::unordered_set<RmlGeometry*> liveGeometry;
        std::unordered_set<RmlTexture*> liveTextures;

        // frame recording
        bool inFrame = false;
        bool loggedOutsideFrame = false;
        Rml::Vector2i contextSize{0, 0};
        Rml::Vector2i targetSize{0, 0};
        std::vector<Rml::Vertex> frameVertices;
        std::vector<uint32_t> frameIndices;
        std::vector<DrawCommand> commands;
        std::vector<Rml::Matrix4f> transforms;
        Rml::Matrix4f projection;
        uint32_t currentTransform = 0;
        bool scissorEnabled = false;
        Rml::Rectanglei scissorRegion;
        std::vector<RmlTexture*> pendingReleases;
        size_t lastFrameDrawCount = 0;
        bool framed = false;
        uint64_t frameNumber = 0;

        // GPU objects
        std::shared_ptr<ShaderRender> colorShader;
        std::shared_ptr<ShaderRender> texturedShader;
        bool pipelinesLoaded = false;
        BufferRender vertexBuffer;
        BufferRender indexBuffer;
        size_t vertexCapacity = 0;
        size_t indexCapacity = 0;
        ObjectRender colorObject;
        ObjectRender texturedObject;
        int colorParamsSlot = -1;
        int texturedParamsSlot = -1;
        std::pair<int, int> texturedTextureSlot{-1, -1};
        BufferRender compositeVertexBuffer;
        BufferRender compositeIndexBuffer;
        ObjectRender compositeObject;
        TextureRender placeholderWhite;
    };

}

#endif //RMLRENDERER_H
