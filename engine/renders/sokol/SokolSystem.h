// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef sokolsystem_h
#define sokolsystem_h

#include <stddef.h>
#include <stdint.h>
#include <string>

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
        // SokolTexture, SokolCmdQueue): a STREAM one is reserved first, and dropped when
        // refused (appendBuffer: -1)
        static void updateBuffer(sg_buffer buffer, const sg_range& data);
        static int appendBuffer(sg_buffer buffer, const sg_range& data);
        static void updateImage(sg_image image, const sg_image_data& data);
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
        static void shutdown();
        static const RenderFeatures& getFeatures();
        static uint64_t getPipelineCacheMisses();
        static uint64_t getPipelineApplies();
        static size_t getPipelineCacheSize();

        // the GPU work of the last committed frame and the live resources, on one line (empty
        // unless JASPERENGINE_FRAME_STATS turned sokol's statistics on at setup)
        static std::string getFrameStatsLine();
        // a render pass begun (with JASPERENGINE_FRAME_STATS on): its label and target size go
        // in the frame's pass list, which getFrameStatsLine appends ("pass list: ...")
        static void recordPass(const sg_pass& pass);

        // Every engine render pass begins and ends here. Passes do not nest: a pass begun while
        // another is open is an engine error (Vulkan has one dynamic rendering instance at a
        // time: the nested vkCmdBeginRendering and the outer pass's then unmatched
        // vkCmdEndRendering crash NVIDIA's driver; GL only rebinds its framebuffer). It is
        // logged and counted (getNestedPassCount), and begun anyway, as before.
        static void beginPass(const sg_pass& pass);
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
