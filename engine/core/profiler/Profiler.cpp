// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Profiler.h"

#if JASPERENGINE_PROFILER

#include "FrameTimes.h"
#include "GpuMemory.h"
#include "io/AtomicFile.h"

#include "json.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(__EMSCRIPTEN__)
    #if defined(_MSC_VER)
        #include <intrin.h>
    #else
        #include <x86intrin.h>
    #endif
#endif

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__EMSCRIPTEN__)
    #include <emscripten.h>
    #include <pthread.h>
#elif defined(__APPLE__)
    #include <pthread.h>
#elif defined(__linux__) || defined(__ANDROID__)
    #include <sys/syscall.h>
    #include <unistd.h>
#endif

using namespace jasperengine;
using namespace jasperengine::profiler;

std::atomic<bool> profiler::enabledFlag{false};

namespace {

    // ---- clock: events take the CPU's counter (a few cycles; a steady_clock read is a vDSO call
    // that costs tens of ns on a virtual machine), converted to steady_clock nanoseconds when
    // drained, with a factor the main thread refines every frame

    uint64_t steadyNs(){
#if defined(__EMSCRIPTEN__)
        return (uint64_t)(emscripten_get_now() * 1.0e6);
#else
        return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
    }

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(__EMSCRIPTEN__)
    inline uint64_t readTicks(){ return __rdtsc(); }
    #define JE_PROFILER_COUNTER_CLOCK 1
#elif defined(__aarch64__) && !defined(_MSC_VER) && !defined(__EMSCRIPTEN__)
    inline uint64_t readTicks(){
        uint64_t value;
        asm volatile("mrs %0, cntvct_el0" : "=r"(value));
        return value;
    }
    #define JE_PROFILER_COUNTER_CLOCK 1
#else
    inline uint64_t readTicks(){ return steadyNs(); }
    #define JE_PROFILER_COUNTER_CLOCK 0
#endif

    struct TickClock {
        uint64_t baseTicks = 0;
        uint64_t baseNs = 0;
        std::atomic<double> nsPerTick{1.0};   // refined by the main thread, read by any
        TickClock(){
            baseTicks = readTicks();
            baseNs = steadyNs();
#if JE_PROFILER_COUNTER_CLOCK
            // a first factor over 2 ms; frameMark refines it over the whole run
            const uint64_t until = baseNs + 2000000;
            uint64_t ns = baseNs;
            while (ns < until)
                ns = steadyNs();
            const uint64_t ticks = readTicks();
            if (ticks > baseTicks)
                nsPerTick.store((double)(ns - baseNs) / (double)(ticks - baseTicks), std::memory_order_relaxed);
#endif
        }
        uint64_t toNs(uint64_t ticks) const{
#if JE_PROFILER_COUNTER_CLOCK
            return baseNs + (uint64_t)((double)(int64_t)(ticks - baseTicks) * nsPerTick.load(std::memory_order_relaxed));
#else
            return ticks;
#endif
        }
        void refine(){
#if JE_PROFILER_COUNTER_CLOCK
            const uint64_t ticks = readTicks();
            const uint64_t ns = steadyNs();
            if (ns > baseNs + 100000000 && ticks > baseTicks)
                nsPerTick.store((double)(ns - baseNs) / (double)(ticks - baseTicks), std::memory_order_relaxed);
#endif
        }
    };

    TickClock& tickClock(){
        static TickClock* clock = new TickClock();
        return *clock;
    }

    uint64_t nowNs(){
        return tickClock().toNs(readTicks());
    }

    uint64_t osThreadId(){
#if defined(_WIN32)
        return (uint64_t)GetCurrentThreadId();
#elif defined(__EMSCRIPTEN__)
        return (uint64_t)(uintptr_t)pthread_self();
#elif defined(__APPLE__)
        uint64_t id = 0;
        pthread_threadid_np(nullptr, &id);
        return id;
#elif defined(__linux__) || defined(__ANDROID__)
        return (uint64_t)syscall(SYS_gettid);
#else
        return 0;
#endif
    }

    // ---- names

    constexpr uint8_t CATEGORY_INHERIT = 0xFF;

    struct Names {
        std::mutex mutex;
        std::deque<std::string> strings{std::string("?")};     // id 0: none (stable addresses)
        std::unordered_map<std::string_view, uint32_t> ids;
        std::vector<uint8_t> categories{CATEGORY_INHERIT};
    };

    Names& names(){
        static Names* n = new Names();
        return *n;
    }

    // the standard scopes of R16 and their chart category
    Category standardCategory(std::string_view name){
        static const std::pair<const char*, Category> table[] = {
            {"Draw", Category::Rendering}, {"Commit", Category::Rendering}, {"Shadows", Category::Rendering},
            {"Depth prepass", Category::Rendering}, {"GBuffer", Category::Rendering}, {"Opaque", Category::Rendering},
            {"Transparent", Category::Rendering}, {"SSAO", Category::Rendering}, {"SSR", Category::Rendering},
            {"Bloom", Category::Rendering}, {"PostFX", Category::Rendering}, {"Render", Category::Rendering},
            {"Editor UI", Category::Rendering},
            {"Update", Category::Scripts}, {"Lua", Category::Scripts},
            {"FixedUpdate", Category::Physics}, {"Physics2D", Category::Physics}, {"Physics3D", Category::Physics},
            {"Animation", Category::Animation},
            {"UI Update", Category::UI}, {"HTML UI", Category::UI},
            {"Asset loading", Category::Loading}, {"Shader compile", Category::Loading},
            {"SokolCmdQueue execute", Category::Loading},
            {"Present/Wait", Category::Wait},
        };
        for (const auto& [text, category] : table){
            if (name == text)
                return category;
        }
        if (name.rfind("Wait: ", 0) == 0)
            return Category::Wait;
        return Category::Other;
    }

    uint32_t internLocked(Names& n, std::string_view name){
        auto it = n.ids.find(name);
        if (it != n.ids.end())
            return it->second;
        const uint32_t id = (uint32_t)n.strings.size();
        n.strings.emplace_back(name);
        n.ids.emplace(std::string_view(n.strings.back()), id);
        const Category category = standardCategory(name);
        n.categories.push_back(category == Category::Other ? CATEGORY_INHERIT : (uint8_t)category);
        return id;
    }

    uint8_t categoryOf(uint32_t nameId){
        Names& n = names();
        std::lock_guard<std::mutex> lock(n.mutex);
        return nameId < n.categories.size() ? n.categories[nameId] : CATEGORY_INHERIT;
    }

    // ---- per-thread rings

    constexpr uint32_t KIND_BEGIN = 0u;
    constexpr uint32_t KIND_END = 1u << 31;
    constexpr uint32_t NAME_MASK = ~KIND_END;

    struct Event {
        uint64_t ns;
        uint32_t nameKind;
        uint32_t aux;
    };
    static_assert(sizeof(Event) == 16, "16-byte events (S15 R14)");

    struct OpenScope {
        uint32_t nameId;
        uint64_t startNs;
        uint16_t flags;
    };

    struct ThreadBuffer {
        Event* events = nullptr;
        uint64_t capacity = 0;
        uint64_t mask = 0;
        alignas(64) std::atomic<uint64_t> head{0};     // the owner writes
        alignas(64) std::atomic<uint64_t> tail{0};     // the main thread reads
        std::atomic<uint32_t> dropped{0};
        std::atomic<bool> exited{false};
        uint32_t openDepth = 0;                        // owner only: recorded scopes not ended
        uint32_t id = 0;
        // the main thread's
        std::vector<OpenScope> stack;
        std::vector<Interval> intervals;
        ~ThreadBuffer(){ delete[] events; }
    };

    struct ThreadTable {
        std::mutex mutex;
        std::vector<ThreadBuffer*> buffers;            // live rings
        std::vector<ThreadInfo> infos;                 // every thread seen (history names them)
        uint32_t nextId = 1;
        uint32_t mainId = 0;
        size_t ringBytes = 1024 * 1024;
        uint64_t generation = 0;
    };

    ThreadTable& threadTable(){
        static ThreadTable* t = new ThreadTable();
        return *t;
    }

    struct ThreadLocal {
        ThreadBuffer* buffer = nullptr;
        std::string pendingName;                       // setThreadName before the first event
        ~ThreadLocal(){
            if (buffer)
                buffer->exited.store(true, std::memory_order_release);
        }
    };
    thread_local ThreadLocal threadLocal;

    std::string displayName(const std::string& raw){
        auto numbered = [&](const char* prefix, const char* label) -> std::string {
            const size_t length = std::strlen(prefix);
            if (raw.compare(0, length, prefix) == 0 && raw.size() > length)
                return std::string(label) + raw.substr(length);
            return std::string();
        };
        if (raw == "je-main") return "Main Thread";
        if (raw == "je-video") return "Video Decoder";
        if (raw == "je-log") return "Logger";
        std::string named = numbered("je-worker-", "Job Worker #");
        if (named.empty()) named = numbered("je-block-", "Job Blocking #");
        if (named.empty()) named = numbered("je-io-", "Asset Reader #");
        return named.empty() ? raw : named;
    }

    ThreadBuffer* createBuffer(){
        ThreadTable& t = threadTable();
        ThreadBuffer* buffer = new ThreadBuffer();
        std::lock_guard<std::mutex> lock(t.mutex);
        uint64_t capacity = 1;
        while (capacity * sizeof(Event) < t.ringBytes)
            capacity <<= 1;
        buffer->events = new Event[capacity];
        buffer->capacity = capacity;
        buffer->mask = capacity - 1;
        buffer->id = t.nextId++;
        ThreadInfo info;
        info.id = buffer->id;
        info.osId = osThreadId();
        info.name = threadLocal.pendingName.empty() ? "Thread " + std::to_string(info.osId) : displayName(threadLocal.pendingName);
        t.infos.push_back(info);
        t.buffers.push_back(buffer);
        t.generation++;
        threadLocal.buffer = buffer;
        return buffer;
    }

    // ---- monitors

    constexpr size_t MONITOR_SAMPLES = 600;

    struct Monitor {
        std::string name;
        MonitorUnit unit = MonitorUnit::Quantity;
        std::atomic<uint64_t> pendingBits{0};
        std::atomic<bool> pending{false};
        double ring[MONITOR_SAMPLES] = {};
        size_t head = 0;
        size_t count = 0;
        double latest = 0.0;
    };

    struct Monitors {
        std::mutex mutex;
        std::vector<std::unique_ptr<Monitor>> list;
        std::unordered_map<std::string, size_t> index;
    };

    Monitors& monitorTable(){
        static Monitors* m = new Monitors();
        return *m;
    }

    Monitor* findMonitor(const char* name, MonitorUnit unit, bool setUnit){
        Monitors& m = monitorTable();
        std::lock_guard<std::mutex> lock(m.mutex);
        auto it = m.index.find(name);
        if (it != m.index.end()){
            Monitor* monitor = m.list[it->second].get();
            if (setUnit)
                monitor->unit = unit;
            return monitor;
        }
        auto monitor = std::make_unique<Monitor>();
        monitor->name = name;
        monitor->unit = unit;
        m.index.emplace(monitor->name, m.list.size());
        m.list.push_back(std::move(monitor));
        return m.list.back().get();
    }

    // ---- frames and captures

    constexpr size_t HISTORY_FRAMES = 600;

    struct Capture {
        CaptureInfo info;
        std::vector<Frame> frames;
        std::vector<ThreadInfo> threads;
        std::vector<std::string> names;                 // a loaded capture's own names
        std::vector<std::string> monitorNames;          // its frames' monitorValues
        std::string semantics;
        bool previousEnabled = false;
        uint32_t graceFrames = 0;                       // after the last frame, for its GPU times
        gpumem::Report vram;
        bool hasVram = false;
    };

    struct PendingGpu {
        uint64_t frame = 0;
        uint32_t viewTag = 0;
        uint64_t commit = 0;
        std::vector<GpuPassSample> passes;
        double frameGpuMs = 0.0;
        bool frameValid = false;
        bool gameFrame = false;
    };

    void applyGpu(Frame& frame, const PendingGpu& gpu){
        for (const GpuPassSample& pass : gpu.passes){
            GpuPassSample sample = pass;
            sample.viewTag = gpu.viewTag;
            sample.commit = gpu.commit;
            frame.gpu.push_back(std::move(sample));
        }
        if (gpu.gameFrame && gpu.frameValid)
            frame.gpuMs = std::max(frame.gpuMs, 0.0) + gpu.frameGpuMs;
    }

    struct State {
        std::deque<Frame> history;
        size_t detailFrames = 300;
        size_t detailBytes = 64ull * 1024 * 1024;
        uint64_t lastMarkNs = 0;
        bool partialNext = true;
        std::vector<ThreadBuffer*> drained;             // the main thread's copy of the ring list
        uint64_t drainedGeneration = ~0ull;
        std::vector<std::unique_ptr<Capture>> captures;
        CaptureId nextCapture = 1;
        CaptureId active = 0;
        double pairCostNs = 0.0;
        std::string backend;
        std::string device;
        // GPU times of the frame still being made (a light frame's commit can resolve before
        // the frame ends, on Vulkan at 1,000+ frames a second): applied when it is marked
        std::vector<PendingGpu> pendingGpu;
    };

    State& state(){
        static State* s = new State();
        return *s;
    }

    Capture* findCapture(CaptureId id){
        for (auto& capture : state().captures){
            if (capture->info.id == id)
                return capture.get();
        }
        return nullptr;
    }

    size_t detailBytesOf(const Frame& frame){
        size_t bytes = 0;
        for (const ThreadTimeline& timeline : frame.threads)
            bytes += timeline.intervals.size() * sizeof(Interval);
        return bytes;
    }

    // drains one ring up to `until`; intervals go to buffer->intervals when `keep`
    void drainRing(ThreadBuffer* buffer, uint64_t until, bool keep){
        const uint64_t head = buffer->head.load(std::memory_order_acquire);
        uint64_t tail = buffer->tail.load(std::memory_order_relaxed);
        while (tail < head){
            const Event& event = buffer->events[tail & buffer->mask];
            const uint64_t eventNs = tickClock().toNs(event.ns);
            if (eventNs > until)
                break;   // the next frame's: a ring is in time order
            if ((event.nameKind & KIND_END) == 0){
                buffer->stack.push_back({event.nameKind & NAME_MASK, eventNs, 0});
            }else if (!buffer->stack.empty()){
                const OpenScope open = buffer->stack.back();
                buffer->stack.pop_back();
                if (keep){
                    Interval interval;
                    interval.startNs = open.startNs;
                    interval.endNs = std::max(eventNs, open.startNs);
                    interval.nameId = open.nameId;
                    interval.depth = (uint16_t)buffer->stack.size();
                    interval.flags = open.flags;
                    buffer->intervals.push_back(interval);
                }
            }
            tail++;
        }
        buffer->tail.store(tail, std::memory_order_release);
    }

    // main-thread self time per category: a scope's category is its name's, or its parent's
    void computeCategories(Frame& frame, uint32_t mainId){
        for (double& ms : frame.categoryMs)
            ms = 0.0;
        const ThreadTimeline* main = nullptr;
        for (const ThreadTimeline& timeline : frame.threads){
            if (timeline.thread == mainId)
                main = &timeline;
        }
        if (!main)
            return;
        struct Open { const Interval* interval; uint8_t category; uint64_t childNs; };
        std::vector<Open> stack;
        auto close = [&](){
            const Open open = stack.back();
            stack.pop_back();
            const uint64_t duration = open.interval->endNs - open.interval->startNs;
            const uint64_t self = duration > open.childNs ? duration - open.childNs : 0;
            frame.categoryMs[open.category] += (double)self / 1.0e6;
            if (!stack.empty())
                stack.back().childNs += duration;
        };
        for (const Interval& interval : main->intervals){
            while (!stack.empty() && stack.back().interval->depth >= interval.depth)
                close();
            uint8_t category = categoryOf(interval.nameId);
            if (category == CATEGORY_INHERIT)
                category = stack.empty() ? (uint8_t)Category::Other : stack.back().category;
            stack.push_back({&interval, category, 0});
        }
        while (!stack.empty())
            close();
    }

    void evictDetail(State& s){
        size_t frames = 0;
        size_t bytes = 0;
        for (auto it = s.history.rbegin(); it != s.history.rend(); ++it){
            if (!it->hasDetail)
                continue;
            const size_t frameBytes = detailBytesOf(*it);
            if (frames >= s.detailFrames || bytes + frameBytes > s.detailBytes){
                it->threads.clear();
                it->threads.shrink_to_fit();
                it->hasDetail = false;
                continue;
            }
            frames++;
            bytes += frameBytes;
        }
    }

    void sampleMonitors(std::vector<float>* snapshot){
        Monitors& m = monitorTable();
        std::lock_guard<std::mutex> lock(m.mutex);
        if (snapshot)
            snapshot->assign(m.list.size(), std::numeric_limits<float>::quiet_NaN());
        for (size_t i = 0; i < m.list.size(); i++){
            Monitor& monitor = *m.list[i];
            if (monitor.pending.exchange(false, std::memory_order_acq_rel)){
                const uint64_t bits = monitor.pendingBits.load(std::memory_order_relaxed);
                std::memcpy(&monitor.latest, &bits, sizeof(double));
            }else if (monitor.count == 0){
                continue;   // never set
            }
            monitor.ring[monitor.head] = monitor.latest;
            monitor.head = (monitor.head + 1) % MONITOR_SAMPLES;
            monitor.count = std::min(monitor.count + 1, MONITOR_SAMPLES);
            if (snapshot)
                (*snapshot)[i] = (float)monitor.latest;
        }
    }

    void finishCapture(State& s, Capture& capture){
        capture.info.complete = true;
        Names& n = names();
        {
            std::lock_guard<std::mutex> lock(n.mutex);
            capture.names.assign(n.strings.begin(), n.strings.end());
        }
        {
            ThreadTable& t = threadTable();
            std::lock_guard<std::mutex> lock(t.mutex);
            capture.threads = t.infos;
        }
        capture.monitorNames = monitorNames();
        capture.vram = gpumem::report(false);
        capture.hasVram = true;
        if (s.active == capture.info.id)
            s.active = 0;
        setEnabled(capture.previousEnabled);
    }

    // ---- .jetrace.json

    void appendEscaped(std::string& out, const std::string& text){
        for (unsigned char c : text){
            switch (c){
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20){
                        char escaped[8];
                        std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                        out += escaped;
                    }else{
                        out += (char)c;
                    }
            }
        }
    }

    std::string number(double value, int decimals = 3){
        if (!std::isfinite(value))
            return "0";
        char text[48];
        std::snprintf(text, sizeof(text), "%.*f", decimals, value);
        return text;
    }

    constexpr uint32_t GPU_TID = 1000000;

}

// ---- public

const char* profiler::categoryName(Category category){
    static const char* const table[CATEGORY_COUNT] = {
        "Rendering", "Scripts", "Physics", "Animation", "UI", "Loading", "Jobs", "Wait", "Other"
    };
    const size_t index = (size_t)category;
    return index < CATEGORY_COUNT ? table[index] : "Other";
}

void profiler::setEnabled(bool enabled){
    const bool was = enabledFlag.exchange(enabled, std::memory_order_relaxed);
    if (enabled && !was){
        State& s = state();
        s.partialNext = true;
        if (s.pairCostNs <= 0.0){
            // a scope pair into a ring nobody reads: the overhead estimate of the descriptions
            ThreadBuffer scratch;
            scratch.events = new Event[4096];
            scratch.capacity = 4096;
            scratch.mask = 4095;
            const uint64_t start = nowNs();
            for (int i = 0; i < 1000; i++){
                const uint64_t h = scratch.head.load(std::memory_order_relaxed);
                scratch.events[h & scratch.mask] = {readTicks(), KIND_BEGIN | 1u, 0};
                scratch.events[(h + 1) & scratch.mask] = {readTicks(), KIND_END, 0};
                scratch.head.store(h + 2, std::memory_order_release);
            }
            s.pairCostNs = (double)(nowNs() - start) / 1000.0;
        }
    }
}

uint32_t profiler::registerSite(ScopeSite& site){
    const uint32_t id = internName(site.name ? site.name : "?");
    site.id.store(id, std::memory_order_relaxed);
    return id;
}

uint32_t profiler::internName(std::string_view name){
    // most lookups hit the calling thread's cache, which points into the stable name storage
    thread_local std::unordered_map<std::string_view, uint32_t> cache;
    auto cached = cache.find(name);
    if (cached != cache.end())
        return cached->second;
    Names& n = names();
    std::lock_guard<std::mutex> lock(n.mutex);
    const uint32_t id = internLocked(n, name);
    cache.emplace(std::string_view(n.strings[id]), id);
    return id;
}

std::string profiler::nameOf(uint32_t nameId){
    Names& n = names();
    std::lock_guard<std::mutex> lock(n.mutex);
    return nameId < n.strings.size() ? n.strings[nameId] : std::string("?");
}

void profiler::setNameCategory(uint32_t nameId, Category category){
    Names& n = names();
    std::lock_guard<std::mutex> lock(n.mutex);
    if (nameId < n.categories.size())
        n.categories[nameId] = (uint8_t)category;
}

bool profiler::beginScope(uint32_t nameId){
    ThreadBuffer* buffer = threadLocal.buffer ? threadLocal.buffer : createBuffer();
    const uint64_t head = buffer->head.load(std::memory_order_relaxed);
    const uint64_t tail = buffer->tail.load(std::memory_order_acquire);
    // room for this begin and the end of every scope still open (ends are never dropped)
    if (head - tail + buffer->openDepth + 2 > buffer->capacity){
        buffer->dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    buffer->events[head & buffer->mask] = {readTicks(), (nameId & NAME_MASK) | KIND_BEGIN, 0};
    buffer->head.store(head + 1, std::memory_order_release);
    buffer->openDepth++;
    return true;
}

void profiler::endScope(){
    ThreadBuffer* buffer = threadLocal.buffer;
    if (!buffer || buffer->openDepth == 0)
        return;
    const uint64_t head = buffer->head.load(std::memory_order_relaxed);
    buffer->events[head & buffer->mask] = {readTicks(), KIND_END, 0};
    buffer->head.store(head + 1, std::memory_order_release);
    buffer->openDepth--;
}

void profiler::setThreadName(const char* name){
    if (!name)
        return;
    threadLocal.pendingName = name;
    ThreadBuffer* buffer = threadLocal.buffer;
    if (!buffer)
        return;
    ThreadTable& t = threadTable();
    std::lock_guard<std::mutex> lock(t.mutex);
    for (ThreadInfo& info : t.infos){
        if (info.id == buffer->id)
            info.name = displayName(name);
    }
}

void profiler::setRingBytes(size_t bytes){
    ThreadTable& t = threadTable();
    std::lock_guard<std::mutex> lock(t.mutex);
    t.ringBytes = std::clamp<size_t>(bytes, 256 * 1024, 8 * 1024 * 1024);
}

void profiler::setDetailLimits(size_t frames, size_t bytes){
    State& s = state();
    s.detailFrames = std::min<size_t>(frames, 300);
    s.detailBytes = bytes;
    evictDetail(s);
}

void profiler::frameMark(){
    State& s = state();
    tickClock().refine();
    const uint64_t now = nowNs();
    const bool recording = isEnabled();
    ThreadTable& t = threadTable();
    {
        std::lock_guard<std::mutex> lock(t.mutex);
        if (t.mainId == 0){
            // the thread that ends frames is the main thread
            ThreadBuffer* mine = threadLocal.buffer ? threadLocal.buffer : nullptr;
            if (mine){
                t.mainId = mine->id;
                for (ThreadInfo& info : t.infos)
                    info.main = info.id == mine->id;
            }
        }
        if (s.drainedGeneration != t.generation){
            s.drained = t.buffers;
            s.drainedGeneration = t.generation;
        }
    }
    if (t.mainId == 0 && recording){
        // the main thread has not recorded yet: give it its ring so it heads the timelines
        ThreadBuffer* mine = threadLocal.buffer ? threadLocal.buffer : createBuffer();
        std::lock_guard<std::mutex> lock(t.mutex);
        t.mainId = mine->id;
        for (ThreadInfo& info : t.infos)
            info.main = info.id == mine->id;
        s.drained = t.buffers;
        s.drainedGeneration = t.generation;
    }

    Frame frame;
    const FrameTimeSample timing = FrameTimes::latest();
    frame.index = timing.frame;
    frame.startNs = s.lastMarkNs ? s.lastMarkNs : now;
    frame.endNs = now;
    frame.cpuMs = timing.cpuMs;
    frame.gpuMs = timing.gpuMs;
    frame.partial = s.partialNext;
    // the GPU times that arrived while this frame was being made
    if (!s.pendingGpu.empty()){
        // (FrameTimes kept the game frame's sum of the same commits: it is added up again here)
        for (const PendingGpu& gpu : s.pendingGpu){
            if (gpu.frame == frame.index && gpu.gameFrame && gpu.frameValid)
                frame.gpuMs = -1.0;
        }
        for (const PendingGpu& gpu : s.pendingGpu){
            if (gpu.frame == frame.index)
                applyGpu(frame, gpu);
        }
        s.pendingGpu.erase(std::remove_if(s.pendingGpu.begin(), s.pendingGpu.end(),
                                          [&](const PendingGpu& gpu){ return gpu.frame <= frame.index; }),
                           s.pendingGpu.end());
    }
    bool exitedAny = false;
    for (ThreadBuffer* buffer : s.drained){
        buffer->intervals.clear();
        drainRing(buffer, now, recording);
        frame.dropped += buffer->dropped.exchange(0, std::memory_order_relaxed);
        if (recording){
            // scopes open across the boundary: split, this frame's part now, the rest next frame
            for (size_t depth = 0; depth < buffer->stack.size(); depth++){
                OpenScope& open = buffer->stack[depth];
                Interval interval;
                interval.startNs = std::max(open.startNs, frame.startNs);
                interval.endNs = now;
                interval.nameId = open.nameId;
                interval.depth = (uint16_t)depth;
                interval.flags = (uint16_t)(open.flags | INTERVAL_CONTINUES);
                buffer->intervals.push_back(interval);
                open.startNs = now;
                open.flags = INTERVAL_CONTINUED;
            }
            if (!buffer->intervals.empty()){
                ThreadTimeline timeline;
                timeline.thread = buffer->id;
                timeline.intervals = buffer->intervals;
                std::sort(timeline.intervals.begin(), timeline.intervals.end(), [](const Interval& a, const Interval& b){
                    return a.startNs != b.startNs ? a.startNs < b.startNs : a.depth < b.depth;
                });
                frame.events += (uint32_t)timeline.intervals.size() * 2;
                frame.threads.push_back(std::move(timeline));
            }
        }
        exitedAny = exitedAny || buffer->exited.load(std::memory_order_acquire);
    }
    if (exitedAny){
        // a thread that ended and whose ring is empty: its ring goes, its name stays
        std::lock_guard<std::mutex> lock(t.mutex);
        for (auto it = t.buffers.begin(); it != t.buffers.end();){
            ThreadBuffer* buffer = *it;
            if (buffer->exited.load(std::memory_order_acquire) &&
                buffer->tail.load(std::memory_order_relaxed) == buffer->head.load(std::memory_order_acquire)){
                it = t.buffers.erase(it);
                delete buffer;
            }else{
                ++it;
            }
        }
        t.generation++;
        s.drained = t.buffers;
        s.drainedGeneration = t.generation;
    }
    std::sort(frame.threads.begin(), frame.threads.end(), [&](const ThreadTimeline& a, const ThreadTimeline& b){
        if ((a.thread == t.mainId) != (b.thread == t.mainId))
            return a.thread == t.mainId;
        return a.thread < b.thread;
    });

    sampleMonitors(recording ? &frame.monitorValues : nullptr);

    if (recording && s.lastMarkNs != 0){
        computeCategories(frame, t.mainId);
        frame.hasDetail = true;
        s.partialNext = false;
        // a capture takes whole frames from the second after arming
        if (s.active){
            Capture* capture = findCapture(s.active);
            if (capture && !frame.partial && capture->info.framesRecorded < capture->info.framesWanted){
                capture->frames.push_back(frame);
                capture->info.framesRecorded++;
            }else if (capture && capture->info.framesRecorded >= capture->info.framesWanted){
                if (++capture->graceFrames >= 6)
                    finishCapture(s, *capture);
            }
        }
        s.history.push_back(std::move(frame));
        while (s.history.size() > HISTORY_FRAMES)
            s.history.pop_front();
        evictDetail(s);
    }
    s.lastMarkNs = now;
}

void profiler::recordGpuFrame(uint64_t frameIndex, uint32_t viewTag, uint64_t commit, const GpuPassSample* passes, size_t count,
                              double frameGpuMs, bool frameValid, bool gameFrame){
    State& s = state();
    PendingGpu gpu;
    gpu.frame = frameIndex;
    gpu.viewTag = viewTag;
    gpu.commit = commit;
    gpu.passes.assign(passes, passes + count);
    gpu.frameGpuMs = frameGpuMs;
    gpu.frameValid = frameValid;
    gpu.gameFrame = gameFrame;
    if (s.history.empty() || frameIndex > s.history.back().index){
        // the frame is not marked yet: its times wait for it (GPU campaign perf2: Vulkan's light
        // frames resolved in their own frame and were lost, so captures had no Game view)
        if (s.pendingGpu.size() < 64)
            s.pendingGpu.push_back(gpu);
    }else{
        for (auto it = s.history.rbegin(); it != s.history.rend(); ++it){
            if (it->index == frameIndex){
                applyGpu(*it, gpu);
                break;
            }
            if (it->index < frameIndex)
                break;
        }
    }
#if JASPERENGINE_TRACY
    // Tracy: a plot per pass ("GPU/<label>"), no GPU zones; Tracy keeps the name pointers
    if (gameFrame){
        static std::unordered_set<std::string> plotNames;
        for (size_t i = 0; i < count; i++){
            if (!passes[i].valid)
                continue;
            const std::string& name = *plotNames.insert("GPU/" + passes[i].label).first;
            TracyPlot(name.c_str(), passes[i].gpuMs);
        }
        if (frameValid)
            TracyPlot("GPU frame ms", frameGpuMs);
    }
#endif
    for (auto& capture : s.captures){
        if (capture->info.loaded)
            continue;
        for (Frame& frame : capture->frames){
            if (frame.index == frameIndex)
                applyGpu(frame, gpu);
        }
    }
}

void profiler::setGpuSemantics(const std::string& semantics){
    State& s = state();
    for (auto& capture : s.captures){
        if (!capture->info.loaded && !capture->info.complete)
            capture->semantics = semantics;
    }
}

void profiler::setDeviceInfo(const std::string& backend, const std::string& device){
    State& s = state();
    s.backend = backend;
    s.device = device;
}

void profiler::setDeepLua(bool deep){
    deepLuaFlag.store(deep, std::memory_order_relaxed);
}

std::atomic<bool> profiler::deepLuaFlag{false};

// ---- monitors

const char* profiler::monitorUnitName(MonitorUnit unit){
    switch (unit){
        case MonitorUnit::Quantity: return "quantity";
        case MonitorUnit::Memory: return "bytes";
        case MonitorUnit::TimeMs: return "ms";
        case MonitorUnit::Percent: return "percent";
    }
    return "quantity";
}

void profiler::counter(const char* name, double value){
    if (!name)
        return;
    Monitor* monitor = findMonitor(name, MonitorUnit::Quantity, false);
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(double));
    monitor->pendingBits.store(bits, std::memory_order_relaxed);
    monitor->pending.store(true, std::memory_order_release);
}

profiler::MonitorHandle profiler::monitorHandle(const char* name, MonitorUnit unit){
    return name ? findMonitor(name, unit, true) : nullptr;
}

void profiler::counter(MonitorHandle handle, double value){
    Monitor* monitor = static_cast<Monitor*>(handle);
    if (!monitor)
        return;
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(double));
    monitor->pendingBits.store(bits, std::memory_order_relaxed);
    monitor->pending.store(true, std::memory_order_release);
}

void profiler::counter(const char* name, double value, MonitorUnit unit){
    if (!name)
        return;
    Monitor* monitor = findMonitor(name, unit, true);
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(double));
    monitor->pendingBits.store(bits, std::memory_order_relaxed);
    monitor->pending.store(true, std::memory_order_release);
}

namespace {
    MonitorInfo infoOf(const Monitor& monitor){
        MonitorInfo info;
        info.name = monitor.name;
        info.unit = monitor.unit;
        info.latest = monitor.latest;
        info.samples = monitor.count;
        if (monitor.count == 0)
            return info;
        double sum = 0.0;
        info.minimum = monitor.ring[(monitor.head + MONITOR_SAMPLES - monitor.count) % MONITOR_SAMPLES];
        info.maximum = info.minimum;
        for (size_t i = 0; i < monitor.count; i++){
            const double value = monitor.ring[(monitor.head + MONITOR_SAMPLES - monitor.count + i) % MONITOR_SAMPLES];
            sum += value;
            info.minimum = std::min(info.minimum, value);
            info.maximum = std::max(info.maximum, value);
        }
        info.average = sum / (double)monitor.count;
        return info;
    }
}

std::vector<MonitorInfo> profiler::monitors(){
    Monitors& m = monitorTable();
    std::lock_guard<std::mutex> lock(m.mutex);
    std::vector<MonitorInfo> out;
    for (const auto& monitor : m.list)
        out.push_back(infoOf(*monitor));
    return out;
}

bool profiler::monitorInfo(const std::string& name, MonitorInfo& out){
    Monitors& m = monitorTable();
    std::lock_guard<std::mutex> lock(m.mutex);
    auto it = m.index.find(name);
    if (it == m.index.end())
        return false;
    out = infoOf(*m.list[it->second]);
    return true;
}

std::vector<std::string> profiler::monitorNames(){
    Monitors& m = monitorTable();
    std::lock_guard<std::mutex> lock(m.mutex);
    std::vector<std::string> out;
    out.reserve(m.list.size());
    for (const auto& monitor : m.list)
        out.push_back(monitor->name);
    return out;
}

std::vector<double> profiler::monitorSeries(const std::string& name, size_t count){
    Monitors& m = monitorTable();
    std::lock_guard<std::mutex> lock(m.mutex);
    auto it = m.index.find(name);
    std::vector<double> out;
    if (it == m.index.end())
        return out;
    const Monitor& monitor = *m.list[it->second];
    const size_t n = std::min(count, monitor.count);
    for (size_t i = monitor.count - n; i < monitor.count; i++)
        out.push_back(monitor.ring[(monitor.head + MONITOR_SAMPLES - monitor.count + i) % MONITOR_SAMPLES]);
    return out;
}

// ---- frames

size_t profiler::frameCount(){
    return state().history.size();
}

const Frame* profiler::frameAt(size_t index){
    State& s = state();
    return index < s.history.size() ? &s.history[index] : nullptr;
}

const Frame* profiler::findFrame(uint64_t frameIndex){
    State& s = state();
    for (auto it = s.history.rbegin(); it != s.history.rend(); ++it){
        if (it->index == frameIndex)
            return &*it;
    }
    return nullptr;
}

std::vector<ThreadInfo> profiler::threads(){
    ThreadTable& t = threadTable();
    std::lock_guard<std::mutex> lock(t.mutex);
    return t.infos;
}

uint32_t profiler::mainThreadId(){
    ThreadTable& t = threadTable();
    std::lock_guard<std::mutex> lock(t.mutex);
    return t.mainId;
}

void profiler::clearHistory(){
    State& s = state();
    s.history.clear();
    s.pendingGpu.clear();
    s.partialNext = true;
}

double profiler::scopePairCostNs(){
    return state().pairCostNs;
}

// ---- views

std::vector<TreeRow> profiler::buildTree(const Frame& frame, uint32_t thread, const NameLookup& lookup){
    std::vector<TreeRow> rows;
    const ThreadTimeline* timeline = nullptr;
    for (const ThreadTimeline& candidate : frame.threads){
        if (candidate.thread == thread)
            timeline = &candidate;
    }
    if (!timeline)
        return rows;
    // the row of each open scope, and the index of each path's row
    std::unordered_map<std::string, size_t> rowOfPath;
    struct Open { const Interval* interval; size_t row; uint64_t childNs; };
    std::vector<Open> stack;
    auto close = [&](){
        const Open open = stack.back();
        stack.pop_back();
        const uint64_t duration = open.interval->endNs - open.interval->startNs;
        TreeRow& row = rows[open.row];
        row.selfMs += (double)(duration > open.childNs ? duration - open.childNs : 0) / 1.0e6;
        if (!stack.empty())
            stack.back().childNs += duration;
    };
    for (const Interval& interval : timeline->intervals){
        while (!stack.empty() && stack.back().interval->depth >= interval.depth)
            close();
        const std::string name = lookup ? lookup(interval.nameId) : nameOf(interval.nameId);
        const std::string path = (stack.empty() ? std::string() : rows[stack.back().row].path + "/") + name;
        size_t row;
        auto found = rowOfPath.find(path);
        if (found == rowOfPath.end()){
            row = rows.size();
            TreeRow entry;
            entry.name = name;
            entry.depth = (uint32_t)stack.size();
            entry.path = path;
            rows.push_back(entry);
            rowOfPath.emplace(path, row);
        }else{
            row = found->second;
        }
        rows[row].totalMs += (double)(interval.endNs - interval.startNs) / 1.0e6;
        // a split scope is one call: its continued part adds time, not a call
        if (!(interval.flags & INTERVAL_CONTINUED))
            rows[row].calls++;
        stack.push_back({&interval, row, 0});
    }
    while (!stack.empty())
        close();
    // depth-first order: each row under its parent, siblings in first-call order
    std::vector<TreeRow> ordered;
    ordered.reserve(rows.size());
    std::vector<std::vector<size_t>> children(rows.size() + 1);
    for (size_t i = 0; i < rows.size(); i++){
        const size_t slash = rows[i].path.rfind('/');
        size_t parent = rows.size();
        if (slash != std::string::npos)
            parent = rowOfPath[rows[i].path.substr(0, slash)];
        children[parent].push_back(i);
    }
    std::vector<size_t> pending(children[rows.size()].rbegin(), children[rows.size()].rend());
    while (!pending.empty()){
        const size_t index = pending.back();
        pending.pop_back();
        ordered.push_back(rows[index]);
        for (auto it = children[index].rbegin(); it != children[index].rend(); ++it)
            pending.push_back(*it);
    }
    return ordered;
}

std::vector<FlatRow> profiler::buildFlat(const Frame& frame, const NameLookup& lookup){
    std::unordered_map<uint32_t, FlatRow> rows;
    for (const ThreadTimeline& timeline : frame.threads){
        struct Open { const Interval* interval; uint64_t childNs; };
        std::vector<Open> stack;
        std::unordered_map<uint32_t, int> active;   // open intervals per name (recursion)
        auto close = [&](){
            const Open open = stack.back();
            stack.pop_back();
            const uint64_t duration = open.interval->endNs - open.interval->startNs;
            FlatRow& row = rows[open.interval->nameId];
            row.selfMs += (double)(duration > open.childNs ? duration - open.childNs : 0) / 1.0e6;
            if (--active[open.interval->nameId] == 0){
                const double ms = (double)duration / 1.0e6;
                row.totalMs += ms;
                auto thread = std::find_if(row.perThread.begin(), row.perThread.end(),
                                           [&](const std::pair<uint32_t, double>& p){ return p.first == timeline.thread; });
                if (thread == row.perThread.end())
                    row.perThread.emplace_back(timeline.thread, ms);
                else
                    thread->second += ms;
            }
            if (!stack.empty())
                stack.back().childNs += duration;
        };
        for (const Interval& interval : timeline.intervals){
            while (!stack.empty() && stack.back().interval->depth >= interval.depth)
                close();
            FlatRow& row = rows[interval.nameId];
            if (row.name.empty())
                row.name = lookup ? lookup(interval.nameId) : nameOf(interval.nameId);
            if (!(interval.flags & INTERVAL_CONTINUED))
                row.calls++;
            active[interval.nameId]++;
            stack.push_back({&interval, 0});
        }
        while (!stack.empty())
            close();
    }
    std::vector<FlatRow> out;
    out.reserve(rows.size());
    for (auto& [id, row] : rows)
        out.push_back(std::move(row));
    std::sort(out.begin(), out.end(), [](const FlatRow& a, const FlatRow& b){
        return a.totalMs != b.totalMs ? a.totalMs > b.totalMs : a.name < b.name;
    });
    return out;
}

std::vector<ThreadBusy> profiler::buildThreadBusy(const Frame& frame){
    std::vector<ThreadBusy> out;
    const double frameMs = frame.frameMs();
    for (const ThreadTimeline& timeline : frame.threads){
        ThreadBusy busy;
        busy.thread = timeline.thread;
        for (const Interval& interval : timeline.intervals){
            if (interval.depth != 0)
                continue;
            busy.busyMs += (double)(interval.endNs - interval.startNs) / 1.0e6;
            busy.scopes++;
        }
        busy.busyPercent = frameMs > 0.0 ? std::min(100.0, busy.busyMs * 100.0 / frameMs) : 0.0;
        out.push_back(busy);
    }
    return out;
}

// ---- captures

CaptureId profiler::startCapture(uint32_t frames){
    State& s = state();
    if (s.active || frames == 0)
        return 0;
    auto capture = std::make_unique<Capture>();
    capture->info.id = s.nextCapture++;
    capture->info.framesWanted = std::min<uint32_t>(frames, (uint32_t)HISTORY_FRAMES);
    capture->info.backend = s.backend;
    capture->info.device = s.device;
    capture->previousEnabled = isEnabled();
    capture->frames.reserve(capture->info.framesWanted);
    s.active = capture->info.id;
    // the oldest captures go: eight are kept
    while (s.captures.size() >= 8)
        s.captures.erase(s.captures.begin());
    s.captures.push_back(std::move(capture));
    setEnabled(true);
    return s.active;
}

bool profiler::captureInfo(CaptureId id, CaptureInfo& out){
    Capture* capture = findCapture(id);
    if (!capture)
        return false;
    out = capture->info;
    return true;
}

std::vector<CaptureInfo> profiler::captures(){
    std::vector<CaptureInfo> out;
    for (const auto& capture : state().captures)
        out.push_back(capture->info);
    return out;
}

CaptureId profiler::activeCapture(){
    return state().active;
}

void profiler::cancelCapture(){
    State& s = state();
    Capture* capture = findCapture(s.active);
    if (capture)
        finishCapture(s, *capture);
    s.active = 0;
}

const std::vector<Frame>* profiler::captureFrames(CaptureId id){
    Capture* capture = findCapture(id);
    return capture ? &capture->frames : nullptr;
}

std::vector<ThreadInfo> profiler::captureThreads(CaptureId id){
    Capture* capture = findCapture(id);
    if (!capture)
        return {};
    if (!capture->info.complete && !capture->info.loaded)
        return threads();
    return capture->threads;
}

std::vector<std::string> profiler::captureMonitorNames(CaptureId id){
    Capture* capture = findCapture(id);
    if (!capture)
        return {};
    if (!capture->info.complete && !capture->info.loaded)
        return monitorNames();
    return capture->monitorNames;
}

CaptureId profiler::captureFromHistory(){
    State& s = state();
    auto capture = std::make_unique<Capture>();
    capture->info.id = s.nextCapture++;
    capture->info.backend = s.backend;
    capture->info.device = s.device;
    for (const Frame& frame : s.history){
        if (frame.hasDetail)
            capture->frames.push_back(frame);
    }
    if (capture->frames.empty())
        return 0;
    capture->info.framesWanted = (uint32_t)capture->frames.size();
    capture->info.framesRecorded = capture->info.framesWanted;
    capture->previousEnabled = isEnabled();
    const CaptureId id = capture->info.id;
    while (s.captures.size() >= 8)
        s.captures.erase(s.captures.begin());
    s.captures.push_back(std::move(capture));
    // complete: names, threads, monitors and VRAM as of now; recording goes on as it was
    finishCapture(s, *s.captures.back());
    return id;
}

std::string profiler::captureNameOf(CaptureId id, uint32_t nameId){
    Capture* capture = findCapture(id);
    if (capture && (capture->info.complete || capture->info.loaded) && nameId < capture->names.size())
        return capture->names[nameId];
    return nameOf(nameId);
}

std::string profiler::captureJson(CaptureId id){
    Capture* capture = findCapture(id);
    if (!capture)
        return std::string();
    const std::vector<ThreadInfo> threadList = captureThreads(id);
    const std::vector<std::string> monitorList = captureMonitorNames(id);
    auto nameFor = [&](uint32_t nameId){ return captureNameOf(id, nameId); };
    uint64_t origin = capture->frames.empty() ? 0 : capture->frames.front().startNs;
    auto us = [&](uint64_t ns){ return number((double)(ns - std::min(ns, origin)) / 1000.0, 3); };

    std::string out;
    out.reserve(1 << 20);
    out += "{\"traceEvents\":[\n";
    out += "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,\"tid\":0,\"args\":{\"name\":\"JasperEngine\"}}";
    for (const ThreadInfo& thread : threadList){
        out += ",\n{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":" + std::to_string(thread.id) + ",\"args\":{\"name\":\"";
        appendEscaped(out, thread.name);
        out += "\"}}";
        out += ",\n{\"name\":\"thread_sort_index\",\"ph\":\"M\",\"pid\":1,\"tid\":" + std::to_string(thread.id) +
               ",\"args\":{\"sort_index\":" + std::to_string(thread.main ? 0 : thread.id) + "}}";
    }
    out += ",\n{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":" + std::to_string(GPU_TID) + ",\"args\":{\"name\":\"GPU\"}}";
    for (size_t f = 0; f < capture->frames.size(); f++){
        const Frame& frame = capture->frames[f];
        for (const ThreadTimeline& timeline : frame.threads){
            for (const Interval& interval : timeline.intervals){
                out += ",\n{\"name\":\"";
                appendEscaped(out, nameFor(interval.nameId));
                out += "\",\"ph\":\"X\",\"pid\":1,\"tid\":" + std::to_string(timeline.thread) + ",\"ts\":" + us(interval.startNs) +
                       ",\"dur\":" + number((double)(interval.endNs - interval.startNs) / 1000.0, 3) +
                       ",\"args\":{\"frame\":" + std::to_string(frame.index) + ",\"depth\":" + std::to_string(interval.depth);
                if (interval.flags)
                    out += ",\"split\":" + std::to_string(interval.flags);
                out += "}}";
            }
        }
        // the GPU passes, back to back from the frame's start (GPU clocks are not the CPU's)
        uint64_t cursor = frame.startNs;
        for (const GpuPassSample& pass : frame.gpu){
            if (!pass.valid)
                continue;
            const uint64_t duration = (uint64_t)(pass.gpuMs * 1.0e6);
            out += ",\n{\"name\":\"";
            appendEscaped(out, pass.label);
            out += "\",\"cat\":\"GPU\",\"ph\":\"X\",\"pid\":1,\"tid\":" + std::to_string(GPU_TID) + ",\"ts\":" + us(cursor) +
                   ",\"dur\":" + number(pass.gpuMs * 1000.0, 3) + ",\"args\":{\"frame\":" + std::to_string(frame.index) +
                   ",\"view\":" + std::to_string(pass.viewTag) + ",\"cpuMs\":" + number(pass.cpuMs, 4) +
                   ",\"draws\":" + std::to_string(pass.draws) + "}}";
            cursor += duration;
        }
        for (size_t m = 0; m < frame.monitorValues.size() && m < monitorList.size(); m++){
            if (std::isnan(frame.monitorValues[m]))
                continue;
            out += ",\n{\"name\":\"";
            appendEscaped(out, monitorList[m]);
            out += "\",\"ph\":\"C\",\"pid\":1,\"tid\":0,\"ts\":" + us(frame.startNs) + ",\"args\":{\"value\":" +
                   number(frame.monitorValues[m], 4) + "}}";
        }
    }
    out += "\n],\n\"displayTimeUnit\":\"ms\",\n\"jasper\":{\"version\":1,\"backend\":\"";
    appendEscaped(out, capture->info.backend);
    out += "\",\"device\":\"";
    appendEscaped(out, capture->info.device);
    out += "\",\"clock\":\"";
#if defined(__EMSCRIPTEN__)
    out += "emscripten_get_now";
#else
    out += "steady_clock";
#endif
    out += "\",\"precisionNs\":1,\"semantics\":\"";
    appendEscaped(out, capture->semantics);
    out += "\",\"threads\":[";
    for (size_t i = 0; i < threadList.size(); i++){
        out += (i ? "," : "") + std::string("{\"id\":") + std::to_string(threadList[i].id) + ",\"name\":\"";
        appendEscaped(out, threadList[i].name);
        out += std::string("\",\"main\":") + (threadList[i].main ? "true" : "false") + "}";
    }
    out += "],\n\"frames\":[";
    for (size_t f = 0; f < capture->frames.size(); f++){
        const Frame& frame = capture->frames[f];
        out += (f ? ",\n" : "\n") + std::string("{\"index\":") + std::to_string(frame.index) + ",\"startUs\":" + us(frame.startNs) +
               ",\"frameMs\":" + number(frame.frameMs()) + ",\"cpuMs\":" + number(frame.cpuMs) +
               ",\"gpuMs\":" + (frame.gpuMs >= 0.0 ? number(frame.gpuMs) : std::string("null")) +
               ",\"events\":" + std::to_string(frame.events) + ",\"dropped\":" + std::to_string(frame.dropped) + ",\"categories\":{";
        for (size_t c = 0; c < CATEGORY_COUNT; c++){
            out += (c ? "," : "") + std::string("\"") + categoryName((Category)c) + "\":" + number(frame.categoryMs[c]);
        }
        out += "}}";
    }
    out += "],\n\"passes\":[";
    bool firstPass = true;
    for (const Frame& frame : capture->frames){
        for (const GpuPassSample& pass : frame.gpu){
            out += (firstPass ? "\n" : ",\n") + std::string("{\"frame\":") + std::to_string(frame.index) + ",\"view\":" +
                   std::to_string(pass.viewTag) + ",\"commit\":" + std::to_string(pass.commit) + ",\"label\":\"";
            appendEscaped(out, pass.label);
            out += "\",\"gpuMs\":" + number(pass.gpuMs, 4) + ",\"cpuMs\":" + number(pass.cpuMs, 4) + ",\"draws\":" +
                   std::to_string(pass.draws) + ",\"valid\":" + (pass.valid ? "true" : "false") + "}";
            firstPass = false;
        }
    }
    out += "],\n\"vram\":";
    if (capture->hasVram){
        out += "{\"estimated\":true,\"totalBytes\":" + std::to_string(capture->vram.totalBytes) + ",\"peakBytes\":" +
               std::to_string(capture->vram.peakBytes) + ",\"count\":" + std::to_string(capture->vram.count) + ",\"categories\":{";
        for (size_t c = 0; c < gpumem::CATEGORY_COUNT; c++){
            out += (c ? "," : "") + std::string("\"") + gpumem::categoryKey((gpumem::Category)c) + "\":" + std::to_string(capture->vram.categoryBytes[c]);
        }
        out += "}}";
    }else{
        out += "null";
    }
    out += "}}\n";
    return out;
}

bool profiler::saveCapture(CaptureId id, const std::string& path, std::string* error){
    Capture* capture = findCapture(id);
    if (!capture){
        if (error) *error = "no capture " + std::to_string(id);
        return false;
    }
    const std::string text = captureJson(id);
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    const io::WriteResult result = io::writeFileAtomic(std::filesystem::path(path), text, options);
    if (!result.ok()){
        if (error) *error = result.message.empty() ? "cannot write " + path : result.message;
        return false;
    }
    capture->info.path = path;
    return true;
}

CaptureId profiler::loadCapture(const std::string& path, std::string& error){
    std::ifstream file(path, std::ios::binary);
    if (!file){
        error = "cannot read " + path;
        return 0;
    }
    std::stringstream text;
    text << file.rdbuf();
    nlohmann::json root = nlohmann::json::parse(text.str(), nullptr, false);
    if (root.is_discarded() || !root.is_object()){
        error = path + " is not JSON";
        return 0;
    }
    const nlohmann::json jasper = root.value("jasper", nlohmann::json());
    if (!jasper.is_object() || !jasper.contains("version") || !jasper["version"].is_number()){
        error = path + " is not a JasperEngine capture (no jasper.version)";
        return 0;
    }
    const int version = jasper["version"].get<int>();
    if (version > 1){
        error = path + " is a version " + std::to_string(version) + " capture; this editor reads version 1";
        return 0;
    }
    auto capture = std::make_unique<Capture>();
    capture->info.loaded = true;
    capture->info.complete = true;
    capture->info.path = path;
    capture->info.backend = jasper.value("backend", std::string());
    capture->info.device = jasper.value("device", std::string());
    capture->semantics = jasper.value("semantics", std::string());
    capture->names.push_back("?");
    std::unordered_map<std::string, uint32_t> nameIds;
    auto nameId = [&](const std::string& name) -> uint32_t {
        auto it = nameIds.find(name);
        if (it != nameIds.end())
            return it->second;
        const uint32_t id = (uint32_t)capture->names.size();
        capture->names.push_back(name);
        nameIds.emplace(name, id);
        return id;
    };
    if (jasper.contains("threads") && jasper["threads"].is_array()){
        for (const auto& thread : jasper["threads"]){
            ThreadInfo info;
            info.id = thread.value("id", 0u);
            info.name = thread.value("name", std::string());
            info.main = thread.value("main", false);
            capture->threads.push_back(info);
        }
    }
    // frames by index, in the file's order
    std::unordered_map<uint64_t, size_t> frameOf;
    if (jasper.contains("frames") && jasper["frames"].is_array()){
        for (const auto& entry : jasper["frames"]){
            Frame frame;
            frame.index = entry.value("index", (uint64_t)0);
            const double startUs = entry.value("startUs", 0.0);
            frame.startNs = (uint64_t)(startUs * 1000.0);
            frame.endNs = frame.startNs + (uint64_t)(entry.value("frameMs", 0.0) * 1.0e6);
            frame.cpuMs = entry.value("cpuMs", 0.0);
            frame.gpuMs = entry.contains("gpuMs") && entry["gpuMs"].is_number() ? entry["gpuMs"].get<double>() : -1.0;
            frame.events = entry.value("events", 0u);
            frame.dropped = entry.value("dropped", 0u);
            if (entry.contains("categories") && entry["categories"].is_object()){
                for (size_t c = 0; c < CATEGORY_COUNT; c++){
                    frame.categoryMs[c] = entry["categories"].value(categoryName((Category)c), 0.0);
                }
            }
            frame.hasDetail = true;
            frameOf.emplace(frame.index, capture->frames.size());
            capture->frames.push_back(std::move(frame));
        }
    }
    // the counters: one per monitor and frame, at the frame's start
    std::unordered_map<std::string, size_t> monitorIndex;
    std::unordered_map<uint64_t, size_t> frameAtStart;
    for (size_t f = 0; f < capture->frames.size(); f++)
        frameAtStart.emplace(capture->frames[f].startNs / 1000, f);
    if (root.contains("traceEvents") && root["traceEvents"].is_array()){
        for (const auto& event : root["traceEvents"]){
            if (!event.is_object() || event.value("ph", std::string()) != "C")
                continue;
            const uint64_t ts = (uint64_t)(event.value("ts", 0.0));
            auto frame = frameAtStart.find(ts);
            if (frame == frameAtStart.end())
                continue;
            const std::string name = event.value("name", std::string());
            auto index = monitorIndex.find(name);
            if (index == monitorIndex.end()){
                index = monitorIndex.emplace(name, capture->monitorNames.size()).first;
                capture->monitorNames.push_back(name);
            }
            std::vector<float>& values = capture->frames[frame->second].monitorValues;
            if (values.size() <= index->second)
                values.resize(index->second + 1, std::numeric_limits<float>::quiet_NaN());
            const nlohmann::json args = event.value("args", nlohmann::json::object());
            values[index->second] = (float)args.value("value", 0.0);
        }
    }
    if (jasper.contains("passes") && jasper["passes"].is_array()){
        for (const auto& entry : jasper["passes"]){
            auto frame = frameOf.find(entry.value("frame", (uint64_t)0));
            if (frame == frameOf.end())
                continue;
            GpuPassSample pass;
            pass.label = entry.value("label", std::string());
            pass.viewTag = entry.value("view", 0u);
            pass.commit = entry.value("commit", (uint64_t)0);
            pass.gpuMs = entry.value("gpuMs", 0.0);
            pass.cpuMs = entry.value("cpuMs", 0.0);
            pass.draws = entry.value("draws", 0u);
            pass.valid = entry.value("valid", false);
            capture->frames[frame->second].gpu.push_back(pass);
        }
    }
    if (root.contains("traceEvents") && root["traceEvents"].is_array()){
        for (const auto& event : root["traceEvents"]){
            if (!event.is_object() || event.value("ph", std::string()) != "X")
                continue;
            const nlohmann::json args = event.value("args", nlohmann::json::object());
            const uint64_t index = args.value("frame", (uint64_t)0);
            auto frame = frameOf.find(index);
            if (frame == frameOf.end())
                continue;
            Frame& target = capture->frames[frame->second];
            const uint32_t tid = event.value("tid", 0u);
            const std::string name = event.value("name", std::string());
            const double ts = event.value("ts", 0.0);
            const double dur = event.value("dur", 0.0);
            if (tid == GPU_TID)
                continue;   // jasper.passes has them all, the invalid ones too
            Interval interval;
            interval.startNs = (uint64_t)(ts * 1000.0);
            interval.endNs = interval.startNs + (uint64_t)(dur * 1000.0);
            interval.nameId = nameId(name);
            interval.depth = (uint16_t)args.value("depth", 0u);
            interval.flags = (uint16_t)args.value("split", 0u);
            ThreadTimeline* timeline = nullptr;
            for (ThreadTimeline& candidate : target.threads){
                if (candidate.thread == tid)
                    timeline = &candidate;
            }
            if (!timeline){
                target.threads.push_back(ThreadTimeline());
                timeline = &target.threads.back();
                timeline->thread = tid;
            }
            timeline->intervals.push_back(interval);
        }
    }
    for (Frame& frame : capture->frames){
        for (ThreadTimeline& timeline : frame.threads){
            std::sort(timeline.intervals.begin(), timeline.intervals.end(), [](const Interval& a, const Interval& b){
                return a.startNs != b.startNs ? a.startNs < b.startNs : a.depth < b.depth;
            });
        }
    }
    capture->info.framesRecorded = (uint32_t)capture->frames.size();
    capture->info.framesWanted = capture->info.framesRecorded;
    State& s = state();
    capture->info.id = s.nextCapture++;
    const CaptureId id = capture->info.id;
    while (s.captures.size() >= 8)
        s.captures.erase(s.captures.begin());
    s.captures.push_back(std::move(capture));
    return id;
}

void profiler::dropCapture(CaptureId id){
    State& s = state();
    if (s.active == id)
        cancelCapture();
    s.captures.erase(std::remove_if(s.captures.begin(), s.captures.end(), [&](const std::unique_ptr<Capture>& c){
        return c->info.id == id;
    }), s.captures.end());
}

#endif
