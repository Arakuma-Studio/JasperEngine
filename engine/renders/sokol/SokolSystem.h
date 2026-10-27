// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef sokolsystem_h
#define sokolsystem_h

#include <stddef.h>
#include <stdint.h>
#include <string>

struct sg_pass;

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

        static void scheduleCleanup(void (*cleanupFunc)(void* cleanupData), void* cleanupData, int32_t numFramesToDefer = 0);
        static void addQueueCommand(void (*custom_cb)(void* custom_data), void* custom_data);
    };
}

#endif //sokolsystem_h
