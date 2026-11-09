// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HotReloadService.h"
#include "sprite/SpriteAtlasBuilder.h"
#include "sprite/SpriteSheetBuilder.h"
#include "sprite/SpriteSheetPool.h"
#include "sprite/SpriteSheetSources.h"

#include "handlers/ReloadHandlers.h"

#include "AppSettings.h"
#include "EditorHost.h"
#include "Project.h"
#include "model/ModelCookQueue.h"
#include "texture/TextureCookQueue.h"
#include "tilemap/import/TilemapImportApply.h"
#include "log/Logger.h"

#include <algorithm>
#include <cmath>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    using Clock = std::chrono::steady_clock;

    // R22: adoption per frame, and the texels uploaded per frame
    constexpr double RELOAD_ADOPT_BUDGET_MS = 2.0;
    constexpr uint64_t RELOAD_TEXEL_BUDGET = 16ull * 1024 * 1024;
    // a failure is tried once more after this (a file still being written, R11)
    constexpr std::chrono::milliseconds RELOAD_RETRY_DELAY{500};
    constexpr size_t RELOAD_RECORDS_MAX = 1000;
    // problems and UI reloads the watcher found that no MCP call took yet
    constexpr size_t RELOAD_LATE_MAX = 200;

    // FNV-1a of a file's bytes (0 when it cannot be read): told apart only for racy stamps
    uint64_t contentHashOf(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return 0;
        uint64_t hash = 1469598103934665603ull;
        char buffer[64 * 1024];
        while (in) {
            in.read(buffer, sizeof(buffer));
            const std::streamsize got = in.gcount();
            for (std::streamsize i = 0; i < got; i++) {
                hash ^= static_cast<unsigned char>(buffer[i]);
                hash *= 1099511628211ull;
            }
        }
        return hash;
    }

    double reloadMsSince(Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    std::string appliedKey(const fs::path& path) {
        return watch::pathToUtf8(path.lexically_normal());
    }

    // "12 textures", "1 shader", "3 UI files"
    std::string reloadCountOf(size_t count, const std::string& kind) {
        std::string noun = kind;
        if (kind == "audio") noun = "audio clip";
        else if (kind == "ui") noun = "UI file";
        else if (kind == "particles") noun = "particle effect";
        else if (kind == "palette") noun = "palette";
        else if (kind == "tileset") noun = "tileset";
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
        job->task.wait();
    }
    for (const jobs::Task<AdoptFn>& task : superseded) task.wait();
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
    lateProblems.clear();
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

bool HotReloadService::isCurrent(const FileChange& change) const {
    auto it = applied.find(appliedKey(change.path));
    if (it == applied.end()) return false;
    if (change.sequence != 0 && change.sequence <= it->second.sequence) return true;
    watch::PathStat now;
    if (!watch::statPath(change.path, now) || !now.sameContent(it->second.stat) || now.identity != it->second.stat.identity) return false;
    // a save in the same timestamp tick as the one reloaded has the same stat: the bytes tell
    // (the editor's own write is the one it reloaded: it knows what it wrote)
    if (!it->second.racy || change.origin == ChangeOrigin::Editor || now.directory) return true;
    return it->second.contentHash != 0 && contentHashOf(change.path) == it->second.contentHash;
}

void HotReloadService::forgetApplied(const fs::path& path) {
    applied.erase(appliedKey(path));
}

void HotReloadService::stamp(const fs::path& path, const Seen* seen, uint64_t sequence) {
    if (!seen) {
        applied.erase(appliedKey(path));
        return;
    }
    Applied& entry = applied[appliedKey(path)];
    entry.stat = seen->stat;
    entry.racy = watch::isRacyStamp(seen->stat.mtimeNs);
    entry.contentHash = entry.racy ? seen->contentHash : 0;
    entry.sequence = std::max(entry.sequence, sequence);
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
        const std::string k = appliedKey(change.path);
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
    std::vector<fs::path> openDocuments, gone;
    // graph files removed or moved away: their open documents learn the file is gone
    std::vector<fs::path> graphsGone;
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
        if (change.kind == FileChangeKind::Removed) gone.push_back(change.path);
        if (change.kind == FileChangeKind::Moved) gone.push_back(change.oldPath);
        if (change.kind == FileChangeKind::Removed && reload::kindOf(change.path) == "graph") graphsGone.push_back(change.path);
        if (change.kind == FileChangeKind::Moved && reload::kindOf(change.oldPath) == "graph") graphsGone.push_back(change.oldPath);
        if (change.kind == FileChangeKind::Removed) {
            notify(change.path, reload::kindOf(change.path), Event::Dropped);
            if (sprite::SpriteAtlasBuilder::get().getProject() == project) {
                // S45: an atlas gone takes its build along; a member gone rebuilds the atlases packing it
                if (SpriteSheetSources::isAtlasPath(change.path.string())) sprite::SpriteAtlasBuilder::get().forget(change.path.string());
                else if (SpriteSheetSources::isTexturePath(change.path.string()) || SpriteSheetPool::isAsepritePath(change.path.string())) {
                    sprite::SpriteSheetBuilder::get().invalidate(project->normalizeToAssetsRelative(change.path).generic_string());
                    sprite::SpriteAtlasBuilder::get().memberChanged(change.path.string());
                }
            }
            // a removed HTML document is reported missing by its scene's documents
            if (reload::isHtmlUiFile(watch::pathToUtf8(change.path))) external["ui"].push_back(change.path);
            // a removed .clips sidecar (an undo, a delete): the model's clips play the importer's
            // settings again, so its clip documents and players reload (S77 R29)
            if (change.path.extension() == ".clips") {
                (change.origin == ChangeOrigin::Editor ? editorWritten : external)["animation"].push_back(change.path);
            }
            continue;
        }
        // S45: a renamed atlas is forgotten under its old name (built again under the new one
        // below); a renamed member rebuilds the atlases that packed it and those it joins now
        if (change.kind == FileChangeKind::Moved && sprite::SpriteAtlasBuilder::get().getProject() == project) {
            const std::string oldPath = change.oldPath.string();
            if (SpriteSheetSources::isAtlasPath(oldPath)) {
                sprite::SpriteAtlasBuilder::get().forget(oldPath, false);
            } else if (SpriteSheetSources::isTexturePath(oldPath) || SpriteSheetPool::isAsepritePath(oldPath)) {
                sprite::SpriteSheetBuilder::get().invalidate(project->normalizeToAssetsRelative(change.oldPath).generic_string());
                sprite::SpriteAtlasBuilder::get().memberChanged(oldPath);
                sprite::SpriteAtlasBuilder::get().memberChanged(change.path.string());
            }
        }
        // so is a renamed one under its old name (a document reading the old name reloads and
        // reports it missing; the new name is handled below like an added file)
        if (change.kind == FileChangeKind::Moved && reload::isHtmlUiFile(watch::pathToUtf8(change.oldPath))) {
            (change.origin == ChangeOrigin::Editor ? editorWritten : external)["ui"].push_back(change.oldPath);
        }
        // another program's write of a sidecar (a pull, a merge): the importers' settings in it
        // changed, so their cooks go again (S53 R13; S14's textures as well)
        if (change.origin == ChangeOrigin::External && change.path.extension() == ".meta" && project->isInsideAssetsPath(change.path)) {
            fs::path asset = change.path;
            asset.replace_extension();
            const std::string assetPath = project->normalizeToAssetsRelative(asset).generic_string();
            if (ModelCookQueue::get().getProject() == project) ModelCookQueue::get().settingsChanged(assetPath);
            if (TextureCookQueue::get().getProject() == project) TextureCookQueue::get().settingsChanged(assetPath);
            // S45: a texture's Sprite Mode, an atlas member's settings
            if (sprite::SpriteAtlasBuilder::get().getProject() == project) {
                sprite::SpriteSheetBuilder::get().invalidate(assetPath);
                sprite::SpriteAtlasBuilder::get().memberChanged(assetPath);
            }
        }
        // S45: a Sprite Atlas written (by an agent, a pull, the inspector) builds again
        if (SpriteSheetSources::isAtlasPath(change.path.string()) && sprite::SpriteAtlasBuilder::get().getProject() == project) {
            sprite::SpriteAtlasBuilder::get().rebuild(change.path.string(), false);
        }
        const std::string kind = reload::kindOf(change.path);
        if (kind.empty()) continue;
        // a text file reloads only the code editor's buffer of it: with none open there is
        // nothing to stamp or read (a checkout of 10,000 data files)
        if (kind == "text" && !reload::isOpenInCodeEditor(change.path)) continue;
        // reloaded already (by the watcher, a tool's own sync, another call's pick-up): reported,
        // not done again (the shared applied-stamp table, R29)
        if (!options.force && !options.full && isCurrent(change) && kind != "scene" && kind != "project") {
            // still reloading (the watcher started it a moment ago): pending, so the caller waits
            if (!idleFor({change.path})) {
                if (change.origin == ChangeOrigin::External) result.pending.push_back(relPath);
                continue;
            }
            Record record;
            if (change.origin == ChangeOrigin::External && lastRecordOf(relPath, record)) {
                result.reloaded.push_back({{"path", record.path}, {"kind", record.kind}, {"ms", std::round(record.ms)}});
            }
            // a clip's or pose animation's load problems as the file is now: the watcher's report
            // of them goes stale when the asset scan writes the clip's GUID into it meanwhile (a
            // Background job), and the reading is cheap
            if (change.origin == ChangeOrigin::External && kind == "animation") {
                reload::addAnimationFileProblems(*project, change.path, result.problems);
            }
            // (and what the watcher found in these is the file's as it is now, not as it was)
            if (change.origin == ChangeOrigin::External && kind == "particles") reload::addParticleEffectFileProblems(*project, change.path, result.problems);
            if (change.origin == ChangeOrigin::External && kind == "graph") reload::addGraphFileProblems(*project, change.path, result.problems);
            continue;
        }
        if (change.origin == ChangeOrigin::Editor) {
            // (an agent's write_file of a clip or a pose animation: their documents reload too,
            // and its load problems are the call's)
            // (a palette the Palette window or edit_palette wrote: the swaps drawn with it update; an
            // .aseprite an editor tool wrote: imported again like another program's save, S44 R23)
            if (kind == "material" || kind == "texture" || kind == "animation" || kind == "palette" || kind == "tileset" || kind == "graph" ||
                kind == "aseprite" || kind == "font")
                editorWritten[kind].push_back(change.path);
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

    // What each file to reload is now, before a handler reads it, and the journal change it is
    // reloaded for (0: a path without one)
    std::map<std::string, Seen> seenBefore;
    for (const auto* map : {&external, &editorWritten}) {
        for (const auto& [kind, list] : *map) {
            for (const fs::path& path : list) {
                if (seenBefore.count(appliedKey(path))) continue;
                Seen seen;
                if (!watch::statPath(path, seen.stat)) continue;
                // written a moment ago: a save in the same tick would keep the stat
                if (!seen.stat.directory && watch::isRacyStamp(seen.stat.mtimeNs)) seen.contentHash = contentHashOf(path);
                seenBefore[appliedKey(path)] = seen;
            }
        }
    }
    std::map<std::string, uint64_t> sequenceOf;
    for (const FileChange& change : changes) {
        uint64_t& sequence = sequenceOf[appliedKey(change.path)];
        sequence = std::max(sequence, change.sequence);
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
    for (const char* kind : {"shader", "ui", "material", "text", "particles", "graph"}) {
        for (const fs::path& path : files(external, kind)) code.push_back(path);
    }
    reload::reloadCodeEditorBuffers(ctx, code, options.full);
    reload::reloadScripts(ctx, scripts, options.full);
    reload::removeScriptClasses(ctx, gone);
    reload::reloadMaterials(ctx, both("material"));
    reload::reloadShaders(ctx, files(external, "shader"));
    std::vector<fs::path> textures;
    for (const fs::path& path : both("texture")) {
        if (!reload::isEditorOwnedFile(relative(path))) textures.push_back(path);
    }
    reload::reloadTextures(ctx, textures);
    reload::reloadModels(ctx, files(external, "model"));
    reload::reloadAseprite(ctx, both("aseprite"));
    reload::reloadAudio(ctx, files(external, "audio"));
    // the project's audio mixer is heard as its file now says, in Edit mode and Play alike: the
    // editor's own writes (audio_mixer edit, the Audio Mixer window, their undo) and another
    // program's (upstream's ProjectFileWrites::refresh did it; the MCP sync comes here instead)
    {
        std::vector<fs::path> paths;
        for (const FileChange& change : changes) paths.push_back(change.path);
        project->audioFilesChanged(paths);
    }
    if (!playing) {
        // S50 R47: linked tile maps, after this pass: they write .tileset files
        std::vector<fs::path> paths;
        for (const FileChange& change : changes) {
            if (change.kind != FileChangeKind::Removed && !change.directory) paths.push_back(change.path);
        }
        if (!paths.empty()) {
            Project* target = project;
            getEditorHost().enqueueMainThreadTask([target, paths]() {
                if (HotReloadService::get().getProject() == target) tilemapimport::linkedSourcesChanged(*target, paths);
            });
        }
    }
    reload::reloadParticleEffects(ctx, files(external, "particles"));
    reload::reloadPalettes(ctx, both("palette"));
    reload::reloadTilesets(ctx, both("tileset"));
    {
        std::vector<fs::path> graphDocuments = files(editorWritten, "graph");
        graphDocuments.insert(graphDocuments.end(), graphsGone.begin(), graphsGone.end());
        reload::reloadGraphs(ctx, files(external, "graph"), graphDocuments);
    }
    reload::reloadFonts(ctx, files(external, "font"));
    {
        std::vector<fs::path> bitmapFonts = both("font");
        for (const fs::path& path : both("texture")) bitmapFonts.push_back(path);
        reload::reloadBitmapFonts(ctx, bitmapFonts);
    }
    reload::reloadAnimations(ctx, both("animation"), files(external, "model"), options.full);
    // another program's changes not reloaded yet (the dedupe above): the documents' own stamps
    // cannot tell a save in the same timestamp tick from the one they read
    reload::reloadHtmlUi(ctx, both("ui"), files(external, "ui"));
    reload::noteOpenDocuments(ctx, openDocuments, externallyChangedScenes, projectFileChanged);
    reload::applyAssetChanges(ctx, changes);

    for (const auto* map : {&external, &editorWritten}) {
        for (const auto& [kind, list] : *map) {
            for (const fs::path& path : list) {
                auto seen = seenBefore.find(appliedKey(path));
                stamp(path, seen != seenBefore.end() ? &seen->second : nullptr, sequenceOf[appliedKey(path)]);
            }
        }
    }
    for (const std::shared_ptr<Job>& job : jobs) {
        const std::string pendingPath = relative(job->path);
        if (job->batch == batchId && std::find(result.pending.begin(), result.pending.end(), pendingPath) == result.pending.end()) {
            result.pending.push_back(pendingPath);
        }
    }
    // the watcher's reload of another program's edit: the next MCP call reports what it found
    const bool anyExternal = std::any_of(changes.begin(), changes.end(), [](const FileChange& c) { return c.origin == ChangeOrigin::External; });
    if (options.fromWatcher && anyExternal) {
        for (const Json& problem : result.problems) addLateProblem(problem);
        for (const Json& item : result.scriptProperties) {
            // no MCP client takes them: the oldest go
            if (lateScriptProperties.size() >= RELOAD_LATE_MAX) lateScriptProperties.pop_front();
            lateScriptProperties.push_back(item);
        }
        if (!result.uiReloaded.empty()) {
            lateUi.push_back({std::chrono::steady_clock::now(), result.uiReloaded});
            while (lateUi.size() > RELOAD_LATE_MAX) lateUi.pop_front();
        }
    }
    currentBatch = 0;
    closeBatchIfDone(batchId);
    return result;
}

// ------------------------------------------------------------------ jobs

jobs::Task<HotReloadService::AdoptFn> HotReloadService::launch(WorkFn work) {
    // Background: the decodes give way to the frame's own jobs (S06 frame protection)
    return jobs::submit([work]() {
        // the idle editor loop wakes to adopt it
        struct Wake {
            ~Wake() { getEditorHost().enqueueMainThreadTask([]() {}); }
        } wake;
        return work();
    }, {jobs::Lane::Background, jobs::GateClass::Default, "hot-reload"});
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
            if ((*it)->task.isValid()) superseded.push_back((*it)->task);
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
    job->task = launch(job->work);
    batches[currentBatch].outstanding++;
    jobs.push_back(job);
}

bool HotReloadService::update() {
    if (!project) return false;
    const auto start = Clock::now();
    bool adopted = false;
    superseded.erase(std::remove_if(superseded.begin(), superseded.end(), [](const jobs::Task<AdoptFn>& task) {
        return task.isReady();
    }), superseded.end());
    uint64_t texels = 0;
    for (auto it = jobs.begin(); it != jobs.end();) {
        Job& job = **it;
        if (job.waitingRetry) {
            if (Clock::now() >= job.retryAt) {
                job.waitingRetry = false;
                job.task = launch(job.work);
            }
            ++it;
            continue;
        }
        if (!job.task.isReady()) {
            ++it;
            continue;
        }
        // the rest waits for the next frame (R22)
        if (reloadMsSince(start) >= RELOAD_ADOPT_BUDGET_MS) break;
        if (texels > 0 && texels + job.texelBytes > RELOAD_TEXEL_BUDGET) break;
        std::string error;
        bool ok = false;
        try {
            AdoptFn adopt = job.task.get();
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
            finishJob(*done, false, error, reloadMsSince(done->started), true);
            continue;
        }
        if (!ok && !job.retried) {
            // a partial write: once more, a little later
            job.retried = true;
            job.waitingRetry = true;
            job.retryAt = Clock::now() + RELOAD_RETRY_DELAY;
            ++it;
            continue;
        }
        std::shared_ptr<Job> done = *it;
        it = jobs.erase(it);
        finishJob(*done, ok, error, reloadMsSince(done->started));
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
            project->refreshAssetDatabase(true);
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
        JE_LOG_WARN(logCategoryAsset(), "[Hot Reload] %s: %s (kept the previous version)", record.path.c_str(), error.c_str());
        addLateProblem({{"path", record.path}, {"severity", "warning"}, {"message", error}});
    } else {
        JE_LOG_ERROR(logCategoryAsset(), "[Hot Reload] %s: %s (kept the previous version)", record.path.c_str(), error.c_str());
        // a shader reports its compile errors with their lines itself
        if (job.kind != "shader") addLateProblem({{"path", record.path}, {"message", "Reload failed: " + error + " (the previous version is kept)."}});
        // a later save of the same bytes would not be tried again otherwise
        applied.erase(appliedKey(job.path));
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
    while (records.size() > RELOAD_RECORDS_MAX) records.pop_front();
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
        JE_LOG_INFO(logCategoryAsset(), "[Hot Reload] Reloaded %s (%s, %d ms)", ok[0]->path.c_str(), ok[0]->kind.c_str(), static_cast<int>(std::lround(ok[0]->ms)));
        return;
    }
    std::map<std::string, size_t> byKind;
    for (const Record* record : ok) byKind[record->kind]++;
    std::vector<std::pair<std::string, size_t>> kinds(byKind.begin(), byKind.end());
    std::stable_sort(kinds.begin(), kinds.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string text;
    for (const auto& [kind, count] : kinds) text += (text.empty() ? "" : ", ") + reloadCountOf(count, kind);
    JE_LOG_INFO(logCategoryAsset(), "[Hot Reload] Reloaded %zu files: %s", ok.size(), text.c_str());
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
    Json out = Json::array();
    for (const LateProblem& late : lateProblems) {
        watch::PathStat now;
        const bool exists = project && watch::statPath(project->getProjectPath() / watch::pathFromUtf8(late.problem.value("path", std::string())), now, false);
        if (exists == late.existed && (!exists || now.sameContent(late.stat))) out.push_back(late.problem);
    }
    lateProblems.clear();
    return out;
}

Json HotReloadService::takeLateScriptProperties() {
    Json out = Json::array();
    for (const Json& item : lateScriptProperties) out.push_back(item);
    lateScriptProperties.clear();
    return out;
}

void HotReloadService::addLateProblem(const Json& problem) {
    LateProblem late;
    late.problem = problem;
    if (project) late.existed = watch::statPath(project->getProjectPath() / watch::pathFromUtf8(problem.value("path", std::string())), late.stat, false);
    // no MCP client takes them: the oldest go
    if (lateProblems.size() >= RELOAD_LATE_MAX) lateProblems.pop_front();
    lateProblems.push_back(std::move(late));
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
            JE_LOG_ERROR(logCategoryAsset(), "[Hot Reload] %s", error.c_str());
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
