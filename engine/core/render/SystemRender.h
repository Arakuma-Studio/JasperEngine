// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SystemRender_h
#define SystemRender_h

#include "Export.h"
#include <stddef.h>
#include <stdint.h>

namespace jasperengine{

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
        static const uint32_t FRAME_STATS_FRAME = 0xFFFFFFFFu;
        static void logFrameStats(uint64_t frame, uint32_t view = FRAME_STATS_FRAME);

        static void scheduleCleanup(void (*cleanupFunc)(void* cleanupData), void* cleanupData, int32_t numFramesToDefer = 0);
        static void addQueueCommand(void (*custom_cb)(void* custom_data), void* custom_data);
    };
}

#endif /* SystemRender_h */