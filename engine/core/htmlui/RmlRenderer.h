// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RMLRENDERER_H
#define RMLRENDERER_H

#include "Export.h"

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Matrix4.h>

#include "HtmlUIPrimitive.h"
#include "HtmlUIRenderStats.h"
#include "HtmlUITexture.h"
#include "math/Rect.h"
#include "render/BufferRender.h"
#include "render/CameraRender.h"
#include "render/ObjectRender.h"
#include "render/PipelineState.h"
#include "render/Render.h"
#include "render/TextureRender.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jasperengine {

    class Framebuffer;
    class FramebufferRender;
    class ShaderRender;
    class RmlFilterContext;

    namespace htmlui {
        class EffectRenderer;
    }

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
    //
    // Region layers (fork patch P7-F2): a filter's layer whose window extends beyond the viewport
    // (RmlUi's RenderInterface::PushLayerRegion) covers that region of the target plane, the
    // viewport's pixel grid continued beyond its edges: a pooled RGBA8 + DEPTH_STENCIL framebuffer
    // of the region's size (its own depth-stencil, same sample count), and the filter chain's four
    // post-process targets in a set of that size, shared by the regions of that size. Sizes are
    // rounded up to 64 px so a moving region reuses them; an entry not used for kRegionKeepFrames
    // frames, or beyond kRegionKeepFree free ones, is destroyed at the end of a frame.
    class JASPERENGINE_INTERNAL RmlRenderTarget {
    public:
        static constexpr int POSTPROCESS_COUNT = 4;
        static constexpr int BLEND_MASK = 3;
        static constexpr uint64_t kRegionKeepFrames = 120;
        static constexpr size_t kRegionKeepFree = 4;

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
        // changes whenever one of its framebuffers is made (unique across targets): what a
        // renderer knows about their contents holds while it is the same
        uint64_t getGeneration() const;

        // region layers and post-process sets held (in use or kept for reuse), and their bytes
        // (colour, depth-stencil and post-process images, all samples)
        size_t getRegionLayerCount() const;
        size_t getRegionPostSetCount() const;
        size_t getRegionBytes() const;

    private:
        friend class RmlRenderer;
        friend class RmlFilterContext;

        struct RegionLayer {
            std::unique_ptr<Framebuffer> framebuffer;
            int width = 0;
            int height = 0;
            bool inUse = false;
            uint64_t lastUsed = 0;
        };
        struct RegionPost {
            int width = 0;
            int height = 0;
            std::unique_ptr<Framebuffer> targets[POSTPROCESS_COUNT];
            uint64_t lastUsed = 0;
        };

        Framebuffer* ensureLayer(int index);
        Framebuffer* ensurePostprocess(int index);
        // a free region layer of that size, made when there is none (created: it was); -1 when it
        // cannot be made. Released when its layer is popped
        int acquireRegionLayer(int width, int height, bool& created);
        void releaseRegionLayer(int pool);
        Framebuffer* regionLayer(int pool) const;
        // the post-process set of that size (made empty when there is none), a target of it
        RegionPost* regionPost(int width, int height);
        Framebuffer* ensureRegionPost(RegionPost& set, int index, bool& created);
        // end of frame: what was not used for a while, and free entries beyond the kept count, go
        void trimRegions();
        // A framebuffer's GPU objects are checked once per frame (Framebuffer::isCreated asks
        // sokol for every attachment view: hundreds of lookups a frame with layers and filters,
        // on memory the driver has just evicted); they can only go between frames (the view
        // reloads) or through destroy()
        void newFrame();
        bool checkedThisFrame(const Framebuffer& framebuffer) const;

        std::vector<std::unique_ptr<Framebuffer>> layers;
        std::unique_ptr<Framebuffer> postprocess[POSTPROCESS_COUNT];
        std::vector<std::unique_ptr<RegionLayer>> regionLayers;
        std::vector<std::unique_ptr<RegionPost>> regionPosts;
        std::vector<uint64_t> layerChecked;              // the frame each layer was last found created
        uint64_t postprocessChecked[POSTPROCESS_COUNT] = {};
        uint64_t frame = 1;
        int width = 0;
        int height = 0;
        int sampleCount = 1;
        uint64_t createCount = 0;
        uint64_t generation = 0;
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
        size_t declaredStops = 0;                   // the stops given (only MAX_STOPS of them drawn)
        int stopRow = -1;                           // its row of the renderer's stop table (S92 R7), -1: none
    };

    // RmlUi's render interface on the engine render abstraction, with every optional RmlUi 6.3
    // feature (port of RmlUi's GL3 renderer onto sokol passes): stencil clip masks, layers,
    // SaveLayerAsTexture/SaveLayerAsMaskImage, filters (filter and backdrop-filter), gradients.
    // One per HtmlUIContext. Between beginFrame and endFrame it records RmlUi's calls into a
    // command list (phase-1 spec 3.2.10), which endFrame replays into the context's
    // RmlRenderTarget, opening a sokol pass only when it needs a different target, so a frame
    // without layers or filters is one pass.
    class JASPERENGINE_INTERNAL RmlRenderer : public Rml::RenderInterface {
    public:
        RmlRenderer();
        ~RmlRenderer() override;                    // releaseGpuResources()

        // Rml::RenderInterface, required
        Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
        void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
        void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
        // phase-2 hook H7: "@htmlui/..." sources are compat's virtual files; the query parameter
        // htmlui-filter=nearest gives the texture nearest sampling (image-rendering: pixelated)
        Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
        // the source without its htmlui-filter=nearest query parameter (other parameters stay)
        static std::string stripTextureFilter(const std::string& source, bool& nearest);
        // also without the fork's rmlui-wrap=repeat|repeat-x|repeat-y (phase 3, p3-paint: a background or mask image
        // repeated along those axes, sampled with repeat wrap there; CONTRACTS)
        static std::string stripTextureFilter(const std::string& source, bool& nearest, bool& repeatX, bool& repeatY);
        bool SupportsTextureWrapHint() override { return true; }
        Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
        // a generated texture sampled with repeat wrap (an SVG rasterized at a background tile's size, repeated)
        Rml::TextureHandle GenerateTextureWithWrap(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions, bool repeatX,
                                                   bool repeatY) override;
        // deletes the HtmlUITexture behind any handle (also those handed to RmlUi through
        // CallbackTextureInterface::SetTextureHandle), after the frame when one is in progress
        void ReleaseTexture(Rml::TextureHandle texture) override;
        void EnableScissorRegion(bool enable) override;
        void SetScissorRegion(Rml::Rectanglei region) override;

        // Rml::RenderInterface, optional
        void EnableClipMask(bool enable) override;
        void RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation) override;
        // fork S16-F2: the mask's shape, for stencil elision and the analytic clip of the batched replay
        void RenderToClipMaskShape(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                                   const Rml::ClipShape& shape) override;
        void SetTransform(const Rml::Matrix4f* transform) override;
        Rml::LayerHandle PushLayer() override;
        // fork P7-F2: a layer covering region (document px) beyond the viewport, a region layer of
        // RmlRenderTarget (an ordinary layer when it covers no more than the viewport: region is
        // then invalid). The region in target px is cropped to regionLimit() on each axis, keeping
        // its part inside the layer it is pushed on (where its composite shows) and as much of the
        // rest as fits around it, and rounded up to 64 px; region returns what it covers
        Rml::LayerHandle PushLayerRegion(Rml::Rectanglei& region) override;
        void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend_mode,
                             Rml::Span<const Rml::CompiledFilterHandle> filters) override;
        void PopLayer() override;
        Rml::TextureHandle SaveLayerAsTexture() override;
        // the scissor region of the top layer into a texture of any size (fork: a box shadow larger than the window,
        // rendered in tiles); a zero texture makes a cleared one of textureDimensions first. Dimensions and offset
        // are in document px; the texture holds target pixels (scaled by the target-to-context ratio)
        Rml::TextureHandle SaveLayerIntoTexture(Rml::TextureHandle texture, Rml::Vector2i textureDimensions,
                                                Rml::Vector2i destinationOffset) override;
        Rml::CompiledFilterHandle SaveLayerAsMaskImage() override;
        Rml::CompiledFilterHandle CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) override;
        void ReleaseFilter(Rml::CompiledFilterHandle filter) override;
        Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override;
        void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                          Rml::TextureHandle texture) override;
        void ReleaseShader(Rml::CompiledShaderHandle shader) override;
        // fork S16-F1: solid backgrounds, borders, outlines and box shadows as HTMLUI_PRIM instances
        // (setSdfPrimitivesForTesting, the project's htmlUiSdfPrimitives)
        bool SupportsPrimitives() override;
        // fork S16-F4: elements skip what is wholly outside the scissor (with the batched replay, D1)
        bool CullsOffscreenPaint() override;
        // fork S16-F5: linear and radial gradients of at most four stops are HTMLUI_PRIM instances
        bool SupportsGradientPrimitive(Rml::CompiledShaderHandle shader) override;
        void RenderPrimitives(Rml::Span<const Rml::BoxPrimitive> primitives, Rml::Vector2f translation) override;

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
        // corners (-1,1) top-left .. (1,-1) bottom-right (a world-space canvas).
        // compositePipeline is PIP_DEFAULT_NODEPTH (swapchain) or PIP_RTT_NODEPTH (offscreen),
        // or PIP_MSAA_NODEPTH for the post-processed camera's multisampled scene target
        // (shader library phase 4 MSAA) of msaaSamples samples, drawn through the pipeline
        // cache since its sample count is the scene's. S92 R4: for the last frame's layer 0, only
        // the 64x64 tiles it wrote, in one draw (false, nothing drawn, when it wrote none)
        bool drawComposite(TextureRender& source, PipelineType compositePipeline, bool flipY);
        bool drawComposite(TextureRender& source, PipelineType compositePipeline, const Rml::Matrix4f& clipFromQuad,
                           int msaaSamples = 1);
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
        // the same not clamped to the target (the target plane beyond it: region layers); empty
        // when invalid
        static Rml::Rectanglei toTargetPixels(Rml::Rectanglei region, Rml::Vector2i contextSize, Rml::Vector2i targetSize);
        // P7-F2, pure: the target px a region layer for 'pixels' covers, pushed on a layer covering
        // 'parent': on each axis at most limit px, keeping pixels' part within parent (where the
        // layer's composite can show) and as much of the rest as fits, evenly on both sides; the
        // size then rounded up to a multiple of 64 px (not beyond limit)
        static Rml::Rectanglei regionFrame(Rml::Rectanglei pixels, Rml::Rectanglei parent, Rml::Vector2i limit);
        // the largest region layer side: the device's largest texture, and twice the target's side
        // (at least the target's side plus 512 px), which bounds a region layer's memory to 4x the
        // target's (P7-F2, docs/html-ui/phase-1-integration.md)
        static Rml::Vector2i regionLimit(Rml::Vector2i targetSize, int maxTextureSize);

        // GL3's blur parameters: downscale passes and the remaining sigma (max 10 passes,
        // single-pass sigma at most 3) and the 4 normalized weights of a 7-tap pass
        static void sigmaToParameters(float desiredSigma, int& passLevel, float& sigma);
        static void blurWeights(float sigma, float weights[4]);

        // tests: false draws every backdrop through its temporary layer, as RmlUi's GL3 renderer
        // (DeferredComposite); the images are the same
        static void setCompositeFusionForTesting(bool enabled);
        // tests: false draws every call at once instead of recording the frame and replaying it
        // in endFrame (phase-1 spec 3.2.10); the images are the same. Read by beginFrame
        static void setCommandRecordingForTesting(bool enabled);
        static bool isCommandRecording();
        // the commands the last frame recorded (0 when it drew immediately)
        size_t getLastFrameCommandCount() const;
        // tests (S16 D1): false replays every recorded call as its own draw, with every clip mask
        // drawn into the stencil; true (the default) merges geometry into runs, keeps a run cache,
        // elides clip masks and clips rounded rects analytically (RmlRendererBatch.cpp). Read by
        // beginFrame; immediate drawing is never batched
        static void setBatchingForTesting(bool enabled);
        static bool isBatching();
        // S16 D1: false sends no primitives (RmlUi generates meshes and shadow textures) and draws no
        // quad list as instances; read by RmlUi when it generates an element's geometry (a change
        // applies to geometry generated after it). The project's htmlUiSdfPrimitives
        static void setSdfPrimitivesForTesting(bool enabled);
        static bool isSdfPrimitives();
        // pure function, unit-tested (R10): whether a mesh is a list of RmlUi quads (GenerateQuad's
        // index order, parallelograms within 1e-4 px, one colour per quad, uvs affine with the
        // positions), and their HTMLUI_PRIM instances (QUAD, no translation)
        static bool classifyQuadList(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices,
                                     std::vector<HtmlUIPrimitive>* instances);
        // pure function, unit-tested: a box primitive as an HTMLUI_PRIM instance at the translation
        static HtmlUIPrimitive toInstance(const Rml::BoxPrimitive& primitive, Rml::Vector2f translation);

        // For phase-5 effects (they draw with the same geometry layout and u_vs_htmluiParams):
        // projection x the SetTransform matrix (or projection alone): document px to the clip
        // space of the viewport
        const Rml::Matrix4f& getCurrentTransform() const;
        // what draws into the top layer use: the clip space of the part of the target plane it
        // covers (getCurrentTransform for an ordinary layer; P7-F2)
        Rml::Matrix4f getDrawTransform() const;
        // the top layer's part of the target plane (target px: the viewport's FromSize(target
        // size), or a region layer's beyond it), and in document px
        Rml::Rectanglei getTopLayerFrame() const;
        Rml::Rectanglef getTopLayerDocumentRect() const;
        // target px of the top layer, top-left origin; invalid when no scissor is enabled
        Rml::Rectanglei getCurrentScissor() const;
        bool isClipMaskEnabled() const;
        uint8_t getStencilRef() const;          // the stencil value geometry is tested against
        Rml::Vector2f getPixelScale() const;    // target px per document px
        // geometry state in the current layer: blend, the stencil test of the clip mask
        PipelineState getGeometryState(BlendMode blend = BlendMode::PREMULTIPLIED) const;
        // the top layer as the target of the next draw (opens or keeps its pass)
        bool bindTopLayer();
        // tests (S92 R12): each draw tested against the clip mask from now on (nullptr: none), with
        // the depth-stencil it tests (nullptr: layer 0's, which the ordinary layers share; else the
        // region layer's framebuffer), the mask version that depth-stencil holds and the current
        // one. Every draw that tests the stencil reports itself here before it is drawn
        struct StencilAuditEntry {
            const void* attachment = nullptr;
            uint64_t attachmentVersion = 0;
            uint64_t maskVersion = 0;
        };
        void setStencilAuditForTesting(std::vector<StencilAuditEntry>* audit);
        void auditStencilDraw(const PipelineState& state);
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

        // S16: glyph atlas pages (RmlFontEngine): an R8 coverage page with a white block at
        // (whiteX, whiteY) (4x4 texels of full coverage, < 0: none), or an RGBA8 MTSDF page of
        // distanceRange texels per side, or RGBA8 premultiplied; drawFrame receives the frame of
        // its last draw. A variant draws a distance page's image with a text effect (effect,
        // dilation, sigma: HtmlUITexture::textEffect) and owns nothing; released like any texture
        struct AtlasTexture {
            HtmlUITexture::Sample sample = HtmlUITexture::Sample::RGBA;
            int whiteX = -1, whiteY = -1;
            float distanceRange = 0.0f;
            bool nearest = false;           // bitmap faces' pages (S51 R15): magnified as blocks
            std::shared_ptr<uint64_t> drawFrame;
            // the page's texels and their generation: uploaded again (in place) when it changes
            std::shared_ptr<const GlyphAtlas> atlas;
            std::shared_ptr<const uint64_t> generation;
        };
        Rml::TextureHandle createAtlasTexture(int width, int height, const AtlasTexture& desc);
        Rml::TextureHandle createTextureVariant(Rml::TextureHandle base, const float textEffect[3], std::shared_ptr<uint64_t> drawFrame);
        // the glyph atlas textures this renderer made (tests)
        std::vector<const HtmlUITexture*> getAtlasTextures() const;
        // the batched replay's gather (S16 R22, R23): CPU time of the last frame's gathering (the
        // runs planned, their vertices packed into the gather pages), the driver's time for the
        // pages' buffers (created, updated, released: a software driver waits there for the GPU
        // work it does on the CPU), the bytes its chunks hold, the capacity of the gather pages
        // (the stream and the run cache's ranges)
        double getLastGatherMs() const;
        double getLastUploadMs() const;
        // tests: the next frame gathers every run again (the pages stay)
        void clearRunCacheForTesting();
        size_t getGatherLiveBytes() const;
        size_t getGatherCapacityBytes() const;
        // why the last plan started each run (benchmarks): after a command that ends runs (layers,
        // composites, shaders, stencil masks, large geometry) or first, another kind (vertices or
        // instances), another clip or layer, transform, texture, or a scissor it cannot share
        enum class RunBreak : uint8_t { NONE, COMMAND, KIND, CLIP, TRANSFORM, TEXTURE, SCISSOR, COUNT };
        const std::array<uint32_t, (size_t)RunBreak::COUNT>& getLastRunBreaks() const { return runBreaks; }
        // the last batched frame's gradient and effect shader draws, layer composites and filter
        // passes asked for (benchmarks: what the draws that are not runs are)
        struct EffectCounts { uint32_t shaders = 0, composites = 0, filters = 0, clipMasks = 0; };
        EffectCounts getLastEffectCounts() const { return effectCounts; }
        // the font engine's per-frame text counters, added to this frame's statistics
        void addTextStats(uint32_t glyphsRasterized, uint32_t textRegenerations, uint32_t textEffectFallbacks, uint32_t atlasEvictions);
        // sokol ids of every buffer and image the renderer owns right now
        void getGpuHandles(std::vector<uint32_t>& buffers, std::vector<uint32_t>& images) const;

        // The renderer of an RmlUi context (HtmlUIContext and the tests' RenderTestContext bind
        // theirs at creation): elements that draw through it (<video>) find it here; nullptr for
        // a context on another render interface
        static RmlRenderer* forContext(const Rml::Context* context);
        static void bindContext(const Rml::Context* context, RmlRenderer* renderer);
        static void unbindContext(const Rml::Context* context);
        // the renderer of a bound context's render manager (the font engine's atlas textures)
        static RmlRenderer* forRenderManager(const Rml::RenderManager* renderManager);

        // <video> (phase 6): a frame's Y, U and V planes (target->planes, uploaded) are converted
        // into target, a 1-sample RGBA8 RENDER_TARGET texture, by one HTMLUI_POST Yuv pass;
        // colorMatrix is VideoColorConvert::matrix (column-major), chromaScale its chromaScale.
        // Queued (a newer job for the same target replaces it) and run by runVideoConversions,
        // which HtmlUIContext calls before beginFrame, outside any pass. A released target drops
        // its job.
        struct VideoConversion {
            HtmlUITexture* target = nullptr;
            float colorMatrix[16] = {};
            float chromaScale[2] = {1.0f, 1.0f};
        };
        void queueVideoConversion(const VideoConversion& conversion);
        void cancelVideoConversion(const HtmlUITexture* target);
        bool hasVideoConversion(const HtmlUITexture* target) const;
        // runs the queue (outside any pass, once the programs exist); returns the passes drawn
        int runVideoConversions();
        uint64_t getVideoConversionCount() const;   // passes run over the renderer's life

        // S92 frame reuse (RmlRendererFrame.cpp). Why a frame is drawn whatever it recorded (R2):
        // it reads something its signature cannot version (stats.liveSources)
        enum LiveSource : uint32_t {
            LIVE_VIDEO = 1u << 0,           // a <video>'s texture (HtmlUITexture::liveContent)
            LIVE_RENDER_TARGET = 1u << 1,   // a texture on an image the UI does not write (an engine render target)
            LIVE_EFFECT = 1u << 2,          // a phase-5 effect on a running clock, or a stateful one
            LIVE_PARTICLES = 1u << 3,       // a <particle-system> that drew particles
            LIVE_MSDF_PENDING = 1u << 4,    // distance glyphs asked for and not made yet
            LIVE_TEXT_SETTLE = 1u << 5,     // transform-aware text settling at its scale (RenderManager::RequestRender)
            LIVE_REQUESTED = 1u << 6,       // HtmlUIContext::requestRedraw (custom native content, settings)
        };
        // a live source of the frame being recorded; outside a recording, of the next frame
        void addLiveSource(uint32_t reasons);
        // The signature of the recorded commands [first, end) and of the state in effect at first
        // (transform, scissor, clip mask): geometry and textures by serial and content version,
        // compiled filters, shaders and effects by their parameters, everything else by value.
        // With an origin (target px), translations, scissors and region layers are taken relative
        // to it, unless a transform other than the identity is in effect (a layer scope's key: an
        // element that moves keeps it). live receives the live sources the commands read
        uint64_t signatureOf(size_t first, size_t end, Rml::Vector2i origin, uint32_t& live);
        // the commands recorded so far in this frame (a range's end)
        size_t getRecordedCommandCount() const { return commands.size(); }
        // R1: a recorded frame whose signature equals the last drawn frame's, with no live source,
        // into the same target, is not replayed (stats.framesKept 1, nothing drawn or uploaded):
        // the target keeps its image and its composite tiles. Tests: false draws every frame (not a
        // user setting: a stale frame is a bug to fix, S92 risk 1). Read by endFrame
        static void setFrameReuseForTesting(bool enabled);
        static bool isFrameReuse();
        bool wasLastFrameKept() const { return lastFrameKept; }
        // the signature's CPU time in the last recorded frame (R33)
        double getLastSignatureMs() const { return lastSignatureMs; }
        // R3 (tests, JASPER_HTMLUI_VERIFY_KEPT=1 or the setter): a kept frame is also replayed into a
        // scratch target, which is compared with the kept image byte for byte (on GL at once, on the
        // other backends once the frame is committed: the next frame, or checkKeptFrame). A difference
        // logs "[HTML UI] kept frame differs" and counts in stats.keptMismatches
        static void setKeptFrameVerificationForTesting(bool enabled);
        static bool isKeptFrameVerification();
        // a comparison still pending, done now when its frame was committed; false when none is pending
        bool checkKeptFrame();
        uint32_t getKeptMismatchCount() const { return keptMismatchCount; }
        // R5 (tests): OFF stores every depth-stencil, as before S92; PLANNED (the default) stores
        // one only when a later pass tests it; EVERY_PASS stores none, so every later stencil test
        // finds its mask dropped and draws it again (the fallback a wrong plan relies on). The
        // images are the same. Read by the replay
        enum class DepthStencilDiscard : uint8_t { OFF, PLANNED, EVERY_PASS };
        static void setDepthStencilDiscardForTesting(DepthStencilDiscard mode);
        // R4: the composite's draws over the renderer's life (one per drawComposite that drew)
        uint64_t getCompositeDrawCount() const { return compositeDraws; }

        // S92 layer cache (RmlRendererLayerCache.cpp, R9). A layer scope (a PUSH_LAYER to its
        // COMPOSITE) keeps its filtered result, without a trailing opacity, as an image of its
        // window, keyed by its signature relative to the window: a later frame recording the
        // same scope draws the image with the composite's blend, scissor, clip mask and opacity
        // (one draw, no pass, no filter). Scopes reading live sources, saved-layer textures or
        // layers outside them are not kept. Tests: false replays every scope (read by the replay)
        static void setLayerCacheForTesting(bool enabled);
        static bool isLayerCache();
        // the cache's budget per renderer, least recently used first: 8 MiB on Android, iOS and
        // the web, 32 MiB on desktop; tests set another (0: the platform's)
        static void setLayerCacheBudgetForTesting(size_t bytes);
        static size_t getLayerCacheBudget();
        static constexpr uint64_t kLayerCacheKeepFrames = 120;   // replays an unused entry is kept
        size_t getLayerCacheEntryCount() const;
        size_t getLayerCacheBytes() const;                         // entries and free images
        // R30 (tests): the last frame's layer and post-process passes, each by the 64-px granules
        // of the window of the layer it serves (what RmlUi asked the layer to cover; the whole
        // target for a layer pushed without one), and the largest such pass's render area
        // and how many were layer passes and post-process passes (the rest of stats.passes: layer 0's
        // and the cache's copies)
        struct LayerPassStats { uint64_t windowPixels = 0; uint32_t largestPass = 0; uint32_t layerPasses = 0; uint32_t postPasses = 0; };
        LayerPassStats getLastLayerPassStats() const { return layerPassStats; }

    private:
        friend class RmlFilterContext;
        // phase 5: the UI effects draw with the renderer's passes, geometry and state
        friend class htmlui::EffectRenderer;

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
            std::pair<int, int> chromaUSlot{-1, -1};    // Yuv (phase 6)
            std::pair<int, int> chromaVSlot{-1, -1};
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

        // What an MSAA layer holds, so its resolves cover only what they must, with the result of
        // full ones: content, the rect outside which every sample is transparent; stale, the rect
        // outside which its resolve image equals its samples. Invalid rects are empty; a layer not
        // seen since it was made is full/full.
        struct TargetRegions {
            Rml::Rectanglei content = Rml::Rectanglei::MakeInvalid();
            Rml::Rectanglei stale = Rml::Rectanglei::MakeInvalid();
        };
        TargetRegions& regionsOf(const Framebuffer& framebuffer);
        // a target's sample count without asking its framebuffer object (the layers have the
        // render target's, the post-process targets one)
        int samplesOf(Framebuffer& framebuffer, TargetKind kind) const;
        // the open pass wrote colour within its viewport and scissor
        void noteColorWrite();
        // the layer's resolve image is current (before it is read): ends its pass with the resolve,
        // or resolves it in a pass of its own
        void ensureResolved(Framebuffer& layer);

        // A backdrop's filter output not drawn into its temporary layer yet. RmlUi composites the
        // filtered backdrop into a pushed layer and that layer at once into the destination
        // (ElementEffects): the second composite draws the post target straight into the
        // destination (the layer would hold exactly it within the first scissor, transparent
        // elsewhere), saving the layer's pass and resolve. Anything else that binds a target or
        // reads the layer first draws it in (flushDeferredComposite); popping the layer drops it.
        struct DeferredComposite {
            int layer = -1;                  // the pushed layer; -1: none
            int post = -1;                   // the post target holding the filter output
            Rml::Rectanglei scissor;         // the first composite's (the pushed layer's px)
            PipelineState state;             // the first composite's (no clip mask)
            // S92 R10: the post target (of the source layer's frame, a region layer's set too) and
            // the frames it and the pushed layer cover (target px)
            Framebuffer* postImage = nullptr;
            Rml::Rectanglei postFrame, layerFrame;
        };
        // a post draw's source of frame `from` placed by the target plane into a target of frame `to`
        static void placeByFrames(PostDraw& draw, Rml::Rectanglei from, Rml::Rectanglei to);
        DeferredComposite deferred;
        bool fusingDeferred = false;
        void flushDeferredComposite();

        // P7-F2: what a pushed layer draws into. An ordinary layer covers the viewport (rect is
        // FromSize(targetSize), the image target->layers[index]); a region layer covers rect of the
        // target plane (target px, beyond the viewport) in a pooled framebuffer with its own
        // depth-stencil, which holds the clip mask of maskVersion (0: none)
        struct LayerFrame {
            Rml::Rectanglei rect;
            Framebuffer* framebuffer = nullptr;
            int pool = -1;
            uint64_t maskVersion = 0;
            Rml::Rectanglei window;           // S92 R30: what RmlUi asked it to cover (target px)
        };
        std::vector<LayerFrame> frames;               // the replay's, per layer stack index
        std::vector<Rml::Rectanglei> recordFrames;    // the recording's rects, per layer stack index
        Rml::Rectanglei filterFrame = Rml::Rectanglei::MakeInvalid();   // the frame of the layer being filtered (prepareFilter)
        Rml::Rectanglei standardFrame() const;
        bool isRegionLayer(int index) const;
        Rml::Rectanglei frameRectOf(int index) const;
        // the layer's image (made when needed), or the one already made (nullptr when none)
        Framebuffer* layerAt(int index);
        Framebuffer* currentLayerAt(int index) const;
        // the scissor region in the pixels of a layer covering frame (top-left origin, empty when
        // clipped away; all of it without a scissor)
        Rml::Rectanglei scissorIn(Rml::Rectanglei frame) const;
        Rml::Matrix4f transformIn(Rml::Rectanglei frame) const;
        // a matrix to the target's clip space (projection x ...) made one to the frame's (S16's
        // batched runs and primitives in a region layer); the matrix itself for the viewport's frame
        Rml::Matrix4f toFrame(Rml::Rectanglei frame, const Rml::Matrix4f& matrix) const;
        // the post-process targets of a layer covering frame: nullptr for the viewport's (the
        // render target's postprocess), else the region set of its size
        RmlRenderTarget::RegionPost* postSetFor(Rml::Rectanglei frame);
        Framebuffer* postTargetIn(RmlRenderTarget::RegionPost* set, int index);

        // P7-F2: the clip mask as RmlUi set it, since its last Set/SetInverse, so that a
        // depth-stencil that has not seen it gets it before a draw tests it (every ordinary layer
        // shares layer 0's; each region layer has its own). maskVersion counts the operations
        struct MaskOp {
            Rml::ClipMaskOperation operation;
            Rml::CompiledGeometryHandle geometry;
            Rml::Vector2f translation;
            Rml::Matrix4f transform;      // getCurrentTransform's, when it was drawn
            // S92 R5: the scissor it was drawn with (maskHistory replays it exactly), and the
            // depth-stencil it went into (nullptr: layer 0's shared one, else a region layer's)
            bool scissored = false;
            Rml::Rectanglei scissorPixels;
            const Framebuffer* attachment = nullptr;
        };
        std::vector<MaskOp> maskOps;
        // S92 R5: every stencil write into each depth-stencil since it was last cleared (layer 0's
        // shared one this frame, a region layer's since its push), in order, with its scissor: a
        // depth-stencil a DONTCARE store dropped is cleared and gets them all again, the stencil
        // it held outside the current mask's scissor included
        std::vector<MaskOp> maskHistory;
        std::vector<const Framebuffer*> replayDs;   // dropped and cleared since (nullptr: the shared one)
        // the mask version each dropped depth-stencil held when it was dropped
        std::vector<std::pair<const Framebuffer*, uint64_t>> droppedVersions;
        const Framebuffer* attachmentOf(int index) const;
        // a dropped and cleared depth-stencil gets what it held back (its writes replayed, its
        // version again), before anything tests or writes it; the layer is bound
        void restoreDroppedMask(int index);
        void drawMaskOp(const MaskOp& op);
        uint64_t maskVersion = 0;
        uint64_t sharedMaskVersion = 0;   // layer 0's depth-stencil's
        std::vector<StencilAuditEntry>* stencilAudit = nullptr;
        bool drawingMask = false;
        int drawIndexOverride = -1;       // ensureMask's layer, which the mask draws go to
        // the layer a geometry draw goes to: the top one (or ensureMask's)
        int drawLayerIndex() const;
        uint64_t& maskVersionOf(int index);
        // the layer's depth-stencil holds the current clip mask (drawn again into it when not)
        void ensureMask(int index);

        // targets and passes
        // resolve: an MSAA layer's pass ends with its resolve (stale region); else, on GL, the
        // resolve is left for when the layer is read (other backends resolve at every pass end)
        void endPass(bool resolve = false);
        bool beginPass(Framebuffer& framebuffer, TargetKind kind, int index, LoadAction colorLoad, LoadAction depthStencilLoad);
        bool bindLayer(int index);
        // the top layer, or for a stencil-only draw over a deferred composite the layer under it
        bool bindDrawLayer(bool stencilOnly);
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
        // size: the images' (the layer's frame)
        // first: the first pass's variant and parameters instead of a copy of the source (a drop shadow)
        void blurInto(Framebuffer& source, Framebuffer& destination, Framebuffer& temp, float sigma, Rml::Rectanglei window, Rml::Vector2i size,
                      const PostDraw* first = nullptr);
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
        HtmlUITexture* createStaticTexture(const uint8_t* rgba, int width, int height, const char* label,
                                           TextureFilter filter = TextureFilter::LINEAR,
                                           TextureWrap wrapU = TextureWrap::CLAMP_TO_EDGE, TextureWrap wrapV = TextureWrap::CLAMP_TO_EDGE,
                                           ColorFormat format = ColorFormat::RGBA);
        // a texture drawn in this frame (its drawFrame)
        void noteDrawn(HtmlUITexture* texture);
        // the atlas pages whose texels changed, uploaded (before the frame's passes)
        void updateAtlasTextures();
        void uploadAtlasTexture(HtmlUITexture& texture);
        std::vector<HtmlUITexture*> atlasTextures;
        HtmlUITexture* toTexture(Rml::TextureHandle handle);
        void destroyTexture(HtmlUITexture* texture);
        void flushPendingReleases();

        bool loadPrograms();
        bool loadPrimObject();
        void destroyPipelineObjects();
        void abortFrame();

        // the RenderInterface calls drawn at once (immediate drawing and the replay)
        void renderGeometryNow(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture);
        // the replay of RenderGeometry: the texture was checked when it was recorded
        void drawRecordedGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, HtmlUITexture* texture);
        void renderToClipMaskNow(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation);
        // frame: an ordinary layer when invalid, else a region layer covering it (target px);
        // window: what RmlUi asked it to cover (S92 R30; invalid: the frame)
        Rml::LayerHandle pushLayerNow(Rml::Rectanglei frame = Rml::Rectanglei::MakeInvalid(),
                                      Rml::Rectanglei window = Rml::Rectanglei::MakeInvalid());
        void popLayerNow();
        struct LayerCacheScope;
        // scope: a layer cache miss (S92 R9): the result without its trailing opacity is stored
        // and drawn from the cache, the opacity applied at the composite
        void compositeLayersNow(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blendMode,
                                Rml::Span<const Rml::CompiledFilterHandle> filters, const LayerCacheScope* scope = nullptr);
        void renderShaderNow(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation);
        void releaseGeometryNow(Rml::CompiledGeometryHandle geometry);
        void releaseFilterNow(Rml::CompiledFilterHandle filter);
        void releaseShaderNow(Rml::CompiledShaderHandle shader);
        // SaveLayerAs*/SaveLayerIntoTexture: the top layer, cleared and resolved to be read (nullptr
        // when it cannot be made); a 1-sample RGBA8 render-target texture (target px); the copy of
        // the scissor region of the top layer (bounds) into it, at offset when tiled
        Framebuffer* readableTopLayer();
        HtmlUITexture* createLayerTexture(int width, int height);
        void copyLayerInto(Framebuffer& layer, HtmlUITexture& texture, Rml::Rectanglei bounds, bool tiled, Rml::Vector2i offset, LoadAction load);
        void copyLayerIntoMask(Framebuffer& layer, Framebuffer& mask);

        // Command recording (phase-1 spec 3.2.10). One POD per call; what RmlUi passes by pointer
        // or span goes into the side pools. Everything is cleared, its capacity kept, every frame
        enum class CommandType : uint8_t {
            GEOMETRY, CLIP_MASK, SCISSOR_ENABLE, SCISSOR_REGION, CLIP_ENABLE, TRANSFORM,
            PUSH_LAYER, POP_LAYER, COMPOSITE, SAVE_TEXTURE, SAVE_MASK, SHADER,
            PRIMITIVES      // S16: i[0], i[1]: first and count in commandPrimitives; i[2], i[3]: their content hash
        };
        struct Command {
            CommandType type;
            uint8_t flag;           // enable, clip operation, blend mode, load action (SAVE_TEXTURE), tiled
            int32_t i[4];           // a rectangle; layers; a pool index and count; an offset
            float x, y;             // translation
            uintptr_t a, b;         // geometry, texture, shader, filter handles
        };
        static_assert(sizeof(Command) <= 48, "a command is 48 bytes");
        enum class ReleaseKind : uint8_t { GEOMETRY, SHADER, FILTER };
        struct DeferredRelease {
            ReleaseKind kind;
            uintptr_t handle;
        };
        // the data of a recorded geometry released before the replay, not yet in persistent storage
        struct KeptMesh {
            std::vector<Rml::Vertex> vertices;
            std::vector<int> indices;
        };
        Command& recordCommand(CommandType type);
        void replayCommands();
        // one recorded call drawn as it is (the unbatched replay, and the batched one's commands outside runs)
        void replayCommand(const Command& command);
        // the releases deferred while recording (after the replay, before persistGeometry), and the
        // kept meshes; also drops the list (a frame aborted)
        void finishRecording();

        // S16: the batched replay (RmlRendererBatch.cpp). The frame is planned first (clip scopes,
        // then runs of geometry merged by state, gathered into pages or found in the run cache), the
        // new gathered data uploaded once, then the plan is drawn
        struct ClipShapeRecord {
            Rml::Rectanglef rect;            // relative to the mask's translation
            Rml::Vector2f radii[4];          // top-left, top-right, bottom-right, bottom-left
            // S92: a convex polygon (counter-clockwise, relative to the translation) the rect bounds; 0: none
            int polygonPoints = 0;
            Rml::Vector2f polygon[Rml::ClipShape::MaxPolygonPoints];
        };
        // u_fs_htmluiClip (includes/htmlui_sdf.glsl)
        struct ClipParams {
            float rect[2][4] = {};           // x, y, width, height (document px, before the element transform)
            float radiiX[2][4] = {};
            float radiiY[2][4] = {};
            float params[4] = {};            // x: shapes, y: texture kind (0 RGBA, 1 R8 coverage, 2 MTSDF),
                                             // z: the shape that is a polygon + 1 (0: none), w: its points
            float text[4] = {};              // MTSDF: effect, dilation, blur sigma (document px), distance range (texels)
            float polygon[Rml::ClipShape::MaxPolygonPoints / 2][4] = {};   // S92: the polygon's points, two a row
        };
        enum class ScopeMode : uint8_t { STENCIL, ELIDED, ANALYTIC };
        // the commands under one clip mask list: from its CLIP_ENABLE to the next one
        struct ClipScope {
            ScopeMode mode = ScopeMode::STENCIL;
            uint32_t masks = 0;              // the mask draws of its list
            Rml::Rectanglei scissor;         // ELIDED: target px
            ClipParams clip;                 // ANALYTIC
            // ANALYTIC with masks that are all rectangles on the target's axes: their intersection
            // (target px), which clips a layer composite in its shader (S92, U27: no stencil)
            bool compositeClip = false;
            Rml::Rectanglef compositeRect;
        };
        enum class RunKind : uint8_t { VERTICES, INSTANCES };
        // a run cut into chunks of bounded size, each found in the run cache or gathered on its own
        struct BatchChunk {
            uint32_t run = 0;
            uint32_t first = 0, members = 0;     // in batchMembers
            uint32_t vertices = 0, indices = 0;  // gathered sizes (VERTICES: vertices and indices; INSTANCES: instances)
            uint32_t bytes = 0;
            uint64_t hash = 0;
            bool hit = false;                    // found in the run cache
            uint32_t page = 0;
            uint32_t dataFirst = 0, dataCount = 0;   // vertices or instances in the page
            uint32_t firstIndex = 0, count = 0;      // VERTICES: indices in the page (relative to its first vertex)
        };
        struct BatchRun {
            RunKind kind = RunKind::VERTICES;
            int layer = 0;
            bool stencil = false;
            uint8_t stencilRef = 0;
            int scope = -1;                  // its ANALYTIC clip scope, -1: none
            uint32_t transformId = 0;
            Rml::Matrix4f transform;         // projection x the element transform
            HtmlUITexture* texture = nullptr;   // VERTICES
            bool allUntextured = true;
            // INSTANCES: the textures of its textured members, a slot each (HTMLUI_PRIM binds four)
            std::array<HtmlUITexture*, 4> slots{};
            uint8_t slotCount = 0;
            bool scissorSet = false;         // a member's scissor clips it: the run draws with it
            Rml::Rectanglei scissor;
            Rml::Rectanglef freeBounds;      // target px: the members drawn without their scissor
            bool freeBoundsValid = false;
            uint32_t firstChunk = 0, chunkCount = 0;
            uint32_t members = 0;
            uint32_t firstMember = 0;        // in batchMembers
        };
        // a geometry (its vertices, or its quad list's instances), or a PRIMITIVES command's instances
        struct BatchMember {
            RmlGeometry* geometry = nullptr;     // nullptr: primitives
            uint64_t generation = 0;             // the geometry's, or the primitives' content hash
            uint32_t first = 0, count = 0;       // primitives: in commandPrimitives (first is not compared); instances
            Rml::Vector2f translation;
            HtmlUITexture* texture = nullptr;
            bool operator==(const BatchMember& other) const{
                return geometry == other.geometry && generation == other.generation && count == other.count &&
                       translation == other.translation && texture == other.texture;
            }
        };
        struct CachedChunk {
            RunKind kind = RunKind::VERTICES;
            HtmlUITexture* runTexture = nullptr;
            std::array<HtmlUITexture*, 4> runSlots{};   // INSTANCES: the slots its instances name
            uint32_t page = 0, first = 0, count = 0, vertexFirst = 0, vertexCount = 0;
            std::vector<BatchMember> members;
        };
        // gathered data: vertices (20-byte Rml::Vertex, translation baked) with 32-bit indices
        // relative to the page's start, or instances; uploaded whole, at most once a frame, when it
        // gets new chunks (a page holding chunks found in the cache is not written)
        struct GatherPage {
            RunKind kind = RunKind::VERTICES;
            BufferRender data;               // vertices or instances
            BufferRender indices;            // VERTICES
            size_t dataCapacity = 0, indexCapacity = 0;
            std::vector<uint8_t> dataShadow, indexShadow;
            size_t dataTop = 0, indexTop = 0;
            size_t liveBytes = 0;            // of chunks drawn this frame
            uint64_t updateFrame = UINT64_MAX;
            bool writable = false;           // no chunk of it found in the cache this frame: it starts over
            bool written = false;            // got new chunks this frame
        };
        struct ReplayOp {
            enum class Kind : uint8_t { COMMAND, RUN } kind;
            uint32_t index;                  // command or run
            int32_t scope;                   // CLIP_ENABLE: the scope it starts (-1: disabled)
        };
        void replayBatched();
        // the unbatched replay with clip scopes (analytic clips, no elision)
        void replayScoped();
        // a CLIP_ENABLE of the replay: the scope it starts (-1: disabled) with its stencil, scissor or analytic clip
        void applyClipEnable(const Command& command, int32_t scope);
        // the clip scopes of the frame: elided (batched replay) or analytic (SDF primitives) where they can be
        void planClipScopes();
        void planRuns();
        void uploadGathered();
        void executePlan();
        void drawRun(const BatchRun& run);
        void destroyGatherPages();
        Rml::Rectanglef geometryBounds(RmlGeometry& geometry);
        // a quad list's instances, classified on first use (sdf primitives on)
        bool isQuadList(RmlGeometry& geometry);
        // what count PRIMITIVES instances from first in commandPrimitives can touch (document px, no translation)
        Rml::Rectanglef primitiveBounds(uint32_t first, uint32_t count) const;
        Rml::Rectanglei effectiveScissorTarget() const;
        // the clip of the current scope and the texture's kind (nullptr: untextured)
        const ClipParams& currentClipParams(const HtmlUITexture* texture);
        // HTMLUI_PRIM instances (translation baked) drawn at once from a transient buffer: the
        // primitives of the unbatched replay and of immediate drawing
        void drawPrimitivesNow(const HtmlUIPrimitive* primitives, size_t count, Rml::Vector2f translation);
        // draws count instances from buffer at byteOffset with the current state (slots: the run's
        // textures, nullptr where unused) into a layer covering `frame` of the target plane: the
        // transform is to the target's clip space, the scissor in the frame's px (P7-F2)
        bool drawInstances(BufferRender& buffer, size_t byteOffset, uint32_t count, const std::array<HtmlUITexture*, 4>& slots,
                           const Rml::Matrix4f& transform, const ClipParams& clipParams, const PipelineState& state, Rml::Rectanglei scissor,
                           Rml::Rectanglei frame);

        std::vector<ClipShapeRecord> commandClipShapes;   // RenderToClipMaskShape's (CLIP_MASK i[0] - 1)
        std::vector<HtmlUIPrimitive> commandPrimitives;   // RenderPrimitives' (PRIMITIVES), without their translation
        std::vector<HtmlUIPrimitive> primitiveScratch;    // drawPrimitivesNow's translated copy
        std::vector<ClipScope> clipScopes;
        std::vector<int32_t> scopeOfCommand;              // CLIP_ENABLE true: its scope; else -1
        std::vector<BatchRun> batchRuns;
        std::vector<BatchChunk> batchChunks;
        std::vector<BatchMember> batchMembers;
        std::vector<ReplayOp> replayOps;
        std::vector<uint32_t> pendingChunks;              // chunks to gather after planning
        std::unordered_map<uint64_t, CachedChunk> runCache, nextRunCache;
        std::vector<std::unique_ptr<GatherPage>> gatherPages;
        std::array<uint32_t, (size_t)RunBreak::COUNT> runBreaks{};   // the last plan's new runs, by why
        EffectCounts effectCounts;
        int activeClipScope = -1;            // the replay's current non-stencil scope (-1: none)
        ClipParams noClip;
        ClipParams clipScratch;
        bool batching = false;               // this frame replays batched
        double lastGatherMs = 0.0;
        double lastUploadMs = 0.0;
        bool primitivesFrame = false;        // this frame draws SDF primitives and analytic clips (isSdfPrimitives)
        bool transformSet = false;           // RmlUi's current transform is not the identity (SetTransform)
        // a glyph atlas draw's translation on whole target pixels: the font engine places glyphs on
        // whole target pixels of the string's origin (S16 R14, a fractional pixel ratio)
        Rml::Vector2f snapTextTranslation(Rml::Vector2f translation, const HtmlUITexture* texture) const;

        bool recording = false;
        int recordLayers = 0;              // the layer depth at record time (PushLayer's handle)
        Rml::Rectanglei frameStartScissor; // the scissor region beginFrame left
        std::vector<Command> commands;
        std::vector<Rml::Matrix4f> commandMatrices;
        std::vector<Rml::CompiledFilterHandle> commandFilters;
        std::vector<Rml::Rectanglei> commandRects;     // SetScissorRegion's (document px)
        std::vector<DeferredRelease> deferredReleases;
        std::vector<KeptMesh> keptMeshes;
        size_t keptMeshesUsed = 0;
        size_t lastFrameCommandCount = 0;

        std::unordered_set<RmlGeometry*> liveGeometry;
        std::vector<RmlGeometry*> releasedGeometry;   // released, kept for reuse (RmlGeometry::alive)
        std::unordered_set<HtmlUITexture*> liveTextures;
        std::vector<VideoConversion> videoConversions;
        uint64_t videoConversionCount = 0;
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
        Rml::Rectanglei scissorPixels;      // scissorRegion in target px, not clamped (toTargetPixels), set with it
        bool clipMaskEnabled = false;
        uint8_t stencilRef = 0;
        int layersSize = 0;
        std::vector<bool> layerClearPending;
        // the shared depth-stencil (the clip masks of every layer) not cleared yet this frame: the
        // frame's first layer pass clears it, whichever layer that is (a filtered element's pushed
        // layer can come before layer 0, with the clip mask the composite into layer 0 tests)
        bool depthStencilClearPending = false;

        // the open pass
        CameraRender pass;
        TargetKind activeKind = TargetKind::NONE;
        int activeIndex = -1;
        Framebuffer* activeFramebuffer = nullptr;
        Rml::Rectanglei appliedViewport;
        Rml::Rectanglei appliedScissor;

        std::unordered_map<const Framebuffer*, TargetRegions> regions;
        struct RegionsCacheEntry {
            const Framebuffer* framebuffer = nullptr;
            TargetRegions* regions = nullptr;
        };
        RegionsCacheEntry regionsCache[2];   // the last two regionsOf answers (cleared with regions)
        int activeSamples = 1;               // the open pass's framebuffer's sample count
        Rml::Rectanglei activeFull;          // and its full rectangle
        TargetRegions* activeRegions = nullptr;   // the open pass's, when it is an MSAA layer
        uint64_t regionsGeneration = 0;

        HtmlUIRenderStats stats;
        uint64_t frameMissesStart = 0;
        uint64_t frameAppliesStart = 0;
        size_t lastFrameDrawCount = 0;
        uint64_t gpuCreateCount = 0;
        bool texturesWithoutView = false;

        // GPU objects
        std::shared_ptr<ShaderRender> geometryShader;     // HTMLUI Tex (untextured draws bind white)
        std::shared_ptr<ShaderRender> gradientShader;     // HTMLUI_GRADIENT
        std::shared_ptr<ShaderRender> primShader;         // HTMLUI_PRIM
        bool pipelinesLoaded = false;
        ObjectRender geometryObject;
        ObjectRender gradientObject;
        // HTMLUI_PRIM: the unit quad and its indices, the layout template of the instance buffers
        ObjectRender primObject;
        BufferRender primQuadBuffer;
        BufferRender primQuadIndexBuffer;
        BufferRender primInstanceTemplate;
        int primParamsSlot = -1;
        int primClipSlot = -1;
        int primSlotsSlot = -1;                           // u_fs_htmluiSlots
        std::array<std::pair<int, int>, 4> primTextureSlots{{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}};
        int geometryParamsSlot = -1;
        int geometryClipSlot = -1;
        std::pair<int, int> geometryTextureSlot{-1, -1};
        int gradientParamsSlot = -1;
        int gradientFsSlot = -1;
        static constexpr int POST_PROGRAM_COUNT = 7;   // ShaderPool's HTMLUI_POST_VARIANT_COUNT
        PostProgram postPrograms[POST_PROGRAM_COUNT];
        // the composite quad, also the layout template of the dynamic geometry objects
        BufferRender compositeVertexBuffer;
        BufferRender compositeIndexBuffer;
        ObjectRender compositeObject;
        // the composite into a multisampled camera target (PIP_MSAA_NODEPTH): the same
        // bindings on the dynamic path
        ObjectRender compositeMsaaObject;
        TextureRender placeholderWhite;

        // S92 gradient stop table (R7, D9): the stops of the gradient primitives of more than
        // HtmlUIPrimitive::GRADIENT_STOPS, a row each, kept from CompileShader to ReleaseShader;
        // the texture is made when a primitive first reads a changed table (none without rows)
        static constexpr int STOP_TABLE_WIDTH = 2 * HtmlUIPrimitive::GRADIENT_TABLE_STOPS;   // colours, then positions
        std::vector<uint8_t> stopTable;                   // rows of STOP_TABLE_WIDTH RGBA8 texels
        std::vector<uint64_t> stopRowHashes;              // each row's content (the primitives' recorded hash)
        std::vector<int> freeStopRows;
        bool stopTableDirty = false;
        TextureRender stopTableTexture;
        std::vector<TextureRender> retiredStopTables;     // replaced in this frame: destroyed in a later one
        uint64_t retiredStopTablesFrame = 0;
        std::pair<int, int> primStopsSlot{-1, -1};        // u_htmluiStops
        void assignStopRow(RmlCompiledShader& shader);
        void releaseStopRow(RmlCompiledShader& shader);
        // the table's texture with every row written (placeholderWhite without rows)
        TextureRender* currentStopTable();
        void destroyStopTables();
        // phase 5: handles with bit 0 set (CompileShader/CompileFilter "jfx")
        std::unique_ptr<htmlui::EffectRenderer> effects;

        // S92 frame reuse
        uint32_t frameLiveSources = 0;       // the frame being recorded's (addLiveSource)
        uint32_t pendingLiveSources = 0;     // asked for between frames: the next frame's
        uint64_t textureSignature(const HtmlUITexture* texture, uint32_t& live) const;
        uint64_t filterSignature(Rml::CompiledFilterHandle filter, uint32_t& live) const;
        uint64_t shaderSignature(Rml::CompiledShaderHandle shader, uint32_t& live) const;
        // R1: the last drawn frame (its signature, target and the target's generation after it)
        uint64_t keptSignature = 0;
        bool keptValid = false;
        const RmlRenderTarget* keptTarget = nullptr;
        uint64_t keptGeneration = 0;
        uint64_t frameSignature = 0;         // the frame being ended's
        bool lastFrameKept = false;
        double lastSignatureMs = 0.0;
        // what a replay leaves undrawn (a pipeline compiling, an upload past the staging buffer)
        uint64_t skippedBefore = 0;
        uint64_t skippedWork() const;
        // endFrame: the frame's signature; true when it is kept (then nothing is replayed)
        bool keepRecordedFrame();
        // a kept frame's end (its textures count as drawn, R3's verification)
        void finishKeptFrame();
        // a replayed frame's end (recorded: not drawn at once): it can be kept from now on when
        // everything was drawn; its composite tiles
        void rememberDrawnFrame(bool recorded);
        // the frame statistics both ends fill
        void closeFrameStats();
        uint32_t targetBytesOf(const RmlRenderTarget& renderTarget) const;
        // R4: the 64x64 tiles of layer 0 the recorded frame writes (draw bounds after transform and
        // scissor, composites' scissors), as quads of horizontal runs merged down identical rows
        static constexpr int TILE_SIZE = 64;
        std::vector<uint8_t> tileMask;
        std::vector<uint8_t> uploadedTileMask;
        int tileColumns = 0, tileRows = 0;
        bool tilesFull = true;               // the whole quad (immediate drawing, an upload refused)
        const TextureRender* tileSource = nullptr;   // the layer-0 image the tiles are of
        uint32_t tileQuads = 0;
        uint32_t tileCount = 0;
        uint32_t tilePixels = 0;
        std::vector<Rml::Vertex> tileVertices;
        BufferRender tileVertexBuffer;
        BufferRender tileIndexBuffer;
        // the composite on the dynamic path, its buffers swapped for the tiles' (every backend
        // takes them there), at the camera pass's format
        ObjectRender compositeTileObject;
        uint32_t tileCapacity = 0;           // quads the buffers hold
        uint64_t tileUploadFrame = UINT64_MAX;
        uint64_t compositeDraws = 0;
        void computeCompositeTiles();
        void markTiles(float x0, float y0, float x1, float y1, bool scissorOn, const Rml::Rectanglei& scissor);
        void buildTileQuads();
        void destroyTileBuffers();
        // R5: the replay's depth-stencil plan. Per replay position (an op of the batched plan, else a
        // command): whether it uses the stencil, and the next position that surely begins another
        // pass; a pass begun where no stencil use follows its end stores DONTCARE, one where none
        // follows at all also loads DONTCARE. A depth-stencil stored DONTCARE is cleared and its mask
        // drawn again if a later pass needs it after all
        enum : uint8_t { DS_STENCIL = 1, DS_BREAK = 2 };
        std::vector<uint8_t> dsFlags;
        std::vector<uint32_t> dsNextBreak;
        int64_t dsLastStencil = -1;
        bool dsPlanned = false;
        bool dsEveryPass = false;            // DepthStencilDiscard::EVERY_PASS (tests)
        size_t replayPosition = 0;
        bool activeDsDiscard = false;        // the open pass stores its depth-stencil DONTCARE
        const Framebuffer* activeDsAttachment = nullptr;   // nullptr: the shared one
        bool sharedDsDiscarded = false;
        std::vector<const Framebuffer*> discardedRegionDs;
        void planDepthStencil();
        // R3
        std::unique_ptr<RmlRenderTarget> verifyTarget;
        bool verifyPending = false;
        uint64_t verifyFrame = 0;
        RmlRenderTarget* verifyKeptTarget = nullptr;
        uint64_t verifyKeptGeneration = 0;   // its framebuffers then: made again since, nothing to compare
        std::string verifySource;            // the first command reading an unversioned source
        uint32_t keptMismatchCount = 0;
        void verifyKeptFrame();
        void compareKeptFrame();

        // S92 layer cache (RmlRendererLayerCache.cpp)
        struct LayerCacheEntry {
            std::unique_ptr<Framebuffer> image;   // 1-sample RGBA8
            Rml::Rectanglei rect;                 // the window's pixels in it
            size_t bytes = 0;
            uint64_t lastUsed = 0;                // replay
            bool hit = false;                     // drawn from since it was stored
            bool transient = false;               // over the budget, for an equal scope of the replay only
        };
        // what the replay does with a recorded command of a cached scope: a hit's commands draw
        // nothing (its clip masks only set the mask, drawn where it is tested), its composite
        // draws the image; a miss's composite stores its result
        enum class CacheOp : uint8_t { NONE, SKIP, MASK, HIT, STORE };
        struct LayerCacheScope {
            uint64_t key = 0;
            Rml::Rectanglei window;               // target px: the composite's scissor within the layer
            float opacity = 1.0f;                 // a trailing opacity filter, applied at the composite
            int filters = 0;                      // the composite's filters before it
            bool keep = false;                    // a store an equal scope later in the replay draws
            bool afterStore = false;              // a hit on a store earlier in the replay
        };
        std::unordered_map<uint64_t, LayerCacheEntry> layerCache;
        // images of evicted entries, reused by size by the next replay's stores (an element that
        // changes every frame stores every frame), destroyed when it takes none
        struct LayerCacheFreeImage {
            std::unique_ptr<Framebuffer> image;
            uint64_t freed = 0;                   // replay
        };
        std::vector<LayerCacheFreeImage> layerCacheFree;
        size_t layerCacheBytes = 0;
        uint64_t layerCacheReplay = 0;
        uint32_t recordedPushes = 0;             // the frame's PUSH_LAYER commands (none: no plan)
        std::vector<CacheOp> cacheOps;           // per command (empty: no cached scope this frame)
        std::vector<int32_t> cacheScopeOf;       // HIT, STORE: the scope in cacheScopes
        std::vector<LayerCacheScope> cacheScopes;
        LayerPassStats layerPassStats;
        int filterSourceLayer = -1;              // the layer post-process passes serve (-1: the top one)
        // before the replay: the scopes' keys, hits and misses
        void planLayerCache();
        // The state a scope starts in (the replay's at its PUSH_LAYER), and the signature of its
        // commands (first, end) relative to origin (target px). Unlike signatureOf, every draw is
        // hashed with the state it is drawn in (scissor, transform, clip) instead of the state
        // commands that set it, and geometry by its vertices: a scope keys the same whatever
        // state the commands before it left, and equal elements key the same
        struct ScopeState {
            int transform = -1;                   // in commandMatrices, -1: none
            bool scissorOn = false;
            Rml::Rectanglei scissor;              // target px
            bool clipOn = false;
            uint64_t mask = 0;                    // the clip mask's operations since its last Set
        };
        uint64_t scopeSignature(size_t first, size_t end, Rml::Vector2i origin, const ScopeState& start, int layer, uint32_t& live);
        // a command of a cached scope replayed: true when it was handled
        bool replayCached(size_t index, const Command& command);
        // a skipped scope's clip mask: the operation without its draw (ensureMask draws it where tested)
        void setClipMaskOnly(const Command& command);
        // the scope's result into the cache (taking a region post target, else a copy), nullptr
        // when the budget has no room for it and no equal scope of the replay draws it
        LayerCacheEntry* storeLayerResult(const LayerCacheScope& scope, RmlFilterContext& context);
        void drawCachedLayer(const LayerCacheEntry& entry, Rml::Rectanglei window, int destination, Rml::BlendMode blend, float opacity);
        // a layer composite into a layer covering destinationFrame: within an analytic clip scope
        // of rectangles, their clip in the shader (PASSTHROUGH: params.w on, texCoordMinMax the
        // rectangle in destination px, params.yz its size) and the scissor within it; false when
        // that leaves nothing to draw
        bool applyCompositeClip(PostDraw& draw, Rml::Rectanglei destinationFrame) const;
        bool compositeClipActive() const;
        // end of a replay: entries unused for kLayerCacheKeepFrames, and stored ones not drawn
        // again in the replay after, go; free images no store took, and beyond a few, go
        void trimLayerCache();
        void releaseLayerCache();
        // a texture in a scope's key: a small static one by its texels, else textureSignature
        uint64_t scopeTextureSignature(const HtmlUITexture* texture, uint32_t& live) const;
        static constexpr size_t kLayerCachePixelHashBytes = 256 * 1024;
        static uint64_t layerCacheTextureHash(const uint8_t* data, size_t bytes, int width, int height, int filter, int wrapU, int wrapV,
                                              int format);
        std::unique_ptr<Framebuffer> takeLayerCacheImage(int width, int height);
        bool makeLayerCacheRoom(size_t bytes);
        // a pass's render area, by the window of the layer it serves (R30)
        void noteLayerPass(Rml::Rectanglei full, TargetKind kind, int index);
    };

}

#endif //RMLRENDERER_H
