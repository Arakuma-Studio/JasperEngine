// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SystemRender_h
#define SystemRender_h

#include "Export.h"
#include <stdint.h>

namespace jasperengine{
    class JASPERENGINE_API SystemRender{
    public:
        static void setup();
        static void commitQueue();
        static void executeQueue();
        static void commit();
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