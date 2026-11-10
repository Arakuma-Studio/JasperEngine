// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RuntimeShaderCompiler.h"

#include "Engine.h"
#include "Log.h"
#include "System.h"
#include "io/Data.h"
#include "io/FileData.h"
#include "jobs/JobSystem.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderDataSerializer.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderVariantCompiler.h"

#include "sokol_time.h"

#include <atomic>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>
#include <vector>

using namespace jasperengine;

namespace {

struct Variant {
    ResourceLoadState state = ResourceLoadState::Loading;
    ShaderData data;
    std::string error;
    std::string log;
    // reported once, when the variant is first handed over
    bool reported = false;
    // the frame a worker (or beginFrame) settled it in, for getPendingCount; none when settled
    // where it was asked for
    uint64_t settledFrame = UINT64_MAX;
    bool fromDisk = false;
    bool cacheWritten = false;
    double milliseconds = 0.0;
};

struct Job {
    ShaderKey key;
    std::shared_ptr<const CustomShaderSources> sources;
    std::string cachePath; // where the compiled variant is cached, empty for none
};

struct State {
    std::mutex mutex;
    std::map<ShaderKey, Variant> variants;
    // forked shader sources, resolved once per session and fork
    std::map<std::string, std::shared_ptr<const CustomShaderSources>> customSources;
    // jobs for the main thread (no worker), one per frame
    std::deque<Job> mainThreadJobs;
    uint64_t frame = 0;
    uint64_t lastMainThreadCompileFrame = UINT64_MAX;

    bool enabled = true;
    bool async = true;
    bool fileSystemSyncPending = false;
    bool cacheDirectoryChecked = false;
    std::string cacheDirectory; // empty when the cache is not usable

    std::atomic<bool> stopping{false};

    // compileQueue's queue, the job system it was made on, and those of job systems replaced since
    std::mutex queueMutex;
    jobs::SerialQueue* queue = nullptr;
    jobs::JobSystem* queueSystem = nullptr;
    std::vector<jobs::SerialQueue*> retiredQueues;
};

// Never destroyed: a compile job may still finish while statics are torn down
State& state(){
    static State* s = new State();
    return *s;
}

// The compiles off the main thread: one at a time (the compiler's globals are not proven
// thread-safe), as Background Heavy jobs on the pool's workers, never a thread of their own.
// A queue stays on the job system it was made on, and once that one is shut down and replaced
// (an engine restart: jobs::initialize) it cancels every compile pushed to it, which left those
// variants building forever: a new queue is made on the process job system. The old one is
// kept, as the job systems it was made on are.
jobs::SerialQueue& compileQueue(){
    State& s = state();
    jobs::JobSystem& system = jobs::system();
    std::lock_guard<std::mutex> lock(s.queueMutex);
    if (s.queueSystem != &system){
        if (s.queue)
            s.retiredQueues.push_back(s.queue);
        s.queue = new jobs::SerialQueue("runtime-shader-compile", jobs::Lane::Background, jobs::GateClass::Heavy, &system);
        s.queueSystem = &system;
    }
    return *s.queue;
}

std::string getCacheRoot(){
    std::string base = System::instance().getCachePath();
    if (base.empty())
        base = ".";
    std::error_code ec;
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::path(base) / "shadercache", ec);
    if (ec)
        root = std::filesystem::path(base) / "shadercache";
    return root.generic_string();
}

std::string getCacheDirectoryName(){
    return ShaderPool::getShaderLangStr() + "-" + ShaderVariantCompiler::toHex(ShaderVariantCompiler::getLibraryHash());
}

// "<variant>_<lang>.sdat"; a forked shader's also carries the hash of its sources, so
// editing the fork compiles it again
std::string getCacheFile(ShaderKey key, const CustomShaderSources* sources){
    const std::string shaderStr = ShaderPool::getShaderStr(ShaderPool::getShaderTypeFromKey(key),
        ShaderPool::getPropertiesFromKey(key), ShaderPool::getCustomIdFromKey(key));
    std::string name = shaderStr + "_" + ShaderPool::getShaderLangStr();
    if (sources)
        name += "_" + ShaderVariantCompiler::toHex(ShaderVariantCompiler::hash(sources->signature));
    return name + ".sdat";
}

// Creates this backend's cache directory and removes the ones older shader libraries
// left behind. Main thread, once per session; the directory, or empty without one.
const std::string& prepareCacheDirectory(State& s){
    if (s.cacheDirectoryChecked)
        return s.cacheDirectory;
    s.cacheDirectoryChecked = true;

    const std::filesystem::path root = getCacheRoot();
    const std::string current = getCacheDirectoryName();
    const std::string prefix = ShaderPool::getShaderLangStr() + "-";

    std::error_code ec;
    std::filesystem::create_directories(root / current, ec);
    if (ec){
        Log::warn("Runtime shader cache disabled, cannot create %s: %s",
                  (root / current).generic_string().c_str(), ec.message().c_str());
        return s.cacheDirectory;
    }

    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)){
        const std::string name = it->path().filename().string();
        if (name != current && name.compare(0, prefix.size(), prefix) == 0){
            std::error_code removeError;
            std::filesystem::remove_all(it->path(), removeError);
        }
    }

    s.cacheDirectory = (root / current).generic_string();
    return s.cacheDirectory;
}


bool writeCacheFile(ShaderKey key, const std::string& path, const ShaderData& data){
    // atomic (io::writeFileAtomic): a run reading the cache never sees half a file
    std::string err;
    return ShaderDataSerializer::writeToFile(path, ShaderPool::getStorageKey(key), data, &err);
}

// Compiles a job's variant. Any thread, without the state lock.
Variant compileJob(const Job& job, const std::atomic<bool>& stopping){
    Variant variant;
    const uint64_t start = stm_now();

    ShaderData data;
    std::string error;
    std::string log;
    const bool compiled = ShaderVariantCompiler::compile(job.key, ShaderPool::getShaderBackend(), job.sources.get(),
        data, &error, &log, {}, [&stopping](){ return stopping.load(); });

    variant.milliseconds = stm_ms(stm_since(start));
    if (compiled){
        variant.state = ResourceLoadState::Finished;
        variant.data = data;
        if (!job.cachePath.empty())
            variant.cacheWritten = writeCacheFile(job.key, job.cachePath, data);
    }else{
        variant.state = ResourceLoadState::Failed;
        variant.error = error;
        variant.log = log;
    }
    return variant;
}

void compileOnStrand(const Job& job){
    State& s = state();
    if (s.stopping.load())
        return;

    Variant variant = compileJob(job, s.stopping);

    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.stopping.load())
        return;
    if (variant.cacheWritten)
        s.fileSystemSyncPending = true;
    variant.settledFrame = s.frame;
    s.variants[job.key] = std::move(variant);
}

// Off the main thread when async and the job system has workers; in single-thread mode the
// main thread compiles one variant a frame (beginFrame)
bool useWorker(const State& s){
    return s.async && !jobs::system().isSingleThreaded();
}

// Hands a settled variant over, logging it the first time. Under the state lock.
ShaderBuildResult settle(ShaderKey key, Variant& variant){
    if (!variant.reported){
        variant.reported = true;
        const std::string shaderStr = ShaderPool::getShaderStr(ShaderPool::getShaderTypeFromKey(key),
            ShaderPool::getPropertiesFromKey(key), ShaderPool::getCustomIdFromKey(key));
        if (variant.state == ResourceLoadState::Finished){
            if (variant.fromDisk){
                Log::verbose("Shader %s (%s) loaded from the runtime shader cache in %.2f ms",
                             shaderStr.c_str(), ShaderPool::getShaderLangStr().c_str(), variant.milliseconds);
            }else{
                Log::verbose("Shader %s (%s) compiled at runtime in %.1f ms%s",
                             shaderStr.c_str(), ShaderPool::getShaderLangStr().c_str(), variant.milliseconds,
                             variant.cacheWritten ? "" : " (not cached on disk)");
            }
        }
    }
    if (variant.state == ResourceLoadState::Finished)
        return ShaderBuildResult(variant.data, ResourceLoadState::Finished);
    return ShaderBuildResult({}, variant.state);
}

} // namespace

bool RuntimeShaderCompiler::isSupported(){
    return ShaderVariantCompiler::isAvailable();
}

void RuntimeShaderCompiler::setEnabled(bool enabled){
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.enabled = enabled;
}

bool RuntimeShaderCompiler::isEnabled(){
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.enabled && isSupported();
}

void RuntimeShaderCompiler::setAsync(bool async){
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.async = async;
}

bool RuntimeShaderCompiler::isAsync(){
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return useWorker(s);
}

ShaderBuildResult RuntimeShaderCompiler::request(ShaderKey key){
    State& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);

    if (auto it = s.variants.find(key); it != s.variants.end()){
        Variant& variant = it->second;
        if (variant.state != ResourceLoadState::Loading)
            return settle(key, variant);
        return ShaderBuildResult({}, ResourceLoadState::Loading);
    }

    // A previous run's cache is not there yet (the web build restores its user data
    // from IndexedDB after starting): asked again next frame
    if (!System::instance().isUserDataLoaded())
        return ShaderBuildResult({}, ResourceLoadState::Loading);

    // First request of this variant in the session
    Job job;
    job.key = key;

    // a library id (a UI effect of the engine library) is built from the library alone
    const uint16_t customId = ShaderPool::getCustomIdFromKey(key);
    if (customId != 0 && !ShaderPool::isLibraryCustomShader(customId)){
        const std::string customShader = ShaderPool::getCustomShaderName(customId);
        const ShaderType shaderType = ShaderPool::getShaderTypeFromKey(key);
        // a library shader's depth variant is its depth program: keyed by type too
        const std::string sourceKey = customShader + "\n" + std::to_string(static_cast<int>(shaderType));
        auto sourceIt = s.customSources.find(sourceKey);
        if (sourceIt == s.customSources.end()){
            auto sources = std::make_shared<CustomShaderSources>(
                ShaderVariantCompiler::resolveCustomShaderSources(customShader, ShaderManifests::readForkFile, shaderType));
            sourceIt = s.customSources.emplace(sourceKey, sources).first;
        }
        job.sources = sourceIt->second;
        if (!job.sources->valid){
            Variant& variant = s.variants[key];
            variant.state = ResourceLoadState::Failed;
            variant.error = job.sources->error;
            return settle(key, variant);
        }
    }
    // a previous run compiled it
    const std::string& cacheDirectory = prepareCacheDirectory(s);
    if (!cacheDirectory.empty()){
        job.cachePath = cacheDirectory + "/" + getCacheFile(key, job.sources.get());

        const uint64_t start = stm_now();
        ShaderData cached;
        if (ShaderDataSerializer::readFromFile(job.cachePath, ShaderPool::getStorageKey(key), cached)){
            Variant& variant = s.variants[key];
            variant.state = ResourceLoadState::Finished;
            variant.data = cached;
            variant.fromDisk = true;
            variant.milliseconds = stm_ms(stm_since(start));
            return settle(key, variant);
        }
    }

    s.variants[key].state = ResourceLoadState::Loading;

    if (useWorker(s)){
        compileQueue().push([job]() { compileOnStrand(job); });
        return ShaderBuildResult({}, ResourceLoadState::Loading);
    }

    // Main thread: at most one compile per frame, the rest wait for beginFrame
    if (s.lastMainThreadCompileFrame == s.frame){
        s.mainThreadJobs.push_back(job);
        return ShaderBuildResult({}, ResourceLoadState::Loading);
    }
    s.lastMainThreadCompileFrame = s.frame;

    lock.unlock();
    Variant variant = compileJob(job, s.stopping);
    lock.lock();

    if (variant.cacheWritten)
        s.fileSystemSyncPending = true;
    Variant& stored = s.variants[key];
    stored = std::move(variant);
    return settle(key, stored);
}

std::string RuntimeShaderCompiler::getError(ShaderKey key){
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto it = s.variants.find(key);
    if (it == s.variants.end() || it->second.state != ResourceLoadState::Failed)
        return "";
    std::string message = it->second.error;
    if (!it->second.log.empty())
        message += ":\n" + it->second.log;
    return message;
}

size_t RuntimeShaderCompiler::getPendingCount(){
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    size_t pending = 0;
    for (const auto& entry : s.variants){
        // and one settled off the draws this frame, not handed over yet: the next frame's draw
        // takes it (its GPU creation is counted from then), so a screenshot of this frame waits
        const Variant& variant = entry.second;
        if (variant.state == ResourceLoadState::Loading || (!variant.reported && variant.settledFrame == s.frame))
            pending++;
    }
    return pending;
}

void RuntimeShaderCompiler::beginFrame(){
    State& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    s.frame++;

    if (s.fileSystemSyncPending){
        s.fileSystemSyncPending = false;
        lock.unlock();
        // persists what was written on platforms that need it (web IndexedDB)
        System::instance().syncFileSystem();
        lock.lock();
    }

    // Variants queued for the main thread, or left there when the worker was turned
    // off: one compile this frame
    if (!s.mainThreadJobs.empty() && s.lastMainThreadCompileFrame != s.frame){
        Job job = s.mainThreadJobs.front();
        s.mainThreadJobs.pop_front();
        s.lastMainThreadCompileFrame = s.frame;

        lock.unlock();
        Variant variant = compileJob(job, s.stopping);
        lock.lock();

        if (variant.cacheWritten)
            s.fileSystemSyncPending = true;
        variant.settledFrame = s.frame;
        s.variants[job.key] = std::move(variant);
    }
}

void RuntimeShaderCompiler::shutdown(){
    State& s = state();
    s.stopping = true;
    // the compiles not started never run; the running one sees `stopping`
    compileQueue().cancelPending();
    compileQueue().waitIdle();

    std::lock_guard<std::mutex> lock(s.mutex);
    s.stopping = false;
    s.variants.clear();
    s.customSources.clear();
    s.mainThreadJobs.clear();
    s.lastMainThreadCompileFrame = UINT64_MAX;
}

std::string RuntimeShaderCompiler::getCacheDirectory(){
    return getCacheRoot() + "/" + getCacheDirectoryName();
}

std::vector<ShaderKey> RuntimeShaderCompiler::getCachedKeys(){
    std::vector<ShaderKey> keys;
    if (!isSupported())
        return keys;

    const std::string suffix = "_" + ShaderPool::getShaderLangStr() + ".sdat";
    std::error_code ec;
    for (std::filesystem::directory_iterator it(getCacheDirectory(), ec), end; !ec && it != end; it.increment(ec)){
        const std::string name = it->path().filename().string();
        if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        ShaderKey key;
        if (ShaderPool::parseShaderStr(name.substr(0, name.size() - suffix.size()), key))
            keys.push_back(key);
    }
    return keys;
}
