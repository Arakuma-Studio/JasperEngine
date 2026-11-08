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
    // last 2 s of S15's frame-time ring (profiler/FrameTimes.h: a game's systemDraw to systemDraw,
    // the editor's own frames in its Play; recorded in every build), draw calls of
    // Engine::FrameStats, as S15's monitors read them
    struct DebugStatsSnapshot {
        float fps = 0.0f;                   // frames in the last 2 s / their duration
        float frameMs = 0.0f;               // the last frame
        float frameMsAvg = 0.0f;
        float frameMsMin = 0.0f;
        float frameMsMax = 0.0f;
        float frameMs1PercentLow = 0.0f;    // the average of the slowest 1% of frames (at least one): "1% low"
        uint32_t drawCalls = 0;             // Engine::getFrameStats: the last drawn frame
        uint32_t setPassCalls = 0;          // pipelines applied (Unity's SetPass calls)
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
        uint32_t coroutines = 0;            // engine coroutines running or waiting (ScriptScheduler)
        uint32_t timers = 0;                // engine timers pending (ScriptScheduler)
        uint32_t htmlUiDrawCalls = 0;       // HTML UI draw calls of the last frame, every scene
    };

    // The sampling of the stats: everything is read when asked (main thread); the overlay keeps
    // no per-frame state of its own
    class JASPERENGINE_API DebugStats {
    public:
        // frames the graph shows (FrameTimes keeps more: FrameTimes::CAPACITY)
        static constexpr size_t historySize = 240;

        // the newest frames' times of FrameTimes, milliseconds, oldest first; at most
        // historySize, a stall (a breakpoint, the app in the background) shown as 1 s
        static std::vector<float> frameHistory();
        // frames FrameTimes completed (its frame serial)
        static size_t recordedFrames();

        static DebugStatsSnapshot sample();
    };

}

#endif // DEBUGSTATS_H
