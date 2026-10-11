// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef sokolsystem_h
#define sokolsystem_h

#include <stdint.h>
#include <string>

namespace jasperengine{
    class SokolSystem{

    public:
        static void setup();
        static void commitQueue();
        static void executeQueue();
        static void commit();
        static void shutdown();

        // the GPU work of the last committed frame and the live resources, on one line (empty
        // unless JASPERENGINE_FRAME_STATS turned sokol's statistics on at setup)
        static std::string getFrameStatsLine();

        static void scheduleCleanup(void (*cleanupFunc)(void* cleanupData), void* cleanupData, int32_t numFramesToDefer = 0);
        static void addQueueCommand(void (*custom_cb)(void* custom_data), void* custom_data);
    };
}

#endif //sokolsystem_h
