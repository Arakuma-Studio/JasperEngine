// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HotReloadService.h"

#include "handlers/ReloadHandlers.h"

#include "AppSettings.h"
#include "EditorHost.h"
#include "Out.h"
#include "Project.h"
#include "thread/ThreadPoolManager.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    using Clock = std::chrono::steady_clock;

    // R22: adoption per frame, and the texels uploaded per frame
    constexpr double ADOPT_BUDGET_MS = 2.0;
    constexpr uint64_t TEXEL_BUDGET = 16ull * 1024 * 1024;
    // a failure is tried once more after this (a file still being written, R11)
    constexpr std::chrono::milliseconds RETRY_DELAY{500};
    constexpr size_t RECORDS_MAX = 1000;
    // problems and UI reloads the watcher found that no MCP call took yet
    constexpr size_t LATE_MAX = 200;

    double msSince(Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    std::string key(const fs::path& path) {
        return watch::pathToUtf8(path.lexically_normal());
    }

    // "12 textures", "1 shader", "3 UI files"
    std::string countOf(size_t count, const std::string& kind) {
        std::string noun = kind;
        if (kind == "audio") noun = "audio clip";
        else if (kind == "ui") noun = "UI file";
        return std::to_string(count) + " " + noun + (count == 1 ? "" : "s");
    }

}

ChangeSet ChangeSet::fromPaths(const std::vector<fs::path>& paths) {
    ChangeSet set;
    for (const fs::path& path : paths) {
        FileChange change;
        change.path = path.lexically_normal();
        watch::PathStat stat;
        if (watch::statPath(change.path, stat)) {
            change.kind = FileChangeKind::Modified;
            change.directory = stat.directory;
            change.size = stat.size;
            change.mtimeNs = stat.mtimeNs;
            change.identity = stat.identity;
        } else {
            change.kind = FileChangeKind::Removed;
        }
        set.changes.push_back(change);
    }
    return set;
}

HotReloadService& HotReloadService::get() {
    static HotReloadService instance;
    return instance;
}

// ------------------------------------------------------------------ lifetime

namespace {

    bool watcherOverrideSet = false;
    WatchBackendKind watcherOverride = WatchBackendKind::Auto;

}

void HotReloadService::setWatcherOverride(WatchBackendKind kind) {
    watcherOverrideSet = true;
    watcherOverride = kind;
}

bool HotReloadService::getWatcherOverride(WatchBackendKind& kind) {
    if (!watcherOverrideSet) return false;
    kind = watcherOverride;
    return true;
}

void HotReloadService::startWatching(Project& projectIn) {
    detach();
    FileWatchService& watcher = FileWatchService::get();
    WatchBackendKind kind = WatchBackendKind::Auto;
    if (!getWatcherOverride(kind)) parseWatchBackendKind(AppSettings::getFileWatcher(), kind);
    // an idle editor loop wakes when changes wait for it (R9)
    watcher.setMainThreadWaker([]() { getEditorHost().enqueueMainThreadTask([]() {}); });
    const auto interval = std::chrono::milliseconds(static_cast<int64_t>(AppSettings::getPollingInterval() * 1000.0f));
    watcher.start(projectIn.getProjectPath(), kind, interval);
    autoRefresh = AppSettings::getAutoRefresh();
    logReloads = AppSettings::getLogReloads();
    attach(projectIn);
}

void HotReloadService::stopWatching() {
    detach();
    FileWatchService::get().stop();
}

void HotReloadService::attach(Project& projectIn) {
    detach();
    project = &projectIn;
    FileWatchService& watcher = FileWatchService::get();
    journalCursor = watcher.journalHead();
    watcher.setPaused(!autoRefresh);
    WatchFilter filter;
    filter.directories = true;          // folder renames for the asset database and the Project window
    filter.deliverEditorWrites = true;  // the editor's writes fan out (R10)
    subscription = watcher.subscribe(filter, [this](std::span<const FileChange> batch) { onWatcherBatch(batch); });
}

void HotReloadService::detach() {
    subscription.reset();
    // a worker may still read what a job captured: let it finish before the project goes
    for (const std::shared_ptr<Job>& job : jobs) {
        if (job->future.valid()) job->future.wait();
    }
    for (std::future<AdoptFn>& future : superseded) {
        if (future.valid()) future.wait();
    }
    superseded.clear();
    jobs.clear();
    batches.clear();
    currentBatch = 0;
    held.clear();
    applied.clear();
    externallyChangedScenes.clear();
    projectFileChanged = false;
    deferredScripts.clear();
    assetRefreshAfterPlay = false;
    lateProblems = Json::array();
    lateUi.clear();
    project = nullptr;
}

// ------------------------------------------------------------------ the watcher

void HotReloadService::onWatcherBatch(std::span<const FileChange> batch) {
    if (!project) return;
    if (paused) {
        held.insert(held.end(), batch.begin(), batch.end());
        return;
    }
    ChangeSet set;
    for (const FileChange& change : batch) {
        if (change.sequence >= journalCursor) journalCursor = change.sequence + 1;
        // a rescan is followed by the changes it found
        if (change.kind != FileChangeKind::Rescan) set.changes.push_back(change);
    }
    if (set.empty()) return;
    ApplyOptions options;
    options.fromWatcher = true;
    apply(set, options);
}

void HotReloadService::setPaused(bool pausedIn) {
    if (paused == pausedIn) return;
    paused = pausedIn;
    if (!paused && !held.empty()) {
        std::vector<FileChange> batch;
        batch.swap(held);
        onWatcherBatch(batch);
    }
}

void HotReloadService::setAutoRefresh(bool on) {
    autoRefresh = on;
    FileWatchService::get().setPaused(!on);
}

size_t HotReloadService::pendingExternalChanges() const {
    if (!project || autoRefresh) return 0;
    return FileWatchService::get().externalChangesSince(journalCursor);
}

void HotReloadService::refresh() {
    if (!project) return;
    FileWatchService& watcher = FileWatchService::get();
    const FileWatchService::Stats stats = watcher.stats();
    std::vector<FileChange> journal = watcher.journalSince(journalCursor);
    const bool overflowed = std::any_of(journal.begin(), journal.end(), [&](const FileChange& c) {
        return c.kind == FileChangeKind::Rescan && c.path == watcher.getRoot();
    });
    ChangeSet set;
    for (const FileChange& change : journal) {
        if (change.kind != FileChangeKind::Rescan) set.changes.push_back(change);
    }
    ApplyOptions options;
    // with no watcher, or a journal that overflowed, everything is looked at again
    options.full = overflowed || stats.state == FileWatchService::State::Off;
    options.fromWatcher = !options.full;
    apply(set, options);
}

// ------------------------------------------------------------------ apply

bool HotReloadService::isCurrent(const fs::path& path) const {
    auto it = applied.find(key(path));
    if (it == applied.end()) return false;
    watch::PathStat now;
    return watch::statPath(path, now) && now.sameContent(it->second) && now.identity == it->second.identity;
}

void HotReloadService::stamp(const fs::path& path) {
    watch::PathStat now;
    if (watch::statPath(path, now)) applied[key(path)] = now;
    else applied.erase(key(path));
}

std::string HotReloadService::relative(const fs::path& path) const {
    return project ? reload::rel(*project, path) : watch::pathToUtf8(path);
}

ApplyResult HotReloadService::apply(const ChangeSet& set, const ApplyOptions& options) {
    ApplyResult result;
    if (!project) return result;

    const uint64_t batchId = nextBatch++;
    Batch& batch = batches[batchId];
    batch.log = logReloads && !options.quiet;
    currentBatch = batchId;
    const bool playing = project->isAnyScenePlaying();
    reload::Context ctx{*this, *project, options, result, playing};

    // One change per path, the last word winning (a folder's rename and its entries kept)
    std::vector<FileChange> changes;
    std::map<std::string, size_t> at;
    for (const FileChange& change : set.changes) {
        if (change.kind == FileChangeKind::Rescan) continue;
        if (change.sequence >= journalCursor) journalCursor = change.sequence + 1;
        const std::string k = key(change.path);
        auto it = at.find(k);
        if (it != at.end() && change.kind != FileChangeKind::Moved && changes[it->second].kind != FileChangeKind::Moved) {
            FileChange merged = change;
            // Added then Modified is still an addition
            if (changes[it->second].kind == FileChangeKind::Added && change.kind == FileChangeKind::Modified) merged.kind = FileChangeKind::Added;
            changes[it->second] = merged;
            continue;
        }
        at[k] = changes.size();
        changes.push_back(change);
    }

    // What to reload, by kind: external changes all; the editor's own writes only where
    // others read them (materials linked elsewhere, textures, HTML UI documents)
    std::map<std::string, std::vector<fs::path>> external, editorWritten;
    std::vector<fs::path> openDocuments;
    for (const FileChange& change : changes) {
        const std::string relPath = relative(change.path);
        result.handled.push_back(change.path);
        switch (change.kind) {
            case FileChangeKind::Added: result.added.push_back(relPath); break;
            case FileChangeKind::Modified: result.modified.push_back(relPath); break;
            case FileChangeKind::Removed: result.removed.push_back(relPath); break;
            case FileChangeKind::Moved:
                result.removed.push_back(relative(change.oldPath));
                result.added.push_back(relPath);
                result.handled.push_back(change.oldPath);
                break;
            case FileChangeKind::Rescan: break;
        }
        if (change.directory) continue;
        if (change.kind == FileChangeKind::Removed) {
            notify(change.path, reload::kindOf(change.path), Event::Dropped);
            // a removed HTML document is reported missing by its scene's documents
            if (reload::isHtmlUiFile(watch::pathToUtf8(change.path))) external["ui"].push_back(change.path);
            continue;
        }
        const std::string kind = reload::kindOf(change.path);
        if (kind.empty()) continue;
        // reloaded already (by the watcher, a tool's own sync, another call's pick-up): reported,
        // not done again (the shared applied-stamp table, R29)
        if (!options.force && !options.full && isCurrent(change.path) && kind != "scene" && kind != "project") {
            // still reloading (the watcher started it a moment ago): pending, so the caller waits
            if (!idleFor({change.path})) {
                if (change.origin == ChangeOrigin::External) result.pending.push_back(relPath);
                continue;
            }
            Record record;
            if (change.origin == ChangeOrigin::External && lastRecordOf(relPath, record)) {
                result.reloaded.push_back({{"path", record.path}, {"kind", record.kind}, {"ms", std::round(record.ms)}});
            }
            continue;
        }
        if (change.origin == ChangeOrigin::Editor) {
            if (kind == "material" || kind == "texture") editorWritten[kind].push_back(change.path);
            if (reload::isHtmlUiFile(watch::pathToUtf8(change.path))) editorWritten["ui"].push_back(change.path);
            continue;
        }
        if (kind == "scene" || kind == "prefab" || kind == "project") {
            // another program's edit of an open document (a tool's own sync of a file it
            // renamed or wrote is quiet: no bar for it)
            if (change.kind == FileChangeKind::Modified && !options.quiet) openDocuments.push_back(change.path);
            if (kind != "prefab") continue;
        }
        external[kind].push_back(change.path);
        if (kind != "ui" && reload::isHtmlUiFile(watch::pathToUtf8(change.path))) external["ui"].push_back(change.path);
    }

    auto files = [](std::map<std::string, std::vector<fs::path>>& map, const char* kind) -> std::vector<fs::path>& { return map[kind]; };
    auto both = [&](const char* kind) {
        std::vector<fs::path> list = files(external, kind);
        for (const fs::path& path : files(editorWritten, kind)) {
            if (std::find(list.begin(), list.end(), path) == list.end()) list.push_back(path);
        }
        return list;
    };

    std::vector<fs::path> scripts = files(external, "script");
    std::vector<fs::path> code = scripts;
    for (const char* kind : {"shader", "ui", "material", "text"}) {
        for (const fs::path& path : files(external, kind)) code.push_back(path);
    }
    reload::reloadCodeEditorBuffers(ctx, code, options.full);
    reload::reloadScripts(ctx, scripts, options.full);
    reload::reloadMaterials(ctx, both("material"));
    reload::reloadShaders(ctx, files(external, "shader"));
    std::vector<fs::path> textures;
    for (const fs::path& path : both("texture")) {
        if (!reload::isEditorOwnedFile(relative(path))) textures.push_back(path);
    }
    reload::reloadTextures(ctx, textures);
    reload::reloadModels(ctx, files(external, "model"));
    reload::reloadAudio(ctx, files(external, "audio"));
    reload::reloadFonts(ctx, files(external, "font"));
    reload::reloadHtmlUi(ctx, both("ui"));
    reload::noteOpenDocuments(ctx, openDocuments, externallyChangedScenes, projectFileChanged);
    reload::applyAssetChanges(ctx, changes);

    for (const auto* map : {&external, &editorWritten}) {
        for (const auto& [kind, list] : *map) {
            for (const fs::path& path : list) stamp(path);
        }
    }
    for (const std::shared_ptr<Job>& job : jobs) {
        const std::string pendingPath = relative(job->path);
        if (job->batch == batchId && std::find(result.pending.begin(), result.pending.end(), pendingPath) == result.pending.end()) {
            result.pending.push_back(pendingPath);
        }
    }
    // the watcher's reload of another program's edit: the next MCP call reports what it found
    const bool external = std::any_of(changes.begin(), changes.end(), [](const FileChange& c) { return c.origin == ChangeOrigin::External; });
    if (options.fromWatcher && external) {
        for (const Json& problem : result.problems) addLateProblem(problem);
        if (!result.uiReloaded.empty()) {
            lateUi.push_back({std::chrono::steady_clock::now(), result.uiReloaded});
            while (lateUi.size() > LATE_MAX) lateUi.pop_front();
        }
    }
    currentBatch = 0;
    closeBatchIfDone(batchId);
    return result;
}

// ------------------------------------------------------------------ jobs

std::future<HotReloadService::AdoptFn> HotReloadService::launch(WorkFn work) {
    return ThreadPoolManager::getInstance().enqueue([work]() {
        // the idle editor loop wakes to adopt it
        struct Wake {
            ~Wake() { getEditorHost().enqueueMainThreadTask([]() {}); }
        } wake;
        return work();
    });
}

void HotReloadService::startJob(const std::string& kind, const fs::path& path, WorkFn work, uint64_t texelBytes) {
    if (currentBatch == 0) {
        currentBatch = nextBatch++;
        batches[currentBatch].log = logReloads;
    }
    // a reload of the same file still running is replaced: its result is out of date (it
    // finishes on its worker and is dropped, the main thread does not wait for it)
    for (auto it = jobs.begin(); it != jobs.end(); ++it) {
        if ((*it)->kind == kind && (*it)->path == path) {
            const uint64_t old = (*it)->batch;
            if ((*it)->future.valid()) superseded.push_back(std::move((*it)->future));
            jobs.erase(it);
            auto batch = batches.find(old);
            if (batch != batches.end() && batch->second.outstanding > 0) batch->second.outstanding--;
            closeBatchIfDone(old);
            break;
        }
    }
    auto job = std::make_shared<Job>();
    job->batch = currentBatch;
    job->kind = kind;
    job->path = path;
    job->work = std::move(work);
    job->started = Clock::now();
    job->texelBytes = texelBytes;
    job->future = launch(job->work);
    batches[currentBatch].outstanding++;
    jobs.push_back(job);
}

bool HotReloadService::update() {
    if (!project) return false;
    const auto start = Clock::now();
    bool adopted = false;
    superseded.erase(std::remove_if(superseded.begin(), superseded.end(), [](std::future<AdoptFn>& future) {
        return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }), superseded.end());
    uint64_t texels = 0;
    for (auto it = jobs.begin(); it != jobs.end();) {
        Job& job = **it;
        if (job.waitingRetry) {
            if (Clock::now() >= job.retryAt) {
                job.waitingRetry = false;
                job.future = launch(job.work);
            }
            ++it;
            continue;
        }
        if (!job.future.valid() || job.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        // the rest waits for the next frame (R22)
        if (msSince(start) >= ADOPT_BUDGET_MS) break;
        if (texels > 0 && texels + job.texelBytes > TEXEL_BUDGET) break;
        std::string error;
        bool ok = false;
        try {
            AdoptFn adopt = job.future.get();
            ok = adopt && adopt(error);
            if (!adopt && error.empty()) error = "nothing was decoded";
        } catch (const std::exception& e) {
            error = e.what();
        }
        texels += job.texelBytes;
        if (ok && !error.empty()) {
            // left out on purpose (a model whose hierarchy changed): a warning, kept as it was
            std::shared_ptr<Job> done = *it;
            it = jobs.erase(it);
            finishJob(*done, false, error, msSince(done->started), true);
            continue;
        }
        if (!ok && !job.retried) {
            // a partial write: once more, a little later
            job.retried = true;
            job.waitingRetry = true;
            job.retryAt = Clock::now() + RETRY_DELAY;
            ++it;
            continue;
        }
        std::shared_ptr<Job> done = *it;
        it = jobs.erase(it);
        finishJob(*done, ok, error, msSince(done->started));
        adopted = true;
    }

    if (!project->isAnyScenePlaying()) {
        // what Play deferred (scripts are not re-parsed while a scene plays)
        if (!deferredScripts.empty()) {
            std::vector<fs::path> scripts;
            scripts.swap(deferredScripts);
            ApplyOptions options;
            options.force = true;
            apply(ChangeSet::fromPaths(scripts), options);
        }
        if (assetRefreshAfterPlay) {
            assetRefreshAfterPlay = false;
            project->refreshAssetDatabase();
        }
    }
    return adopted;
}

void HotReloadService::finishJob(Job& job, bool ok, const std::string& error, double ms, bool warning) {
    Record record;
    record.path = relative(job.path);
    record.kind = job.kind;
    record.ms = ms;
    record.ok = ok;
    record.error = error;
    record.attempts = job.retried ? 2 : 1;
    pushRecord(record);
    if (ok) {
        notify(job.path, job.kind, Event::Reloaded);
        notify(job.path, job.kind, Event::Content);
    } else if (warning) {
        Out::warning("[Hot Reload] %s: %s (kept the previous version)", record.path.c_str(), error.c_str());
        lateProblems.push_back({{"path", record.path}, {"severity", "warning"}, {"message", error}});
    } else {
        Out::error("[Hot Reload] %s: %s (kept the previous version)", record.path.c_str(), error.c_str());
        // a shader reports its compile errors with their lines itself
        if (job.kind != "shader") lateProblems.push_back({{"path", record.path}, {"message", "Reload failed: " + error + " (the previous version is kept)."}});
        // a later save of the same bytes would not be tried again otherwise
        applied.erase(key(job.path));
    }
    auto batch = batches.find(job.batch);
    if (batch != batches.end()) {
        batch->second.done.push_back(record);
        if (batch->second.outstanding > 0) batch->second.outstanding--;
        closeBatchIfDone(job.batch);
    }
}

void HotReloadService::recordDone(const std::string& kind, const fs::path& path, double ms, bool ok, const std::string& error) {
    Record record;
    record.path = relative(path);
    record.kind = kind;
    record.ms = ms;
    record.ok = ok;
    record.error = error;
    pushRecord(record);
    if (ok) notify(path, kind, Event::Reloaded);
    auto batch = batches.find(currentBatch);
    if (batch != batches.end()) batch->second.done.push_back(record);
}

void HotReloadService::pushRecord(Record record) {
    record.serial = nextSerial++;
    if (record.ok) reloads++;
    records.push_back(std::move(record));
    while (records.size() > RECORDS_MAX) records.pop_front();
}

void HotReloadService::closeBatchIfDone(uint64_t id) {
    if (id == currentBatch) return;   // apply() is still filling it
    auto it = batches.find(id);
    if (it == batches.end() || it->second.outstanding > 0) return;
    if (it->second.log) writeBatchLine(it->second);
    batches.erase(it);
}

void HotReloadService::writeBatchLine(const Batch& batch) {
    std::vector<const Record*> ok;
    for (const Record& record : batch.done) {
        if (record.ok) ok.push_back(&record);
    }
    // one line per batch (R25: a checkout of 10,000 files is not 10,000 lines)
    if (ok.empty()) return;
    if (ok.size() == 1) {
        Out::info("[Hot Reload] Reloaded %s (%s, %d ms)", ok[0]->path.c_str(), ok[0]->kind.c_str(), static_cast<int>(std::lround(ok[0]->ms)));
        return;
    }
    std::map<std::string, size_t> byKind;
    for (const Record* record : ok) byKind[record->kind]++;
    std::vector<std::pair<std::string, size_t>> kinds(byKind.begin(), byKind.end());
    std::stable_sort(kinds.begin(), kinds.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string text;
    for (const auto& [kind, count] : kinds) text += (text.empty() ? "" : ", ") + countOf(count, kind);
    Out::info("[Hot Reload] Reloaded %zu files: %s", ok.size(), text.c_str());
}

bool HotReloadService::idleFor(const std::vector<fs::path>& paths) const {
    for (const std::shared_ptr<Job>& job : jobs) {
        for (const fs::path& path : paths) {
            if (job->path == path.lexically_normal()) return false;
        }
    }
    return true;
}

bool HotReloadService::idle() const {
    return jobs.empty();
}

size_t HotReloadService::reloadingCount() const {
    return jobs.size();
}

void HotReloadService::deferScripts(const std::vector<fs::path>& paths) {
    for (const fs::path& path : paths) {
        if (std::find(deferredScripts.begin(), deferredScripts.end(), path) == deferredScripts.end()) deferredScripts.push_back(path);
    }
}

Json HotReloadService::takeLateProblems() {
    Json out = lateProblems;
    lateProblems = Json::array();
    return out;
}

void HotReloadService::addLateProblem(const Json& problem) {
    // no MCP client takes them: the oldest go
    if (lateProblems.size() >= LATE_MAX) lateProblems.erase(lateProblems.begin());
    lateProblems.push_back(problem);
}

HotReloadService::WatcherUi HotReloadService::takeWatcherUi(std::chrono::steady_clock::time_point since) {
    WatcherUi out;
    for (const LateUi& late : lateUi) {
        if (late.at >= since) {
            for (const Json& item : late.reloaded) out.reloaded.push_back(item);
        } else {
            out.current += static_cast<int>(late.reloaded.size());
        }
    }
    lateUi.clear();
    return out;
}

std::vector<HotReloadService::Record> HotReloadService::recordsSince(uint64_t serial) const {
    std::vector<Record> out;
    for (const Record& record : records) {
        if (record.serial > serial) out.push_back(record);
    }
    return out;
}

bool HotReloadService::lastRecordOf(const std::string& relativePath, Record& out) const {
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        if (it->path == relativePath) {
            out = *it;
            return true;
        }
    }
    return false;
}

Json HotReloadService::lastReloadJson() const {
    if (records.empty()) return nullptr;
    const Record& record = records.back();
    Json out = {{"path", record.path}, {"kind", record.kind}, {"ms", std::round(record.ms)}, {"ok", record.ok}};
    if (!record.ok) out["error"] = record.error;
    return out;
}

// ------------------------------------------------------------------ open documents

bool HotReloadService::hasExternalChange(uint32_t sceneId) const {
    return externallyChangedScenes.count(sceneId) > 0;
}

void HotReloadService::keepMine(uint32_t sceneId) {
    // the editor's copy is written over the file on the next save
    externallyChangedScenes.erase(sceneId);
}

void HotReloadService::reloadFromDisk(uint32_t sceneId) {
    if (!project) return;
    SceneProject* scene = project->getScene(sceneId);
    if (!scene) {
        externallyChangedScenes.erase(sceneId);
        return;
    }
    auto reloadNow = [this, sceneId]() {
        if (!project) return;
        std::string error;
        if (!project->reloadSceneFromDisk(sceneId, &error)) {
            Out::error("[Hot Reload] %s", error.c_str());
            return;
        }
        externallyChangedScenes.erase(sceneId);
    };
    if (scene->isModified) {
        getEditorHost().registerConfirmAlert("Reload " + scene->name,
            "\"" + scene->name + "\" has unsaved changes. Reload it from disk and lose them?", reloadNow);
        return;
    }
    reloadNow();
}

void HotReloadService::reloadProjectFile() {
    if (!project) return;
    const fs::path path = project->getProjectPath();
    auto reloadNow = [this, path]() {
        projectFileChanged = false;
        Project* current = project;
        getEditorHost().requestProjectChange([current, path]() { current->loadProject(path); });
    };
    bool unsaved = false;
    for (SceneProject* scene : project->getScenesAndPrefabStages()) unsaved = unsaved || scene->isModified;
    if (unsaved) {
        getEditorHost().registerConfirmAlert("Reload Project", "The project settings changed on disk. Reload the project and lose the unsaved changes?", reloadNow);
        return;
    }
    reloadNow();
}

// ------------------------------------------------------------------ invalidators

uint64_t HotReloadService::addInvalidator(uint8_t events, EventFn fn) {
    const uint64_t id = nextInvalidator++;
    invalidators[id] = {events, std::move(fn)};
    return id;
}

void HotReloadService::removeInvalidator(uint64_t id) {
    invalidators.erase(id);
}

void HotReloadService::notify(const fs::path& path, const std::string& kind, Event event) {
    if (invalidators.empty()) return;
    // a callback may remove itself
    const auto copy = invalidators;
    for (const auto& [id, entry] : copy) {
        if (entry.first & static_cast<uint8_t>(event)) entry.second(path, kind, event);
    }
}

ReloadInvalidator::ReloadInvalidator(uint8_t events, HotReloadService::EventFn fn)
    : id(HotReloadService::get().addInvalidator(events, std::move(fn))) {
}

ReloadInvalidator::~ReloadInvalidator() {
    if (id) HotReloadService::get().removeInvalidator(id);
}

ReloadInvalidator::ReloadInvalidator(ReloadInvalidator&& other) noexcept : id(other.id) {
    other.id = 0;
}

ReloadInvalidator& ReloadInvalidator::operator=(ReloadInvalidator&& other) noexcept {
    if (this != &other) {
        if (id) HotReloadService::get().removeInvalidator(id);
        id = other.id;
        other.id = 0;
    }
    return *this;
}
