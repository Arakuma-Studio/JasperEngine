// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RMLRENDERER_H
#define RMLRENDERER_H

#include "Export.h"

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Matrix4.h>

#include "HtmlUIRenderStats.h"
#include "math/Rect.h"
#include "render/BufferRender.h"
#include "render/CameraRender.h"
#include "render/ObjectRender.h"
#include "render/PipelineState.h"
#include "render/Render.h"
#include "render/TextureRender.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_set>
#include <vector>

namespace jasperengine {

    class Framebuffer;
    class FramebufferRender;
    class ShaderRender;
    class RmlFilterContext;

    struct HtmlUITexture;
    struct RmlGeometry;
    struct RmlGeometryPage;
    struct RmlTransientBuffer;

    // The render targets of one rendered context (an HtmlUIContext member): the layer stack
    // (layer 0 is what the camera pass composites), and four post-process targets (primary,
    // secondary, tertiary, blend mask). Layers are RGBA8 + DEPTH_STENCIL with the context's
    // sample count; every layer after the first shares layer 0's depth-stencil, so the stencil
    // clip survives layer switches. Post-process targets are 1-sample, no depth, created on
    // first use. Everything is recreated only when the size or sample count changes; layers
    // pushed in an earlier frame are reused.
    class JASPERENGINE_INTERNAL RmlRenderTarget {
    public:
        static constexpr int POSTPROCESS_COUNT = 4;
        static constexpr int BLEND_MASK = 3;

        RmlRenderTarget();
        ~RmlRenderTarget();
        RmlRenderTarget(const RmlRenderTarget&) = delete;
        RmlRenderTarget& operator=(const RmlRenderTarget&) = delete;

        // (re)creates layer 0 when the size or sample count changed (or it is gone); true
        // when it did, which also drops every other target
        bool ensure(int width, int height, int sampleCount);
        bool isCreated() const;
        void destroy();

        int getWidth() const;
        int getHeight() const;
        int getSampleCount() const;

        // layer 0 (its colour texture is the resolve image under MSAA); nullptr when not created
        FramebufferRender* getComposited();
        Framebuffer* getLayer(int index) const;
        int getLayerCount() const;          // created layers
        int getPostprocessCount() const;    // created post-process targets
        // framebuffers created over the target's life (performance tests)
        uint64_t getCreateCount() const;

    private:
        friend class RmlRenderer;
        friend class RmlFilterContext;

        Framebuffer* ensureLayer(int index);
        Framebuffer* ensurePostprocess(int index);

        std::vector<std::unique_ptr<Framebuffer>> layers;
        std::unique_ptr<Framebuffer> postprocess[POSTPROCESS_COUNT];
        int width = 0;
        int height = 0;
        int sampleCount = 1;
        uint64_t createCount = 0;
    };

    // CompileFilter handles (bit 0 clear; phase 5 uses handles with bit 0 set)
    struct RmlCompiledFilter {
        enum class Type { INVALID, PASSTHROUGH, BLUR, DROP_SHADOW, COLOR_MATRIX, MASK_IMAGE };
        Type type = Type::INVALID;
        float blendFactor = 1.0f;                   // PASSTHROUGH (opacity)
        float sigma = 0.0f;                         // BLUR, DROP_SHADOW: document px
        Rml::Vector2f offset{0.0f, 0.0f};           // DROP_SHADOW: document px
        Rml::ColourbPremultiplied color;            // DROP_SHADOW
        Rml::Matrix4f colorMatrix = Rml::Matrix4f::Identity();   // COLOR_MATRIX (column-major)
    };

    // CompileShader handles (bit 0 clear; phase 5 uses handles with bit 0 set)
    struct RmlCompiledShader {
        // the htmlui_gradient.frag function
        enum class Function { LINEAR, RADIAL, CONIC, REPEATING_LINEAR, REPEATING_RADIAL, REPEATING_CONIC };
        static constexpr int MAX_STOPS = 16;
        Function function = Function::LINEAR;
        Rml::Vector2f p{0.0f, 0.0f};                // linear: start, radial/conic: centre
        Rml::Vector2f v{0.0f, 0.0f};                // linear: to the end, radial: 1/radius, conic: angle unit vector
        std::vector<float> stopPositions;           // normalized, at most MAX_STOPS
        std::vector<Rml::Colourf> stopColors;       // premultiplied
    };

    // RmlUi's render interface on the engine render abstraction, with every optional RmlUi 6.3
    // feature (port of RmlUi's GL3 renderer onto sokol passes): stencil clip masks, layers,
    // SaveLayerAsTexture/SaveLayerAsMaskImage, filters (filter and backdrop-filter), gradients.
    // One per HtmlUIContext; it draws immediately between beginFrame and endFrame into the
    // context's RmlRenderTarget, opening a sokol pass only when it needs a different target, so
    // a frame without layers or filters is one pass.
    class JASPERENGINE_INTERNAL RmlRenderer : public Rml::RenderInterface {
    public:
        RmlRenderer();
        ~RmlRenderer() override;                    // releaseGpuResources()

        // Rml::RenderInterface, required
        Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
        void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
        void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
        Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
        Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
        // deletes the HtmlUITexture behind any handle (also those handed to RmlUi through
        // CallbackTextureInterface::SetTextureHandle), after the frame when one is in progress
        void ReleaseTexture(Rml::TextureHandle texture) override;
        void EnableScissorRegion(bool enable) override;
        void SetScissorRegion(Rml::Rectanglei region) override;

        // Rml::RenderInterface, optional
        void EnableClipMask(bool enable) override;
        void RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation) override;
        void SetTransform(const Rml::Matrix4f* transform) override;
        Rml::LayerHandle PushLayer() override;
        void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend_mode,
                             Rml::Span<const Rml::CompiledFilterHandle> filters) override;
        void PopLayer() override;
        Rml::TextureHandle SaveLayerAsTexture() override;
        Rml::CompiledFilterHandle SaveLayerAsMaskImage() override;
        Rml::CompiledFilterHandle CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) override;
        void ReleaseFilter(Rml::CompiledFilterHandle filter) override;
        Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override;
        void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                          Rml::TextureHandle texture) override;
        void ReleaseShader(Rml::CompiledShaderHandle shader) override;

        // engine side, called by HtmlUIContext (HtmlUIContextRender.cpp)
        // Every UI program exists once all its shader variants are compiled; false while they
        // compile (ShaderPool::get may hand back a shader that is not created yet).
        bool loadPipelines();
        bool isReady() const;
        // A frame into the target's layers (target pixels; the context is contextSize document
        // px). False, with nothing drawn and RmlUi's calls ignored until endFrame, while the
        // programs are not ready or the target is not created.
        bool beginFrame(RmlRenderTarget& target, Rml::Vector2i contextSize);
        // ends the open pass (layer 0 holds the frame, resolved), then destroys what was
        // released during the frame; true when the frame was drawn
        bool endFrame();
        // the document target as a quad inside the current (camera) pass: the quad spans clip
        // space (phase 0, flipped on Y for offscreen GL passes), or clipFromQuad maps its
        // corners (-1,1) top-left .. (1,-1) bottom-right (a world-space canvas)
        bool drawComposite(TextureRender& source, PipelineType compositePipeline, bool flipY);
        bool drawComposite(TextureRender& source, PipelineType compositePipeline, const Rml::Matrix4f& clipFromQuad);
        // A straight-alpha copy of layer 0 (HTMLUI_POST Unp) into dst, a 1-sample RGBA8 colour
        // framebuffer of any size (render-to-texture documents). Outside a frame
        bool drawUnpremultiplied(RmlRenderTarget& target, FramebufferRender& dst, int width, int height);
        // view lost / context destroyed: every GPU object, handles stay valid as "dead"
        void releaseGpuResources();
        // beginFrame ran in that frame (SystemRender::getFrameCount)
        bool hasFrame(uint64_t frame) const;

        // pure function, unit-tested: RmlUi scissor (document px, top-left origin) to a
        // CameraRender::applyScissor rect (target px, bottom-left origin)
        static Rect toTargetScissor(Rml::Rectanglei region, Rml::Vector2i contextSize,
                                    Rml::Vector2i targetSize, bool openGLFlipped);
        // the same in target px with a top-left origin (applyScissorTopLeft); empty when clipped away
        static Rml::Rectanglei toTargetRect(Rml::Rectanglei region, Rml::Vector2i contextSize, Rml::Vector2i targetSize);

        // GL3's blur parameters: downscale passes and the remaining sigma (max 10 passes,
        // single-pass sigma at most 3) and the 4 normalized weights of a 7-tap pass
        static void sigmaToParameters(float desiredSigma, int& passLevel, float& sigma);
        static void blurWeights(float sigma, float weights[4]);

        // For phase-5 effects (they draw with the same geometry layout and u_vs_htmluiParams):
        // projection x the SetTransform matrix (or projection alone)
        const Rml::Matrix4f& getCurrentTransform() const;
        // target px, top-left origin; invalid when no scissor is enabled
        Rml::Rectanglei getCurrentScissor() const;
        bool isClipMaskEnabled() const;
        uint8_t getStencilRef() const;          // the stencil value geometry is tested against
        Rml::Vector2f getPixelScale() const;    // target px per document px
        // geometry state in the current layer: blend, the stencil test of the clip mask
        PipelineState getGeometryState(BlendMode blend = BlendMode::PREMULTIPLIED) const;
        // the top layer as the target of the next draw (opens or keeps its pass)
        bool bindTopLayer();
        // a compiled geometry's buffers, uploaded on first use; false when it has no GPU data
        bool bindGeometry(Rml::CompiledGeometryHandle geometry, ObjectRender& program, uint32_t& indexCount);

        // statistics: this frame (reset by beginFrame; geometryLive/geometryBytes are current)
        const HtmlUIRenderStats& getStats() const;
        size_t getLiveGeometryCount() const;
        size_t getLiveTextureCount() const;
        size_t getLastFrameDrawCount() const;   // geometry and shader draws of the last frame
        // GPU buffers, textures and framebuffers this renderer created over its life (performance tests)
        uint64_t getGpuCreateCount() const;
        size_t getGeometryBufferCount() const;  // GPU buffers holding geometry (pages, dedicated, free)
        // RmlUi made textures before the view existed (a document laid out by a scene's script
        // in init: <img> sizes load their file): they have no GPU texture and draw nothing
        // until RmlUi makes them again (HtmlUIContext::render, Rml::ReleaseTextures). True once
        bool takeTexturesWithoutView();
        // the GPU texture of a handle (nullptr when unknown or dead)
        TextureRender* getTextureRender(Rml::TextureHandle texture) const;
        // sokol ids of every buffer and image the renderer owns right now
        void getGpuHandles(std::vector<uint32_t>& buffers, std::vector<uint32_t>& images) const;

    private:
        friend class RmlFilterContext;

        enum class TargetKind { NONE, LAYER, POSTPROCESS, OTHER };
        // DROPPED: beginFrame could not draw (shaders compiling, no target); RmlUi's calls
        // are ignored silently until endFrame
        enum class FrameState { NONE, DROPPED, ACTIVE };

        // u_fs_htmluiPost
        struct PostParams {
            float params[4] = {1.0f, 0.0f, 0.0f, 0.0f};
            float texCoordMinMax[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float weights[4] = {1.0f, 0.0f, 0.0f, 0.0f};
            float colorMatrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        };

        struct PostProgram {
            std::shared_ptr<ShaderRender> shader;
            ObjectRender object;
            int vsSlot = -1;
            int fsSlot = -1;
            std::pair<int, int> textureSlot{-1, -1};
            std::pair<int, int> maskSlot{-1, -1};
        };

        // a fullscreen pass of an HTMLUI_POST variant: source sampled through uvTransform
        // (scale.xy, offset.zw of the destination's 0..1 coordinates), into the destination's
        // viewport within scissor (destination pixels, top-left; invalid: all of it)
        struct PostDraw {
            int variant = 0;                    // HtmlUIPostVariant
            TextureRender* source = nullptr;
            TextureRender* mask = nullptr;
            float uvTransform[4] = {1.0f, 1.0f, 0.0f, 0.0f};
            Rml::Rectanglei viewport = Rml::Rectanglei::MakeInvalid();
            Rml::Rectanglei scissor = Rml::Rectanglei::MakeInvalid();
            PostParams params;
        };

        // targets and passes
        void endPass();
        bool beginPass(Framebuffer& framebuffer, TargetKind kind, int index, LoadAction colorLoad, LoadAction depthStencilLoad);
        bool bindLayer(int index);
        // any target: a layer binds as a layer; another target keeps the open pass for LOAD
        bool bindTarget(Framebuffer& framebuffer, LoadAction load);
        TargetKind kindOf(const Framebuffer& framebuffer, int& index) const;
        void ensureLayerCleared(int index);
        void applyPassViewport(Rml::Rectanglei rect);
        void applyPassScissor(Rml::Rectanglei rect);
        Rml::Rectanglei currentScissorTarget() const;
        bool drawPost(Framebuffer& dst, LoadAction load, const PostDraw& draw, const PipelineState& state);
        void drawFullscreenProgram(ObjectRender& program, const PipelineState& state, const std::function<void(ObjectRender&)>& applyUniforms);
        PipelineState layerState(BlendMode blend, ColorWriteMask writeMask, const StencilState& stencil) const;
        PipelineState postTargetState(BlendMode blend) const;
        void stencilClear(uint8_t value);
        bool drawGeometry(RmlGeometry* geometry, Rml::Vector2f translation, HtmlUITexture* texture, const PipelineState& state);
        void blurInto(Framebuffer& source, Framebuffer& destination, Framebuffer& temp, float sigma, Rml::Rectanglei window);
        void applyFilter(RmlFilterContext& context, const RmlCompiledFilter& filter);
        Framebuffer* postTarget(int index);

        // geometry buffers
        // Geometry storage. A geometry first drawn in a frame is appended to that frame's
        // transient buffers; at the end of the frame it moves into persistent storage, where
        // it stays without further uploads: small geometries share pages (one GPU buffer each,
        // uploaded whole at most once a frame), large ones get a dedicated buffer of a
        // power-of-two size class (at least 4 KiB)
        bool appendTransient(const void* data, size_t bytes, bool index, BufferRender*& buffer, size_t& offset);
        void persistGeometry();
        bool allocateRange(const void* data, size_t bytes, bool index, RmlGeometryPage*& page, size_t& offset);
        void releaseRange(RmlGeometryPage* page, size_t bytes);
        void destroyGeometryBuffers();

        // textures
        HtmlUITexture* createStaticTexture(const uint8_t* rgba, int width, int height, const char* label);
        HtmlUITexture* toTexture(Rml::TextureHandle handle);
        void destroyTexture(HtmlUITexture* texture);
        void flushPendingReleases();

        bool loadPrograms();
        void destroyPipelineObjects();
        void abortFrame();

        std::unordered_set<RmlGeometry*> liveGeometry;
        std::unordered_set<HtmlUITexture*> liveTextures;
        std::vector<HtmlUITexture*> pendingReleases;
        std::vector<std::unique_ptr<RmlGeometryPage>> pages;
        std::vector<std::unique_ptr<RmlTransientBuffer>> transientBuffers;
        std::vector<RmlGeometry*> pendingGeometry;   // drawn from transient buffers this frame

        // frame state
        RmlRenderTarget* target = nullptr;
        FrameState frameState = FrameState::NONE;
        bool loggedOutsideFrame = false;
        bool framed = false;
        uint64_t frameNumber = 0;
        Rml::Vector2i contextSize{0, 0};
        Rml::Vector2i targetSize{0, 0};
        Rml::Matrix4f projection;
        Rml::Matrix4f transform;
        bool scissorEnabled = false;
        Rml::Rectanglei scissorRegion;
        bool clipMaskEnabled = false;
        uint8_t stencilRef = 0;
        int layersSize = 0;
        std::vector<bool> layerClearPending;

        // the open pass
        CameraRender pass;
        TargetKind activeKind = TargetKind::NONE;
        int activeIndex = -1;
        Framebuffer* activeFramebuffer = nullptr;
        Rml::Rectanglei appliedViewport;
        Rml::Rectanglei appliedScissor;

        HtmlUIRenderStats stats;
        uint64_t frameMissesStart = 0;
        uint64_t frameAppliesStart = 0;
        size_t lastFrameDrawCount = 0;
        uint64_t gpuCreateCount = 0;
        bool texturesWithoutView = false;

        // GPU objects
        std::shared_ptr<ShaderRender> geometryShader;     // HTMLUI Tex (untextured draws bind white)
        std::shared_ptr<ShaderRender> gradientShader;     // HTMLUI_GRADIENT
        bool pipelinesLoaded = false;
        ObjectRender geometryObject;
        ObjectRender gradientObject;
        int geometryParamsSlot = -1;
        std::pair<int, int> geometryTextureSlot{-1, -1};
        int gradientParamsSlot = -1;
        int gradientFsSlot = -1;
        PostProgram postPrograms[6];
        // the composite quad, also the layout template of the dynamic geometry objects
        BufferRender compositeVertexBuffer;
        BufferRender compositeIndexBuffer;
        ObjectRender compositeObject;
        TextureRender placeholderWhite;
    };

}

#endif //RMLRENDERER_H
