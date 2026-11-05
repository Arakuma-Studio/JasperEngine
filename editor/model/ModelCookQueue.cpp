// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelCookQueue.h"

#include "Out.h"
#include "Project.h"
#include "command/type/SetAssetMetaCmd.h"
#include "component/ModelComponent.h"
#include "reload/HotReloadService.h"
#include "util/AssetDatabase.h"
#include "util/FbxImporter.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <set>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    int64_t fileTime(const fs::path& path){
        std::error_code ec;
        const auto time = fs::last_write_time(path, ec);
        return ec ? 0 : (int64_t)time.time_since_epoch().count();
    }

    // at most this many cooks at once: a model's working set is several times its geometry
    constexpr size_t kConcurrentCooks = 2;

}

const char* editor::modelCookStatusName(ModelCookStatus status){
    switch (status){
        case ModelCookStatus::Queued: return "queued";
        case ModelCookStatus::Cooking: return "cooking";
        case ModelCookStatus::Cooked: return "cooked";
        case ModelCookStatus::Failed: return "failed";
        case ModelCookStatus::Source: return "source";
        case ModelCookStatus::None: default: return "none";
    }
}

bool editor::isModelSource(const std::string& path){
    return ModelCooker::isCookableModel(path);
}

std::vector<std::string> editor::projectModelSources(const Project& project){
    std::vector<std::string> paths;
    const fs::path assets = project.getAssetsPath().lexically_normal();
    project.getAssetDatabase().forEachAsset([&](const AssetDatabase::AssetInfo& info){
        if (info.type != AssetType::Model)
            return;
        const std::string relative = (project.getProjectPath() / info.path).lexically_normal().lexically_relative(assets).generic_string();
        if (relative.empty() || relative == "." || relative.rfind("..", 0) == 0)
            return;
        if (isModelSource(relative))
            paths.push_back(relative);
    });
    std::sort(paths.begin(), paths.end());
    return paths;
}

bool editor::modelSettingsOf(const Project& project, const std::string& assetPath, ModelImportSettings& out, std::string* error){
    std::string reason;
    ModelImportSettings::load((project.getAssetsPath() / FileUtils::pathFromUtf8(assetPath)).string(), out, &reason);
    if (!reason.empty()){
        if (error) *error = reason;
        return false;
    }
    return true;
}

bool editor::gatherModelCookRequest(Project& project, const std::string& assetPath, ModelFamily family, const ModelImportSettings* override,
                                    ModelCookRequest& out, std::string& error){
    out = ModelCookRequest();
    out.assetPath = assetPath;
    out.family = family;
    const fs::path absolute = (project.getAssetsPath() / FileUtils::pathFromUtf8(assetPath)).lexically_normal();
    ModelImportSettings settings;
    if (override){
        settings = *override;
    }else if (!modelSettingsOf(project, assetPath, settings, &error)){
        error = assetPath + ": its model import settings cannot be read: " + error;
        return false;
    }
    out.settings = settings.resolve(family, project.getModelSettings());
    out.blended = ModelUsageIndex::get().usageOf(assetPath).blended();
    if (FbxImporter::isFbxFile(absolute)){
        // an FBX cooks from its import, made now if it is missing or old
        if (!project.importFbxModel(absolute)){
            error = assetPath + " could not be imported (the FBX importer's error is in the Console)";
            return false;
        }
        out.source = project.getImportedModelPath(absolute);
    }else{
        out.source = absolute;
    }
    return true;
}

// ---- jobs

struct ModelCookQueue::Job {
    std::string assetPath;
    ModelFamily family = ModelFamily::Desktop;
    uint64_t epoch = 0;
    ModelCookRequest request;
    uint64_t sourceSize = 0;
    int64_t sourceTime = 0;
    std::atomic<bool> cancel{false};
    jobs::Task<void> task;
    ModelCookResult result;
};

ModelCookQueue& ModelCookQueue::get(){
    static ModelCookQueue* queue = new ModelCookQueue();
    return *queue;
}

ModelCookQueue::ModelCookQueue() = default;

void ModelCookQueue::attach(Project* newProject){
    if (newProject == project)
        return;
    if (project){
        cancelJobs(true);
        CookedModels::setProvider(nullptr);
        SetAssetMetaCmd::removeWriteListener(this);
        cache.close();
        ModelUsageIndex::get().clear();
        std::lock_guard<std::mutex> lock(mutex);
        entries.clear();
        reloads.clear();
        progressDone = progressTotal = 0;
        scanned = false;
    }
    project = newProject;
    if (!project)
        return;
    cache.open(project->getProjectInternalPath() / "cache" / "models" / "v1",
               (uint64_t)std::max(project->getModelSettings().cacheSizeMB, 64) << 20);
    CookedModels::setProvider([this](const std::string& assetPath, CookedModelEntry& out){
        return cookedEntry(assetPath, out);
    });
    SetAssetMetaCmd::addWriteListener(this, [this](Project* writer, const fs::path& asset, const std::string& key){
        if (writer != project || key.rfind("import", 0) != 0)
            return;
        const std::string assetPath = assetPathOf(writer->getProjectPath() / asset);
        if (!assetPath.empty())
            settingsChanged(assetPath);
    });
}

fs::path ModelCookQueue::absoluteOf(const std::string& assetPath) const{
    return project ? (project->getAssetsPath() / FileUtils::pathFromUtf8(assetPath)).lexically_normal() : fs::path();
}

std::string ModelCookQueue::assetPathOf(const fs::path& absolute) const{
    if (!project)
        return {};
    const fs::path relative = absolute.lexically_normal().lexically_relative(project->getAssetsPath().lexically_normal());
    const std::string text = relative.generic_string();
    if (text.empty() || text == "." || text.rfind("..", 0) == 0)
        return {};
    return text;
}

bool ModelCookQueue::cookedEntry(const std::string& assetPath, CookedModelEntry& out){
    if (!isModelSource(assetPath))
        return false;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = entries.find({assetPath, (int)ModelFamily::Desktop});
    if (it == entries.end() || it->second.state.status != ModelCookStatus::Cooked || it->second.state.stale)
        return false;
    const ModelCookState& state = it->second.state;
    // the file must still be what was cooked (a save the watcher has not delivered yet)
    std::error_code ec;
    if ((uint64_t)fs::file_size(state.sourceFile, ec) != state.sourceSize || ec || fileTime(state.sourceFile) != state.sourceTime)
        return false;
    out = CookedModelEntry();
    out.file = cache.artifactPath(state.record.key).string();
    out.bytes = state.record.diskBytes;
    out.sourceBytes = state.record.sourceBytes;
    // the editor always keeps the CPU copy (picking, colliders, bakes)
    out.keepCpuData = true;
    return true;
}

void ModelCookQueue::request(const std::string& assetPath, ModelFamily family, int priority){
    if (!project || !isModelSource(assetPath))
        return;
    std::error_code missing;
    if (!fs::is_regular_file(absoluteOf(assetPath), missing))
        return;
    std::lock_guard<std::mutex> lock(mutex);
    Entry& entry = entries[{assetPath, (int)family}];
    if (entry.queued || entry.job){
        entry.priority = std::max(entry.priority, priority);
        return;
    }
    const ModelCookStatus status = entry.state.status;
    if ((status == ModelCookStatus::Cooked && !entry.state.stale) || status == ModelCookStatus::Failed || status == ModelCookStatus::Source){
        entry.priority = std::max(entry.priority, priority);
        return;
    }
    entry.queued = true;
    entry.priority = priority;
    entry.order = sequence++;
    if (entry.state.status != ModelCookStatus::Cooked)
        entry.state.status = ModelCookStatus::Queued;
    progressTotal++;
}

void ModelCookQueue::requestAll(const std::vector<ModelFamily>& families, int priority){
    if (!project)
        return;
    for (const std::string& path : projectModelSources(*project)){
        for (ModelFamily family : families){
            {
                // a reimport cooks again what failed, too
                std::lock_guard<std::mutex> lock(mutex);
                auto it = entries.find({path, (int)family});
                if (it != entries.end() && it->second.state.status == ModelCookStatus::Failed)
                    it->second.state.status = ModelCookStatus::None;
            }
            request(path, family, priority);
        }
    }
}

void ModelCookQueue::invalidate(const std::string& assetPath){
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& [key, entry] : entries){
        if (key.first != assetPath)
            continue;
        entry.epoch++;
        if (entry.job){
            entry.job->cancel = true;
            entry.job.reset();
        }
        if (entry.state.status == ModelCookStatus::Cooked)
            entry.state.stale = true;
        else
            entry.state.status = ModelCookStatus::Queued;
        if (!entry.queued){
            entry.queued = true;
            progressTotal++;
        }
        entry.order = sequence++;
    }
}

void ModelCookQueue::invalidateAll(){
    std::set<std::string> paths;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& [key, entry] : entries)
            paths.insert(key.first);
    }
    for (const std::string& path : paths)
        invalidate(path);
}

void ModelCookQueue::sourceChanged(const std::string& assetPath){
    if (!project || !isModelSource(assetPath))
        return;
    {
        // a reload of what was cooked (the queue's own reload of a landed cook): nothing changed
        std::lock_guard<std::mutex> lock(mutex);
        auto it = entries.find({assetPath, (int)ModelFamily::Desktop});
        if (it != entries.end() && it->second.state.status == ModelCookStatus::Cooked && !it->second.state.stale){
            const ModelCookState& state = it->second.state;
            std::error_code ec;
            if ((uint64_t)fs::file_size(state.sourceFile, ec) == state.sourceSize && !ec && fileTime(state.sourceFile) == state.sourceTime)
                return;
        }
    }
    invalidate(assetPath);
}

void ModelCookQueue::settingsChanged(const std::string& assetPath){
    if (!project || !isModelSource(assetPath))
        return;
    // the key decides: a write that changed nothing the cook reads comes back as a cache hit
    invalidate(assetPath);
}

void ModelCookQueue::projectSettingsChanged(){
    if (!project)
        return;
    cache.setCap((uint64_t)std::max(project->getModelSettings().cacheSizeMB, 64) << 20);
    invalidateAll();
}

void ModelCookQueue::cancelJobs(bool wait){
    std::vector<std::shared_ptr<Job>> jobsToWait;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& [key, entry] : entries){
            if (entry.queued){
                entry.queued = false;
                if (entry.state.status == ModelCookStatus::Queued)
                    entry.state.status = ModelCookStatus::None;
            }
            if (entry.job){
                entry.job->cancel = true;
                entry.job.reset();
                entry.epoch++;
                if (entry.state.status == ModelCookStatus::Cooking)
                    entry.state.status = ModelCookStatus::None;
            }
        }
        for (const std::shared_ptr<Job>& job : running){
            job->cancel = true;
            job->task.cancel();
        }
        jobsToWait = running;
        progressDone = progressTotal = 0;
    }
    if (!wait)
        return;
    for (const std::shared_ptr<Job>& job : jobsToWait)
        job->task.wait();
    std::lock_guard<std::mutex> lock(mutex);
    running.clear();
}

void ModelCookQueue::cancelAll(){
    cancelJobs(false);
}

bool ModelCookQueue::update(){
    if (!project)
        return false;
    if (!scanned && project->getAssetIndex().isOpen() && project->getAssetIndex().isReady()){
        scanned = true;
        requestAll({ModelFamily::Desktop}, Background);
    }
    collectJobs();
    startJobs();
    reloadShown();
    return isBusy();
}

bool ModelCookQueue::isBusy() const{
    std::lock_guard<std::mutex> lock(mutex);
    if (!running.empty() || !reloads.empty())
        return true;
    for (const auto& [key, entry] : entries)
        if (entry.queued)
            return true;
    return false;
}

ModelCookQueue::Progress ModelCookQueue::getProgress() const{
    std::lock_guard<std::mutex> lock(mutex);
    Progress progress;
    progress.done = progressDone;
    progress.total = progressTotal;
    for (const std::shared_ptr<Job>& job : running){
        if (!job->cancel){
            progress.current = job->assetPath;
            break;
        }
    }
    return progress;
}

bool ModelCookQueue::getState(const std::string& assetPath, ModelFamily family, ModelCookState& out) const{
    std::lock_guard<std::mutex> lock(mutex);
    auto it = entries.find({assetPath, (int)family});
    if (it == entries.end())
        return false;
    out = it->second.state;
    return true;
}

bool ModelCookQueue::isPending(const std::string& assetPath, ModelFamily family) const{
    std::lock_guard<std::mutex> lock(mutex);
    auto it = entries.find({assetPath, (int)family});
    return it != entries.end() && (it->second.queued || it->second.job);
}

ModelCookStatus ModelCookQueue::badgeStatus(const std::string& assetPath) const{
    std::lock_guard<std::mutex> lock(mutex);
    ModelCookStatus worst = ModelCookStatus::None;
    auto rank = [](ModelCookStatus status){
        switch (status){
            case ModelCookStatus::Failed: return 3;
            case ModelCookStatus::Cooking: return 2;
            case ModelCookStatus::Queued: return 1;
            default: return 0;
        }
    };
    for (auto it = entries.lower_bound({assetPath, 0}); it != entries.end() && it->first.first == assetPath; ++it){
        ModelCookStatus status = it->second.state.status;
        if (it->second.job) status = ModelCookStatus::Cooking;
        else if (it->second.queued && status != ModelCookStatus::Failed) status = ModelCookStatus::Queued;
        if (rank(status) > rank(worst))
            worst = status;
    }
    return worst;
}

void ModelCookQueue::startJobs(){
    std::vector<std::pair<std::string, int>> candidates;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (running.size() >= kConcurrentCooks)
            return;
        std::vector<std::pair<const std::pair<std::string, int>*, const Entry*>> queued;
        for (const auto& [key, entry] : entries)
            if (entry.queued && !entry.job)
                queued.push_back({&key, &entry});
        std::sort(queued.begin(), queued.end(), [](const auto& a, const auto& b){
            if (a.second->priority != b.second->priority)
                return a.second->priority > b.second->priority;
            return a.second->order < b.second->order;
        });
        for (size_t i = 0; i < queued.size() && running.size() + candidates.size() < kConcurrentCooks; i++)
            candidates.push_back(*queued[i].first);
    }
    if (candidates.empty())
        return;
    // the scenes' blend overrides (cook inputs), read again at most every two seconds
    const auto now = std::chrono::steady_clock::now();
    if (now - lastUsageRefresh > std::chrono::seconds(2)){
        ModelUsageIndex::get().refresh(*project);
        lastUsageRefresh = now;
    }
    for (const auto& key : candidates){
        auto job = std::make_shared<Job>();
        job->assetPath = key.first;
        job->family = (ModelFamily)key.second;
        std::string error;
        const bool gathered = gatherModelCookRequest(*project, key.first, job->family, nullptr, job->request, error);
        std::error_code ec;
        job->sourceSize = gathered ? (uint64_t)fs::file_size(job->request.source, ec) : 0;
        job->sourceTime = gathered ? fileTime(job->request.source) : 0;
        std::lock_guard<std::mutex> lock(mutex);
        auto it = entries.find(key);
        if (it == entries.end() || !it->second.queued || it->second.job)
            continue;
        Entry& entry = it->second;
        entry.queued = false;
        if (!gathered){
            entry.state.status = ModelCookStatus::Failed;
            entry.state.error = error;
            progressDone++;
            Out::error("Model cook of %s failed: %s", key.first.c_str(), error.c_str());
            continue;
        }
        entry.job = job;
        job->epoch = entry.epoch;
        if (entry.state.status != ModelCookStatus::Cooked)
            entry.state.status = ModelCookStatus::Cooking;
        running.push_back(job);
        jobs::SubmitOptions options;
        options.lane = jobs::Lane::Background;
        options.gate = jobs::GateClass::Heavy;
        options.name = "model-cook";
        ModelCookCache* cookCache = &cache;
        job->task = jobs::submit([job, cookCache](){
            if (job->cancel)
                return;
            job->result = ModelCooker::cook(*cookCache, job->request, [job](){ return job->cancel.load(); });
        }, options);
    }
}

void ModelCookQueue::collectJobs(){
    std::vector<std::shared_ptr<Job>> finished;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = running.begin(); it != running.end();){
            if (!(*it)->task.isValid() || !(*it)->task.isReady()){
                ++it;
                continue;
            }
            finished.push_back(*it);
            it = running.erase(it);
        }
    }
    for (const std::shared_ptr<Job>& job : finished){
        try{
            job->task.get();
        }catch (const std::exception& e){
            if (job->result.error.empty())
                job->result.error = e.what();
            job->result.ok = false;
        }
        std::error_code missing;
        const bool gone = !job->result.ok && !fs::is_regular_file(absoluteOf(job->assetPath), missing);
        bool landed = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = entries.find({job->assetPath, (int)job->family});
            const bool current = it != entries.end() && it->second.job == job && it->second.epoch == job->epoch && !job->cancel;
            if (!current)
                continue;
            Entry& entry = it->second;
            entry.job.reset();
            progressDone++;
            ModelCookState& state = entry.state;
            if (job->result.ok){
                state.status = ModelCookStatus::Cooked;
                state.stale = false;
                state.cacheHit = job->result.cacheHit;
                state.error.clear();
                state.key = job->result.key;
                state.resolved = job->request.settings;
                state.record = job->result.record;
                state.sourceFile = job->request.source;
                state.sourceSize = job->sourceSize;
                state.sourceTime = job->sourceTime;
                landed = job->family == ModelFamily::Desktop;
            }else if (job->result.cancelled || gone){
                state.status = ModelCookStatus::None;
            }else{
                state.status = ModelCookStatus::Failed;
                state.stale = false;
                state.error = job->result.error;
                state.resolved = job->request.settings;
                // the loader's findings that explain it (R5)
                state.record = job->result.record;
            }
            if (landed)
                reloads.push_back(job->assetPath);
        }
        if (job->result.ok && !job->result.cacheHit)
            Out::info("Cooked %s for %s: %s, %.0f ms", job->assetPath.c_str(), modelFamilyName(job->family),
                      job->result.record.compression.c_str(), job->result.record.cookMs);
        else if (!job->result.ok && !job->result.cancelled && !gone)
            Out::error("Model cook of %s for %s failed: %s", job->assetPath.c_str(), modelFamilyName(job->family), job->result.error.c_str());
    }
    if (!finished.empty())
        cache.saveSources();
    std::lock_guard<std::mutex> lock(mutex);
    bool idle = running.empty();
    for (const auto& [key, entry] : entries)
        idle = idle && !entry.queued;
    if (idle)
        progressDone = progressTotal = 0;
}

void ModelCookQueue::reloadShown(){
    std::vector<std::string> paths;
    {
        std::lock_guard<std::mutex> lock(mutex);
        paths.swap(reloads);
    }
    if (paths.empty())
        return;
    // the models an open scene shows reload from their cook, as a changed file does
    std::vector<fs::path> files;
    for (const std::string& path : paths){
        const fs::path absolute = absoluteOf(path);
        bool shown = false;
        for (SceneProject* sceneProject : project->getScenesAndPrefabStages()){
            Scene* scene = sceneProject->scene;
            if (!scene || !scene->isComponentRegistered<ModelComponent>())
                continue;
            auto models = scene->getComponentArray<ModelComponent>();
            for (size_t i = 0; i < models->size() && !shown; i++){
                const ModelComponent& model = models->getComponentFromIndex(i);
                shown = !model.filename.empty() && project->resolveAssetPath(model.filename).lexically_normal() == absolute;
            }
            if (shown)
                break;
        }
        if (shown)
            files.push_back(absolute);
    }
    if (files.empty())
        return;
    ApplyOptions options;
    options.force = true;
    options.quiet = true;
    HotReloadService::get().apply(ChangeSet::fromPaths(files), options);
}

bool ModelCookQueue::waitFor(const std::vector<std::string>& assetPaths, const std::vector<ModelFamily>& families, double timeoutSeconds){
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(std::max(0.0, timeoutSeconds));
    for (const std::string& path : assetPaths)
        for (ModelFamily family : families)
            request(path, family, Selected);
    while (true){
        update();
        jobs::drainMainQueue();
        bool pending = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (const std::string& path : assetPaths)
                for (ModelFamily family : families){
                    auto it = entries.find({path, (int)family});
                    if (it != entries.end() && (it->second.queued || it->second.job))
                        pending = true;
                }
        }
        if (!pending)
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

std::vector<ModelFamily> ModelCookQueue::familiesForExport(const std::vector<std::string>& platforms){
    std::vector<ModelFamily> families;
    auto add = [&](ModelFamily family){
        if (std::find(families.begin(), families.end(), family) == families.end())
            families.push_back(family);
    };
    for (const std::string& platform : platforms){
        if (platform == "windows" || platform == "linux" || platform == "macos") add(ModelFamily::Desktop);
        else if (platform == "ios" || platform == "android") add(ModelFamily::Mobile);
        else if (platform == "web") add(ModelFamily::Web);
    }
    return families;
}

// ---- batch

void ModelBatchCook::run(ModelCookCache& cache, std::vector<ModelBatchItem>& items, int threads,
                         const std::function<void(int, int, const std::string&)>& progress, const std::function<bool()>& cancelled){
    if (items.empty())
        return;
    if (threads <= 0)
        threads = std::max(1, jobs::hasSystem() ? jobs::system().getWorkerCount() : (int)std::thread::hardware_concurrency());
    threads = std::min<int>(threads, (int)items.size());
    std::mutex batchMutex;
    size_t next = 0;
    int done = 0;
    auto isCancelled = [&](){ return cancelled && cancelled(); };
    auto worker = [&](){
        while (true){
            size_t index;
            {
                std::lock_guard<std::mutex> lock(batchMutex);
                if (next >= items.size())
                    return;
                index = next++;
            }
            ModelBatchItem& item = items[index];
            if (item.sourceOnly){
                item.result.ok = true;
            }else if (isCancelled()){
                item.result.cancelled = true;
                item.result.error = "cancelled";
            }else{
                item.result = ModelCooker::cook(cache, item.request, cancelled ? cancelled : std::function<bool()>());
            }
            std::lock_guard<std::mutex> lock(batchMutex);
            done++;
            if (progress)
                progress(done, (int)items.size(), item.assetPath);
        }
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; t++)
        pool.emplace_back(worker);
    worker();
    for (std::thread& thread : pool)
        thread.join();
    cache.saveSources();
}
