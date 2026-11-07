// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef sokolsystem_h
#define sokolsystem_h

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

#include "render/RenderStats.h"
struct sg_pass;
struct sg_attachments;
struct sg_buffer;
struct sg_image;
struct sg_range;
struct sg_image_data;

namespace jasperengine{
    struct RenderFeatures;

    class SokolSystem{

    public:
        static void setup();

        // the resource pools, by SystemRender's GpuResourceKind (buffer, image, sampler, view,
        // pipeline, shader): objects alive, pool size, name; full-pool events since setup
        static int getResourcesInUse(int kind);
        static int getResourceLimit(int kind);
        static const char* getResourceKindName(int kind);
        static uint64_t getResourcePoolExhaustions(int kind);
        static bool hasResourcePoolRoom();

        // sokol's per-frame GPU buffers the engine budgets (Engine::GpuResourceLimits), each
        // allocated once per frame in flight. What a frame puts in each is counted on every
        // backend (the bytes as sokol places them); where the backend has the buffer the item that
        // does not fit is refused (and its draw or upload skipped and counted), never handed to
        // sokol to drop or overrun; the first frame that needed more is logged once, as an error
        // naming its bytes, the limit and the setting to raise (a warning where the backend has
        // no such buffer).
        enum{
            // uniform blocks (sg_apply_uniforms, each rounded up to 256 bytes): uniformBytes,
            // a buffer on Vulkan, Metal and WebGPU (GL, D3D11: none)
            UNIFORM_BUDGET = 0,
            // uploads into STREAM buffers and images (sg_update_buffer, sg_append_buffer,
            // sg_update_image of a stream_update resource, each rounded up to 16 bytes):
            // streamStagingBytes, Vulkan's stream staging buffer (elsewhere written into the
            // resource)
            STREAM_BUDGET = 1,
            // descriptor sets of the draws (Vulkan's descriptor buffer: sokol's own count, other
            // backends 0): descriptorBytes
            DESCRIPTOR_BUDGET = 2
        };

        // A uniform block of the draw being set up; false: refused (the draw is skipped,
        // countSkippedDraw)
        static bool reserveUniformBytes(unsigned int size);
        static void countSkippedDraw();
        // An upload of these bytes (an image's mip levels) into a STREAM resource; false:
        // refused and counted as skipped (all or nothing)
        static bool reserveStreamBytes(const uint64_t* sizes, int count);
        // Every engine upload into a STREAM or DYNAMIC resource goes through these (SokolBuffer,
        // SokolTexture, SokolCmdQueue): a STREAM one is reserved first, and not made when
        // refused (false, appendBuffer -1: the caller tries again next frame)
        static bool updateBuffer(sg_buffer buffer, const sg_range& data);
        static int appendBuffer(sg_buffer buffer, const sg_range& data);
        static bool updateImage(sg_image image, const sg_image_data& data);
        // The draw about to be bound and drawn (its pipeline applied, its uniforms applied):
        // false when its descriptor sets do not fit the frame's descriptor buffer (Vulkan; the
        // draw is skipped and counted)
        static bool reserveDescriptorBytes();

        // a budget's bytes in the last committed frame (what it asked for, refused items too),
        // the most a frame asked for, the limit, whether items past it are refused, and the
        // items refused since setup
        static uint64_t getBudgetLastFrame(int kind);
        static uint64_t getBudgetPeak(int kind);
        static uint64_t getBudgetLimit(int kind);
        static bool isBudgetEnforced(int kind);
        static uint64_t getBudgetSkipped(int kind);
        // (tests) a budget's limit and whether it is enforced, until restoreBudgets (the
        // GpuResourceLimits sokol was set up with: sokol's own buffers stay that size)
        static void overrideBudget(int kind, uint64_t limit, bool enforced);
        static void restoreBudgets();
        // (tests, Vulkan) sokol's own count of the frame being recorded in a per-frame buffer
        // (0 stream staging, 1 uniforms, 2 descriptors) and its size; false on other backends
        static bool getVulkanFrameBufferUsage(int which, uint64_t& used, uint64_t& size);
        static void commitQueue();
        static void executeQueue();
        static void commit();
        // the frames committed so far (sokol resets STREAM appends at each)
        static uint64_t getCommitCount();
        // between frames: returns once the GPU finished what was committed (Metal; nothing elsewhere)
        static void waitGpuIdle();
        static void shutdown();
        static const RenderFeatures& getFeatures();
        static uint64_t getPipelineCacheMisses();
        static uint64_t getPipelineApplies();
        static size_t getPipelineCacheSize();

        // the GPU work of the last committed frame and the live resources, on one line (empty
        // unless JASPERENGINE_FRAME_STATS turned sokol's statistics on at setup); its pass list
        // ("pass list: ...") is the label and target size of each pass SokolCamera began
        static std::string getFrameStatsLine();

        // the view the commits from now draw (the game frame, or an editor view id)
        static void setViewTag(uint32_t tag);
        static uint32_t getViewTag();
        static RenderStats getRenderStats(uint32_t viewTag);
        static PassList getPassList(uint32_t viewTag, bool resolvedOnly);
        static std::vector<uint32_t> getRecordedViewTags();
        static BackendFrameCounters getBackendFrameCounters();
        static const char* getBackendName();
        static std::string getDeviceName();
        // the context's API version ("OpenGL ES 3.2 Mesa ...", "Vulkan 1.3.255")
        static std::string getApiVersion();
        static void setDrawListEnabled(bool enabled);

        // GPU timing runs while a consumer holds it (the profiler recording, dynamic resolution)
        // or a lease has not run out (an agent asking for pass times); off, no query is issued
        static void acquireGpuTiming();
        static void releaseGpuTiming();
        static void leaseGpuTiming(double seconds);
        static GpuTimingState getGpuTimingState();
        static void setVulkanHostQueryReset(bool enabled);

        // Draws a batcher holds back (RenderSystem's dynamic batching, sprite-batching spec 6.2):
        // while a flush is installed, any other GPU command of the pass (an object's beginDraw, a
        // pipeline applied by the cache, a viewport, the pass's end) runs it first, so the held
        // draws keep their place in the pass whoever draws next. Scissors are lazy meanwhile: a
        // requested rect reaches the GPU before the next draw that is not the batcher's (the
        // batcher applies the rect each of its draws captured), so a scissor set and reset
        // around an element that draws nothing costs nothing. Without a flush installed every
        // call here is one branch and scissors apply at once, as before. Main thread only.
        typedef void (*DeferredFlush)(void* owner);
        struct ScissorRect{
            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;
            bool originTopLeft = false;
            // false: none known (no pass open). A rect covering the pass's whole target is that
            // pass's own, whatever its origin (applyScissor normalises it)
            bool set = false;

            bool operator==(const ScissorRect& rhs) const;
            bool operator!=(const ScissorRect& rhs) const { return !(*this == rhs); }
        };
        // null removes it (the scissor requested meanwhile is applied then)
        static void setDeferredFlush(DeferredFlush flush, void* owner);
        static bool isDeferringDraws();
        // runs the installed flush (not from inside itself)
        static void flushDeferred();
        // before a draw that is not the batcher's: the flush, then the requested scissor
        static void beforeForeignDraw();
        // the batcher's own draws: the rect it captured, applied when it is not the one applied
        static void applyBatchScissor(const ScissorRect& rect);
        // SokolCamera's scissor: deferred while a flush is installed, else applied at once
        static void applyScissor(int x, int y, int width, int height, bool originTopLeft);
        static const ScissorRect& getRequestedScissor();

        // Every engine render pass begins and ends here. Passes do not nest: a pass begun while
        // another is open is an engine error (Vulkan has one dynamic rendering instance at a
        // time: the nested vkCmdBeginRendering and the outer pass's then unmatched
        // vkCmdEndRendering crash NVIDIA's driver; GL only rebinds its framebuffer). It is
        // logged and counted (getNestedPassCount), and begun anyway, as before.
        static void beginPass(const sg_pass& pass);
        // (runs the Frame Debugger's preview of the pass, SokolPassPreview::afterEndPass)
        static void endPass();
        // passes begun while another was open, since start (0 in a correct program)
        static uint64_t getNestedPassCount();
        // A pass that only gives new attachments defined contents and a readable layout
        // (SokolFramebuffer, Vulkan). Called inside an open pass (a target created while
        // drawing, as an HTML UI layer the first time a frame pushes it), it runs right after
        // that pass ends, before any later pass can use the attachments: if they are destroyed
        // by then, it is dropped.
        static void initializeAttachments(const sg_attachments& attachments, bool loadDepthStencil);

        static void scheduleCleanup(void (*cleanupFunc)(void* cleanupData), void* cleanupData, int32_t numFramesToDefer = 0);
        static void addQueueCommand(void (*custom_cb)(void* custom_data), void* custom_data);
    };
}

#endif //sokolsystem_h
