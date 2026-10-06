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
    // Before every top-level tools/call, a cheap stat-based scan finds the project's Lua, C++,
    // shader, .material and image files that changed since the last scan (not .scene / .prefab /
    // project.yaml, nor the terrain maps the terrain tools write: the editor owns those). If
    // any did, the sync_files pick-up runs on them (code editor buffers, script properties, Lua
    // syntax, materials, shader forks, textures, asset database) and the call's result gets
    //   external_changes: {changed, added?, removed?, problems: [{path, line, message}],
    //                      script_properties?, deferred?}
    // plus one text line. Shader forks using a changed shader file are compiled before the
    // tool runs, so their errors are in `problems` too.
    //
    // Files the MCP tools write themselves are not external: syncFiles (the funnel of
    // write_file, create_script, the material and asset tools) restamps what it handled, and
    // after each call that may write files a "settle" scan takes every change stamped inside
    // the call's time window as the tool's own, already handled by it (fork_shader,
    // screenshots saved into the project, terrain maps...): absorbed, not picked up again.
    // Changes older than the call stay external and are reported by the next call.
    //
    // While a scene plays, the editor does not re-parse scripts (Play restores the scene from a
    // snapshot when it stops), so changes are listed with `deferred` (Lua syntax still checked)
    // and picked up on the first call after Play stops.
    //
    // Large projects: hot files (changed this session), folders and code files are checked
    // whole on every scan (up to 2000 each, beyond that they rotate too); images rotate
    // within a time budget and the next scan resumes where the last one stopped.
    //
    // Main thread only.
    class McpAutoSync {
    public:
        static McpAutoSync& get();

        // Every editor frame: (re)takes the baseline when the open project changes, so edits
        // made before the first call are still found.
        void tick(McpContext& ctx);

        // When the MCP server starts (before the startup project loads): files of the
        // `--project` project changed after this, while it was loading, are reported by the
        // first call instead of being taken into the baseline unseen.
        void serverStarted();

        // Takes the baseline now if the project changed since the last frame (syncFiles, before
        // it compares the project with its snapshot)
        void prepare(McpContext& ctx);

        // Before a top-level tools/call. Calls `run(report)` (report: external_changes, or
        // null when nothing changed) now, or once the shader forks touched by the changes are
        // compiled (the call waits for them through call->waitUntil).
        void beforeCall(McpContext& ctx, const CallPtr& call, const std::string& tool,
                        std::function<void(Json report)> run);

        // When a top-level call has completed (post it to the main thread: never from inside
        // a handler). Takes the changes stamped inside [start, end] as the tool's own.
        void afterCall(McpContext& ctx, const std::string& tool,
                       std::chrono::system_clock::time_point start, std::chrono::system_clock::time_point end);

        // Whether a call may write project files, so it needs afterCall: read-only tools don't,
        // except where they save a file on request (screenshot save_path)
        static bool mayWriteFiles(bool readOnly, const Json& args);

        // syncFiles handled these absolute paths (a tool wrote them, or sync_files ran): they
        // are current. `full`: the whole project was rescanned and picked up.
        void filesSynced(McpContext& ctx, const std::vector<std::filesystem::path>& paths, bool full);

        // Adds `report` to a tools/call result (structuredContent.external_changes + one text item)
        static void attach(Json& result, const Json& report);

        // {files, dirs, checked, complete, scan_ms, settle_ms, scan_cpu_ms, settle_cpu_ms} (editor_status)
        Json stats() const;

        // Files the scan tracks: Lua, C++, shaders, .material, images (by extension)
        static bool isTrackedFile(const std::string& path);
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
        };
        using FileMap = std::map<std::string, Stamp>;   // project-relative generic path -> stamp
        struct Change {
            std::string rel;
            char kind = 'm';       // a(dded), m(odified), r(emoved)
            int64_t when = 0;      // mtime, or (removed) the mtime of the nearest existing folder
            bool report = true;    // false: a tool's own write, picked up silently
        };
        struct Pending {
            char kind = 'm';
            bool report = true;
        };

        std::filesystem::path root;                // project folder of the baseline; empty = none
        int64_t serverStartNs = 0;                 // serverStarted(), ns since the Unix epoch
        bool startupBaselineTaken = false;
        FileMap code;                              // Lua, C++, shaders, materials
        FileMap images;
        std::map<std::string, int64_t> dirs;       // project-relative folder ("" = root) -> mtime
        std::vector<std::string> hot;              // recently changed files, newest last
        std::string dirCursor, codeCursor, imageCursor;

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
        void scan(std::vector<Change>& changes, double budgetMs, size_t minImages, bool& complete);
        void checkDir(const std::string& rel, std::vector<Change>& changes);
        void checkFile(const std::string& rel, std::vector<Change>& changes);
        void removeTree(const std::string& relDir, std::vector<Change>* changes);
        void markHot(const std::string& rel);
        int64_t nearestDirTime(const std::string& rel) const;
        std::string absolute(const std::string& rel) const;
        FileMap& mapFor(const std::string& rel);
        bool insideSkippedDir(const std::string& rel) const;
        void restamp(const std::string& rel);

        static void merge(std::map<std::string, Pending>& into, const std::string& rel, char kind, bool report);
        // Runs the sync_files pick-up on the changes; the report (null if none is reportable)
        Json process(McpContext& ctx, const std::vector<Change>& changes, std::vector<std::string>& shaderFiles);
    };

}
