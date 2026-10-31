// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SystemRender_h
#define SystemRender_h

#include "Export.h"
#include "GpuCreation.h"
#include "RenderStats.h"
#include "texture/TextureFormat.h"
#include <stddef.h>
#include <stdint.h>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine{

    class TextureRender;
    class FramebufferRender;
    class ShaderRender;

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

    class JASPERENGINE_API SystemRender{
    public:
        static void setup();
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
        static std::string getApiVersion();
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

        // ---- asynchronous shader and pipeline creation (spec S32, SokolAsyncCreate)

        // once a frame before the views draw (Engine::runFrame for a game, the editor's loop):
        // finishes and publishes what is ready, within the frame's budget
        static void updateGpuCreation();
        static GpuCreationStats getGpuCreationStats();
        static bool isGpuCreationIdle();
        // updates until idle or the timeout (tests, the benchmark): true when idle
        static bool waitGpuCreationIdle(double timeoutMs);
        static void resetGpuCreationMax();
        // applied at setup (the mode); the budget and warm draws also at once
        static void setShaderCreationSettings(const ShaderCreationSettings& settings);
        static const ShaderCreationSettings& getShaderCreationSettings();

        // ---- the shader warm-up's pipelines (spec S32 R10-R12, ShaderWarmup)

        // every pipeline created from now is reported: its shader (the backend handle id) and its
        // description as backend-neutral JSON (SokolPipelineRecord). Any thread; null stops it
        static void setPipelineRecorder(std::function<void(uint32_t shaderId, const std::string& record)> recorder);
        // the pipeline of a record on `shader` (created, or found, in the intern table) held until
        // releaseWarmPipeline; false with *error for a record that does not parse. A record from the
        // other backend family (GL against the rest) also warms the other winding of an offscreen
        // target: GL flips it (SokolObject::makePipelineDesc)
        // The handles held are appended to `held` (each released with releaseWarmPipeline)
        static bool warmPipeline(ShaderRender* shader, const std::string& record, bool otherFamily, std::vector<uint32_t>& held,
                                 std::string* error = nullptr);
        // created (VALID) or failed: nothing more to wait for
        static bool isPipelineSettled(uint32_t pipeline);
        static void releaseWarmPipeline(uint32_t pipeline);
        // the backend handle id of a shader (0 when it has none)
        static uint32_t getShaderHandleId(ShaderRender* shader);

        static void scheduleCleanup(void (*cleanupFunc)(void* cleanupData), void* cleanupData, int32_t numFramesToDefer = 0);
        static void addQueueCommand(void (*custom_cb)(void* custom_data), void* custom_data);
    };
}

#endif /* SystemRender_h */