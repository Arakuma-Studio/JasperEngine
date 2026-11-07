// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureCookQueue.h"
#include "export/ExportFiles.h"

#include "Out.h"
#include "Project.h"
#include "command/type/SetAssetMetaCmd.h"
#include "texsynth/SynthesisRecipe.h"
#include "pool/TextureDataPool.h"
#include "pool/TexturePool.h"
#include "util/AssetDatabase.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    std::string cookLowerExtension(const std::string& path){
        std::string extension = fs::path(path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return extension;
    }

    // what a cook of a texture without settings stores: the inferred type and its reason (R2)
    TextureImportSettings cookInferredSettings(const TextureInference& inference){
        TextureImportSettings settings;
        settings.type = inference.type;
        settings.inferredFrom = inference.reason;
        if (inference.linear && TextureImportSettings::defaultSrgb(inference.type))
            settings.srgb = false;
        if (inference.shape == TextureShape::Cube)
            settings.shape = TextureShape::Cube;
        return settings;
    }

    std::vector<std::string> cookWarnings(const TextureImportSettings& settings, const TextureInference& inference,
                                          const TextureCookResult& result){
        std::vector<std::string> warnings = TextureTypeInference::conflicts(settings.resolvedType(), inference);
        for (const std::string& warning : result.plan.choice.warnings)
            warnings.push_back(warning);
        for (const std::string& warning : result.record.warnings)
            if (std::find(warnings.begin(), warnings.end(), warning) == warnings.end())
                warnings.push_back(warning);
        return warnings;
    }

}

const char* editor::textureCookStatusName(TextureCookStatus status){
    switch (status){
        case TextureCookStatus::Queued: return "queued";
        case TextureCookStatus::Cooking: return "cooking";
        case TextureCookStatus::Cooked: return "cooked";
        case TextureCookStatus::Failed: return "failed";
        case TextureCookStatus::Source: return "source";
        case TextureCookStatus::None: default: return "none";
    }
}

bool editor::isCookableTexture(const std::string& path){
    static const std::set<std::string> extensions = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".gif", ".hdr", ".exr", ".pic", ".pnm"};
    // a name that is not UTF-8 (a Latin-1 file) cannot be a key of the cache's tables or of an
    // export's index.json: such a file loads from its source, as before cooks
    return extensions.count(cookLowerExtension(path)) > 0 && texsynth::isValidUtf8(path);
}

bool editor::isTextureSource(const std::string& path){
    const std::string extension = cookLowerExtension(path);
    return isCookableTexture(path) || extension == ".svg" || extension == ".ktx2";
}

TextureProjectSettings editor::textureProjectSettingsOf(const Project& project){
    TextureProjectSettings settings = project.getTextureSettings();
    settings.strategy = project.getTextureStrategy() == TextureStrategy::FIT ? TextureSizeStrategy::Fit
                      : project.getTextureStrategy() == TextureStrategy::RESIZE ? TextureSizeStrategy::Resize
                                                                               : TextureSizeStrategy::None;
    return settings;
}

std::vector<std::string> editor::projectTextureSources(const Project& project, bool cookableOnly){
    std::vector<std::string> paths;
    const fs::path assets = project.getAssetsPath().lexically_normal();
    // an earlier export left in the project ships nothing, its cooks neither (Exporter: W-EXPORT-FOLDER-SKIPPED)
    exportfiles::ExportFolderFinder exports(project.getProjectPath(), project.getName().empty() ? "jasperengine-project"
                                                                                                 : project.getLibName());
    project.getAssetDatabase().forEachAsset([&](const AssetDatabase::AssetInfo& info){
        if (info.type != AssetType::Texture)
            return;
        if (exports.contains(project.getProjectPath() / info.path))
            return;
        const std::string relative = (project.getProjectPath() / info.path).lexically_normal().lexically_relative(assets).generic_string();
        if (relative.empty() || relative == "." || relative.rfind("..", 0) == 0)
            return;
        if (cookableOnly ? isCookableTexture(relative) : isTextureSource(relative))
            paths.push_back(relative);
    });
    std::sort(paths.begin(), paths.end());
    return paths;
}

bool editor::gatherTextureInputs(Project& project, const std::string& assetPath, TextureCookInputs& out){
    out = TextureCookInputs();
    out.assetPath = assetPath;
    out.absolute = (project.getAssetsPath() / fs::path(assetPath)).lexically_normal();
    out.projectRelative = project.normalizeToProjectRelative(out.absolute).generic_string();
    std::string error;
    out.hasSettings = TextureImportSettings::load(out.absolute.string(), out.settings, &error);
    if (!error.empty())
        return false;
    out.inference = TextureTypeInference::infer(project, out.projectRelative);
    if (!out.hasSettings)
        out.settings = cookInferredSettings(out.inference);
    return true;
}

TextureCookRequest editor::makeTextureCookRequest(const TextureCookInputs& inputs, const TextureProjectSettings& project, TextureFamily family){
    TextureCookRequest request;
    request.source = inputs.absolute;
    request.assetPath = inputs.assetPath;
    request.settings = inputs.settings;
    request.project = project;
    request.family = family;
    request.cpuReadable = inputs.inference.cpuReadable;
    request.cpuReason = inputs.inference.cpuReason;
    return request;
}

// ---- jobs

struct TextureCookQueue::Job {
    std::string assetPath;
    TextureFamily family = TextureFamily::Desktop;
    uint64_t epoch = 0;
    // main thread's part of the inputs
    TextureCookInputs inputs;
    std::vector<DepEdge> edges;
    TextureProjectSettings project;
    std::atomic<bool> cancel{false};
    bool cooking = false;               // the second stage (the plan missed the cache)
    uint64_t workingBytes = 0;
    jobs::Task<void> task;
    // results
    TextureCookResult result;
    bool sourceOnly = false;
    bool inferred = false;              // settings were inferred: written by the main thread
};

struct TextureCookQueue::SwapIn {
    std::string assetPath;
    jobs::Task<std::vector<std::pair<std::string, std::shared_ptr<std::array<TextureData, 6>>>>> task;
};

TextureCookQueue& TextureCookQueue::get(){
    static TextureCookQueue* queue = new TextureCookQueue();
    return *queue;
}

TextureCookQueue::TextureCookQueue() = default;
TextureCookQueue::~TextureCookQueue() = default;

void TextureCookQueue::attach(Project* newProject){
    if (newProject == project)
        return;
    if (project){
        cancelJobs(true);
        for (const std::shared_ptr<SwapIn>& swap : swaps)
            swap->task.wait();
        swaps.clear();
        CookedTextures::setProvider(nullptr, "");
        TextureDataPool::setProcessingProvider(nullptr);
        SetAssetMetaCmd::removeWriteListener(this);
        cache.saveSources();
        cache.close();
        std::lock_guard<std::mutex> lock(mutex);
        entries.clear();
        settingsMemo.clear();
        loaderInbox.clear();
        metaWrites.clear();
        progressDone = progressTotal = 0;
        memoryInUse = 0;
        scanned = false;
    }
    project = newProject;
    if (!project)
        return;
    projectSettings = textureProjectSettingsOf(*project);
    cache.open(project->getProjectInternalPath() / "cache" / "textures" / "v1", projectSettings.globalCacheDir,
               (uint64_t)std::max(projectSettings.cacheSizeMB, 64) << 20);
    registerEditorTextureCodecs();
    CookedTextures::setProvider([this](const std::string& assetPath, CookedTextureEntry& out){
        return cookedEntry(assetPath, out);
    }, textureFamilyName(getViewFamily()));
    TextureDataPool::setProcessingProvider([this](const std::string& path, TextureProcessing& out){
        return processingFor(CookedTextures::normalize(path), out);
    });
    SetAssetMetaCmd::addWriteListener(this, [this](Project* writer, const fs::path& asset, const std::string& key){
        if (writer != project || key.rfind("import", 0) != 0)
            return;
        const std::string assetPath = assetPathOf(writer->getProjectPath() / asset);
        if (!assetPath.empty())
            settingsChanged(assetPath);
    });
}

fs::path TextureCookQueue::absoluteOf(const std::string& assetPath) const{
    return project ? (project->getAssetsPath() / fs::path(assetPath)).lexically_normal() : fs::path();
}

std::string TextureCookQueue::assetPathOf(const fs::path& absolute) const{
    if (!project)
        return {};
    const fs::path relative = absolute.lexically_normal().lexically_relative(project->getAssetsPath().lexically_normal());
    const std::string text = relative.generic_string();
    if (text.empty() || text == "." || text.rfind("..", 0) == 0)
        return {};
    return text;
}

bool TextureCookQueue::isEditorDerived(const std::string& assetPath) const{
    if (!project)
        return false;
    const std::string relative = absoluteOf(assetPath).lexically_relative(project->getProjectInternalPath().lexically_normal()).generic_string();
    return !relative.empty() && relative != "." && relative.rfind("..", 0) != 0;
}

void TextureCookQueue::setViewFamily(TextureFamily family){
    if ((int)family == viewFamily.load())
        return;
    viewFamily = (int)family;
    if (!project)
        return;
    CookedTextures::setProvider([this](const std::string& assetPath, CookedTextureEntry& out){
        return cookedEntry(assetPath, out);
    }, textureFamilyName(family));
    // what renders now reloads with the new family's cooks (raw until they land)
    std::vector<std::string> paths;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& [key, entry] : entries)
            if (key.second == (int)TextureFamily::Desktop)
                paths.push_back(key.first);
    }
    for (const std::string& path : paths)
        startSwap(path);
}

TextureFamily TextureCookQueue::getViewFamily() const{
    return (TextureFamily)viewFamily.load();
}

void TextureCookQueue::setMemoryBudget(uint64_t bytes){
    memoryBudget = std::max<uint64_t>(bytes, 64ull << 20);
}

uint64_t TextureCookQueue::getMemoryBudget() const{
    return memoryBudget;
}

TextureProjectSettings TextureCookQueue::getProjectSettings() const{
    std::lock_guard<std::mutex> lock(mutex);
    return projectSettings;
}

void TextureCookQueue::projectSettingsChanged(){
    if (!project)
        return;
    const TextureProjectSettings next = textureProjectSettingsOf(*project);
    TextureProjectSettings previous;
    {
        std::lock_guard<std::mutex> lock(mutex);
        previous = projectSettings;
        projectSettings = next;
    }
    if (previous.globalCacheDir != next.globalCacheDir){
        cancelJobs(true);
        cache.saveSources();
        cache.close();
        cache.open(project->getProjectInternalPath() / "cache" / "textures" / "v1", next.globalCacheDir,
                   (uint64_t)std::max(next.cacheSizeMB, 64) << 20);
    }else{
        cache.setCap((uint64_t)std::max(next.cacheSizeMB, 64) << 20);
    }
    // the per-family defaults and the strategy are inputs of every key
    if (previous.defaultQuality != next.defaultQuality || previous.defaultMaxSize != next.defaultMaxSize ||
        previous.strategy != next.strategy || previous.globalCacheDir != next.globalCacheDir)
        invalidateAll();
}

// ---- settings and inputs

bool TextureCookQueue::settingsOf(const std::string& assetPath, TextureImportSettings& out){
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = settingsMemo.find(assetPath);
        if (it != settingsMemo.end()){
            out = it->second.second;
            return it->second.first;
        }
    }
    TextureImportSettings settings;
    std::string error;
    const bool has = TextureImportSettings::load(absoluteOf(assetPath).string(), settings, &error);
    if (!has && !error.empty())
        Out::warning("%s: texture import settings: %s", assetPath.c_str(), error.c_str());
    std::lock_guard<std::mutex> lock(mutex);
    settingsMemo[assetPath] = {has, settings};
    out = settings;
    return has;
}

bool TextureCookQueue::processingFor(const std::string& assetPath, TextureProcessing& out){
    if (!project || !isCookableTexture(assetPath) || isEditorDerived(assetPath))
        return false;
    TextureImportSettings settings;
    if (!settingsOf(assetPath, settings))
        return false;
    TextureProjectSettings projectCopy;
    {
        std::lock_guard<std::mutex> lock(mutex);
        projectCopy = projectSettings;
    }
    out = settings.resolve(getViewFamily(), projectCopy).processing();
    return true;
}

bool TextureCookQueue::cookedEntry(const std::string& assetPath, CookedTextureEntry& out){
    if (!isCookableTexture(assetPath) || isEditorDerived(assetPath))
        return false;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = entries.find({assetPath, viewFamily.load()});
    if (it != entries.end() && it->second.state.status == TextureCookStatus::Cooked && !it->second.state.stale){
        const TextureCookRecord& record = it->second.state.record;
        out = CookedTextureEntry();
        out.file = cache.artifactPath(record.key).string();
        out.width = record.logicalWidth;
        out.height = record.logicalHeight;
        out.format = record.format;
        out.flags = record.flags;
        out.levels = record.levels;
        out.faces = record.faces;
        out.bytes = record.diskBytes;
        out.tailBytes = record.tailBytes;
        if (record.cpuBytes > 0)
            out.cpuFile = cache.artifactPath(record.key, ".cpu.ktx2").string();
        return true;
    }
    // loaded through the raw path: cook it soon
    if (it == entries.end() || it->second.state.status != TextureCookStatus::Source)
        loaderInbox.push_back(assetPath);
    return false;
}

// ---- requests

void TextureCookQueue::request(const std::string& assetPath, TextureFamily family, int priority){
    if (!project || !isCookableTexture(assetPath) || isEditorDerived(assetPath))
        return;
    // a reference to a file that is not there (a model's missing image) is the reference's
    // problem, reported where it resolves: nothing to cook
    std::error_code missing;
    if (!fs::is_regular_file(absoluteOf(assetPath), missing))
        return;
    std::lock_guard<std::mutex> lock(mutex);
    Entry& entry = entries[{assetPath, (int)family}];
    if (entry.queued || entry.job){
        entry.priority = std::max(entry.priority, priority);
        return;
    }
    const TextureCookStatus status = entry.state.status;
    if ((status == TextureCookStatus::Cooked && !entry.state.stale) || status == TextureCookStatus::Failed ||
        status == TextureCookStatus::Source){
        entry.priority = std::max(entry.priority, priority);
        return;
    }
    entry.queued = true;
    entry.priority = priority;
    entry.order = sequence++;
    entry.state.status = TextureCookStatus::Queued;
    progressTotal++;
}

void TextureCookQueue::requestAll(const std::vector<TextureFamily>& families, int priority){
    if (!project)
        return;
    for (const std::string& path : projectTextureSources(*project, true)){
        for (TextureFamily family : families){
            {
                // a reimport cooks again what failed, too
                std::lock_guard<std::mutex> lock(mutex);
                auto it = entries.find({path, (int)family});
                if (it != entries.end() && it->second.state.status == TextureCookStatus::Failed)
                    it->second.state.status = TextureCookStatus::None;
            }
            request(path, family, priority);
        }
    }
}

void TextureCookQueue::invalidate(const std::string& assetPath){
    std::vector<TextureFamily> families;
    {
        std::lock_guard<std::mutex> lock(mutex);
        settingsMemo.erase(assetPath);
        for (auto& [key, entry] : entries){
            if (key.first != assetPath)
                continue;
            entry.epoch++;
            if (entry.job){
                entry.job->cancel = true;
                entry.job.reset();
            }
            if (entry.state.status == TextureCookStatus::Cooked)
                entry.state.stale = true;
            else if (entry.state.status != TextureCookStatus::Queued)
                entry.state.status = TextureCookStatus::None;
            if (entry.queued){
                entry.queued = false;
                progressTotal = std::max(0, progressTotal - 1);
            }
            families.push_back((TextureFamily)key.second);
        }
    }
    for (TextureFamily family : families){
        std::lock_guard<std::mutex> lock(mutex);
        Entry& entry = entries[{assetPath, (int)family}];
        entry.queued = true;
        entry.order = sequence++;
        if (entry.state.status != TextureCookStatus::Cooked)
            entry.state.status = TextureCookStatus::Queued;
        progressTotal++;
    }
    // what renders it now reloads (raw: the stale cook is no longer offered)
    startSwap(assetPath);
}

void TextureCookQueue::invalidateAll(){
    std::set<std::string> paths;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& [key, entry] : entries)
            paths.insert(key.first);
    }
    for (const std::string& path : paths)
        invalidate(path);
}

void TextureCookQueue::sourceChanged(const std::string& assetPath){
    if (!project || !isCookableTexture(assetPath))
        return;
    invalidate(assetPath);
}

void TextureCookQueue::settingsChanged(const std::string& assetPath){
    if (!project || !isTextureSource(assetPath))
        return;
    TextureImportSettings now;
    std::string error;
    const bool has = TextureImportSettings::load(absoluteOf(assetPath).string(), now, &error);
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = settingsMemo.find(assetPath);
        // our own write of inferred settings, or a write that changed nothing of the importer
        if (it != settingsMemo.end() && it->second.first == has && it->second.second == now)
            return;
        settingsMemo[assetPath] = {has, now};
    }
    if (!isCookableTexture(assetPath)){
        startSwap(assetPath);
        return;
    }
    {
        // a type change can make a source-only texture cookable again
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& [key, entry] : entries)
            if (key.first == assetPath && entry.state.status == TextureCookStatus::Source)
                entry.state.status = TextureCookStatus::None;
    }
    invalidate(assetPath);
    // invalidate() dropped the memo: keep the settings just read
    std::lock_guard<std::mutex> lock(mutex);
    settingsMemo[assetPath] = {has, now};
}

void TextureCookQueue::cancelJobs(bool wait){
    std::vector<std::shared_ptr<Job>> jobsToWait;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& [key, entry] : entries){
            if (entry.queued){
                entry.queued = false;
                if (entry.state.status == TextureCookStatus::Queued)
                    entry.state.status = TextureCookStatus::None;
            }
            if (entry.job){
                entry.job->cancel = true;
                entry.job.reset();
                entry.epoch++;
                if (entry.state.status == TextureCookStatus::Cooking)
                    entry.state.status = TextureCookStatus::None;
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
    memoryInUse = 0;
}

void TextureCookQueue::cancelAll(){
    cancelJobs(false);
}

// ---- the loop

bool TextureCookQueue::update(){
    if (!project)
        return false;
    // the loads that went raw since the last update: cook them first
    std::vector<std::string> inbox;
    {
        std::lock_guard<std::mutex> lock(mutex);
        inbox.swap(loaderInbox);
    }
    const TextureFamily view = getViewFamily();
    for (const std::string& path : inbox)
        request(path, view, Loaded);
    if (!scanned && project->getAssetIndex().isOpen() && project->getAssetIndex().isReady())
        scanProject();

    collectJobs();
    writeInferredSettings();
    startJobs();
    collectSwaps();
    return isBusy();
}

void TextureCookQueue::scanProject(){
    scanned = true;
    requestAll({TextureFamily::Desktop}, Background);
}

bool TextureCookQueue::isBusy() const{
    std::lock_guard<std::mutex> lock(mutex);
    if (!running.empty() || !swaps.empty() || !metaWrites.empty())
        return true;
    for (const auto& [key, entry] : entries)
        if (entry.queued)
            return true;
    return false;
}

TextureCookQueue::Progress TextureCookQueue::getProgress() const{
    std::lock_guard<std::mutex> lock(mutex);
    Progress progress;
    progress.done = progressDone;
    progress.total = progressTotal;
    for (const std::shared_ptr<Job>& job : running){
        if (job->cooking && !job->cancel){
            progress.current = job->assetPath;
            break;
        }
    }
    return progress;
}

bool TextureCookQueue::getState(const std::string& assetPath, TextureFamily family, TextureCookState& out) const{
    std::lock_guard<std::mutex> lock(mutex);
    auto it = entries.find({assetPath, (int)family});
    if (it == entries.end())
        return false;
    out = it->second.state;
    return true;
}

bool TextureCookQueue::isPending(const std::string& assetPath, TextureFamily family) const{
    std::lock_guard<std::mutex> lock(mutex);
    auto it = entries.find({assetPath, (int)family});
    return it != entries.end() && (it->second.queued || it->second.job);
}

TextureCookStatus TextureCookQueue::badgeStatus(const std::string& assetPath) const{
    std::lock_guard<std::mutex> lock(mutex);
    TextureCookStatus worst = TextureCookStatus::None;
    auto rank = [](TextureCookStatus status){
        switch (status){
            case TextureCookStatus::Failed: return 3;
            case TextureCookStatus::Cooking: return 2;
            case TextureCookStatus::Queued: return 1;
            default: return 0;
        }
    };
    for (auto it = entries.lower_bound({assetPath, 0}); it != entries.end() && it->first.first == assetPath; ++it)
        if (rank(it->second.state.status) > rank(worst))
            worst = it->second.state.status;
    return worst;
}

void TextureCookQueue::startJobs(){
    // plans are light; cooks take working memory: at most the budget (one over-budget cook
    // runs alone)
    const int maxPlans = 4;
    std::vector<std::pair<std::string, int>> candidates;
    std::vector<std::shared_ptr<Job>> toCook;
    {
        std::lock_guard<std::mutex> lock(mutex);
        int plans = 0;
        for (const std::shared_ptr<Job>& job : running)
            plans += job->cooking ? 0 : 1;
        if (plans >= maxPlans)
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
        for (size_t i = 0; i < queued.size() && plans + (int)candidates.size() < maxPlans; i++)
            candidates.push_back(*queued[i].first);
    }
    for (const auto& key : candidates){
        auto job = std::make_shared<Job>();
        job->assetPath = key.first;
        job->family = (TextureFamily)key.second;
        // the main thread's part: settings and the references from the index
        job->inputs.assetPath = key.first;
        job->inputs.absolute = absoluteOf(key.first);
        job->inputs.projectRelative = project->normalizeToProjectRelative(job->inputs.absolute).generic_string();
        job->inputs.hasSettings = settingsOf(key.first, job->inputs.settings);
        job->edges = TextureTypeInference::referenceEdges(*project, job->inputs.projectRelative);
        {
            std::lock_guard<std::mutex> lock(mutex);
            job->project = projectSettings;
            auto it = entries.find(key);
            if (it == entries.end() || !it->second.queued || it->second.job)
                continue;
            Entry& entry = it->second;
            entry.queued = false;
            entry.job = job;
            job->epoch = entry.epoch;
            running.push_back(job);
        }
        jobs::SubmitOptions options;
        options.lane = jobs::Lane::Background;
        options.name = "texture-plan";
        Project* owner = project;
        TextureCookCache* cookCache = &cache;
        job->task = jobs::submit([job, owner, cookCache](){
            if (job->cancel)
                return;
            TextureCookInputs& inputs = job->inputs;
            inputs.inference = TextureTypeInference::fromEdges(*owner, inputs.projectRelative, job->edges);
            if (!inputs.hasSettings){
                inputs.settings = cookInferredSettings(inputs.inference);
                job->inferred = true;
            }
            if (inputs.inference.sourceOnly){
                job->sourceOnly = true;
                job->result.ok = true;
                return;
            }
            const TextureCookRequest request = makeTextureCookRequest(inputs, job->project, job->family);
            if (!TextureCooker::plan(*cookCache, request, job->result.plan, job->result.error))
                return;
            if (cookCache->has(job->result.plan.key) && cookCache->load(job->result.plan.key, job->result.record)){
                job->result.ok = true;
                job->result.cacheHit = true;
                return;
            }
            job->workingBytes = TextureCooker::workingBytes(job->result.plan.analysis, job->result.plan.resolved);
        }, options);
    }

    // second stages whose plans missed the cache, within the memory budget
    std::lock_guard<std::mutex> lock(mutex);
    for (const std::shared_ptr<Job>& job : running){
        if (!job->cooking || job->task.isValid() || job->cancel)
            continue;
        if (memoryInUse > 0 && memoryInUse + job->workingBytes > memoryBudget)
            continue;
        memoryInUse += job->workingBytes;
        auto entry = entries.find({job->assetPath, (int)job->family});
        if (entry != entries.end() && entry->second.job == job)
            entry->second.state.status = TextureCookStatus::Cooking;
        toCook.push_back(job);
    }
    for (const std::shared_ptr<Job>& job : toCook){
        jobs::SubmitOptions options;
        options.lane = jobs::Lane::Background;
        options.gate = jobs::GateClass::Heavy;
        options.name = "texture-cook";
        TextureCookCache* cookCache = &cache;
        job->task = jobs::submit([job, cookCache](){
            const TextureCookRequest request = makeTextureCookRequest(job->inputs, job->project, job->family);
            job->result = TextureCooker::cook(*cookCache, request, [job](){ return job->cancel.load(); });
        }, options);
    }
}

void TextureCookQueue::collectJobs(){
    std::vector<std::shared_ptr<Job>> finished;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = running.begin(); it != running.end();){
            const std::shared_ptr<Job>& job = *it;
            // a cook stage cancelled before it started
            if (!job->task.isValid() && job->cancel){
                it = running.erase(it);
                continue;
            }
            if (!job->task.isValid() || !job->task.isReady()){
                ++it;
                continue;
            }
            finished.push_back(job);
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
        bool swap = false;
        bool planned = false;
        // the source went away while it cooked (deleted, moved): no cook and no error
        std::error_code missing;
        const bool gone = !job->result.ok && !fs::is_regular_file(job->inputs.absolute, missing);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (job->cooking)
                memoryInUse -= std::min(memoryInUse, job->workingBytes);
            auto it = entries.find({job->assetPath, (int)job->family});
            const bool current = it != entries.end() && it->second.job == job && it->second.epoch == job->epoch && !job->cancel;
            if (job->inferred && current){
                metaWrites.push_back({job->assetPath, YAML::Dump(job->inputs.settings.serialize())});
                job->inferred = false;
            }
            if (!current)
                continue;
            Entry& entry = it->second;
            TextureCookState& state = entry.state;
            if (!job->cooking && job->result.ok == false && job->result.error.empty() && job->workingBytes > 0){
                // the plan missed the cache: the cook stage starts within the budget
                job->cooking = true;
                job->task = jobs::Task<void>();
                running.push_back(job);
                state.status = TextureCookStatus::Queued;
                state.plan = job->result.plan;
                planned = true;
            }else{
                entry.job.reset();
                progressDone++;
                state.warnings = cookWarnings(job->inputs.settings, job->inputs.inference, job->result);
                state.problems = TextureTypeInference::conflicts(job->inputs.settings.resolvedType(), job->inputs.inference);
                state.problems.insert(state.problems.end(), job->result.plan.choice.problems.begin(), job->result.plan.choice.problems.end());
                if (job->sourceOnly){
                    state.status = TextureCookStatus::Source;
                    state.stale = false;
                }else if (job->result.ok){
                    state.status = TextureCookStatus::Cooked;
                    state.stale = false;
                    state.cacheHit = job->result.cacheHit;
                    state.error.clear();
                    state.plan = job->result.plan;
                    state.record = job->result.record;
                    swap = job->family == getViewFamily();
                }else if (job->result.cancelled || job->cancel || gone){
                    state.status = TextureCookStatus::None;
                }else{
                    state.status = TextureCookStatus::Failed;
                    state.error = job->result.error;
                    state.plan = job->result.plan;
                }
            }
        }
        if (planned)
            continue;
        if (job->result.ok && !job->sourceOnly && !job->result.cacheHit)
            Out::info("Cooked %s for %s: %s, %.0f ms", job->assetPath.c_str(), textureFamilyName(job->family),
                      job->result.record.label.c_str(), job->result.record.cookMs);
        else if (!job->result.ok && !job->result.cancelled && !job->cancel && !gone)
            Out::error("Texture cook of %s for %s failed: %s", job->assetPath.c_str(), textureFamilyName(job->family),
                       job->result.error.c_str());
        if (swap)
            startSwap(job->assetPath);
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

void TextureCookQueue::writeInferredSettings(){
    std::deque<std::pair<std::string, std::string>> writes;
    {
        std::lock_guard<std::mutex> lock(mutex);
        writes.swap(metaWrites);
    }
    for (const auto& [assetPath, yaml] : writes){
        const fs::path absolute = absoluteOf(assetPath);
        // the sidecar S04 made with the GUID; none yet: inferred again next time
        std::error_code ec;
        if (!fs::exists(SetAssetMetaCmd::metaPathOf(absolute), ec))
            continue;
        // (an absent key reads as a null node, which yaml-cpp counts as defined)
        const YAML::Node stored = SetAssetMetaCmd::read(absolute, TextureImportSettings::metaKey);
        if (stored && !stored.IsNull())
            continue;
        TextureImportSettings settings;
        if (!TextureImportSettings::parse(YAML::Load(yaml), settings))
            continue;
        {
            // the write's own listener call sees nothing new
            std::lock_guard<std::mutex> lock(mutex);
            settingsMemo[assetPath] = {true, settings};
        }
        // not a step of the history: the settings a texture always had, written down (R30)
        SetAssetMetaCmd command(project, project->normalizeToProjectRelative(absolute), TextureImportSettings::metaKey,
                                settings.serialize());
        command.execute();
    }
}

void TextureCookQueue::startSwap(const std::string& assetPath){
    const fs::path absolute = absoluteOf(assetPath);
    std::vector<std::pair<std::string, TextureSource>> sources;
    for (const std::string& id : TextureDataPool::findIdsUsingFile(absolute.string())){
        TextureSource source;
        if (TextureDataPool::getSource(id, source))
            sources.emplace_back(id, source);
    }
    if (sources.empty())
        return;
    auto swap = std::make_shared<SwapIn>();
    swap->assetPath = assetPath;
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.name = "texture-swap";
    swap->task = jobs::submit([sources](){
        std::vector<std::pair<std::string, std::shared_ptr<std::array<TextureData, 6>>>> decoded;
        for (const auto& [id, source] : sources)
            decoded.emplace_back(id, std::make_shared<std::array<TextureData, 6>>(TextureDataPool::decode(id, source)));
        return decoded;
    }, options);
    std::lock_guard<std::mutex> lock(mutex);
    swaps.push_back(swap);
}

void TextureCookQueue::collectSwaps(){
    std::vector<std::shared_ptr<SwapIn>> done;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = swaps.begin(); it != swaps.end();){
            if ((*it)->task.isReady()){
                done.push_back(*it);
                it = swaps.erase(it);
            }else{
                ++it;
            }
        }
    }
    bool changed = false;
    for (const std::shared_ptr<SwapIn>& swap : done){
        try{
            for (const auto& [id, data] : swap->task.get()){
                TextureDataPool::replace(id, data);
                TexturePool::invalidate(id);
                changed = true;
            }
        }catch (const std::exception& e){
            // the texture keeps what it shows
            Out::warning("Could not reload %s: %s", swap->assetPath.c_str(), e.what());
        }
    }
    if (changed)
        for (SceneProject* sceneProject : project->getScenesAndPrefabStages())
            sceneProject->needUpdateRender = true;
}

bool TextureCookQueue::waitFor(const std::vector<std::string>& assetPaths, const std::vector<TextureFamily>& families, double timeoutSeconds){
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(std::max(0.0, timeoutSeconds));
    for (const std::string& path : assetPaths)
        for (TextureFamily family : families)
            request(path, family, Selected);
    while (true){
        update();
        jobs::drainMainQueue();
        bool pending = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (const std::string& path : assetPaths){
                for (TextureFamily family : families){
                    auto it = entries.find({path, (int)family});
                    if (it != entries.end() && (it->second.queued || it->second.job))
                        pending = true;
                }
            }
        }
        if (!pending)
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

std::vector<TextureFamily> TextureCookQueue::familiesForExport(const std::vector<std::string>& platforms, bool androidEtc2Fallback){
    std::vector<TextureFamily> families;
    auto add = [&](TextureFamily family){
        if (std::find(families.begin(), families.end(), family) == families.end())
            families.push_back(family);
    };
    for (const std::string& platform : platforms){
        if (platform == "windows" || platform == "linux" || platform == "macos")
            add(TextureFamily::Desktop);
        else if (platform == "ios")
            add(TextureFamily::Mobile);
        else if (platform == "android"){
            add(TextureFamily::Mobile);
            if (androidEtc2Fallback)
                add(TextureFamily::AndroidEtc2);
        }else if (platform == "web")
            add(TextureFamily::Web);
    }
    return families;
}

// ---- batch

void TextureBatchCook::run(TextureCookCache& cache, const TextureProjectSettings& project,
                           const std::map<std::string, TextureCookInputs>& inputs, std::vector<TextureBatchItem>& items,
                           int threads, uint64_t memoryBudget, const std::function<void(int, int, const std::string&)>& progress,
                           const std::function<bool()>& cancelled){
    if (items.empty())
        return;
    if (threads <= 0)
        threads = std::max(1, jobs::hasSystem() ? jobs::system().getWorkerCount() : (int)std::thread::hardware_concurrency());
    threads = std::min<int>(threads, (int)items.size());
    std::mutex batchMutex;
    std::condition_variable memoryFreed;
    uint64_t inUse = 0;
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
            TextureBatchItem& item = items[index];
            auto found = inputs.find(item.assetPath);
            if (found == inputs.end()){
                item.result.error = "no inputs gathered for " + item.assetPath;
            }else if (found->second.inference.sourceOnly){
                item.sourceOnly = true;
                item.result.ok = true;
            }else if (isCancelled()){
                item.result.cancelled = true;
                item.result.error = "cancelled";
            }else{
                const TextureCookRequest request = makeTextureCookRequest(found->second, project, item.family);
                TextureCookPlan plan;
                std::string error;
                if (!TextureCooker::plan(cache, request, plan, error)){
                    item.result.error = error;
                    item.result.plan = plan;
                }else if (cache.has(plan.key) && cache.load(plan.key, item.result.record)){
                    item.result.ok = true;
                    item.result.cacheHit = true;
                    item.result.plan = plan;
                }else{
                    const uint64_t need = TextureCooker::workingBytes(plan.analysis, plan.resolved);
                    {
                        std::unique_lock<std::mutex> lock(batchMutex);
                        memoryFreed.wait(lock, [&](){ return inUse == 0 || inUse + need <= memoryBudget || isCancelled(); });
                        inUse += need;
                    }
                    item.result = TextureCooker::cook(cache, request, cancelled ? cancelled : std::function<bool()>([](){ return false; }));
                    {
                        std::lock_guard<std::mutex> lock(batchMutex);
                        inUse -= std::min(inUse, need);
                    }
                    memoryFreed.notify_all();
                }
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
