// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SystemRender_h
#define SystemRender_h

#include "Export.h"
#include "RenderStats.h"
#include "texture/TextureFormat.h"
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace jasperengine{

    class TextureRender;
    class FramebufferRender;

    // what the device supports, queried once the render system is set up
    struct RenderFeatures {
        bool originTopLeft = false;     // framebuffer and texture row 0 at the top (all but GL)
        bool msaaRGBA8 = false;         // RGBA8 colour attachments can be multisampled
        bool msaaDepthStencil = false;  // DEPTH_STENCIL attachments can be multisampled
        // float colour attachments (ColorFormat RGBA16F, RGBA32F, R32F, RG16F, R16F): can be rendered to,
        // blended into (additive passes) and sampled with a linear filter
        bool renderRGBA16F = false, blendRGBA16F = false, filterRGBA16F = false;
        bool renderRGBA32F = false, blendRGBA32F = false, filterRGBA32F = false;
        bool renderR32F = false, blendR32F = false, filterR32F = false;
        // the cooked texture formats it samples with linear filtering (spec S14 R15)
        TextureFormatSupport textureFormats;
        bool renderRG16F = false, blendRG16F = false, filterRG16F = false;
        bool renderR16F = false, blendR16F = false, filterR16F = false;
        // the largest side of a 2D texture (0: unknown)
        int maxTextureSize2D = 0;
    };

    // the kinds of GPU objects, each in a pool of its own (Engine::GpuResourceLimits)
    enum class GpuResourceKind { BUFFER, IMAGE, SAMPLER, VIEW, PIPELINE, SHADER };
    constexpr int GPU_RESOURCE_KINDS = 6;

    class JASPERENGINE_API SystemRender{
    public:
        // with the pool sizes of Engine::getGpuResourceLimits
        static void setup();
        // a new id each setup: a GPU handle made before means nothing after it
        static uint64_t getContextId();

        // GPU objects of a kind alive now, and the most there can be
        static int getResourcesInUse(GpuResourceKind kind);
        static int getResourceLimit(GpuResourceKind kind);
        static const char* getResourceKindName(GpuResourceKind kind);
        // objects not made because their pool was full, since setup (the first of each kind
        // is logged as an error, naming the pool and its limit)
        static uint64_t getResourcePoolExhaustions();
        static uint64_t getResourcePoolExhaustions(GpuResourceKind kind);
        // every pool that was full since the last call has a free slot again (true when none
        // was): what a load that failed for want of one waits for before it is tried again
        static bool hasResourcePoolRoom();

        // the frame's uniform blocks against Engine::GpuResourceLimits::uniformBytes (counted on
        // every backend, each block rounded up to 256 bytes): the last committed frame's bytes,
        // the most a frame needed, the limit, whether this backend skips the draws past it
        // (Vulkan, Metal, WebGPU) and how many it skipped since setup
        static uint64_t getUniformBytesLastFrame();
        static uint64_t getUniformBytesPeak();
        static uint64_t getUniformBytesLimit();
        static bool hasUniformBufferLimit();
        static uint64_t getUniformSkippedDraws();
        // the frame's uploads into STREAM buffers and textures (bone matrices, HTML UI geometry,
        // video frames, instance data) against Engine::GpuResourceLimits::streamStagingBytes
        // (counted on every backend, each upload rounded up to 16 bytes as Vulkan's per-frame
        // stream staging buffer takes them): the last committed frame's bytes, the most a frame
        // needed, the limit, whether this backend skips the uploads past it (Vulkan) and how
        // many it skipped since setup (their resources keep the data they had)
        static uint64_t getStreamBytesLastFrame();
        static uint64_t getStreamBytesPeak();
        static uint64_t getStreamBytesLimit();
        static bool hasStreamStagingLimit();
        static uint64_t getStreamSkippedUploads();
        // the frame's descriptor sets in Vulkan's per-frame descriptor buffer against
        // Engine::GpuResourceLimits::descriptorBytes (0 on the other backends): the last
        // committed frame's bytes, the most a frame needed, the limit, whether the draws past
        // it are skipped (Vulkan) and how many were since setup
        static uint64_t getDescriptorBytesLastFrame();
        static uint64_t getDescriptorBytesPeak();
        static uint64_t getDescriptorBytesLimit();
        static bool hasDescriptorBufferLimit();
        static uint64_t getDescriptorSkippedDraws();

        static void commitQueue();
        static void executeQueue();
        static void commit();
        // frames committed so far: work that may run once per frame keys on it
        static uint64_t getFrameCount();
        // cached on the first call after setup (defaults before it)
        static const RenderFeatures& getFeatures();
        // sg_query_pixelformat(f).sample && .filter of the format's one sokol format, cached at
        // device creation, or what setTextureFormatSupportOverride says (spec S14 R15)
        static bool isTextureFormatSupported(GpuTextureFormat format);
        static TextureFormatSupport getTextureFormatSupport();
        // tests fake a device ("no BPTC"), the editor's Game view simulates a platform; null
        // returns to the device's answer
        static void setTextureFormatSupportOverride(const TextureFormatSupport* support);
        static bool hasTextureFormatSupportOverride();
        // the dynamic pipeline cache (ObjectRender::beginDraw(PipelineState)): pipelines
        // created and applied since start, and how many it holds now
        static uint64_t getPipelineCacheMisses();
        static uint64_t getPipelineApplies();
        static size_t getPipelineCacheSize();
        // render passes begun while another was open, since start: an engine bug, logged as an
        // error (passes do not nest; see SokolSystem::beginPass)
        static uint64_t getNestedPassCount();
        static void shutdown();

        // logs "Frame stats (frame N): ..." with the GPU work of the last committed frame and
        // the live GPU resources, in the render thread's order (JASPERENGINE_FRAME_STATS=1;
        // nothing otherwise); with a view id, "Frame stats (frame N, view V): ..." for a second view
        // the editor drew and committed in that frame (Engine::drawView)
        static constexpr uint32_t FRAME_STATS_FRAME = 0xFFFFFFFFu;
        static void logFrameStats(uint64_t frame, uint32_t view = FRAME_STATS_FRAME);

        // ---- diagnostics (spec S15): what the editor and MCP read, never sokol directly

        // the view the commits from now draw: FRAME_STATS_FRAME for the game frame
        // (Engine::systemDraw), the view id for Engine::drawView
        static void setViewTag(uint32_t tag);
        static uint32_t getViewTag();
        // a view's counters in the newest frame that drew it (draw calls, instances and
        // triangles of the game frame are Engine::FrameStats; a view's come from its passes)
        static RenderStats getRenderStats(uint32_t viewTag);
        // the ordered passes of a view's newest frame; with resolvedOnly, of the newest one
        // whose GPU times all arrived when one has
        static PassList getPassList(uint32_t viewTag, bool resolvedOnly);
        static std::vector<uint32_t> getRecordedViewTags();
        static BackendFrameCounters getBackendFrameCounters();
        // "OpenGL 3.3 core", "Vulkan" ... and the device sokol runs on ("llvmpipe (LLVM 15.0.7, 256 bits)")
        static const char* getBackendName();
        static std::string getDeviceName();
        // per-draw records in the pass lists from now (a capture, the Frame Debugger)
        static void setDrawListEnabled(bool enabled);

        // GPU timing (sokol_gpu_timer.h) runs while a consumer holds it or a lease runs;
        // off, no query object is issued
        static void acquireGpuTiming();
        static void releaseGpuTiming();
        static void leaseGpuTiming(double seconds);
        static GpuTimingState getGpuTimingState();
        // Vulkan, by whoever makes the VkDevice, before setup: whether it enabled hostQueryReset
        // (the GPU timer needs it; without it the timer reports device_unsupported)
        static void setVulkanHostQueryReset(bool enabled);

        // the Frame Debugger's preview of one attachment of a pass (one-shot: the next frame's
        // matching pass is copied into the preview texture, or refused with a reason)
        static void requestPassPreview(const PassPreviewRequest& request);
        static void cancelPassPreview();
        static PassPreviewResult getPassPreview();
        static TextureRender* getPassPreviewTexture();
        static FramebufferRender* getPassPreviewTarget();

        static void scheduleCleanup(void (*cleanupFunc)(void* cleanupData), void* cleanupData, int32_t numFramesToDefer = 0);
        static void addQueueCommand(void (*custom_cb)(void* custom_data), void* custom_data);
    };
}

#endif /* SystemRender_h */