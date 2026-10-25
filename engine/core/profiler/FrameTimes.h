// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FrameTimes_h
#define FrameTimes_h

// The frame-time ring (spec S15 R19): one 32-byte sample per frame for the last 600 frames,
// recorded in every build whether the profiler is on, compiled in or not (the editor's
// get_frame_times, Lua's Profiler.frameStats, the Game view's Stats). A frame is one
// Engine::systemDraw in a game and one rendered iteration of the editor's loop in the editor.
// Main thread only.

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace jasperengine {

    struct FrameTimeSample {
        uint64_t frame = 0;         // the frame serial (FrameTimes::currentFrame when it ran)
        float frameMs = 0.0f;       // its start to the next frame's start (1000 / fps)
        float cpuMs = 0.0f;         // its start to the end of its CPU work (before present and waits)
        float gpuMs = -1.0f;        // the game frame's GPU time (< 0: not measured, or not arrived)
        float fps = 0.0f;
        uint32_t drawCalls = 0;
        uint32_t flags = 0;         // FLAG_*
    };
    static_assert(sizeof(FrameTimeSample) == 32, "one 32-byte sample per frame (S15 R19)");

    class JASPERENGINE_API FrameTimes {
    public:
        static constexpr size_t CAPACITY = 600;
        // the frame's GPU time is the sum of its passes (the GL family), not a timestamp span
        static constexpr uint32_t FLAG_GPU_SUM = 1u << 0;

        // Starts frame currentFrame()+1 and completes the previous one's sample. The Engine calls
        // it from systemDraw unless the host owns the frames (setExternalFrames: the editor).
        static void beginFrame();
        // The CPU work of the current frame is done (the rest until the next beginFrame is
        // presenting and waiting); without it cpuMs = frameMs.
        static void endFrameWork();
        static void setExternalFrames(bool external);
        static bool hasExternalFrames();

        // the serial of the frame being drawn (0 before the first)
        static uint64_t currentFrame();
        static void setDrawCalls(uint32_t drawCalls);
        // the GPU time of an earlier frame, when its timer queries resolved (frames ago)
        static void setGpuMs(uint64_t frame, double gpuMs, bool sumOfPasses);

        // the completed samples, oldest first (at most CAPACITY)
        static size_t size();
        static FrameTimeSample at(size_t index);
        static std::vector<FrameTimeSample> snapshot(size_t newest = CAPACITY);
        // the last completed frame (a default sample before the first)
        static FrameTimeSample latest();
        // the GPU time of the newest of the last `window` frames that has one, ms (< 0: none);
        // GPU times arrive a few frames late (S13 dynamic resolution's GPU law)
        static double latestGpuMs(size_t window = 8);
        static void clear();
    };

}

#endif /* FrameTimes_h */
