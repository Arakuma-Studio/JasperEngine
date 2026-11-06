// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "McpCall.h"
#include "McpJson.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

    class McpContext;

    // Automatic pick-up of files the agent edits on disk with its own tools (Read/Edit/git...).
    //
    // Before every top-level tools/call it finds the project's Lua, C++, shader, .material,
    // HTML UI (.html/.htm/.css) and asset (images, .svg, .ttf/.otf, .json/.lottie) files that
    // changed since the last call (not .scene / .prefab / project.yaml, nor the terrain maps the
    // terrain tools write: the editor owns those): from the file watcher's journal after a
    // flush (S05 R29, `source: "watcher"`), or, when the watcher is off, degraded, fell behind
    // or its flush timed out, from a cheap stat-based scan (`source: "scan"`). If any changed,
    // the sync_files pick-up (HotReloadService::apply) runs on them (code editor buffers,
    // script properties, Lua syntax, materials, shader forks, textures, models, audio, fonts,
    // HTML UI documents, asset database), the call waits (at most 10 s) for the reloads it
    // started on workers, and the call's result gets
    //   external_changes: {changed, added?, removed?, source, reloaded: [{path, kind, ms}],
    //                      pending: [path],
    //                      problems: [{path, line, message} (+ col, severity, code, suggestion for UI)],
    //                      script_properties?, ui_reloaded?, ui_current?, deferred?}
    // plus one text line. Shader forks using a changed shader file are compiled before the
    // tool runs, so their errors are in `problems` too; a reload that failed is a problem (the
    // previous version is kept). A change the watcher reloaded before the call is reported
    // once and reloaded once (the hot reload's applied stamps).
    //
    // Files the MCP tools write themselves are not external: syncFiles (the funnel of
    // write_file, create_script, the material and asset tools) restamps what it handled, and
    // after each call that may write files a "settle" takes every change it finds as the tool's
    // own, already handled by it (fork_shader, screenshots saved into the project, terrain
    // maps...): absorbed, not picked up again. It reads the journal behind a barrier, or scans
    // where the barrier is not a mere drain. The server sends that call's result only after its
    // settle (McpServer::runSettles), so whatever the client writes once it has the result is
    // newer than the settle and reported by the next call, at any delay and with any stamp.
    // Changes stamped before the call started stay external and are reported by the next call.
    // A program writing while a call runs, with a stamp from inside the call, can't be told from
    // the tool (that would need every editor write path to report its writes): absorbed.
    //
    // A file or folder stamp read less than 2 s after its mtime is "racy": a write in the same
    // filesystem clock tick could leave it equal. Racy files are verified by a hash of their
    // content on every scan and pick-up (the journal can't tell either: the watcher compares the
    // same stat), racy folders listed again by the stat scan (a new file is in the journal),
    // until a scan reads them 2 s after their mtime.
    //
    // While a scene plays, the editor does not re-parse scripts (Play restores the scene from a
    // snapshot when it stops), so changes are listed with `deferred` (Lua syntax still checked)
    // and picked up on the first call after Play stops. HTML UI files are not deferred: the
    // running and the edit-time documents reload at once (changed images reach the documents
    // at once and the scene textures after Play).
    //
    // Large projects: hot files (changed this session), folders and code files (HTML UI
    // included) are checked whole on every scan (up to 2000 each, beyond that they rotate too);
    // assets (images, fonts, json/lottie) rotate within a time budget and the next scan resumes
    // where the last one stopped.
    //
    // Main thread only.
    class McpAutoSync {
    public:
        static McpAutoSync& get();

        // When the MCP server starts (before the startup project loads): files of the
        // `--project` project changed after this, while it was loading, are reported by the
        // first call instead of being taken into the baseline unseen.
        void serverStarted();

        // Called every editor frame and before syncFiles: takes the baseline when the open
        // project changes, so edits made before the first call are still found.
        void prepare(McpContext& ctx);

        // Before a top-level tools/call. Calls `run(report)` (report: external_changes, or
        // null when nothing changed) now, or once the shader forks touched by the changes are
        // compiled (the call waits for them through call->waitUntil).
        void beforeCall(McpContext& ctx, const CallPtr& call, const std::string& tool,
                        std::function<void(Json report)> run);

        // When a top-level call has completed, before its result is sent (main thread, never
        // from inside a handler). Takes what changed since the pick-up before it as the tool's
        // own, except changes stamped before `start` (when its handler started).
        void afterCall(McpContext& ctx, const std::string& tool, std::chrono::system_clock::time_point start);

        // A call that may write files starts its handler (McpServer; afterCall ends it): while one
        // runs, the hot reload holds the watcher's batches, so the call's writes reload once,
        // with its sync (S05 R30)
        // (a call that never completes stops holding it after 10 minutes)
        void callStarted() { writingCalls++; lastCallStart = std::chrono::steady_clock::now(); }
        bool writingCallInFlight() const {
            return writingCalls > 0 && std::chrono::steady_clock::now() - lastCallStart < std::chrono::minutes(10);
        }


        // syncFiles handled these absolute paths (a tool wrote them, or sync_files ran): they
        // are current. `full`: the whole project was rescanned and picked up.
        void filesSynced(McpContext& ctx, const std::vector<std::filesystem::path>& paths, bool full);

        // With Auto Refresh off: the external changes the journal held when this call started
        // (its pick-up applied them since; editor_status counts them as pending)
        size_t heldAtCallStart() const { return heldAtCall; }

        // Adds `report` to a tools/call result (structuredContent.external_changes + one text item)
        static void attach(Json& result, const Json& report);

        // {source, files, dirs, checked, complete, scan_ms, settle_ms, scan_cpu_ms, settle_cpu_ms} (editor_status)
        Json stats() const;

        // Files the scan tracks: Lua, C++, shaders, .material, .particles, a graph kind's files, HTML UI, assets and pose files
        // (.poseanim, .humanoid), by extension
        static bool isTrackedFile(const std::string& path);
        // .html/.htm/.css: the code tier (checked whole every scan)
        static bool isUiCodeFile(const std::string& path);
        // images (.svg included), .ttf/.otf, .json/.lottie: the rotating assets tier
        static bool isAssetFile(const std::string& path);
        // Folders the scan skips: hidden (.jasperengine, .git...), build, node_modules, trash
        static bool isSkippedDirName(const std::string& name);
        // Files the editor writes from its own state (terrain_maps/terrain_edit_*.png): not tracked
        static bool isEditorOwnedFile(const std::string& rel);
        // isTrackedFile and not editor-owned
        static bool tracks(const std::string& rel);

    private:
        McpAutoSync() = default;

        struct Stamp {
            int64_t mtime = 0;     // ns since the Unix epoch
            uint64_t size = 0;
            bool racy = false;     // read less than 2 s after its mtime: verified by `hash`
            uint64_t hash = 0;     // of the content, while racy (0: not read yet)
        };
        struct DirStamp {
            int64_t mtime = 0;
            bool racy = false;     // read less than 2 s after its mtime: the scan lists it again
        };
        using FileMap = std::map<std::string, Stamp>;   // project-relative generic path -> stamp
        struct Change {
            std::string rel;
            char kind = 'm';       // a(dded), m(odified), r(emoved)
            int64_t when = 0;      // mtime, or (removed) the mtime of the nearest existing folder
            bool report = true;    // false: a tool's own write, picked up silently
            uint64_t sequence = 0; // the file watcher's journal change (0: found by a scan)
        };
        struct Pending {
            char kind = 'm';
            bool report = true;
            uint64_t sequence = 0;
        };

        int writingCalls = 0;                      // callStarted() without afterCall yet
        uint64_t journalCursor = 1;                // the file watcher's journal, next to read
        std::string lastSource = "scan";           // what the last pick-up read: watcher or scan
        size_t heldAtCall = 0;
        std::chrono::steady_clock::time_point lastCallStart{};
        std::filesystem::path root;                // project folder of the baseline; empty = none
        int64_t serverStartNs = 0;                 // serverStarted(), ns since the Unix epoch
        bool startupBaselineTaken = false;
        FileMap code;                              // Lua, C++, shaders, materials, .html/.htm/.css
        FileMap assets;                            // images, fonts, .json/.lottie
        std::map<std::string, DirStamp> dirs;      // project-relative folder ("" = root) -> stamp
        std::map<std::string, std::filesystem::path> linkedFolders;   // the links entered -> where each leads
        bool entersFolder(const std::filesystem::directory_entry& entry, const std::string& rel);
        std::vector<std::string> hot;              // recently changed files, newest last
        // The racy files, by mtime: what a scan or a pick-up verifies (entries go stale, verifyRacy drops them)
        std::set<std::pair<int64_t, std::string>> racyFiles;
        std::string dirCursor, codeCursor, assetCursor;

        // Found by a settle scan but older than the call: reported by the next call
        std::map<std::string, Pending> pendingExternal;
        // Changes seen during Play: picked up once Play stops
        std::map<std::string, Pending> deferred;

        // Last scans: wall and thread CPU time, entries checked (stats for editor_status)
        double lastScanMs = 0.0, lastScanCpuMs = 0.0;
        double lastSettleMs = 0.0, lastSettleCpuMs = 0.0;
        size_t scanChecked = 0, lastScanChecked = 0;
        bool lastComplete = true;

        // False when there is no project; rebased = a new baseline was just taken
        bool ensureRoot(McpContext& ctx, bool* rebased = nullptr);
        void baseline(const std::filesystem::path& projectRoot);
        void walkDir(const std::string& rel, std::vector<Change>* changes, int depth);
        void scan(std::vector<Change>& changes, double budgetMs, size_t minAssets, bool& complete);
        void checkDir(const std::string& rel, std::vector<Change>& changes);
        void checkFile(const std::string& rel, std::vector<Change>& changes);
        void removeTree(const std::string& relDir, std::vector<Change>* changes);
        void markHot(const std::string& rel);
        int64_t nearestDirTime(const std::string& rel) const;
        std::string absolute(const std::string& rel) const;
        FileMap& mapFor(const std::string& rel);
        bool insideSkippedDir(const std::string& rel) const;
        void restamp(const std::string& rel);
        // The stamp of a file just stat'ed at `readNs` (with its hash when racy)
        void record(const std::string& rel, int64_t mtime, uint64_t size, int64_t readNs);
        // Racy files whose stat did not move: another content is a modification. Within a time
        // budget, newest first (at least MIN_ITEMS).
        void verifyRacy(std::vector<Change>& changes, double budgetMs);

        static void merge(std::map<std::string, Pending>& into, const std::string& rel, char kind, bool report, uint64_t sequence = 0);
        // syncFiles' problems as reported ({path, line?, message} + the UI keys when present)
        static Json compactProblems(const Json& problems);
        // ui_reloaded (when not empty) and ui_current (when > 0)
        static void addUiReport(Json& report, const Json& reloaded, int current);
        // Runs the sync_files pick-up on the changes; the report (null if none is reportable)
        Json process(McpContext& ctx, const std::vector<Change>& changes);
        // The file watcher's changes since the last read (its journal, after a flush); false
        // when it does not run, is degraded, fell behind or the flush timed out (the scan then).
        // `settling`: for a call's settle, which also scans where the flush is not a mere drain.
        bool takeJournal(std::vector<Change>& found, bool settling);
    };

}
