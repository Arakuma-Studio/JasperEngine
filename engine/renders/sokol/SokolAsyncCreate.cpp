// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SokolAsyncCreate.h"

#include "sokol_async.h"
#include "SokolPipelineCache.h"

#include "Engine.h"
#include "Log.h"
#include "System.h"
#include "io/AtomicFile.h"
#include "jobs/detail/TimedWait.h"
#include "profiler/Profile.h"
#include "render/GpuWorkerContext.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace jasperengine;

const char* jasperengine::gpuCreationModeName(GpuCreationMode mode){
    switch (mode){
    case GpuCreationMode::Sync: return "sync";
    case GpuCreationMode::GlWorker: return "worker";
    case GpuCreationMode::GlParallel: return "parallel";
    case GpuCreationMode::GlDeferred: return "deferred";
    case GpuCreationMode::Thread: return "thread";
    case GpuCreationMode::Metal: return "metal";
    }
    return "sync";
}

namespace {
    // see waitCompileStart (perf2: 5 and 20 ms still overlapped, 60 ms never did)
    constexpr double COMPILE_START_DELAY_MS = 60.0;
}

namespace {

    using Clock = std::chrono::steady_clock;

    double msSince(Clock::time_point start){
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    // frames the deferred GL mode waits before it reads a link status
    constexpr uint64_t DEFERRED_FRAMES = 2;
    // the Vulkan pipeline cache is saved at most this often while pipelines are created
    constexpr double VK_CACHE_SAVE_INTERVAL_S = 60.0;
    // how long setup waits for the worker to make its GL context current
    constexpr int WORKER_CONTEXT_TIMEOUT_MS = 5000;

    // An sg_shader_desc that owns its strings and bytes: the caller's (ShaderRender's source,
    // released after the queued create) may go before the creation runs
    struct OwnedShaderDesc {
        sg_shader_desc desc = {};
        std::deque<std::string> strings;
        std::deque<std::vector<uint8_t>> bytes;

        const char* keep(const char* text){
            if (!text)
                return nullptr;
            strings.emplace_back(text);
            return strings.back().c_str();
        }

        void keepFunction(sg_shader_function& function){
            function.source = keep(function.source);
            function.entry = keep(function.entry);
            function.d3d11_target = keep(function.d3d11_target);
            function.d3d11_filepath = keep(function.d3d11_filepath);
            if (function.bytecode.ptr && function.bytecode.size > 0){
                const uint8_t* data = static_cast<const uint8_t*>(function.bytecode.ptr);
                bytes.emplace_back(data, data + function.bytecode.size);
                function.bytecode.ptr = bytes.back().data();
            }
        }

        void assign(const sg_shader_desc& source){
            desc = source;
            keepFunction(desc.vertex_func);
            keepFunction(desc.fragment_func);
            keepFunction(desc.compute_func);
            for (sg_shader_vertex_attr& attr : desc.attrs){
                attr.glsl_name = keep(attr.glsl_name);
                attr.hlsl_sem_name = keep(attr.hlsl_sem_name);
            }
            for (sg_shader_uniform_block& block : desc.uniform_blocks){
                for (sg_glsl_shader_uniform& uniform : block.glsl_uniforms)
                    uniform.glsl_name = keep(uniform.glsl_name);
            }
            for (sg_shader_texture_sampler_pair& pair : desc.texture_sampler_pairs)
                pair.glsl_name = keep(pair.glsl_name);
            desc.label = keep(desc.label);
        }
    };

    // Waiting: a pipeline whose shader is pending. Running: on the worker, in the GL driver
    // (issued) or in Metal's handlers. Completed: its result is known, publishing waits for the budget
    enum class Stage : uint8_t { Waiting, Running, Completed };
    enum class Path : uint8_t { Worker, GlIssued, Metal };

    struct ShaderJob {
        sg_shader handle = {};
        OwnedShaderDesc desc;
        void* slot = nullptr;
        Path path = Path::Worker;
        Stage stage = Stage::Running;
        sg_resource_state result = SG_RESOURCESTATE_ALLOC;
        sgj_gl_program issued = {};
        uint64_t issuedFrame = 0;
        void* metal = nullptr;
        bool cancelled = false;
        bool destroyRequested = false;
        Clock::time_point start;
        double createMs = 0.0;
    };

    struct PipelineJob {
        sg_pipeline handle = {};
        sg_pipeline_desc desc = {};
        std::string label;
        void* slot = nullptr;
        Path path = Path::Worker;
        Stage stage = Stage::Waiting;
        sg_resource_state result = SG_RESOURCESTATE_ALLOC;
        void* metal = nullptr;
        bool cancelled = false;
        bool destroyRequested = false;
        Clock::time_point start;
        double createMs = 0.0;
    };

    struct WarmJob {
        uint64_t key = 0;
        uint32_t program = 0;
        sgj_gl_warm_draw draw = {};
        Clock::time_point start = Clock::now();   // requested (waitCompileStart)
    };

    // The worker reaches its job through the task, never through the maps (the render thread
    // inserts into them while it runs); a job is erased only after its task came back
    struct Task {
        enum class Kind : uint8_t { Shader, Pipeline, Warm } kind = Kind::Shader;
        uint32_t id = 0;        // the shader or pipeline handle
        uint64_t warmKey = 0;
        void* job = nullptr;
    };

    struct Completion {
        bool pipeline = false;
        uint32_t id = 0;
    };

    struct State {
        bool setUp = false;
        ShaderCreationSettings settings;
        GpuCreationMode mode = GpuCreationMode::Sync;
        sgj_async_caps caps = {};

        std::unique_ptr<GpuWorkerContext> workerContext;
        // a creation on the worker starts at least this long after its request (setup)
        double compileStartDelayMs = 0.0;
        std::thread worker;
        std::mutex mutex;                   // queue, finished and the worker's job fields
        std::condition_variable wake;
        std::deque<Task> queue;
        std::vector<Task> finished;
        bool stopping = false;
        int delayMs = 0;

        std::unordered_map<uint32_t, std::unique_ptr<ShaderJob>> shaders;
        // the ids of `shaders`, for isPending from another thread (the async loading thread's
        // ShaderRender::createShader): the map itself is the render thread's
        std::mutex pendingMutex;
        std::unordered_set<uint32_t> pendingShaderIds;
        std::unordered_map<uint32_t, std::unique_ptr<PipelineJob>> pipelines;
        std::unordered_map<uint32_t, std::vector<uint32_t>> waitingOnShader;
        std::unordered_map<uint32_t, int> shaderUsers;     // pipelines being created on a shader
        std::vector<sg_shader> deferredShaderDestroys;
        std::deque<Completion> completed;

        // A GL driver makes its variant of a program for a draw state at the draw, into the
        // program's list shared by the contexts (Mesa: unlocked): the worker's warm draw of a
        // program and the render thread's first draw of it with another state never overlap.
        // Under `mutex`: the program the worker draws now, and the programs the render thread
        // drew for the first time in this frame (the worker waits for the next update())
        uint32_t warmingProgram = 0;
        std::unordered_set<uint32_t> firstDrawn;
        std::condition_variable firstDrawsDone;

        std::unordered_map<uint64_t, std::unique_ptr<WarmJob>> warmPending;
        std::unordered_set<uint64_t> warmDone;
        std::unordered_map<uint32_t, std::vector<uint64_t>> warmByProgram;

        uint64_t frame = 0;
        double frameMainMs = 0.0;
        double frameSpentMs = 0.0;          // update()'s publishing in this frame, against the budget
        GpuCreationStats stats;

        std::string vkCachePath;
        bool vkCacheDirty = false;
        Clock::time_point vkCacheSaved;
    };

    State& state(){
        static State s;
        return s;
    }

    // the render thread's creation work, counted into the frame's total
    struct MainWork {
        State& s;
        Clock::time_point start = Clock::now();
        explicit MainWork(State& state): s(state) {}
        ~MainWork(){ s.frameMainMs += msSince(start); }
    };

    bool asyncOn(const State& s){
        return s.setUp && s.mode != GpuCreationMode::Sync;
    }

    void noteCreate(State& s, double ms){
        if (ms > s.stats.maxCreateMs)
            s.stats.maxCreateMs = ms;
    }

    void addPendingShader(State& s, uint32_t id, std::unique_ptr<ShaderJob> job){
        s.shaders[id] = std::move(job);
        std::lock_guard<std::mutex> lock(s.pendingMutex);
        s.pendingShaderIds.insert(id);
    }

    void enqueue(State& s, const Task& task){
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.queue.push_back(task);
        }
        s.wake.notify_one();
    }

    // A software GL (llvmpipe, softpipe, SwiftShader) gets no worker context (spec S32 §6.3,
    // deviation 6): on llvmpipe the render thread crashed drawing programs the worker had drawn
    // (warm draws; AtmosphereGpu.ScreenCloudsHideFarTerrain) and once hung in a draw after the
    // worker had linked its programs. JASPER_GL_SOFTWARE_WORKER=1 allows the worker (and its warm
    // draws) there anyway, for driver investigations
    bool softwareWorkerAllowed(){
        static const bool allowed = [](){
            const char* value = std::getenv("JASPER_GL_SOFTWARE_WORKER");
            return value && std::string(value) == "1";
        }();
        return allowed;
    }

    // GLES (Android): warm draws wait for a run on Adreno/Mali (the GPU campaign verified desktop
    // GL on NVIDIA only); JASPER_GL_WARM_DRAWS=1 allows them there, for driver investigations
    bool glesWarmDrawsAllowed(){
#if defined(SOKOL_GLES3)
        static const bool allowed = [](){
            const char* value = std::getenv("JASPER_GL_WARM_DRAWS");
            return value && std::string(value) == "1";
        }();
        return allowed;
#else
        return true;
#endif
    }

    // GL warm draws (spec S32 R4): asked for, in worker mode, on a hardware desktop GL
    bool warmOn(const State& s){
        if (s.mode != GpuCreationMode::GlWorker || !s.settings.glWarmDraws || !glesWarmDrawsAllowed())
            return false;
        return !s.caps.gl_software || softwareWorkerAllowed();
    }

    // ---------------------------------------------------------------- the worker thread

    // A compile (or warm draw) on the GL worker that starts while the render thread still handles
    // its request's frames held the render thread's GL calls for ~8-10 ms on NVIDIA, every new
    // variant; started ~60 ms later it held nothing (GPU campaign perf2, R22 9.1x -> 1.7x:
    // docs/perf/gpu-campaign.md). A 2 s compile does not notice the wait.
    void waitCompileStart(const State& s, Clock::time_point requested){
        if (s.compileStartDelayMs <= 0.0)
            return;
        const double waited = msSince(requested);
        if (waited < s.compileStartDelayMs)
            std::this_thread::sleep_for(std::chrono::microseconds((int64_t)((s.compileStartDelayMs - waited) * 1000.0)));
    }

    void runTask(State& s, const Task& task){
        JE_PROFILE_SCOPE("GPU create worker");
        if (s.delayMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(s.delayMs));
        // the render thread reads the results after the task came back through the mutex
        if (task.kind == Task::Kind::Shader){
            ShaderJob* job = static_cast<ShaderJob*>(task.job);
            waitCompileStart(s, job->start);
            const Clock::time_point start = Clock::now();
            job->result = sgj_shader_create(job->slot, &job->desc.desc);
            if (s.caps.gl)
                sgj_gl_finish();
            job->createMs = msSince(start);
        }else if (task.kind == Task::Kind::Pipeline){
            PipelineJob* job = static_cast<PipelineJob*>(task.job);
            waitCompileStart(s, job->start);
            const Clock::time_point start = Clock::now();
            job->result = sgj_pipeline_create(job->slot, &job->desc);
            job->createMs = msSince(start);
        }else{
            const WarmJob* job = static_cast<WarmJob*>(task.job);
            // a warm draw compiles too (the driver's program for its state): the same wait
            waitCompileStart(s, job->start);
            {
                std::unique_lock<std::mutex> lock(s.mutex);
                // (bounded: a render thread that stopped updating does not hold it forever)
                jobs::detail::timedWait(s.firstDrawsDone, lock, std::chrono::milliseconds(200),
                    [&s, job]{ return s.stopping || !s.firstDrawn.count(job->program); });
                s.warmingProgram = job->program;
            }
            sgj_gl_warm_run(&job->draw);
            std::lock_guard<std::mutex> lock(s.mutex);
            s.warmingProgram = 0;
        }
    }

    void workerMain(State* s, std::promise<bool>* contextReady){
        JE_PROFILE_THREAD("GPU Create Worker");
        bool hasContext = false;
        if (s->workerContext){
            hasContext = s->workerContext->makeCurrent();
            contextReady->set_value(hasContext);
            if (!hasContext)
                return;
        }else{
            contextReady->set_value(true);
        }
        for (;;){
            Task task;
            {
                std::unique_lock<std::mutex> lock(s->mutex);
                s->wake.wait(lock, [s]{ return s->stopping || !s->queue.empty(); });
                if (s->queue.empty())
                    break;
                task = s->queue.front();
                s->queue.pop_front();
            }
            runTask(*s, task);
            std::lock_guard<std::mutex> lock(s->mutex);
            s->finished.push_back(task);
        }
        if (hasContext){
            sgj_gl_warm_release();
            s->workerContext->release();
        }
    }

    // ---------------------------------------------------------------- creation on the render thread

    void forgetWarmKeys(State& s, sg_shader shader){
        if (!s.caps.gl || s.warmByProgram.empty())
            return;
        const uint32_t program = sgj_gl_shader_program(shader);
        auto it = s.warmByProgram.find(program);
        if (it == s.warmByProgram.end())
            return;
        // a later program can get the same GL name: its draws must warm again
        for (uint64_t key : it->second)
            s.warmDone.erase(key);
        s.warmByProgram.erase(it);
    }

    void destroyShaderNow(State& s, sg_shader shader){
        forgetWarmKeys(s, shader);
        if (sg_isvalid())
            sg_destroy_shader(shader);
    }

    bool shaderInUse(State& s, uint32_t id){
        auto users = s.shaderUsers.find(id);
        return (users != s.shaderUsers.end() && users->second > 0) || s.waitingOnShader.count(id) > 0;
    }

    void flushDeferredShaderDestroys(State& s){
        for (size_t i = 0; i < s.deferredShaderDestroys.size();){
            const sg_shader shader = s.deferredShaderDestroys[i];
            if (!shaderInUse(s, shader.id) && !s.shaders.count(shader.id)){
                destroyShaderNow(s, shader);
                s.deferredShaderDestroys.erase(s.deferredShaderDestroys.begin() + (std::ptrdiff_t)i);
            }else{
                i++;
            }
        }
    }

    void publishPipelineResult(State& s, PipelineJob& job){
        sgj_pipeline_publish(job.slot, job.handle, &job.desc, job.result);
        s.stats.createdPipelines++;
        noteCreate(s, job.createMs);
        if (s.caps.pipeline_on_thread)
            s.vkCacheDirty = true;
    }

    // a pipeline whose shader is VALID (or that never had to wait): prepared, then created where
    // the backend creates it. Erases the job when it is done at once
    void dispatchPipeline(State& s, std::unique_ptr<PipelineJob>& owned){
        PipelineJob& job = *owned;
        const uint32_t id = job.handle.id;
        job.start = Clock::now();
        job.desc.label = job.label.empty() ? nullptr : job.label.c_str();
        job.slot = sgj_pipeline_prepare(job.handle, &job.desc);
        if (!job.slot){
            // invalid or its shader failed: prepare published it FAILED
            s.stats.createdPipelines++;
            const bool destroy = job.destroyRequested;
            const sg_pipeline handle = job.handle;
            s.pipelines.erase(id);
            if (destroy && sg_isvalid())
                sg_destroy_pipeline(handle);
            return;
        }
        if (s.mode == GpuCreationMode::Thread && s.caps.pipeline_on_thread){
            job.path = Path::Worker;
            job.stage = Stage::Running;
            s.shaderUsers[job.desc.shader.id]++;
            enqueue(s, Task{Task::Kind::Pipeline, id, 0, &job});
            return;
        }
        if (s.mode == GpuCreationMode::Metal){
            job.metal = sgj_mtl_pipeline_begin(job.slot, &job.desc);
            if (job.metal){
                job.path = Path::Metal;
                job.stage = Stage::Running;
                s.shaderUsers[job.desc.shader.id]++;
                return;
            }
        }
        {
            JE_PROFILE_SCOPE("GPU pipeline create");
            job.result = sgj_pipeline_create(job.slot, &job.desc);
        }
        job.createMs = msSince(job.start);
        publishPipelineResult(s, job);
        const bool destroy = job.destroyRequested;
        const sg_pipeline handle = job.handle;
        s.pipelines.erase(id);
        if (destroy && sg_isvalid())
            sg_destroy_pipeline(handle);
    }

    void publishShader(State& s, uint32_t id){
        auto it = s.shaders.find(id);
        if (it == s.shaders.end())
            return;
        std::unique_ptr<ShaderJob> owned = std::move(it->second);
        s.shaders.erase(it);
        {
            std::lock_guard<std::mutex> lock(s.pendingMutex);
            s.pendingShaderIds.erase(id);
        }
        ShaderJob& job = *owned;
        if (!job.cancelled){
            if (job.path == Path::GlIssued){
                const Clock::time_point start = Clock::now();
                job.result = sgj_gl_adopt(job.slot, &job.desc.desc, &job.issued);
                job.createMs += msSince(start);
            }else if (job.path == Path::Metal){
                job.result = sgj_mtl_job_finish(job.metal, job.slot, &job.desc.desc);
            }
        }else if (job.path == Path::GlIssued){
            sgj_gl_discard(&job.issued);
        }
        if (job.metal){
            sgj_mtl_job_free(job.metal);
            job.metal = nullptr;
        }
        if (job.result != SG_RESOURCESTATE_VALID)
            job.result = SG_RESOURCESTATE_FAILED;
        sgj_shader_publish(job.slot, job.handle, &job.desc.desc, job.result);
        s.stats.createdShaders++;
        noteCreate(s, job.createMs);

        // the pipelines that waited for it go on (or fail at prepare, the shader having failed);
        // a shader on its way out takes them with it
        auto waiting = s.waitingOnShader.find(id);
        if (waiting != s.waitingOnShader.end()){
            std::vector<uint32_t> ids = std::move(waiting->second);
            s.waitingOnShader.erase(waiting);
            for (uint32_t pipelineId : ids){
                auto pit = s.pipelines.find(pipelineId);
                if (pit == s.pipelines.end())
                    continue;
                if (job.destroyRequested || pit->second->destroyRequested){
                    const sg_pipeline handle = pit->second->handle;
                    s.pipelines.erase(pit);
                    if (sg_isvalid())
                        sg_destroy_pipeline(handle);
                    continue;
                }
                dispatchPipeline(s, pit->second);
            }
        }
        if (job.destroyRequested){
            if (shaderInUse(s, id)){
                s.deferredShaderDestroys.push_back(job.handle);
            }else{
                destroyShaderNow(s, job.handle);
            }
        }
    }

    void publishPipeline(State& s, uint32_t id){
        auto it = s.pipelines.find(id);
        if (it == s.pipelines.end())
            return;
        std::unique_ptr<PipelineJob> owned = std::move(it->second);
        s.pipelines.erase(it);
        PipelineJob& job = *owned;
        if (job.path == Path::Metal && job.metal){
            if (!job.cancelled)
                job.result = sgj_mtl_job_finish(job.metal, job.slot, &job.desc);
            sgj_mtl_job_free(job.metal);
            job.metal = nullptr;
        }
        if (job.result != SG_RESOURCESTATE_VALID)
            job.result = SG_RESOURCESTATE_FAILED;
        publishPipelineResult(s, job);
        auto users = s.shaderUsers.find(job.desc.shader.id);
        if (users != s.shaderUsers.end() && --users->second <= 0)
            s.shaderUsers.erase(users);
        if (job.destroyRequested && sg_isvalid())
            sg_destroy_pipeline(job.handle);
    }

    // ---------------------------------------------------------------- the Vulkan pipeline cache

    struct VkCacheHeader {
        char magic[4];              // "JEVK"
        uint32_t version;
        uint32_t vendorId;
        uint32_t deviceId;
        uint32_t driverVersion;
        uint8_t uuid[16];
        uint64_t size;
        uint64_t checksum;          // FNV-1a of the data
    };
    constexpr uint32_t VK_CACHE_VERSION = 1;

    uint64_t fnv64(const uint8_t* data, size_t size){
        uint64_t h = 14695981039346656037ull;
        for (size_t i = 0; i < size; i++){
            h ^= data[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    std::string hex(const uint8_t* data, size_t size){
        static const char digits[] = "0123456789abcdef";
        std::string out;
        for (size_t i = 0; i < size; i++){
            out += digits[data[i] >> 4];
            out += digits[data[i] & 15];
        }
        return out;
    }

    std::string vkCachePathFor(const sgj_vk_device_identity& id){
        std::string base = System::instance().getCachePath();
        if (base.empty())
            base = ".";
        char name[64];
        std::snprintf(name, sizeof(name), "%04x-%04x-", id.vendor_id, id.device_id);
        return (std::filesystem::path(base) / "pipelinecache" / (std::string(name) + hex(id.uuid, sizeof(id.uuid)) + ".vkpc")).generic_string();
    }

    // the cached data when the file is this device's and intact, else empty
    std::vector<uint8_t> readVkCache(const std::string& path, const sgj_vk_device_identity& id){
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return {};
        VkCacheHeader header = {};
        if (!file.read(reinterpret_cast<char*>(&header), sizeof(header)))
            return {};
        if (std::memcmp(header.magic, "JEVK", 4) != 0 || header.version != VK_CACHE_VERSION ||
                header.vendorId != id.vendor_id || header.deviceId != id.device_id ||
                header.driverVersion != id.driver_version || std::memcmp(header.uuid, id.uuid, sizeof(id.uuid)) != 0 ||
                header.size == 0 || header.size > (256ull << 20)){
            Log::verbose("Vulkan pipeline cache %s is another device's or version's: ignored", path.c_str());
            return {};
        }
        std::vector<uint8_t> data((size_t)header.size);
        if (!file.read(reinterpret_cast<char*>(data.data()), (std::streamsize)data.size()) ||
                fnv64(data.data(), data.size()) != header.checksum){
            Log::warn("Vulkan pipeline cache %s is damaged: ignored", path.c_str());
            return {};
        }
        return data;
    }

    void writeVkCache(State& s){
        if (s.vkCachePath.empty())
            return;
        sgj_vk_device_identity id = {};
        if (!sgj_vk_device_identity_query(&id))
            return;
        const size_t size = sgj_vk_pipeline_cache_data(nullptr, 0);
        if (size == 0)
            return;
        std::string bytes(sizeof(VkCacheHeader) + size, '\0');
        const size_t written = sgj_vk_pipeline_cache_data(&bytes[sizeof(VkCacheHeader)], size);
        if (written == 0)
            return;
        bytes.resize(sizeof(VkCacheHeader) + written);
        VkCacheHeader header = {};
        std::memcpy(header.magic, "JEVK", 4);
        header.version = VK_CACHE_VERSION;
        header.vendorId = id.vendor_id;
        header.deviceId = id.device_id;
        header.driverVersion = id.driver_version;
        std::memcpy(header.uuid, id.uuid, sizeof(id.uuid));
        header.size = written;
        header.checksum = fnv64(reinterpret_cast<const uint8_t*>(&bytes[sizeof(VkCacheHeader)]), written);
        std::memcpy(&bytes[0], &header, sizeof(header));
        io::AtomicWriteOptions options;
        options.durability = io::Durability::Cache;
        const io::WriteResult result = io::writeFileAtomic(s.vkCachePath, bytes, options);
        if (!result.ok())
            Log::warn("Vulkan pipeline cache not saved: %s", result.message.c_str());
        s.vkCacheDirty = false;
        s.vkCacheSaved = Clock::now();
    }

    void setupVkCache(State& s){
        sgj_vk_device_identity id = {};
        if (!sgj_vk_device_identity_query(&id))
            return;
        s.vkCachePath = vkCachePathFor(id);
        const std::vector<uint8_t> data = readVkCache(s.vkCachePath, id);
        if (!sgj_vk_pipeline_cache_create(data.empty() ? nullptr : data.data(), data.size())){
            Log::warn("Vulkan pipeline cache not available: pipelines compile without one");
            s.vkCachePath.clear();
        }
        s.vkCacheSaved = Clock::now();
    }

    // JASPER_ASYNC_SHADERS: a forced mode, as a GL mode name; empty when unset
    std::string forcedMode(){
        const char* value = std::getenv("JASPER_ASYNC_SHADERS");
        return value ? std::string(value) : std::string();
    }

    // the GL mode of this device, with or without a worker context (see the header)
    GpuCreationMode chooseGlMode(State& s, const std::string& forced){
        bool wantWorker = forced.empty() || forced == "worker";
        if (wantWorker && s.caps.gl_software && !softwareWorkerAllowed()){
            if (forced == "worker")
                Log::warn("[gpu] JASPER_ASYNC_SHADERS=worker: no worker context on a software GL (JASPER_GL_SOFTWARE_WORKER=1 allows one)");
            wantWorker = false;
        }
        if (wantWorker){
            const GpuWorkerContextFactory& factory = getGpuWorkerContextFactory();
            if (factory)
                s.workerContext = factory();
            if (s.workerContext){
                std::promise<bool> contextReady;
                std::future<bool> ready = contextReady.get_future();
                s.stopping = false;
                s.worker = std::thread(workerMain, &s, &contextReady);
                const bool current = ready.wait_for(std::chrono::milliseconds(WORKER_CONTEXT_TIMEOUT_MS)) == std::future_status::ready && ready.get();
                if (current)
                    return GpuCreationMode::GlWorker;
                // the thread ended without a context (or will: it waits on nothing else)
                s.worker.join();
                s.workerContext.reset();
                Log::warn("[gpu] the worker GL context could not be made current: shaders compile on the render thread");
            }else if (forced == "worker"){
                Log::warn("[gpu] JASPER_ASYNC_SHADERS=worker: this platform has no worker GL context");
            }
        }
        if (forced == "deferred")
            return GpuCreationMode::GlDeferred;
        if (forced == "parallel" && !s.caps.gl_parallel_compile)
            Log::warn("[gpu] JASPER_ASYNC_SHADERS=parallel: KHR_parallel_shader_compile is not available");
        if (!s.caps.gl_parallel_compile)
            return GpuCreationMode::GlDeferred;
        // the driver's compile threads: the extension leaves their initial count to the driver, and
        // NVIDIA's is none (its glLinkProgram then returns once linked, 2.1 s for a Lit lobe variant
        // on an RTX 3080 Ti, and COMPLETION_STATUS is always true). Only in this mode: with a worker
        // context the setting makes the worker's links return early and the render thread's first
        // draw wait for them (docs/perf/gpu-campaign.md, S32). Not on a software GL: llvmpipe's
        // compile was complete at once without them (the CI reference), and with them a lazily
        // made shader can finish after the frame a test or an export check reads
        if (!s.caps.gl_software)
            sgj_gl_enable_compile_threads();
        return GpuCreationMode::GlParallel;
    }

}

// ==================================================================== public

void SokolAsyncCreate::setup(){
    State& s = state();
    if (s.setUp)
        return;
    s.stats = GpuCreationStats();
    s.frameMainMs = s.frameSpentMs = 0.0;
    sgj_async_setup();
    s.caps = sgj_async_query_caps();
    const char* delay = std::getenv("JASPER_ASYNC_SHADERS_DELAY_MS");
    s.delayMs = delay ? std::max(0, std::atoi(delay)) : 0;

    const std::string forced = forcedMode();
    GpuCreationMode mode = GpuCreationMode::Sync;
    if (s.settings.asyncCreation && forced != "sync"){
        if (s.caps.gl){
            mode = chooseGlMode(s, forced);
        }else if (s.caps.metal){
            mode = GpuCreationMode::Metal;
        }else if (s.caps.shader_on_thread || s.caps.pipeline_on_thread){
            mode = GpuCreationMode::Thread;
        }
    }
    if (s.caps.pipeline_on_thread)
        setupVkCache(s);
    // Vulkan and D3D11 create on a worker of their own (GL's started with its context above)
    if (mode == GpuCreationMode::Thread){
        std::promise<bool> started;
        s.stopping = false;
        s.worker = std::thread(workerMain, &s, &started);
        started.get_future().wait();
    }
    s.mode = mode;
    s.stats.mode = mode;
    // GL worker on a hardware GL (measured on NVIDIA; on Vulkan it made no measurable
    // difference, and software drivers have no such stall); JASPER_GPU_COMPILE_DELAY_MS overrides
    // (0: none) in any thread mode
    const char* compileDelay = std::getenv("JASPER_GPU_COMPILE_DELAY_MS");
    const bool hardwareWorker = mode == GpuCreationMode::GlWorker && !s.caps.gl_software;
    s.compileStartDelayMs = compileDelay ? std::max(0.0, std::atof(compileDelay)) : (hardwareWorker ? COMPILE_START_DELAY_MS : 0.0);
    s.setUp = true;
    Log::print("[gpu] shader creation: %s", gpuCreationModeName(mode));
    if (s.caps.gl && s.caps.gl_software && mode != GpuCreationMode::GlWorker && mode != GpuCreationMode::Sync)
        Log::verbose("[gpu] a software GL: no worker context, the driver's compile threads or a deferred link status instead");
}

void SokolAsyncCreate::shutdown(){
    State& s = state();
    if (!s.setUp)
        return;

    // what has not started is cancelled; what runs ends (a creation cannot be interrupted)
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        for (const Task& task : s.queue){
            if (task.kind == Task::Kind::Shader){
                static_cast<ShaderJob*>(task.job)->cancelled = true;
            }else if (task.kind == Task::Kind::Pipeline){
                static_cast<PipelineJob*>(task.job)->cancelled = true;
            }
        }
        s.queue.clear();
        s.stopping = true;
        s.firstDrawn.clear();
    }
    s.wake.notify_all();
    s.firstDrawsDone.notify_all();
    if (s.worker.joinable())
        s.worker.join();
    s.workerContext.reset();
    s.finished.clear();
    // what was warmed stays warm while the GL context lives (a setting toggled sets the creation
    // up again over the same programs); forgetWarmDraws drops it with the context
    s.warmPending.clear();

    // Metal's handlers write into their jobs: they run before the jobs go
    for (const auto& [id, job] : s.shaders){
        while (job->metal && !sgj_mtl_job_ready(job->metal))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (const auto& [id, job] : s.pipelines){
        while (job->metal && !sgj_mtl_job_ready(job->metal))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // every slot leaves ALLOC (sg_shutdown releases what was created); waiting pipelines are
    // released before their shaders
    std::vector<uint32_t> ids;
    for (const auto& [id, job] : s.pipelines){
        if (job->stage == Stage::Waiting)
            ids.push_back(id);
    }
    for (uint32_t id : ids){
        if (sg_isvalid())
            sg_destroy_pipeline(s.pipelines[id]->handle);
        s.pipelines.erase(id);
    }
    s.waitingOnShader.clear();
    ids.clear();
    for (const auto& [id, job] : s.shaders){
        ids.push_back(id);
    }
    for (uint32_t id : ids){
        auto it = s.shaders.find(id);
        if (it != s.shaders.end() && it->second->stage != Stage::Completed)
            it->second->cancelled = it->second->cancelled || it->second->path != Path::Worker;
        publishShader(s, id);
    }
    ids.clear();
    for (const auto& [id, job] : s.pipelines){
        ids.push_back(id);
    }
    for (uint32_t id : ids){
        publishPipeline(s, id);
    }
    s.completed.clear();
    s.shaderUsers.clear();
    flushDeferredShaderDestroys(s);
    s.deferredShaderDestroys.clear();

    if (s.caps.pipeline_on_thread){
        // what this run added since the last save (an unchanged file is left as it is)
        if (s.vkCacheDirty)
            writeVkCache(s);
        sgj_vk_pipeline_cache_destroy();
    }
    s.vkCachePath.clear();
    sgj_async_shutdown();
    s.mode = GpuCreationMode::Sync;
    s.setUp = false;
}

void SokolAsyncCreate::setSettings(const ShaderCreationSettings& settings){
    State& s = state();
    ShaderCreationSettings next = settings;
    if (!(next.mainThreadBudgetMs >= 0.5f))
        next.mainThreadBudgetMs = 0.5f;
    if (next.mainThreadBudgetMs > 16.0f)
        next.mainThreadBudgetMs = 16.0f;
    // the mode is chosen at setup: turning creation on or off sets it up again (what is pending
    // is published first)
    const bool restart = s.setUp && next.asyncCreation != s.settings.asyncCreation;
    if (restart)
        shutdown();
    s.settings = next;
    if (restart)
        setup();
}

const ShaderCreationSettings& SokolAsyncCreate::getSettings(){
    return state().settings;
}

sg_shader SokolAsyncCreate::makeShader(const sg_shader_desc& desc){
    State& s = state();
    if (!asyncOn(s)){
        // the render thread's creation, timed like the asynchronous paths' share of it
        JE_PROFILE_SCOPE("GPU create submit");
        const Clock::time_point start = Clock::now();
        const sg_shader shader = sg_make_shader(&desc);
        const double ms = msSince(start);
        s.frameMainMs += ms;
        noteCreate(s, ms);
        s.stats.createdShaders++;
        return shader;
    }
    const sg_shader shader = sg_alloc_shader();
    if (shader.id != SG_INVALID_ID)
        initShader(shader, desc);
    return shader;
}

void SokolAsyncCreate::initShader(sg_shader shader, const sg_shader_desc& desc){
    State& s = state();
    // a strand's queued make whose handle was destroyed first (ShaderPool::clear): nothing to make or count
    if (sg_query_shader_state(shader) == SG_RESOURCESTATE_INVALID)
        return;
    if (!asyncOn(s)){
        // the render thread's creation, timed like the asynchronous paths' share of it
        JE_PROFILE_SCOPE("GPU create submit");
        const Clock::time_point start = Clock::now();
        sg_init_shader(shader, &desc);
        const double ms = msSince(start);
        s.frameMainMs += ms;
        noteCreate(s, ms);
        s.stats.createdShaders++;
        return;
    }
    MainWork work(s);
    JE_PROFILE_SCOPE("GPU create submit");
    auto job = std::make_unique<ShaderJob>();
    job->handle = shader;
    job->start = Clock::now();
    job->desc.assign(desc);
    job->slot = sgj_shader_prepare(shader, &job->desc.desc);
    if (!job->slot){
        // published FAILED by prepare (it logged why)
        s.stats.createdShaders++;
        return;
    }
    switch (s.mode){
    case GpuCreationMode::GlWorker:
    case GpuCreationMode::Thread:
        if (s.caps.gl || s.caps.shader_on_thread){
            job->path = Path::Worker;
            ShaderJob* raw = job.get();
            addPendingShader(s, shader.id, std::move(job));
            enqueue(s, Task{Task::Kind::Shader, shader.id, 0, raw});
            return;
        }
        break;
    case GpuCreationMode::GlParallel:
    case GpuCreationMode::GlDeferred:
        if (sgj_gl_issue(&job->desc.desc, &job->issued)){
            job->path = Path::GlIssued;
            job->issuedFrame = s.frame;
            job->createMs = msSince(job->start);
            addPendingShader(s, shader.id, std::move(job));
            return;
        }
        break;
    case GpuCreationMode::Metal:
        job->metal = sgj_mtl_shader_begin(job->slot, &job->desc.desc);
        if (job->metal){
            job->path = Path::Metal;
            addPendingShader(s, shader.id, std::move(job));
            return;
        }
        break;
    case GpuCreationMode::Sync:
        break;
    }
    // what the mode does not move (Vulkan shader modules, a GL compute shader): created now
    job->result = sgj_shader_create(job->slot, &job->desc.desc);
    job->createMs = msSince(job->start);
    sgj_shader_publish(job->slot, shader, &job->desc.desc, job->result);
    s.stats.createdShaders++;
    noteCreate(s, job->createMs);
}

void SokolAsyncCreate::destroyShader(sg_shader shader){
    State& s = state();
    if (shader.id == SG_INVALID_ID)
        return;
    if (!s.setUp){
        if (sg_isvalid())
            sg_destroy_shader(shader);
        return;
    }
    auto it = s.shaders.find(shader.id);
    if (it != s.shaders.end()){
        it->second->destroyRequested = true;
        return;
    }
    if (shaderInUse(s, shader.id)){
        s.deferredShaderDestroys.push_back(shader);
        return;
    }
    destroyShaderNow(s, shader);
}

bool SokolAsyncCreate::isPending(sg_shader shader){
    State& s = state();
    if (shader.id == SG_INVALID_ID)
        return false;
    std::lock_guard<std::mutex> lock(s.pendingMutex);
    return s.pendingShaderIds.count(shader.id) > 0;
}

sg_pipeline SokolAsyncCreate::makePipeline(const sg_pipeline_desc& desc){
    State& s = state();
    if (!asyncOn(s)){
        // the render thread's creation, timed like the asynchronous paths' share of it
        JE_PROFILE_SCOPE("GPU pipeline create");
        const Clock::time_point start = Clock::now();
        const sg_pipeline pipeline = sg_make_pipeline(&desc);
        const double ms = msSince(start);
        s.frameMainMs += ms;
        noteCreate(s, ms);
        s.stats.createdPipelines++;
        return pipeline;
    }
    const sg_pipeline pipeline = sg_alloc_pipeline();
    if (pipeline.id != SG_INVALID_ID)
        initPipeline(pipeline, desc);
    return pipeline;
}

void SokolAsyncCreate::initPipeline(sg_pipeline pipeline, const sg_pipeline_desc& desc){
    State& s = state();
    if (!asyncOn(s)){
        // the render thread's creation, timed like the asynchronous paths' share of it
        JE_PROFILE_SCOPE("GPU pipeline create");
        const Clock::time_point start = Clock::now();
        sg_init_pipeline(pipeline, &desc);
        const double ms = msSince(start);
        s.frameMainMs += ms;
        noteCreate(s, ms);
        s.stats.createdPipelines++;
        return;
    }
    MainWork work(s);
    JE_PROFILE_SCOPE("GPU create submit");
    auto job = std::make_unique<PipelineJob>();
    job->handle = pipeline;
    job->desc = desc;
    if (desc.label)
        job->label = desc.label;
    const uint32_t id = pipeline.id;
    if (s.shaders.count(desc.shader.id)){
        job->stage = Stage::Waiting;
        s.waitingOnShader[desc.shader.id].push_back(id);
        s.pipelines[id] = std::move(job);
        return;
    }
    std::unique_ptr<PipelineJob>& owned = s.pipelines[id];
    owned = std::move(job);
    dispatchPipeline(s, owned);
}

void SokolAsyncCreate::destroyPipeline(sg_pipeline pipeline){
    State& s = state();
    if (pipeline.id == SG_INVALID_ID)
        return;
    if (s.setUp){
        auto it = s.pipelines.find(pipeline.id);
        if (it != s.pipelines.end()){
            if (it->second->stage == Stage::Waiting){
                // nothing was created for it: the slot goes now
                const uint32_t shaderId = it->second->desc.shader.id;
                auto waiting = s.waitingOnShader.find(shaderId);
                if (waiting != s.waitingOnShader.end()){
                    std::vector<uint32_t>& ids = waiting->second;
                    for (size_t i = 0; i < ids.size(); i++){
                        if (ids[i] == pipeline.id){
                            ids.erase(ids.begin() + (std::ptrdiff_t)i);
                            break;
                        }
                    }
                    if (ids.empty())
                        s.waitingOnShader.erase(waiting);
                }
                s.pipelines.erase(it);
                if (sg_isvalid())
                    sg_destroy_pipeline(pipeline);
                flushDeferredShaderDestroys(s);
                return;
            }
            it->second->destroyRequested = true;
            return;
        }
    }
    if (sg_isvalid())
        sg_destroy_pipeline(pipeline);
}

bool SokolAsyncCreate::isPending(sg_pipeline pipeline){
    const State& s = state();
    return pipeline.id != SG_INVALID_ID && s.pipelines.count(pipeline.id) > 0;
}

uint64_t SokolAsyncCreate::requestWarmDraw(sg_pipeline pipeline, const sg_bindings& bindings){
    State& s = state();
    if (!warmOn(s))
        return 0;
    MainWork work(s);
    auto job = std::make_unique<WarmJob>();
    uint64_t key = 0;
    if (!sgj_gl_warm_describe(pipeline, &bindings, &job->draw, &key))
        return 0;
    if (s.warmDone.count(key) || s.warmPending.count(key))
        return key;
    job->key = key;
    job->program = job->draw.program;
    s.warmByProgram[job->program].push_back(key);
    WarmJob* raw = job.get();
    s.warmPending[key] = std::move(job);
    enqueue(s, Task{Task::Kind::Warm, 0, key, raw});
    return key;
}

bool SokolAsyncCreate::beginFirstDraw(sg_shader shader){
    State& s = state();
    if (!warmOn(s))
        return true;
    const uint32_t program = sgj_gl_shader_program(shader);
    if (program == 0)
        return true;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.warmingProgram == program)
        return false;
    s.firstDrawn.insert(program);
    return true;
}

bool SokolAsyncCreate::warmDrawsActive(){
    const State& s = state();
    return warmOn(s);
}

bool SokolAsyncCreate::isWarm(uint64_t warmKey){
    const State& s = state();
    return warmKey == 0 || s.warmDone.count(warmKey) > 0 || !s.warmPending.count(warmKey);
}

namespace {

    // collects what finished and publishes it: within the frame's budget (one at least), or
    // everything when unlimited (a wait), deferred GL programs included
    void pump(State& s, bool unlimited){
        MainWork work(s);
        JE_PROFILE_SCOPE("GPU create publish");
        const Clock::time_point start = Clock::now();

        // what the worker finished
        std::vector<Task> finished;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            finished.swap(s.finished);
        }
        for (const Task& task : finished){
            if (task.kind == Task::Kind::Warm){
                s.warmPending.erase(task.warmKey);
                s.warmDone.insert(task.warmKey);
                s.stats.warmDraws++;
                continue;
            }
            const bool pipeline = task.kind == Task::Kind::Pipeline;
            if (pipeline){
                auto it = s.pipelines.find(task.id);
                if (it != s.pipelines.end())
                    it->second->stage = Stage::Completed;
            }else{
                auto it = s.shaders.find(task.id);
                if (it != s.shaders.end())
                    it->second->stage = Stage::Completed;
            }
            s.completed.push_back(Completion{pipeline, task.id});
        }

        // what the GL driver and Metal finished (asked without waiting)
        bool deferredTaken = false;
        for (auto& [id, job] : s.shaders){
            if (job->stage != Stage::Running)
                continue;
            // JASPER_ASYNC_SHADERS_DELAY_MS (tests) holds the driver's creations as the worker's
            if (s.delayMs > 0 && !unlimited && msSince(job->start) < (double)s.delayMs)
                continue;
            bool ready = false;
            if (job->path == Path::GlIssued){
                if (s.mode == GpuCreationMode::GlParallel){
                    ready = unlimited || sgj_gl_is_complete(&job->issued);
                }else if (unlimited || (!deferredTaken && s.frame >= job->issuedFrame + DEFERRED_FRAMES)){
                    // one a frame: a driver that compiled nothing meanwhile waits in one frame for one
                    ready = true;
                    deferredTaken = true;
                }
            }else if (job->path == Path::Metal){
                ready = sgj_mtl_job_ready(job->metal);
            }
            if (ready){
                job->stage = Stage::Completed;
                s.completed.push_back(Completion{false, id});
            }
        }
        if (s.mode == GpuCreationMode::Metal){
            for (auto& [id, job] : s.pipelines){
                if (job->stage == Stage::Running && job->path == Path::Metal && sgj_mtl_job_ready(job->metal)){
                    job->stage = Stage::Completed;
                    s.completed.push_back(Completion{true, id});
                }
            }
        }

        // published in completion order, within the budget; one at least
        const double budget = s.settings.mainThreadBudgetMs;
        size_t published = 0;
        while (!s.completed.empty()){
            if (!unlimited && published > 0 && msSince(start) >= budget)
                break;
            const Completion next = s.completed.front();
            s.completed.pop_front();
            if (next.pipeline){
                publishPipeline(s, next.id);
            }else{
                publishShader(s, next.id);
            }
            published++;
        }
        flushDeferredShaderDestroys(s);
        s.frameSpentMs += msSince(start);
    }

}

void SokolAsyncCreate::update(){
    State& s = state();
    if (!s.setUp)
        return;
    // a frame's total is what ran since the last update
    s.frame++;
    s.stats.lastFrameMainMs = s.frameMainMs;
    if (s.frameMainMs > s.stats.maxMainMs)
        s.stats.maxMainMs = s.frameMainMs;
    s.frameMainMs = 0.0;
    s.frameSpentMs = 0.0;
    JE_PROFILE_COUNTER("Shaders pending", s.shaders.size());
    JE_PROFILE_COUNTER("Pipelines pending", s.pipelines.size());
    // the interned pipelines released a frame ago and not taken again
    SokolPipelineCache::collect();
    // last frame's first draws are submitted: the worker may warm their programs again
    if (s.caps.gl){
        bool released = false;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            released = !s.firstDrawn.empty();
            s.firstDrawn.clear();
        }
        if (released)
            s.firstDrawsDone.notify_all();
    }
    if (s.shaders.empty() && s.pipelines.empty() && s.warmPending.empty() && s.deferredShaderDestroys.empty() && s.completed.empty())
        return;

    pump(s, false);

    if (s.vkCacheDirty && s.pipelines.empty() &&
            std::chrono::duration<double>(Clock::now() - s.vkCacheSaved).count() >= VK_CACHE_SAVE_INTERVAL_S)
        writeVkCache(s);
}

sg_pipeline SokolAsyncCreate::makePipelineNow(const sg_pipeline_desc& desc){
    State& s = state();
    if (asyncOn(s) && s.shaders.count(desc.shader.id))
        waitFor(desc.shader);
    MainWork work(s);
    JE_PROFILE_SCOPE("GPU pipeline create");
    const Clock::time_point start = Clock::now();
    const sg_pipeline pipeline = sg_make_pipeline(&desc);
    noteCreate(s, msSince(start));
    s.stats.createdPipelines++;
    if (s.caps.pipeline_on_thread)
        s.vkCacheDirty = true;
    return pipeline;
}

namespace {
    // a render-thread wait for a creation in flight: in the profile ("GPU create wait") and,
    // over 20 ms, in the log with what it waited for (a frame stall's cause)
    void noteWait(const Clock::time_point start, const char* kind, const std::string& label){
        const double ms = msSince(start);
        if (ms > 20.0)
            Log::verbose("[gpu] the render thread waited %.1f ms for %s '%s' being created", ms, kind, label.c_str());
    }
}

void SokolAsyncCreate::waitFor(sg_pipeline pipeline){
    State& s = state();
    if (!(s.setUp && isPending(pipeline)))
        return;
    JE_PROFILE_SCOPE("GPU create wait");
    const Clock::time_point start = Clock::now();
    auto it = s.pipelines.find(pipeline.id);
    const std::string label = it != s.pipelines.end() ? it->second->label : std::string();
    while (s.setUp && isPending(pipeline)){
        pump(s, true);
        if (isPending(pipeline))
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    noteWait(start, "pipeline", label);
}

void SokolAsyncCreate::waitFor(sg_shader shader){
    State& s = state();
    if (!(s.setUp && isPending(shader)))
        return;
    JE_PROFILE_SCOPE("GPU create wait");
    const Clock::time_point start = Clock::now();
    auto it = s.shaders.find(shader.id);
    const std::string label = (it != s.shaders.end() && it->second->desc.desc.label) ? it->second->desc.desc.label : std::string();
    while (s.setUp && isPending(shader)){
        pump(s, true);
        if (isPending(shader))
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    noteWait(start, "shader", label);
}

bool SokolAsyncCreate::isIdle(){
    const State& s = state();
    return s.shaders.empty() && s.pipelines.empty() && s.warmPending.empty() && s.completed.empty();
}

bool SokolAsyncCreate::waitIdle(double timeoutMs){
    const Clock::time_point start = Clock::now();
    for (;;){
        update();
        if (isIdle())
            return true;
        if (msSince(start) >= timeoutMs)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool SokolAsyncCreate::isSoftwareGl(){
    return state().caps.gl_software;
}

GpuCreationMode SokolAsyncCreate::getMode(){
    return state().mode;
}

GpuCreationStats SokolAsyncCreate::getStats(){
    const State& s = state();
    GpuCreationStats stats = s.stats;
    stats.mode = s.mode;
    stats.pendingShaders = s.shaders.size();
    stats.pendingPipelines = s.pipelines.size();
    stats.pendingWarmDraws = s.warmPending.size();
    stats.internedPipelines = SokolPipelineCache::internedSize();
    return stats;
}

void SokolAsyncCreate::resetMaxMainMs(){
    State& s = state();
    s.stats.maxMainMs = 0.0;
    s.stats.maxCreateMs = 0.0;
}

void SokolAsyncCreate::savePipelineCache(){
    State& s = state();
    if (s.setUp && s.caps.pipeline_on_thread)
        writeVkCache(s);
}

void SokolAsyncCreate::forgetWarmDraws(){
    State& s = state();
    s.warmDone.clear();
    s.warmByProgram.clear();
}

std::string SokolAsyncCreate::getPipelineCachePath(){
    return state().vkCachePath;
}
