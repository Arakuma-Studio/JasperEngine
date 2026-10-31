// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef DEBUGSTATS_H
#define DEBUGSTATS_H

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace jasperengine {

    // What the stats overlay shows and DebugOverlay.getStats() returns. Frame times are of the
    // last 2 s of frames (unscaled: what the device took, whatever the time scale)
    struct DebugStatsSnapshot {
        float fps = 0.0f;                   // frames in the last 2 s / their duration
        float frameMs = 0.0f;               // the last frame
        float frameMsAvg = 0.0f;
        float frameMsMin = 0.0f;
        float frameMsMax = 0.0f;
        float frameMs1PercentLow = 0.0f;    // the average of the slowest 1% of frames (at least one): "1% low"
        uint32_t drawCalls = 0;             // Engine::getFrameStats: the last drawn frame
        uint32_t instances = 0;
        uint64_t triangles = 0;
        uint64_t processMemoryBytes = 0;    // resident memory of the process; 0 = unknown on this platform
        uint64_t luaMemoryBytes = 0;        // the Lua state's heap
        uint32_t audioVoices = 0;           // sounds playing now
        uint32_t audioVoicesMax = 0;        // the most that play at once (SoLoud's limit)
        uint32_t bodies2D = 0;              // physics bodies of the running scenes
        uint32_t bodies3D = 0;
        uint32_t entities = 0;              // entities of the running scenes (system ones included)
        uint32_t scenes = 0;                // running scenes (Engine::getSceneLayers)
        uint32_t coroutines = 0;            // engine coroutines running (feature coroutines)
        uint32_t timers = 0;                // engine timers pending (feature coroutines)
        uint32_t htmlUiDrawCalls = 0;       // HTML UI draw calls of the last frame, every scene
    };

    // Frame-time history and the sampling of the stats. recordFrame is the only per-frame cost
    // (a float into a ring); sample() reads everything when asked (main thread)
    class JASPERENGINE_API DebugStats {
    public:
        static constexpr size_t historySize = 240;

        // Engine::runFrame, every frame: the measured frame time, before maxDeltatime clamps it
        static void recordFrame(double unscaledFrameSeconds);
        // milliseconds, oldest first; at most historySize
        static std::vector<float> frameHistory();
        static size_t recordedFrames();     // frames recorded since the start (or the last reset)
        static void reset();

        static DebugStatsSnapshot sample();

        // Counts another feature provides (coroutines: ScriptScheduler::coroutineCount and
        // timerCount); null = 0
        using CountFunction = size_t (*)();
        static void setTaskCounters(CountFunction coroutines, CountFunction timers);
    };

}

#endif // DEBUGSTATS_H
