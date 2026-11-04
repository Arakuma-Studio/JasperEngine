// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "io/watch/FileWatchService.h"
#include "jobs/JobSystem.h"

#include "mcp/McpJson.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;
    class EditorHost;
    using mcp::Json;

    // What one apply() handles: settled changes (the watcher's, or paths a tool names)
    struct ChangeSet {
        std::vector<FileChange> changes;

        // syncFiles' form: paths that changed somehow; the disk says how (there: Modified,
        // gone: Removed). Origin External.
        static ChangeSet fromPaths(const std::vector<std::filesystem::path>& paths);
        bool empty() const { return changes.empty(); }
    };

    struct ApplyOptions {
        bool full = false;            // the whole project (sync_files full, a journal overflow)
        bool wait = false;            // the caller waits for the reloads (sync_files wait)
        bool rebuildModels = false;   // a model whose hierarchy changed is rebuilt (one undo step)
        bool fromMcp = false;         // an MCP call's pick-up (sync_files, external_changes)
        bool fromWatcher = false;     // delivered by the file watcher
        bool force = false;           // reload even what is current (sync_files paths)
        bool quiet = false;           // no Console line (the code editor's own save)
    };

    // What happened (the keys syncFiles returned, plus reloaded/pending). Paths are
    // project-relative.
    struct ApplyResult {
        Json added = Json::array(), modified = Json::array(), removed = Json::array();
        Json reloaded = Json::array();          // [{path, kind, ms}] done (or already current)
        Json pending = Json::array();           // [path] reloading on a worker
        Json problems = Json::array();
        Json notes = Json::array();
        Json scriptProperties = Json::array();
        Json uiReloaded = Json::array();
        Json codeEditor = {{"reloaded", Json::array()}, {"conflicts", Json::array()}};
        int uiCurrent = 0;
        int texturesReloaded = 0;
        bool shadersInvalidated = false;
        std::vector<std::filesystem::path> handled;   // absolute paths apply() took (McpAutoSync)
    };

    // The editor's hot reload (S05 R11-R15): one funnel for the file watcher, sync_files,
    // McpAutoSync and Assets > Refresh. Textures, models, materials, shaders, audio and fonts
    // decode on a worker and are adopted on the main thread (the old one stays bound until
    // then, and on failure); scripts, HTML UI documents, prefabs, the asset database and the
    // Project window follow at once. Open scenes, prefabs and project.yaml are never reloaded
    // on their own: the Scene view offers Reload / Keep Mine.
    //
    // Main thread only.
    class HotReloadService {
    public:
        static HotReloadService& get();

        // Project open (Project::loadProject and the other places a project root is set):
        // starts the file watcher on its folder with the Asset Pipeline preferences (or the
        // --file-watcher / JASPER_FILE_WATCHER choice) and attaches
        void startWatching(Project& project);
        // Before a project switch, and at exit: detaches and stops the watcher
        void stopWatching();
        // --file-watcher=auto|native|polling|off (or JASPER_FILE_WATCHER): wins over the
        // preference for this run (R33)
        static void setWatcherOverride(WatchBackendKind kind);
        static bool getWatcherOverride(WatchBackendKind& kind);

        // Subscribes to the file watcher (FileWatchService::start comes first)
        void attach(Project& project);
        // Before a project switch: in-flight reloads are dropped
        void detach();
        Project* getProject() const { return project; }

        ApplyResult apply(const ChangeSet& changes, const ApplyOptions& options);

        // Every editor loop iteration: adopts finished reloads within the frame budget (2 ms,
        // at most 16 MiB of texels), runs retries, writes the Console lines. True when it
        // adopted something (the views redraw).
        bool update();

        // `path` (absolute) is not what was last reloaded from it, whatever its stat says: it holds
        // other content under the same stat (McpAutoSync's verification of a racy stamp)
        void forgetApplied(const std::filesystem::path& path);

        // Nothing of `paths` (absolute) is still reloading
        bool idleFor(const std::vector<std::filesystem::path>& paths) const;
        bool idle() const;
        size_t reloadingCount() const;

        // While an MCP call that may write files runs (R30): watcher batches wait, then go
        // through apply once (what the call's own sync handled is current by then)
        void setPaused(bool paused);
        bool isPaused() const { return paused; }

        // Auto Refresh (R15): off, external changes only reach the journal
        void setAutoRefresh(bool on);
        bool getAutoRefresh() const { return autoRefresh; }
        void setLogReloads(bool on) { logReloads = on; }
        // External changes the journal holds that were not applied (status bar, editor_status)
        size_t pendingExternalChanges() const;
        // Assets > Refresh: applies what the journal holds since the last apply, or a full
        // pass when the journal overflowed
        void refresh();

        // One reload, as editor_status and external_changes report it
        struct Record {
            uint64_t serial = 0;
            std::string path;        // project-relative
            std::string kind;        // texture, model, material, shader, audio, font, script, ui, prefab
            double ms = 0.0;
            bool ok = true;
            std::string error;
            int attempts = 1;        // 2: it failed once and was tried again
        };
        uint64_t lastSerial() const { return nextSerial - 1; }
        std::vector<Record> recordsSince(uint64_t serial) const;
        // The last record of a path (false: never reloaded)
        bool lastRecordOf(const std::string& relativePath, Record& out) const;
        uint64_t reloadCount() const { return reloads; }
        Json lastReloadJson() const;

        // Open documents changed on disk by another program (R11): the Scene view's bar
        bool hasExternalChange(uint32_t sceneId) const;
        // project.yaml changed on disk
        bool hasProjectFileChange() const { return projectFileChanged; }
        // Reload: the scene (or prefab stage) is read again from its file; asks first when it
        // has unsaved changes. Not an undo step.
        void reloadFromDisk(uint32_t sceneId);
        void keepMine(uint32_t sceneId);
        void reloadProjectFile();
        void keepMineProjectFile() { projectFileChanged = false; }

        // S23 registers this (ScriptReloadService): a script changed while a scene plays; true
        // when it is reloaded during Play. Unset (or false), the change waits for Play to stop.
        void setScriptChangedHook(std::function<bool(const std::filesystem::path&)> hook) { scriptChangedHook = std::move(hook); }

        // A reload kind's caches (thumbnails, glyph atlases, S14's cooked textures) follow a
        // path through these events (D6)
        enum class Event : uint8_t { Reloaded = 1, Dropped = 2, Content = 4 };
        using EventFn = std::function<void(const std::filesystem::path& path, const std::string& kind, Event event)>;
        uint64_t addInvalidator(uint8_t events, EventFn fn);
        void removeInvalidator(uint64_t id);

        // ------------------------------------------------------------------ for the handlers
        // (editor/reload/handlers/*.cpp)

        // Starts a reload on a worker. `work` decodes and returns the step that adopts it on
        // the main thread (false + error: it failed, the old one stays; true + error: it was
        // left out on purpose, a warning, no retry); a throw is a failure. A failure is retried
        // once after 500 ms (a file still being written).
        using AdoptFn = std::function<bool(std::string& error)>;
        using WorkFn = std::function<AdoptFn()>;
        void startJob(const std::string& kind, const std::filesystem::path& path, WorkFn work, uint64_t texelBytes = 0);
        // A reload done on the main thread now (scripts, UI, prefabs)
        void recordDone(const std::string& kind, const std::filesystem::path& path, double ms, bool ok = true, const std::string& error = {});
        void notify(const std::filesystem::path& path, const std::string& kind, Event event);
        // A script changed while a scene plays: S23's hook took it (true), or not
        bool handleScriptInPlay(const std::filesystem::path& path) { return scriptChangedHook && scriptChangedHook(path); }
        // Scripts whose properties wait for Play to stop
        void deferScripts(const std::vector<std::filesystem::path>& paths);
        // The asset database looks at the project again once Play stops
        void refreshAssetsAfterPlay() { assetRefreshAfterPlay = true; }
        std::string relative(const std::filesystem::path& path) const;
        // Problems reported after apply() returned (failed reloads): McpAutoSync collects them.
        // A kind that reports its own (shaders: {path, line, message}) adds them here.
        Json takeLateProblems();
        void addLateProblem(const Json& problem);
        // The script property changes the watcher's own reloads of external edits found (one
        // {scene, entity, script, added, removed} each): the next sync that picks the edit up
        // reports them, as the call would have had it reloaded the script itself
        Json takeLateScriptProperties();
        // What the watcher's own reloads of external changes reported (R29): the next MCP
        // call's pick-up carries it. Documents reloaded at or after `since` (the call's flush)
        // are the call's ui_reloaded, earlier ones its ui_current.
        struct WatcherUi {
            Json reloaded = Json::array();
            int current = 0;
        };
        WatcherUi takeWatcherUi(std::chrono::steady_clock::time_point since);

    private:
        HotReloadService() = default;

        struct Job {
            uint64_t batch = 0;
            std::string kind;
            std::filesystem::path path;
            WorkFn work;
            jobs::Task<AdoptFn> task;
            std::chrono::steady_clock::time_point started{};
            std::chrono::steady_clock::time_point retryAt{};
            bool retried = false;
            bool waitingRetry = false;
            uint64_t texelBytes = 0;
        };
        struct Batch {
            bool log = true;
            size_t outstanding = 0;
            std::vector<Record> done;
        };

        Project* project = nullptr;
        FileWatchSubscription subscription;
        bool paused = false;
        bool autoRefresh = true;
        bool logReloads = true;
        std::vector<FileChange> held;          // batches that came while paused
        uint64_t journalCursor = 1;            // the next journal sequence apply() has not seen

        std::vector<std::shared_ptr<Job>> jobs;
        std::vector<jobs::Task<AdoptFn>> superseded;   // replaced by a newer reload of their file
        std::map<uint64_t, Batch> batches;
        uint64_t currentBatch = 0;
        uint64_t nextBatch = 1;

        std::deque<Record> records;
        uint64_t nextSerial = 1;
        uint64_t reloads = 0;

        // what apply() last reloaded each path from (the shared applied-stamp table, R29): the
        // stat, whether it was racy (watch::isRacyStamp), and the last journal change applied
        struct Applied {
            watch::PathStat stat;
            bool racy = false;
            uint64_t contentHash = 0;   // racy only: what the bytes were (a same-tick save differs)
            uint64_t sequence = 0;
        };
        std::map<std::string, Applied> applied;

        std::set<uint32_t> externallyChangedScenes;
        bool projectFileChanged = false;
        std::vector<std::filesystem::path> deferredScripts;   // Lua changed during Play
        bool assetRefreshAfterPlay = false;

        std::function<bool(const std::filesystem::path&)> scriptChangedHook;
        std::map<uint64_t, std::pair<uint8_t, EventFn>> invalidators;
        uint64_t nextInvalidator = 1;
        // with what the file was when the problem was found: one that changed since (fixed,
        // restored) is not reported (its own reload reports what it finds)
        struct LateProblem {
            Json problem;
            bool existed = false;
            watch::PathStat stat;
        };
        std::deque<LateProblem> lateProblems;
        std::deque<Json> lateScriptProperties;
        struct LateUi {
            std::chrono::steady_clock::time_point at;
            Json reloaded;
        };
        std::deque<LateUi> lateUi;

        void onWatcherBatch(std::span<const FileChange> batch);
        static jobs::Task<AdoptFn> launch(WorkFn work);
        // Reloaded already: the same journal change (the watcher's delivery, then a call's
        // pick-up of it), or a file whose stat is the one reloaded from, unless that stamp was
        // racy and another program made the change
        bool isCurrent(const FileChange& change) const;
        // `seen`: the stat taken before the handlers read the file (nullptr: gone). A save that
        // lands while they read leaves the file different from the stamp, so its own change
        // reloads (stamping after the read took it as reloaded)
        struct Seen {
            watch::PathStat stat;
            uint64_t contentHash = 0;   // racy stats only
        };
        void stamp(const std::filesystem::path& path, const Seen* seen, uint64_t sequence);
        void finishJob(Job& job, bool ok, const std::string& error, double ms, bool warning = false);
        void closeBatchIfDone(uint64_t id);
        void writeBatchLine(const Batch& batch);
        void pushRecord(Record record);
    };

    // RAII over a HotReloadService event set (OpenEngine's AssetReloadInvalidator, D6)
    class ReloadInvalidator {
    public:
        ReloadInvalidator() = default;
        ReloadInvalidator(uint8_t events, HotReloadService::EventFn fn);
        ~ReloadInvalidator();
        ReloadInvalidator(ReloadInvalidator&& other) noexcept;
        ReloadInvalidator& operator=(ReloadInvalidator&& other) noexcept;
        ReloadInvalidator(const ReloadInvalidator&) = delete;
        ReloadInvalidator& operator=(const ReloadInvalidator&) = delete;

    private:
        uint64_t id = 0;
    };

}
