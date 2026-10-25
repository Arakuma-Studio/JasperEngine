// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EngineProfiling.h"

#if JASPERENGINE_PROFILER

#include "Engine.h"
#include "Log.h"
#include "FrameTimes.h"
#include "GpuMemory.h"
#include "Profiler.h"
#include "Scene.h"
#include "jobs/JobSystem.h"
#include "render/SystemRender.h"
#include "script/LuaBinding.h"
#include "subsystem/UIDocumentSystem.h"

#include "lua.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
    #include <psapi.h>
    #if defined(_MSC_VER)
        #pragma comment(lib, "psapi.lib")
    #endif
#elif defined(__APPLE__)
    #include <mach/mach.h>
#elif defined(__linux__) || defined(__ANDROID__)
    #include <unistd.h>
#endif

using namespace jasperengine;
using namespace jasperengine::profiler;

namespace {

    // ---- the job system's hooks (S06 JobStats.h): a scope per job, named by its label

    // what each nesting level of jobs and waits on a thread began (a job that started while the
    // profiler was off ends without an event)
    struct HookStack {
        uint64_t begun = 0;
        uint32_t depth = 0;
        void push(bool value){
            if (depth < 64 && value)
                begun |= (1ull << depth);
            else if (depth < 64)
                begun &= ~(1ull << depth);
            depth++;
        }
        bool pop(){
            if (depth == 0)
                return false;
            depth--;
            return depth < 64 && (begun & (1ull << depth));
        }
    };
    thread_local HookStack hookStack;

    // job labels are static strings: one name per pointer
    uint32_t jobNameId(const char* name){
        thread_local const char* lastName = nullptr;
        thread_local uint32_t lastId = 0;
        if (name == lastName && lastId)
            return lastId;
        const uint32_t id = internName(name && name[0] ? name : "job");
        // a job's own name is its category's (Jobs) unless it is a standard scope name
        setNameCategory(id, Category::Jobs);
        lastName = name;
        lastId = id;
        return id;
    }

    void onThreadStart(const char* threadName){
        setThreadName(threadName);
    }

    void onJobBegin(const char* jobName, jobs::Lane lane, int workerId){
        hookStack.push(isEnabled() && beginScope(jobNameId(jobName)));
    }

    void onJobEnd(){
        if (hookStack.pop())
            endScope();
    }

    uint32_t waitNameId(jobs::WaitKind kind){
        static ScopeSite sites[] = {
            {"Wait: Task", __FILE__, __LINE__}, {"Wait: Counter", __FILE__, __LINE__}, {"Wait: ParallelFor", __FILE__, __LINE__},
            {"Wait: Strand", __FILE__, __LINE__}, {"Wait: Channel", __FILE__, __LINE__},
        };
        ScopeSite& site = sites[std::min<size_t>((size_t)kind, 4)];
        const uint32_t id = site.id.load(std::memory_order_relaxed);
        return id ? id : registerSite(site);
    }

    void onWaitBegin(jobs::WaitKind kind){
        hookStack.push(isEnabled() && beginScope(waitNameId(kind)));
    }

    void onWaitEnd(){
        if (hookStack.pop())
            endScope();
    }

    // ---- the command line

    CommandLineOptions commandLine;
    CaptureId cliCapture = 0;
    bool cliCaptureDone = false;
    uint64_t monitorFrame = 0;

    bool parseFrames(const std::string& text, uint32_t& out){
        char* end = nullptr;
        const long value = std::strtol(text.c_str(), &end, 10);
        if (text.empty() || *end != '\0' || value < 1 || value > 600)
            return false;
        out = (uint32_t)value;
        return true;
    }

    // "VRAM Textures", "VRAM Render Targets"... once a category held anything
    bool vramCategorySeen[gpumem::CATEGORY_COUNT] = {};
    const std::vector<std::string> vramCategoryNames = [](){
        std::vector<std::string> names;
        for (size_t c = 0; c < gpumem::CATEGORY_COUNT; c++)
            names.push_back(std::string("VRAM ") + gpumem::categoryName((gpumem::Category)c));
        return names;
    }();

    void publishMonitors(){
        const FrameTimeSample frame = FrameTimes::latest();
        counter("FPS", frame.fps, MonitorUnit::Quantity);
        counter("CPU frame ms", frame.cpuMs, MonitorUnit::TimeMs);
        // the newest frame whose GPU time arrived
        for (size_t i = FrameTimes::size(); i-- > 0 && FrameTimes::size() - i <= 8;){
            const FrameTimeSample sample = FrameTimes::at(i);
            if (sample.gpuMs >= 0.0f){
                counter("GPU frame ms", sample.gpuMs, MonitorUnit::TimeMs);
                break;
            }
        }
        const Engine::FrameStats& stats = Engine::getFrameStats();
        counter("Draw calls", stats.drawCalls, MonitorUnit::Quantity);
        counter("Instances", stats.instances, MonitorUnit::Quantity);
        counter("Triangles", (double)stats.triangles, MonitorUnit::Quantity);
        const BackendFrameCounters backend = SystemRender::getBackendFrameCounters();
        if (backend.valid){
            counter("Passes", backend.passes, MonitorUnit::Quantity);
            counter("Pipelines applied", backend.pipelines, MonitorUnit::Quantity);
            counter("Uniform bytes", backend.uniformBytes, MonitorUnit::Memory);
            counter("Image upload bytes", backend.imageUploadBytes, MonitorUnit::Memory);
            counter("Buffer upload bytes", backend.bufferUploadBytes, MonitorUnit::Memory);
        }
        uint32_t htmlDraws = 0;
        for (Scene* scene : Engine::getSceneLayers()){
            if (auto documents = scene->getSystem<UIDocumentSystem>())
                htmlDraws += documents->getRenderStats().drawCalls;
        }
        counter("HTML UI draws", htmlDraws, MonitorUnit::Quantity);
        // the slower ones a few times a second
        if (monitorFrame++ % 10 != 0)
            return;
        const gpumem::Report vram = gpumem::report(false);
        counter("VRAM estimated", (double)vram.totalBytes, MonitorUnit::Memory);
        // per category, for the GPU Memory module's chart
        for (size_t c = 0; c < gpumem::CATEGORY_COUNT; c++){
            if (vram.categoryBytes[c] || vramCategorySeen[c]){
                vramCategorySeen[c] = true;
                counter(vramCategoryNames[c].c_str(), (double)vram.categoryBytes[c], MonitorUnit::Memory);
            }
        }
        if (const uint64_t rss = processResidentBytes())
            counter("Process RSS", (double)rss, MonitorUnit::Memory);
        if (lua_State* L = LuaBinding::getLuaState())
            counter("Lua heap KiB", (double)lua_gc(L, LUA_GCCOUNT, 0) + (double)lua_gc(L, LUA_GCCOUNTB, 0) / 1024.0, MonitorUnit::Quantity);
        if (jobs::hasSystem()){
            const jobs::Stats jobStats = jobs::stats(10);
            uint32_t queued = 0;
            for (const jobs::LaneStats& lane : jobStats.lanes)
                queued += lane.queued;
            counter("Job queue depth", queued, MonitorUnit::Quantity);
            double busy = 0.0;
            for (const jobs::WorkerStats& worker : jobStats.perWorker)
                busy += worker.busyPct;
            counter("Worker utilisation %", jobStats.perWorker.empty() ? 0.0 : busy / (double)jobStats.perWorker.size(), MonitorUnit::Percent);
        }
    }

}

CommandLineOptions profiler::parseCommandLine(const std::vector<std::string>& args){
    CommandLineOptions options;
    for (size_t i = 0; i < args.size(); i++){
        const std::string& arg = args[i];
        auto value = [&](const char* flag, std::string& out) -> bool {
            const size_t length = std::strlen(flag);
            if (arg.compare(0, length, flag) != 0)
                return false;
            if (arg.size() > length && arg[length] == '='){
                out = arg.substr(length + 1);
                return true;
            }
            if (arg.size() == length && i + 1 < args.size()){
                out = args[++i];
                return true;
            }
            return false;
        };
        std::string text;
        if (arg == "--profile"){
            options.record = true;
        }else if (value("--profile-capture", text)){
            if (text.empty()){
                options.error = true;
                options.message = "--profile-capture needs a file path";
            }
            options.capturePath = text;
        }else if (value("--profile-frames", text)){
            if (!parseFrames(text, options.captureFrames)){
                options.error = true;
                options.message = "--profile-frames must be 1 to 600 (got '" + text + "')";
            }
        }
    }
    return options;
}

void profiler::engineInit(const std::vector<std::string>& args){
    jobs::ProfilerHooks hooks;
    hooks.onThreadStart = onThreadStart;
    hooks.onJobBegin = onJobBegin;
    hooks.onJobEnd = onJobEnd;
    hooks.onWaitBegin = onWaitBegin;
    hooks.onWaitEnd = onWaitEnd;
    jobs::setProfilerHooks(hooks);

    commandLine = parseCommandLine(args);
    cliCapture = 0;
    cliCaptureDone = false;
    if (commandLine.error){
        Log::error("Profiler: %s", commandLine.message.c_str());
        commandLine.capturePath.clear();
    }
    if (commandLine.record)
        setEnabled(true);
}

void profiler::engineFrameEnd(bool scenesLoaded){
    publishMonitors();
    if (commandLine.capturePath.empty() || cliCaptureDone)
        return;
    if (!cliCapture){
        if (!scenesLoaded)
            return;
        cliCapture = startCapture(commandLine.captureFrames);
        if (cliCapture){
            SystemRender::acquireGpuTiming();
            setGpuSemantics(SystemRender::getGpuTimingState().semantics);
            Log::print("Profiler capture started: %u frames for %s", commandLine.captureFrames, commandLine.capturePath.c_str());
        }
        return;
    }
    CaptureInfo info;
    if (!captureInfo(cliCapture, info) || !info.complete)
        return;
    cliCaptureDone = true;
    SystemRender::releaseGpuTiming();
    std::string error;
    if (saveCapture(cliCapture, commandLine.capturePath, &error))
        Log::print("Profiler capture written: %s", commandLine.capturePath.c_str());
    else
        Log::error("Profiler capture not written: %s", error.c_str());
}

void profiler::engineShutdown(){
    jobs::setProfilerHooks(jobs::ProfilerHooks());
}

uint64_t profiler::processResidentBytes(){
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters = {};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return (uint64_t)counters.WorkingSetSize;
    return 0;
#elif defined(__APPLE__)
    mach_task_basic_info info = {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        return (uint64_t)info.resident_size;
    return 0;
#elif defined(__linux__) || defined(__ANDROID__)
    FILE* file = std::fopen("/proc/self/statm", "r");
    if (!file)
        return 0;
    unsigned long long size = 0, resident = 0;
    const int read = std::fscanf(file, "%llu %llu", &size, &resident);
    std::fclose(file);
    return read == 2 ? (uint64_t)resident * (uint64_t)sysconf(_SC_PAGESIZE) : 0;
#else
    return 0;
#endif
}

#endif
