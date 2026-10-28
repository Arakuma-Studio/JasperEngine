// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Which of the project's generated normal maps are stale (their source image changed since, or
// is gone), for the Project window's badge and the Inspector, and the opt-in auto-regenerate
// (SpriteNormalsWindowSettings::autoRegenerate): a stale map is generated again from its recipe
// and written over itself without an undo step (like an FBX reimport), logged in the Console.
//
// The index is every .png of the project carrying a jasper.spritenormals recipe, built off the
// main thread (a detached thread that is cancelled, never waited for) when the project opens and re-scanned at most
// every 5 s (a project without a generated map: only when invalidate() says one may have appeared,
// or every 10 minutes), incrementally: an unchanged PNG (mtime and size) is not read again, an unchanged
// source is not hashed again. Owned and ticked by the App (next to Project::refreshFbxImports);
// paused while a scene plays.

#include "spritenormals/SpriteNormalsSession.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <condition_variable>
#include <mutex>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class SpriteNormalsWatcher {
        friend struct SpriteNormalsWatcherTestAccess;

    public:
        explicit SpriteNormalsWatcher(Project* project);
        ~SpriteNormalsWatcher();

        // The editor's watcher (the App's), for the Project window, the Inspector and the window's
        // writes; null when there is none (tools, most tests)
        static SpriteNormalsWatcher* active();
        static void setActive(SpriteNormalsWatcher* watcher);

        // Main thread, once a frame: collects a finished scan, starts the next one when due,
        // runs auto-regeneration
        void tick();
        // A project switch: a running regeneration is cancelled, the index forgotten (never waits)
        void reset();
        // Editor shutdown: reset, then a running scan waited for at most `timeoutSeconds`
        void shutdown(double timeoutSeconds);

        // a generated output (absolute path) whose source changed since it was made
        bool isStale(const std::filesystem::path& output) const;
        // ... whose source is gone
        bool sourceMissing(const std::filesystem::path& output) const;
        // the generated outputs made from `source` (absolute paths)
        std::vector<std::filesystem::path> outputsOf(const std::filesystem::path& source) const;
        // the recorded source (project-relative) and role of a generated output; false when the
        // index does not know it as one
        bool generatedInfo(const std::filesystem::path& output, std::string& source, std::string& role) const;
        // A file was written or changed: the next scan reads it again, soon (also waking a project
        // whose last walk found no generated map, even when a scan is running meanwhile)
        void invalidate(const std::filesystem::path& path);
        // whether the next walk would read `path` (a .png inside the project, not in a hidden or
        // build directory: the scan's own rules)
        bool wouldIndex(const std::filesystem::path& path) const;
        // The Project window shows a generated PNG: a dormant watcher that has not indexed it walks
        // the project again, once per path (a path the walk never reaches can't wake it again)
        void noticeGenerated(const std::filesystem::path& path);
        // whether the last walk found no generated map: no walk until invalidate (or the slow
        // fallback)
        bool dormant() const { return idleScans > 0; }
        // grows whenever an output's staleness (or the set of outputs) changes
        uint64_t revision() const { return rev; }

    private:
        struct Output {
            std::string role;                     // "normal", "atlas", "height"
            std::string sourcePath;               // as the recipe records it
            std::filesystem::path sourceAbsolute;
            std::string recipeSha1;
            std::filesystem::file_time_type time{};
            uintmax_t size = 0;
            bool stale = false;
            bool missing = false;
        };
        struct Hash {
            std::filesystem::file_time_type time{};
            uintmax_t size = 0;
            std::string sha1;
        };
        struct ScanResult {
            std::map<std::filesystem::path, Output> outputs;
            std::map<std::filesystem::path, Hash> hashes;
        };
        // a scan on its own detached thread: dropped (cancelled) without waiting for it
        struct ScanJob {
            std::atomic<bool> cancel{false};
            std::atomic<bool> done{false};
            ScanResult result;
            std::mutex mutex;
            std::condition_variable finished;
        };

        Project* project;
        std::filesystem::path scannedProject;
        std::map<std::filesystem::path, Output> outputs;
        std::map<std::filesystem::path, Hash> hashes;
        std::shared_ptr<ScanJob> scan;
        double lastScan = -1.0;
        double scanInterval = 5.0;                // seconds between scans
        int idleScans = 0;                        // scans in a row that found no generated map (dormant then)
        double dormantInterval = 600.0;           // seconds between the walks of a dormant project
        double settleSeconds = 1.0;               // a source changed this recently waits
        uint64_t rev = 0;
        std::shared_ptr<std::atomic<int>> hashCount = std::make_shared<std::atomic<int>>(0);
        bool pausedForPlay = false;
        bool failNextScan = false;                // tests: the next scan throws on its thread
        bool wakePending = false;                 // invalidate() while a scan ran: its result wakes
        std::set<std::filesystem::path> noticed;  // the paths noticeGenerated woke the watcher for

        // auto-regenerate: one at a time, a change during a run re-queues
        std::deque<std::filesystem::path> queue;
        std::shared_ptr<spritenormals::SpriteNormalsRun> regen;
        std::filesystem::path regenOutput;
        std::string lastLog;                      // the last Console line (tests)

        double clock() const;
        double interval() const;
        void startScan();
        void collectScan();
        void queueStale();
        void tickRegeneration();
        bool autoRegenerate() const;
    };

}
