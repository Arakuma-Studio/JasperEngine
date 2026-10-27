// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef sokolsystem_h
#define sokolsystem_h

#include <stddef.h>
#include <stdint.h>
#include <string>

struct sg_pass;
struct sg_attachments;

namespace jasperengine{
    struct RenderFeatures;

    class SokolSystem{

    public:
        static void setup();
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
