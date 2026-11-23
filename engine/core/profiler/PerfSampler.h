// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PerfSampler_h
#define PerfSampler_h

// The Performance sampler (docs/specs/release-performance-and-gpu-quirks.md P1, Lua Performance):
// gameplay frame times in every build, for the game's own analytics (the engine sends nothing).
// It rides on FrameTimes: each completed sample goes into a pending second, which is committed to
// the window or discarded once a wall second (paused, resumed, loading, a stall, the game's own
// exclusions). Off it costs one branch per frame; on, a store and a few increments per frame and
// one thread-clock read per second. Main thread (the frame thread) only.

#include "Export.h"
#include "device/DeviceTypes.h"
#include "util/FunctionSubscribe.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine {

    struct FrameTimeSample;

    // R8: the current window
    struct PerfSummary {
        double seconds = 0.0;           // committed gameplay seconds
        uint64_t frames = 0;
        double fpsAvg = 0.0;            // frames / seconds
        double fpsP50 = 0.0;            // 1000 / frameMs p50
        double fpsP10 = 0.0;            // 1000 / frameMs p90: the rate 90 % of frames reach
        double frameMsP50 = 0.0;
        double frameMsP90 = 0.0;
        double frameMsP99 = 0.0;
        double frameMsMax = 0.0;
        double cpuMsAvg = 0.0;
        double cpuMsP90 = 0.0;
        // committed main-thread CPU / committed frames (< 0: unavailable, the web)
        double mainThreadCpuMs = -1.0;
        bool threadClock = false;       // "thread_clock", else "unavailable"
        double gpuMsAvg = 0.0;
        uint64_t gpuFrames = 0;         // frames with a GPU time (0: no gpuMs block)
        uint64_t hitches = 0;           // frames of 250..1000 ms
        // seconds discarded per cause, and the game's reasons for its own exclusions
        double excludedPaused = 0.0;
        double excludedResume = 0.0;
        double excludedLoading = 0.0;
        double excludedStall = 0.0;
        double excludedGame = 0.0;
        std::vector<std::pair<std::string, double>> excludedReasons;
        double thermal[4] = {0.0, 0.0, 0.0, 0.0};   // seconds per ThermalState
        DeviceTier tier = DeviceTier::High;
        int tierChanges = 0;
        std::string gpu;
        std::string driver;
        std::vector<std::string> quirks;
        std::string backend;
        std::string platform;
        std::string version;
        int64_t buildId = 0;
        // options.histogram: the bins' lower edges (ms) and counts
        std::vector<float> histogramEdgesMs;
        std::vector<uint32_t> histogramCounts;
    };

    class JASPERENGINE_API PerfSampler {
    public:
        // R4: 0.5 ms bins over [0, 50), 2 ms over [50, 250), one bin >= 250 ms
        static constexpr int BINS = 201;
        static constexpr int MAX_REASONS = 8;
        static constexpr int DEFAULT_WINDOW_SECONDS = 300;

        // R1: on, or off with the game's reason for the time it excludes (at most 8 names; more
        // count as "other"). Off after having sampled, the window keeps counting the excluded
        // seconds until reset()
        static void setSampling(bool on, std::string_view reason = {});
        static bool isSampling();
        // the game called setSampling since the last reset (--perf-summary leaves it to the game)
        static bool wasSetByGame();
        static void markSetByGame();

        // R8: the current window, without resetting it
        static PerfSummary summary(bool histogram = false);
        // R9: a new window now (sampling stays as it is)
        static void reset();
        // a new session: off, no window, no reasons, the game's choice forgotten (Play start,
        // tests)
        static void resetSession();
        // R9: onWindow's window, 30..3600 s (clamped)
        static void setWindowSeconds(int seconds);
        static int getWindowSeconds();
        // the window reached getWindowSeconds() of committed gameplay (then a new one starts);
        // cleared at Stop (Engine::clearAllSubscriptions)
        static FunctionSubscribe<void(const PerfSummary&)> onWindow;
        // the engine's own window consumer (--perf-summary's file), which no Stop clears
        static void setWindowWriter(void (*writer)(const PerfSummary&));

        // R31, R34: the frame thread's CPU per frame over the last full second (< 0: none yet,
        // or unavailable), and over the newest `seconds` measured seconds
        static double mainThreadCpuMsLastSecond();
        static double mainThreadCpuMsOver(double seconds);
        // keeps the once-a-second thread-clock read running for the next few seconds without
        // sampling (the Stats overlay, the Profiler, get_frame_times)
        static void watchThreadCpu();

        // ---- engine signals (main thread)
        static void notePaused(bool paused);
        static void noteHidden(bool hidden);
        // a resume, a focus regain or the window visible again: the next 2 s are excluded
        static void noteResumed();
        // Engine::onSceneLoaded: the next second is excluded
        static void noteSceneLoaded();

        // FrameTimes::beginFrame and recordSample, behind one branch
        static bool isActive() { return active; }
        static void onFrame(const FrameTimeSample& sample, std::chrono::steady_clock::time_point now);
        // FrameTimes::setGpuMs: a completed frame's GPU time arrived (R6)
        static void onGpuMs(double gpuMs);

        // ---- tests
        // a second (or `seconds`) elapsed on the sampler's own clock: the pending second is decided
        static void tickForTest(double seconds);
        // whether scenes are loading at a tick (null: Engine::areScenesLoaded(false))
        static void setLoadingProbeForTest(bool (*probe)());

        // the summary as JSON with snake_case keys (--perf-summary, get_frame_times)
        static std::string toJson(const PerfSummary& summary);
        // the bin of a frame time, and a bin's lower edge (ms)
        static int binOf(float ms);
        static float binEdgeMs(int bin);

    private:
        static bool active;
    };

}

#endif /* PerfSampler_h */
