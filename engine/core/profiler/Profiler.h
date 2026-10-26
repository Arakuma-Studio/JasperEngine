// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef Profiler_h
#define Profiler_h

// The CPU profiler (spec S15 R13-R20). Code marks scopes with the macros of Profile.h; each
// thread writes 16-byte events into its own lock-free ring (only the owner writes, the main
// thread drains), so a scope costs one relaxed load and a branch while the profiler is off and
// two clock reads and two ring writes while it records. The main thread drains every ring at
// frameMark (JE_PROFILE_FRAME), splits the scopes still open at the boundary and keeps per
// frame: a timeline of every thread's intervals, a summary (time per category, CPU and GPU
// time, monitors) for the last 600 frames, and the intervals of the newest 300 within a memory
// cap. Captures copy N frames for saving as Chrome Trace Event JSON (.jetrace.json), which
// Perfetto and chrome://tracing open, and which the editor loads back.
//
// Compiled only with JASPERENGINE_PROFILER (the editor, Debug and Development Build games):
// a release game has none of it, and the macros evaluate nothing.

#include "Export.h"

#if JASPERENGINE_TRACY
#include "tracy/Tracy.hpp"
#include <cstring>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::profiler {

    // the CPU Usage chart's categories (R16)
    enum class Category : uint8_t {
        Rendering,
        Scripts,
        Physics,
        Animation,
        UI,
        Loading,
        Jobs,
        Wait,
        Other,
        Count
    };
    constexpr size_t CATEGORY_COUNT = (size_t)Category::Count;
    JASPERENGINE_API const char* categoryName(Category category);

    // ---- recording

    struct ScopeSite {
        const char* name;
        const char* file;
        uint32_t line;
        std::atomic<uint32_t> id{0};    // the interned name, set at the first use
    };

    JASPERENGINE_API extern std::atomic<bool> enabledFlag;
    inline bool isEnabled(){ return enabledFlag.load(std::memory_order_relaxed); }
    // Starts or stops recording. The first frame after starting is partial (it began before).
    JASPERENGINE_API void setEnabled(bool enabled);

    JASPERENGINE_API uint32_t registerSite(ScopeSite& site);
    // a name made at run time (Lua functions, asset names): one id per distinct string
    JASPERENGINE_API uint32_t internName(std::string_view name);
    JASPERENGINE_API std::string nameOf(uint32_t nameId);
    // the category of a name: the standard scopes have theirs (R16), others inherit their parent's
    JASPERENGINE_API void setNameCategory(uint32_t nameId, Category category);

    // the calling thread's events (false: the ring had no room, nothing was recorded)
    JASPERENGINE_API bool beginScope(uint32_t nameId);
    JASPERENGINE_API void endScope();

    class Scope {
    public:
        explicit Scope(ScopeSite& site){
            if (isEnabled()){
                uint32_t id = site.id.load(std::memory_order_relaxed);
                if (id == 0)
                    id = registerSite(site);
                begun = beginScope(id);
            }
        }
        ~Scope(){
            if (begun)
                endScope();
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        bool begun = false;
    };

    class DynScope {
    public:
        DynScope(std::string_view name, const char* file, uint32_t line, const char* function)
        #if JASPERENGINE_TRACY
            : tracyZone(line, file, std::strlen(file), function, std::strlen(function), name.data(), name.size(), true)
        #endif
        {
            if (isEnabled())
                begun = beginScope(internName(name));
        }
        ~DynScope(){
            if (begun)
                endScope();
        }
        DynScope(const DynScope&) = delete;
        DynScope& operator=(const DynScope&) = delete;
    private:
        bool begun = false;
        #if JASPERENGINE_TRACY
        ::tracy::ScopedZone tracyZone;
        #endif
    };

    // Names the calling thread in timelines (JE_PROFILE_THREAD). The engine's thread names
    // ("je-worker-3") read as "Job Worker #3"; a thread that never names itself is
    // "Thread <os id>".
    JASPERENGINE_API void setThreadName(const char* name);
    // The ring of each thread from now: 256 KiB to 8 MiB (16 bytes an event; default 1 MiB).
    JASPERENGINE_API void setRingBytes(size_t bytes);
    // Full detail of at most `frames` (≤ 300) newest frames within `bytes` (default 64 MiB).
    JASPERENGINE_API void setDetailLimits(size_t frames, size_t bytes);

    // JE_PROFILE_FRAME: the main thread ends the frame and starts the next (the Engine's
    // systemDraw in a game, every rendered loop of the editor). Drains every ring.
    JASPERENGINE_API void frameMark();
    // the backend and device names written in captures, and the GPU timer's semantics
    JASPERENGINE_API void setDeviceInfo(const std::string& backend, const std::string& device);
    JASPERENGINE_API void setGpuSemantics(const std::string& semantics);

    // Deep Profile Lua: an automatic scope per Lua callback
    JASPERENGINE_API extern std::atomic<bool> deepLuaFlag;
    inline bool isDeepLua(){ return deepLuaFlag.load(std::memory_order_relaxed); }
    JASPERENGINE_API void setDeepLua(bool deep);

    // ---- monitors (R18): named per-frame series

    enum class MonitorUnit : uint8_t { Quantity, Memory, TimeMs, Percent };
    JASPERENGINE_API const char* monitorUnitName(MonitorUnit unit);

    // JE_PROFILE_COUNTER: a value of the current frame (any thread; the last one in a frame wins)
    JASPERENGINE_API void counter(const char* name, double value);
    JASPERENGINE_API void counter(const char* name, double value, MonitorUnit unit);
    // a monitor looked up once (the engine's own, set every frame): no lock, no name lookup
    using MonitorHandle = void*;
    JASPERENGINE_API MonitorHandle monitorHandle(const char* name, MonitorUnit unit);
    JASPERENGINE_API void counter(MonitorHandle monitor, double value);

    struct MonitorInfo {
        std::string name;
        MonitorUnit unit = MonitorUnit::Quantity;
        double latest = 0.0;
        double average = 0.0;
        double minimum = 0.0;
        double maximum = 0.0;
        size_t samples = 0;
    };
    JASPERENGINE_API std::vector<MonitorInfo> monitors();
    // the last `count` samples of a monitor, oldest first (empty for an unknown name)
    JASPERENGINE_API std::vector<double> monitorSeries(const std::string& name, size_t count);
    JASPERENGINE_API bool monitorInfo(const std::string& name, MonitorInfo& out);
    // the monitors in index order (Frame::monitorValues)
    JASPERENGINE_API std::vector<std::string> monitorNames();

    // ---- frames

    struct Interval {
        uint64_t startNs = 0;
        uint64_t endNs = 0;
        uint32_t nameId = 0;
        uint16_t depth = 0;
        uint16_t flags = 0;         // INTERVAL_*
    };
    static constexpr uint16_t INTERVAL_CONTINUED = 1u << 0;  // began in an earlier frame (split)
    static constexpr uint16_t INTERVAL_CONTINUES = 1u << 1;  // goes on in the next frame (split)

    struct ThreadTimeline {
        uint32_t thread = 0;        // ThreadInfo::id
        std::vector<Interval> intervals;    // by start time
    };

    struct GpuPassSample {
        std::string label;
        uint32_t viewTag = 0;
        uint64_t commit = 0;
        double gpuMs = 0.0;
        bool valid = false;
        double cpuMs = 0.0;         // recording its commands
        uint32_t draws = 0;
    };

    // the GPU times of one commit of `frame`, when they arrive (the pass recorder); the game
    // frame's add up to its gpuMs
    JASPERENGINE_API void recordGpuFrame(uint64_t frame, uint32_t viewTag, uint64_t commit, const GpuPassSample* passes, size_t count,
                                         double frameGpuMs, bool frameValid, bool gameFrame);

    struct Frame {
        uint64_t index = 0;         // FrameTimes frame serial
        uint64_t startNs = 0;
        uint64_t endNs = 0;
        double cpuMs = 0.0;         // the main thread's work (FrameTimes cpuMs)
        double gpuMs = -1.0;        // the game frame's GPU time (< 0: none)
        double categoryMs[CATEGORY_COUNT] = {};   // main-thread self time per category
        uint32_t events = 0;
        uint32_t dropped = 0;       // events lost to a full ring
        bool partial = false;       // the first frame after the profiler started
        bool hasDetail = false;     // the timelines are still kept
        std::vector<ThreadTimeline> threads;
        std::vector<GpuPassSample> gpu;
        // every monitor's value at the frame's end, by monitor index (NaN: not set yet);
        // monitorNames() (a capture's: captureMonitorNames) names them
        std::vector<float> monitorValues;
        double frameMs() const { return (double)(endNs - startNs) / 1.0e6; }
    };

    struct ThreadInfo {
        uint32_t id = 0;
        std::string name;
        uint64_t osId = 0;
        bool main = false;
    };

    // frames recorded so far (≤ 600), oldest first; main thread
    JASPERENGINE_API size_t frameCount();
    JASPERENGINE_API const Frame* frameAt(size_t index);
    JASPERENGINE_API const Frame* findFrame(uint64_t frameIndex);
    JASPERENGINE_API std::vector<ThreadInfo> threads();
    JASPERENGINE_API uint32_t mainThreadId();
    JASPERENGINE_API void clearHistory();
    // the measured cost of a scope pair while recording (calibrated at setEnabled(true)), ns
    JASPERENGINE_API double scopePairCostNs();

    // ---- views of a frame

    struct TreeRow {
        std::string name;
        uint32_t depth = 0;
        double totalMs = 0.0;
        double selfMs = 0.0;
        uint32_t calls = 0;
        std::string path;           // "Frame/Draw/Shadows": unique per row
    };
    // the names of a frame's intervals (a loaded capture has its own; null: the live names)
    using NameLookup = std::function<std::string(uint32_t nameId)>;

    // a thread's scope tree: children merged by name under the same parent, in first-call order
    JASPERENGINE_API std::vector<TreeRow> buildTree(const Frame& frame, uint32_t thread, const NameLookup& lookup = nullptr);

    struct FlatRow {
        std::string name;
        double totalMs = 0.0;       // the outermost calls only (recursion counts once)
        double selfMs = 0.0;
        uint32_t calls = 0;
        std::vector<std::pair<uint32_t, double>> perThread;   // thread id, total ms
    };
    // every name over every thread, by total time
    JASPERENGINE_API std::vector<FlatRow> buildFlat(const Frame& frame, const NameLookup& lookup = nullptr);

    struct ThreadBusy {
        uint32_t thread = 0;
        double busyMs = 0.0;        // covered by top-level scopes
        double busyPercent = 0.0;
        uint32_t scopes = 0;
    };
    JASPERENGINE_API std::vector<ThreadBusy> buildThreadBusy(const Frame& frame);

    // ---- captures (R20, R38)

    using CaptureId = uint32_t;
    struct CaptureInfo {
        CaptureId id = 0;
        uint32_t framesWanted = 0;
        uint32_t framesRecorded = 0;
        bool complete = false;
        bool loaded = false;        // read from a file
        std::string path;           // saved to / loaded from
        std::string backend;
        std::string device;
    };
    // Records the next `frames` frames (≤ 600; enables the profiler until they are in, then
    // restores the previous state). 0 when one is already running.
    JASPERENGINE_API CaptureId startCapture(uint32_t frames);
    JASPERENGINE_API bool captureInfo(CaptureId id, CaptureInfo& out);
    JASPERENGINE_API std::vector<CaptureInfo> captures();
    JASPERENGINE_API CaptureId activeCapture();
    JASPERENGINE_API void cancelCapture();
    // the capture's frames (complete or not); null for an unknown id
    JASPERENGINE_API const std::vector<Frame>* captureFrames(CaptureId id);
    JASPERENGINE_API std::vector<ThreadInfo> captureThreads(CaptureId id);
    JASPERENGINE_API std::vector<std::string> captureMonitorNames(CaptureId id);
    // a complete capture of the frames the history still has in full (the Profiler's Save)
    JASPERENGINE_API CaptureId captureFromHistory();
    JASPERENGINE_API std::string captureNameOf(CaptureId id, uint32_t nameId);
    // the capture as .jetrace.json text
    JASPERENGINE_API std::string captureJson(CaptureId id);
    JASPERENGINE_API bool saveCapture(CaptureId id, const std::string& path, std::string* error = nullptr);
    // a .jetrace.json file as a capture (refuses a newer jasper.version with a message)
    JASPERENGINE_API CaptureId loadCapture(const std::string& path, std::string& error);
    JASPERENGINE_API void dropCapture(CaptureId id);

}

#endif /* Profiler_h */
