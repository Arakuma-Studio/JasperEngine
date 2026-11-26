// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PerfSampler.h"

#include "Engine.h"
#include "FrameTimes.h"
#include "System.h"
#include "ThreadCpuClock.h"
#include "device/Device.h"
#include "device/GpuQuirks.h"
#include "render/SystemRender.h"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;

bool PerfSampler::active = false;
FunctionSubscribe<void(const PerfSummary&)> PerfSampler::onWindow;

namespace {

    // the causes a second is discarded for (R3)
    enum PerfCause { PERF_CAUSE_PAUSED, PERF_CAUSE_RESUME, PERF_CAUSE_LOADING, PERF_CAUSE_STALL, PERF_CAUSE_GAME, PERF_CAUSE_COUNT };

    // the frame thread's CPU of the newest measured seconds (Stats, Monitors, get_frame_times)
    constexpr int PERF_CPU_RING = 16;
    // how long watchThreadCpu keeps the thread clock read without sampling
    constexpr double PERF_WATCH_SECONDS = 5.0;

    struct PerfWindow {
        uint32_t frameHist[PerfSampler::BINS];
        uint32_t cpuHist[PerfSampler::BINS];
        uint64_t frames;
        double seconds;
        double sumFrameMs;
        double sumCpuMs;
        float maxFrameMs;
        uint64_t hitches;
        double gpuSumMs;
        uint64_t gpuFrames;
        double threadCpuMs;
        uint64_t threadFrames;
        double thermal[4];
        bool haveTier;
        DeviceTier tier;
        int tierChanges;
    };

    // the second being measured, held whole until its tick commits or discards it (at any frame
    // rate: a histogram, not a frame list)
    struct PerfPending {
        uint32_t frameHist[PerfSampler::BINS];
        uint32_t cpuHist[PerfSampler::BINS];
        uint32_t count;
        double sumFrameMs;
        double sumCpuMs;
        float maxFrameMs;
        uint32_t hitches;
        double gpuSumMs;
        uint32_t gpuFrames;
        bool stall;
    };

    struct PerfReason {
        char name[32];
        double seconds;
    };

    struct PerfState {
        bool sampling = false;
        bool setByGame = false;
        // after an off with a reason (or after sampling), the seconds still count as excluded
        bool clockRunning = false;
        int windowSeconds = PerfSampler::DEFAULT_WINDOW_SECONDS;

        // the sampler's clock: steady_clock seconds, or the tests' own
        bool virtualClock = false;
        double virtualNow = 0.0;
        bool secondOpen = false;
        double secondStart = 0.0;
        uint32_t secondFramesAll = 0;

        // what the current second saw
        bool paused = false;
        bool hidden = false;
        bool pausedInSecond = false;
        bool gameOffInSecond = false;
        double resumeUntil = -1.0;
        double loadedUntil = -1.0;

        double lastCpuMs = -1.0;
        double watchUntil = -1.0;
        float cpuRingMs[PERF_CPU_RING] = {};
        uint32_t cpuRingFrames[PERF_CPU_RING] = {};
        int cpuRingHead = 0;
        int cpuRingCount = 0;

        PerfWindow window = {};
        PerfPending pending = {};
        double excluded[PERF_CAUSE_COUNT] = {};
        PerfReason reasons[PerfSampler::MAX_REASONS] = {};
        int reasonCount = 0;
        double otherReasonSeconds = 0.0;
        // the reason of the last off: 0..7, MAX_REASONS for "other", -1 none
        int currentReason = -1;

        bool (*loadingProbe)() = nullptr;
        void (*windowWriter)(const PerfSummary&) = nullptr;
    };

    PerfState& perfState(){
        static PerfState s;
        return s;
    }

    double perfSecondsOf(std::chrono::steady_clock::time_point t){
        return std::chrono::duration<double>(t.time_since_epoch()).count();
    }

    double perfNow(const PerfState& s){
        return s.virtualClock ? s.virtualNow : perfSecondsOf(std::chrono::steady_clock::now());
    }

    bool perfWatching(const PerfState& s, double now){
        return s.watchUntil >= 0.0 && now < s.watchUntil;
    }

    void perfClearWindow(PerfState& s){
        s.window = PerfWindow();
        s.pending = PerfPending();
        for (double& seconds : s.excluded)
            seconds = 0.0;
        for (PerfReason& reason : s.reasons)
            reason = PerfReason();
        s.reasonCount = 0;
        s.otherReasonSeconds = 0.0;
        s.currentReason = -1;
    }

    // the p-th value of a histogram, interpolated linearly in its bin
    double perfPercentile(const uint32_t* hist, uint64_t total, double p, double maxMs){
        if (total == 0)
            return 0.0;
        const double rank = p * (double)total;
        double cumulative = 0.0;
        for (int b = 0; b < PerfSampler::BINS; b++){
            const double count = hist[b];
            if (count > 0.0 && cumulative + count > rank){
                const double low = PerfSampler::binEdgeMs(b);
                const double high = b + 1 < PerfSampler::BINS ? PerfSampler::binEdgeMs(b + 1) : std::max(maxMs, (double)PerfSampler::binEdgeMs(b));
                const double fraction = std::min(1.0, (rank - cumulative + 0.5) / count);
                return std::min(maxMs > 0.0 ? std::max(maxMs, low) : high, low + fraction * (high - low));
            }
            cumulative += count;
        }
        return maxMs;
    }

    bool perfLoadingNow(const PerfState& s){
        return s.loadingProbe ? s.loadingProbe() : !Engine::areScenesLoaded(false);
    }

    void perfDiscard(PerfState& s, PerfCause cause, double seconds){
        s.excluded[cause] += seconds;
        if (cause == PERF_CAUSE_GAME && s.currentReason >= 0){
            if (s.currentReason < PerfSampler::MAX_REASONS)
                s.reasons[s.currentReason].seconds += seconds;
            else
                s.otherReasonSeconds += seconds;
        }
    }

    void perfCommit(PerfState& s, double seconds, double cpuDeltaMs){
        PerfWindow& w = s.window;
        const PerfPending& p = s.pending;
        if (p.count > 0){
            for (int b = 0; b < PerfSampler::BINS; b++){
                w.frameHist[b] += p.frameHist[b];
                w.cpuHist[b] += p.cpuHist[b];
            }
        }
        w.frames += (uint64_t)p.count;
        w.seconds += seconds;
        w.sumFrameMs += p.sumFrameMs;
        w.sumCpuMs += p.sumCpuMs;
        w.maxFrameMs = std::max(w.maxFrameMs, p.maxFrameMs);
        w.hitches += p.hitches;
        w.gpuSumMs += p.gpuSumMs;
        w.gpuFrames += p.gpuFrames;
        if (cpuDeltaMs >= 0.0 && p.count > 0){
            w.threadCpuMs += cpuDeltaMs;
            w.threadFrames += (uint64_t)p.count;
        }
        const int thermal = std::clamp((int)Device::getThermalState(), 0, 3);
        w.thermal[thermal] += seconds;
        const DeviceTier tier = Device::getTier();
        if (w.haveTier && tier != w.tier)
            w.tierChanges++;
        w.haveTier = true;
        w.tier = tier;
    }

}

void PerfSampler::setSampling(bool on, std::string_view reason){
    PerfState& s = perfState();
    if (on){
        if (!s.sampling){
            s.sampling = true;
            s.clockRunning = true;
            // on before any frame of this second: the second is gameplay whole
            if (s.secondFramesAll == 0)
                s.gameOffInSecond = false;
            // the thread clock's first reading: the next tick's delta starts here
            if (ThreadCpuClock::isAvailable())
                s.lastCpuMs = ThreadCpuClock::nowMs();
        }
        active = true;
        return;
    }
    if (s.sampling){
        s.sampling = false;
        s.gameOffInSecond = true;
    }
    s.currentReason = -1;
    if (!reason.empty()){
        const std::string_view name = reason.substr(0, sizeof(PerfReason::name) - 1);
        int found = -1;
        for (int i = 0; i < s.reasonCount; i++){
            if (name == s.reasons[i].name)
                found = i;
        }
        if (found < 0 && s.reasonCount < MAX_REASONS){
            found = s.reasonCount++;
            std::memcpy(s.reasons[found].name, name.data(), name.size());
            s.reasons[found].name[name.size()] = '\0';
        }
        s.currentReason = found >= 0 ? found : MAX_REASONS;
        // the excluded time is counted from here, even before the first gameplay second
        s.clockRunning = true;
        active = true;
    }
}

bool PerfSampler::isSampling(){
    return perfState().sampling;
}

bool PerfSampler::wasSetByGame(){
    return perfState().setByGame;
}

void PerfSampler::markSetByGame(){
    perfState().setByGame = true;
}

void PerfSampler::reset(){
    PerfState& s = perfState();
    perfClearWindow(s);
    s.secondOpen = false;
    // off, a new window counts nothing until sampling starts again
    s.clockRunning = s.sampling;
}

void PerfSampler::resetSession(){
    PerfState& s = perfState();
    perfClearWindow(s);
    s.sampling = false;
    s.setByGame = false;
    s.clockRunning = false;
    s.windowSeconds = DEFAULT_WINDOW_SECONDS;
    s.virtualClock = false;
    s.virtualNow = 0.0;
    s.secondOpen = false;
    s.paused = s.hidden = s.pausedInSecond = s.gameOffInSecond = false;
    s.resumeUntil = s.loadedUntil = -1.0;
    s.lastCpuMs = -1.0;
    s.watchUntil = -1.0;
    s.cpuRingHead = s.cpuRingCount = 0;
    s.loadingProbe = nullptr;
    active = false;
}

void PerfSampler::setWindowWriter(void (*writer)(const PerfSummary&)){
    perfState().windowWriter = writer;
}

void PerfSampler::setWindowSeconds(int seconds){
    perfState().windowSeconds = std::clamp(seconds, 30, 3600);
}

int PerfSampler::getWindowSeconds(){
    return perfState().windowSeconds;
}

double PerfSampler::mainThreadCpuMsLastSecond(){
    const PerfState& s = perfState();
    if (s.cpuRingCount == 0)
        return -1.0;
    const int newest = (s.cpuRingHead + PERF_CPU_RING - 1) % PERF_CPU_RING;
    return s.cpuRingFrames[newest] ? (double)s.cpuRingMs[newest] / (double)s.cpuRingFrames[newest] : -1.0;
}

double PerfSampler::mainThreadCpuMsOver(double seconds){
    const PerfState& s = perfState();
    const int count = std::min(s.cpuRingCount, std::max(1, (int)std::ceil(seconds)));
    double ms = 0.0;
    uint64_t frames = 0;
    for (int i = 0; i < count; i++){
        const int index = (s.cpuRingHead + PERF_CPU_RING - 1 - i) % PERF_CPU_RING;
        ms += s.cpuRingMs[index];
        frames += s.cpuRingFrames[index];
    }
    return frames ? ms / (double)frames : -1.0;
}

void PerfSampler::watchThreadCpu(){
    if (!ThreadCpuClock::isAvailable())
        return;
    PerfState& s = perfState();
    const double now = perfNow(s);
    if (!perfWatching(s, now) && !s.sampling)
        s.lastCpuMs = ThreadCpuClock::nowMs();
    s.watchUntil = now + PERF_WATCH_SECONDS;
    active = true;
}

void PerfSampler::notePaused(bool paused){
    PerfState& s = perfState();
    if (paused){
        s.paused = true;
        s.pausedInSecond = true;
    }else if (s.paused){
        s.paused = false;
        // resumed before any frame of this second: it is the resume's, not the pause's
        if (s.secondFramesAll == 0 && !s.hidden)
            s.pausedInSecond = false;
        noteResumed();
    }
}

void PerfSampler::noteHidden(bool hidden){
    PerfState& s = perfState();
    if (hidden){
        s.hidden = true;
        s.pausedInSecond = true;
    }else if (s.hidden){
        s.hidden = false;
        if (s.secondFramesAll == 0 && !s.paused)
            s.pausedInSecond = false;
        noteResumed();
    }
}

void PerfSampler::noteResumed(){
    PerfState& s = perfState();
    s.resumeUntil = perfNow(s) + 2.0;
}

void PerfSampler::noteSceneLoaded(){
    PerfState& s = perfState();
    s.loadedUntil = perfNow(s) + 1.0;
}

int PerfSampler::binOf(float ms){
    if (!(ms >= 0.0f))
        return 0;
    if (ms < 50.0f)
        return std::min(99, (int)(ms * 2.0f + 0.5f));
    if (ms < 250.0f)
        return std::min(199, 100 + (int)((ms - 50.0f) * 0.5f));
    return 200;
}

float PerfSampler::binEdgeMs(int bin){
    // below 50 ms the bins are centred on multiples of 0.5 ms, so a percentile is within 0.25 ms
    if (bin <= 0)
        return 0.0f;
    if (bin < 100)
        return (float)bin * 0.5f - 0.25f;
    if (bin < 200)
        return 50.0f + 2.0f * (float)(bin - 100);
    return 250.0f;
}

namespace {

    void perfTick(PerfState& s, double now, double seconds){
        const double start = s.secondStart;
        const bool watching = perfWatching(s, now);
        double cpuDelta = -1.0;
        if ((s.sampling || watching) && ThreadCpuClock::isAvailable()){
            const double cpu = ThreadCpuClock::nowMs();
            if (cpu >= 0.0 && s.lastCpuMs >= 0.0)
                cpuDelta = cpu - s.lastCpuMs;
            s.lastCpuMs = cpu;
            // one second measured whole (a longer gap is a stall or the background)
            if (cpuDelta >= 0.0 && seconds < 1.5){
                s.cpuRingMs[s.cpuRingHead] = (float)cpuDelta;
                s.cpuRingFrames[s.cpuRingHead] = s.secondFramesAll;
                s.cpuRingHead = (s.cpuRingHead + 1) % PERF_CPU_RING;
                s.cpuRingCount = std::min(s.cpuRingCount + 1, PERF_CPU_RING);
            }
        }else{
            s.lastCpuMs = -1.0;
        }

        if (s.clockRunning){
            PerfCause cause = PERF_CAUSE_COUNT;
            if (s.pausedInSecond || s.paused || s.hidden)
                cause = PERF_CAUSE_PAUSED;
            else if (s.pending.stall || seconds >= 1.5)
                cause = PERF_CAUSE_STALL;
            else if (s.gameOffInSecond || !s.sampling)
                cause = PERF_CAUSE_GAME;
            else if (start < s.loadedUntil || perfLoadingNow(s))
                cause = PERF_CAUSE_LOADING;
            else if (start < s.resumeUntil)
                cause = PERF_CAUSE_RESUME;
            if (cause == PERF_CAUSE_COUNT)
                perfCommit(s, seconds, cpuDelta);
            else
                perfDiscard(s, cause, seconds);
        }

        if (s.pending.count > 0){
            std::memset(s.pending.frameHist, 0, sizeof(s.pending.frameHist));
            std::memset(s.pending.cpuHist, 0, sizeof(s.pending.cpuHist));
        }
        s.pending.count = 0;
        s.pending.sumFrameMs = s.pending.sumCpuMs = 0.0;
        s.pending.maxFrameMs = 0.0f;
        s.pending.hitches = 0;
        s.pending.gpuSumMs = 0.0;
        s.pending.gpuFrames = 0;
        s.pending.stall = false;
        s.pausedInSecond = s.paused || s.hidden;
        s.gameOffInSecond = !s.sampling;
        s.secondFramesAll = 0;
        s.secondStart = s.virtualClock ? now : start + std::floor(seconds);

        if (s.window.seconds >= (double)s.windowSeconds && (PerfSampler::onWindow.hasSubscribers() || s.windowWriter)){
            const PerfSummary summary = PerfSampler::summary();
            PerfSampler::reset();
            s.secondOpen = true;
            s.secondStart = s.virtualClock ? now : start + std::floor(seconds);
            if (s.windowWriter)
                s.windowWriter(summary);
            PerfSampler::onWindow.call(summary);
        }
    }

}

void PerfSampler::onFrame(const FrameTimeSample& sample, std::chrono::steady_clock::time_point now){
    PerfState& s = perfState();
    const double t = s.virtualClock ? s.virtualNow : perfSecondsOf(now);
    if (!s.secondOpen){
        s.secondOpen = true;
        s.secondStart = t;
        s.secondFramesAll = 0;
    }
    s.secondFramesAll++;
    if (s.sampling && (sample.flags & FrameTimes::FLAG_HOST_WORK) == 0){
        PerfPending& p = s.pending;
        const float frameMs = sample.frameMs;
        const int frameBin = binOf(frameMs);
        const int cpuBin = binOf(sample.cpuMs);
        p.frameHist[frameBin]++;
        p.cpuHist[cpuBin]++;
        p.count++;
        p.sumFrameMs += frameMs;
        p.sumCpuMs += sample.cpuMs;
        p.maxFrameMs = std::max(p.maxFrameMs, frameMs);
        if (frameMs >= 1000.0f)
            p.stall = true;
        else if (frameMs >= 250.0f)
            p.hitches++;
        if (sample.gpuMs >= 0.0f){
            p.gpuSumMs += sample.gpuMs;
            p.gpuFrames++;
        }
    }
    if (!s.virtualClock && t - s.secondStart >= 1.0){
        perfTick(s, t, std::floor(t - s.secondStart));
        if (!s.sampling && !s.clockRunning && !perfWatching(s, t)){
            active = false;
            s.secondOpen = false;
        }
    }
}

void PerfSampler::onGpuMs(double gpuMs){
    PerfState& s = perfState();
    if (!s.sampling || gpuMs < 0.0)
        return;
    s.pending.gpuSumMs += gpuMs;
    s.pending.gpuFrames++;
}

void PerfSampler::tickForTest(double seconds){
    PerfState& s = perfState();
    if (!s.virtualClock){
        s.virtualClock = true;
        s.virtualNow = 0.0;
        s.secondStart = 0.0;
        s.secondOpen = true;
    }
    s.virtualNow += seconds;
    perfTick(s, s.virtualNow, seconds);
}

void PerfSampler::setLoadingProbeForTest(bool (*probe)()){
    perfState().loadingProbe = probe;
}

PerfSummary PerfSampler::summary(bool histogram){
    const PerfState& s = perfState();
    const PerfWindow& w = s.window;
    PerfSummary out;
    out.excludedPaused = s.excluded[PERF_CAUSE_PAUSED];
    out.excludedResume = s.excluded[PERF_CAUSE_RESUME];
    out.excludedLoading = s.excluded[PERF_CAUSE_LOADING];
    out.excludedStall = s.excluded[PERF_CAUSE_STALL];
    out.excludedGame = s.excluded[PERF_CAUSE_GAME];
    for (int i = 0; i < s.reasonCount; i++)
        out.excludedReasons.emplace_back(s.reasons[i].name, s.reasons[i].seconds);
    if (s.otherReasonSeconds > 0.0)
        out.excludedReasons.emplace_back("other", s.otherReasonSeconds);
    out.threadClock = ThreadCpuClock::isAvailable();
    out.seconds = w.seconds;
    out.frames = w.frames;
    if (w.seconds <= 0.0)
        return out;

    out.fpsAvg = (double)w.frames / w.seconds;
    out.frameMsP50 = perfPercentile(w.frameHist, w.frames, 0.50, w.maxFrameMs);
    out.frameMsP90 = perfPercentile(w.frameHist, w.frames, 0.90, w.maxFrameMs);
    out.frameMsP99 = perfPercentile(w.frameHist, w.frames, 0.99, w.maxFrameMs);
    out.frameMsMax = w.maxFrameMs;
    out.fpsP50 = out.frameMsP50 > 0.0 ? 1000.0 / out.frameMsP50 : 0.0;
    out.fpsP10 = out.frameMsP90 > 0.0 ? 1000.0 / out.frameMsP90 : 0.0;
    out.cpuMsAvg = w.frames ? w.sumCpuMs / (double)w.frames : 0.0;
    out.cpuMsP90 = perfPercentile(w.cpuHist, w.frames, 0.90, 0.0);
    out.mainThreadCpuMs = (out.threadClock && w.threadFrames) ? w.threadCpuMs / (double)w.threadFrames : -1.0;
    out.gpuFrames = w.gpuFrames;
    out.gpuMsAvg = w.gpuFrames ? w.gpuSumMs / (double)w.gpuFrames : 0.0;
    out.hitches = w.hitches;
    for (int i = 0; i < 4; i++)
        out.thermal[i] = w.thermal[i];
    out.tier = w.haveTier ? w.tier : Device::getTier();
    out.tierChanges = w.tierChanges;
    out.gpu = Device::getGpuName();
    out.driver = SystemRender::getDriverVersion();
    out.quirks = GpuQuirks::activeNames();
    out.backend = SystemRender::getBackendName();
    switch (Engine::getPlatform()){
        case Platform::Android: out.platform = "android"; break;
        case Platform::iOS: out.platform = "ios"; break;
        case Platform::MacOS: out.platform = "macos"; break;
        case Platform::Web: out.platform = "web"; break;
        case Platform::Windows: out.platform = "windows"; break;
        case Platform::Linux: out.platform = "linux"; break;
    }
    if (System::hasInstance()){
        out.version = System::instance().getAppVersion();
        out.buildId = System::instance().getAppBuild();
    }
    if (histogram){
        out.histogramEdgesMs.reserve(BINS);
        out.histogramCounts.reserve(BINS);
        for (int b = 0; b < BINS; b++){
            out.histogramEdgesMs.push_back(binEdgeMs(b));
            out.histogramCounts.push_back(w.frameHist[b]);
        }
    }
    return out;
}

std::string PerfSampler::toJson(const PerfSummary& summary){
    using nlohmann::json;
    auto round3 = [](double value){ return std::round(value * 1000.0) / 1000.0; };
    json reasons = json::object();
    for (const auto& [name, seconds] : summary.excludedReasons)
        reasons[name] = round3(seconds);
    json excluded = {{"paused", round3(summary.excludedPaused)}, {"resume", round3(summary.excludedResume)},
                     {"loading", round3(summary.excludedLoading)}, {"stall", round3(summary.excludedStall)},
                     {"game", round3(summary.excludedGame)}, {"reasons", reasons}};
    json out = {{"seconds", round3(summary.seconds)}, {"frames", summary.frames}, {"excluded", excluded}};
    if (summary.seconds <= 0.0)
        return out.dump();
    out["fps"] = {{"avg", round3(summary.fpsAvg)}, {"p50", round3(summary.fpsP50)}, {"p10", round3(summary.fpsP10)}};
    out["frame_ms"] = {{"p50", round3(summary.frameMsP50)}, {"p90", round3(summary.frameMsP90)}, {"p99", round3(summary.frameMsP99)},
                       {"max", round3(summary.frameMsMax)}};
    out["cpu_ms"] = {{"avg", round3(summary.cpuMsAvg)}, {"p90", round3(summary.cpuMsP90)}};
    out["main_thread_cpu_ms"] = summary.mainThreadCpuMs >= 0.0 ? json(round3(summary.mainThreadCpuMs)) : json(nullptr);
    out["main_thread_cpu"] = summary.threadClock ? "thread_clock" : "unavailable";
    if (summary.gpuFrames > 0)
        out["gpu_ms"] = {{"avg", round3(summary.gpuMsAvg)}, {"frames", summary.gpuFrames}};
    out["hitches"] = summary.hitches;
    out["thermal"] = {{"nominal", round3(summary.thermal[0])}, {"fair", round3(summary.thermal[1])},
                      {"serious", round3(summary.thermal[2])}, {"critical", round3(summary.thermal[3])}};
    out["tier"] = DeviceNames::tier(summary.tier);
    out["tier_changes"] = summary.tierChanges;
    out["gpu"] = summary.gpu;
    out["driver"] = summary.driver;
    out["quirks"] = summary.quirks;
    out["backend"] = summary.backend;
    out["platform"] = summary.platform;
    out["version"] = summary.version;
    out["build_id"] = summary.buildId;
    if (!summary.histogramCounts.empty())
        out["histogram"] = {{"edges_ms", summary.histogramEdgesMs}, {"counts", summary.histogramCounts}};
    return out.dump();
}
