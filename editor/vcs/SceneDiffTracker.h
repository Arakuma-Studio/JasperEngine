// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The baseline memo follows OpenEngine's SceneDiffVcs (Apps/Editor/Source/VersionControl/
// SceneDiffVcs.cpp, MIT, Copyright (c) 2026 Gamecrafters Guild Limited and contributors;
// NOTICE.md), keyed on the status poll's (path, HEAD oid, blob oid) instead of size and mtime.

#pragma once

// The scene diffs the editor shows (docs/specs/S29-version-control.md R15): for each open scene
// or prefab stage a consumer watches (the Scene Diff window, the Hierarchy's "Show VCS Changes",
// the Inspector's markers), its semantic diff against the baseline. The current side is the open
// document, so unsaved edits show: encoded on the main thread 500 ms after the last history
// change, then parsed and diffed on an S06 background job. An encode over 10 ms falls back to the
// file on disk ("Showing saved changes"). The baseline is fetched off the main thread and kept
// per (path, HEAD oid, blob oid, revision).

#include "vcs/VcsService.h"
#include "vcs/scene/SceneDiff.h"

#include "jobs/JobSystem.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::vcs {

    struct SceneDiffView {
        uint32_t sceneId = 0;
        std::string rev = "HEAD";
        std::filesystem::path file;          // absolute
        RepoPath key;
        bool fromDisk = false;               // the saved file was diffed ("Showing saved changes")
        bool pending = true;                 // nothing diffed yet, or a new diff is on its way
        std::string error;
        double encodeMs = 0.0;               // the last main-thread encode
        uint64_t version = 0;                // grows with each new diff
        std::shared_ptr<const scene::SceneYamlModel> baseline;
        std::shared_ptr<const scene::SceneDiffResult> diff;
        // the diff's objects by id (overlays and the Inspector look GameObjects up per row)
        std::unordered_map<uint64_t, const scene::ObjectDiff*> byId;

        const scene::ObjectDiff* object(uint64_t id) const;
    };

    class SceneDiffTracker {
    public:
        // R26: over this, the current side is the file on disk
        static constexpr double kEncodeBudgetMs = 10.0;
        static constexpr std::chrono::milliseconds kDebounce{500};

        SceneDiffTracker() = default;
        ~SceneDiffTracker();
        SceneDiffTracker(const SceneDiffTracker&) = delete;
        SceneDiffTracker& operator=(const SceneDiffTracker&) = delete;

        // A consumer watches a document against a revision (HEAD: the baseline, D5) while it is
        // shown; unwatched ones are dropped at the next update
        void watch(uint32_t sceneId, const std::string& rev = "HEAD");
        // Every open document at HEAD (the Hierarchy's Show VCS Changes)
        void setWatchAllOpen(bool watch) { watchAllOpen = watch; }
        bool isWatchingAllOpen() const { return watchAllOpen; }

        // Main thread, once per frame: debounced encodes, finished jobs adopted, baselines asked
        // for. True when a diff changed.
        bool update(Project& project);

        const SceneDiffView* view(uint32_t sceneId, const std::string& rev = "HEAD") const;

        // A diff now, outside the watched set (diff_scene, a closed file): the open document's
        // unsaved state (fromEditor) or the file on disk, against `rev`; done on the main thread
        struct Request {
            uint32_t sceneId = 0;            // an open document, or
            std::filesystem::path file;      // a file (absolute)
            std::string rev = "HEAD";
            bool fromEditor = true;
            bool changedOnly = true;
        };
        using DiffDone = std::function<void(const std::string& error, std::shared_ptr<const scene::SceneYamlModel> baseline,
                                            std::shared_ptr<const scene::SceneDiffResult> diff)>;
        void diffNow(Project& project, const Request& request, DiffDone done);

        // The baselines are kept by HEAD and blob: a new commit or revision makes new ones
        void clear();
        // Main-thread encodes over the budget, counted (the bench, R26)
        uint64_t slowEncodes() const { return slowEncodeCount; }

    private:
        // filled by its fetch job (under its own mutex: the job may outlive the tracker)
        struct Baseline {
            std::mutex mutex;
            std::shared_ptr<const scene::SceneYamlModel> model;
            std::string error;
            bool ready = false;
        };
        struct Entry {
            SceneDiffView view;
            uint64_t historyChanges = ~uint64_t(0);   // CommandHistory::changeCount() last seen
            bool dirty = true;
            std::chrono::steady_clock::time_point changedAt{};
            bool watched = false;
            std::string baselineKey;
            bool slowEncode = false;         // sticky: this document diffs its saved file
            std::optional<jobs::Task<std::shared_ptr<scene::SceneDiffResult>>> job;
            std::shared_ptr<const scene::SceneYamlModel> jobBaseline;
            bool jobFromDisk = false;
        };

        std::string baselineKeyOf(const VcsService& service, const RepoPath& key, const std::string& rev) const;
        // the baseline of a key, asked for when unknown (null until it arrives)
        std::shared_ptr<Baseline> baselineFor(VcsService& service, const RepoPath& key, const std::string& rev, const std::string& memoKey);
        bool startDiff(Project& project, Entry& entry);

        std::map<std::pair<uint32_t, std::string>, Entry> entries;
        std::map<std::string, std::shared_ptr<Baseline>> baselines;
        bool watchAllOpen = false;
        uint64_t slowEncodeCount = 0;

        struct OneOff {
            Request request;
            DiffDone done;
            std::shared_ptr<Baseline> baseline;
            YAML::Node current;              // the editor's encode (fromEditor)
            std::optional<jobs::Task<std::shared_ptr<scene::SceneDiffResult>>> job;
            std::string error;
        };
        std::vector<std::shared_ptr<OneOff>> oneOffs;
    };

}
