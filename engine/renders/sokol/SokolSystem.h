// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef sokolsystem_h
#define sokolsystem_h

#include <stddef.h>
#include <stdint.h>

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

        static void scheduleCleanup(void (*cleanupFunc)(void* cleanupData), void* cleanupData, int32_t numFramesToDefer = 0);
        static void addQueueCommand(void (*custom_cb)(void* custom_data), void* custom_data);
    };
}

#endif //sokolsystem_h
