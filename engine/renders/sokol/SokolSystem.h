// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef sokolsystem_h
#define sokolsystem_h

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

#include "render/RenderStats.h"

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
        static void setDrawListEnabled(bool enabled);

        // GPU timing runs while a consumer holds it (the profiler recording, dynamic resolution)
        // or a lease has not run out (an agent asking for pass times); off, no query is issued
        static void acquireGpuTiming();
        static void releaseGpuTiming();
        static void leaseGpuTiming(double seconds);
        static GpuTimingState getGpuTimingState();
        static void setVulkanHostQueryReset(bool enabled);

        static void scheduleCleanup(void (*cleanupFunc)(void* cleanupData), void* cleanupData, int32_t numFramesToDefer = 0);
        static void addQueueCommand(void (*custom_cb)(void* custom_data), void* custom_data);
    };
}

#endif //sokolsystem_h
