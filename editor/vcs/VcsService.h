// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The project's version control (docs/specs/S29-version-control.md R5-R7, D3): finds the
// repository the project is in, owns its provider, runs the status poller thread and one worker
// for user operations (one VCS process for user operations at a time), and hands the main thread
// the newest snapshot once per frame. Every operation is asynchronous: its callback runs on the
// main thread in update(). Nothing here spawns a process on the main thread (R24).

#include "vcs/IVcsProvider.h"
#include "vcs/VcsStatusPoller.h"

#include "io/watch/FileWatchService.h"
#include "jobs/JobChannel.h"
#include "jobs/Primitives.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace jasperengine::editor::vcs {

    namespace fs = std::filesystem;

    struct VcsSettings {
        ProviderChoice provider = ProviderChoice::Auto;
        fs::path gitPath;                   // Preferences: empty finds it on PATH (R3)
        fs::path svnPath;
        int pollSeconds = 10;               // 2-120 (R5)
        int lockPollSeconds = 60;           // 0 off, 30-600
        int autoFetchMinutes = 0;           // 0 off
        std::string userName;               // svn lock comments
        // tests: set for every child (HOME, the global git config), and sub-second cadences
        std::vector<std::pair<std::string, std::string>> extraEnv;
        std::vector<std::string> svnGlobalArgs;
        // tests: PATH's value for finding the tools (a machine without git)
        bool usePathOverride = false;
        std::string pathOverride;
        int pollMilliseconds = 0;
        int lockPollMilliseconds = 0;

        static constexpr int kMinPollSeconds = 2, kMaxPollSeconds = 120;
        static constexpr int kMinLockPollSeconds = 30, kMaxLockPollSeconds = 600;
        // the values in range (0 keeps a lock poll off)
        void clamp();
    };

    enum class VcsState : uint8_t {
        Off,            // no project, or editor.vcs.provider none
        Detecting,      // looking for the repository and its tool
        NotAvailable,   // no tool (or too old): message says which
        NoRepository,   // the project is in no repository
        Active
    };
    const char* vcsStateName(VcsState state);

    class VcsService {
    public:
        using Done = std::function<void(const VcsResult&)>;

        VcsService();
        ~VcsService();
        VcsService(const VcsService&) = delete;
        VcsService& operator=(const VcsService&) = delete;

        // Looks for the project's repository on the worker; when there is one, the poller starts.
        // watchFiles: the file watcher's batches and the editor's saves wake the poller (R5).
        void open(const fs::path& projectDir, const fs::path& internalDir, VcsSettings settings, bool watchFiles = true);
        // Stops the poller and the worker (the operation in flight is cancelled; its callback runs)
        void close();
        // New settings: a new provider choice or tool path opens again, cadences apply at once
        void applySettings(VcsSettings settings);
        const VcsSettings& getSettings() const { return settings; }

        // Main thread, once per editor frame: takes the newest snapshot and runs the callbacks of
        // finished operations. True when the snapshot changed.
        bool update();

        // ---- state (main thread)
        VcsState state() const { return currentState; }
        bool active() const { return currentState == VcsState::Active; }
        // "Git 2.34.1", "Subversion 1.14.1", or why there is none ("git was not found on PATH")
        const std::string& stateMessage() const { return message; }
        const VcsCaps& caps() const { return capabilities; }
        const fs::path& repoRoot() const { return root; }
        const fs::path& projectDir() const { return project; }
        // The project folder's key below the repository root ("" when it is the root)
        const RepoPath& projectKey() const { return projectKeyValue; }
        const SnapshotPtr& snapshot() const { return current; }
        uint64_t snapshotVersion() const { return currentVersion; }
        // The last failed poll or operation ("" after the next good poll)
        const std::string& lastError() const { return error; }
        // Whether `git lfs locks` (or svn's lock read) last failed: the locks shown may be old
        const std::string& lockError() const;
        // An operation is queued or running (the status bar's progress)
        bool busy() const { return pendingOps.load() > 0; }
        const std::string& busyLabel() const { return runningLabel; }
        // The lock poll is on (Preferences, and a provider that locks)
        bool locksPolled() const;

        // ---- lookups (main thread)
        // The baseline every comparison uses (S29 D5): "HEAD" for git, "BASE" for svn; "" while
        // not active
        std::string baselineRevision() const;
        // The revision a caller's rev means: "HEAD" is the baseline everywhere (svn's own HEAD,
        // the server's newest, would read the network and show other people's commits), so
        // svn reads BASE for it; any other rev is passed on
        std::string effectiveRevision(std::string_view rev) const;
        // The memo key of a file's content at rev: it changes when HEAD moves, the file's blob
        // changes or it is renamed, never per poll
        std::string contentMemoKey(const RepoPath& key, std::string_view rev) const;
        bool toKey(const fs::path& absolute, RepoPath& out) const;
        fs::path toAbsolute(std::string_view key) const { return absoluteOf(root, key); }
        const VcsChange* find(const fs::path& absolute) const;
        // by key (a view computes its folder's key once and appends the names: no path math per row)
        const VcsChange* findKey(std::string_view key) const { return current ? current->find(key) : nullptr; }
        DirState dirStateKey(std::string_view key) const;
        DirState dirState(const fs::path& absolute) const;
        bool isLockable(const fs::path& absolute) const;
        bool isIgnored(const fs::path& absolute) const;

        // ---- wakes (any thread)
        void wake();
        // The next poll reads the locks and the server too (after lock, unlock, commit, push)
        void requestLockRefresh();
        // The next poll reads the ignored set and the lockable files (a .gitignore or
        // .gitattributes changed)
        void requestIgnoredRefresh();

        // ---- operations: on the worker, one at a time, done(...) on the main thread
        using Work = std::function<VcsResult(IVcsProvider& provider, const jobs::CancelToken& cancel)>;
        // `label` names it in the status bar ("Committing 3 files..."); refreshLocks: the next
        // poll reads the locks (a lock, unlock, commit or push)
        void run(std::string label, Work work, Done done, bool refreshLocks = false);
        // Cancels the running operation (a network call), if any
        void cancelRunning();

        // A poll now; with locks (refresh=true in get_vcs_status). done runs once the new
        // snapshot is in (the next update()). Without a repository or the tools, it first looks
        // for them again (a git init or an install since the project opened).
        void refresh(bool withLocks, Done done = {});
        void commit(std::vector<RepoPath> paths, std::string message, bool push, std::function<void(const VcsResult&, const std::string& revision)> done);
        void lock(std::vector<RepoPath> paths, Done done);
        void unlock(std::vector<RepoPath> paths, bool force, Done done);
        void fetch(Done done);
        void pull(Done done);
        void push(Done done);
        void log(std::optional<RepoPath> path, int limit, int skip, std::function<void(const VcsResult&, std::vector<VcsLogEntry>)> done);
        void baseContent(RepoPath path, std::string rev, std::function<void(const VcsResult&, std::string)> done);
        void textDiff(RepoPath path, std::string rev, std::function<void(const VcsResult&, std::string)> done);

        // The provider for a background job that reads (the scene diff's baseline): any thread.
        // Null while not active.
        std::shared_ptr<IVcsProvider> providerForReads() const;

        // Test hooks: how many polls ran, and with locks
        uint64_t pollCount() const { return polls.load(); }
        uint64_t lockPollCount() const { return lockPolls.load(); }

    private:
        void startDetection();
        void installProvider(std::shared_ptr<IVcsProvider> provider, VcsCaps caps, fs::path repoRoot, std::string versionText);
        void pollOnce();
        void post(std::function<void()> fn);
        void onFileChanges(std::span<const FileChange> changes);

        VcsSettings settings;
        fs::path project;
        fs::path internal;
        bool watch = true;
        uint64_t generation = 0;

        // main thread
        VcsState currentState = VcsState::Off;
        std::string message;
        VcsCaps capabilities;
        fs::path root;
        RepoPath projectKeyValue;
        SnapshotPtr current;
        uint64_t currentVersion = 0;
        std::string error;
        std::string runningLabel;
        struct RefreshWaiter {
            uint64_t afterPoll;          // satisfied by a poll that started after this one
            Done done;
        };
        std::vector<RefreshWaiter> refreshWaiters;
        struct DetectionWaiter {
            bool withLocks;
            Done done;
        };
        std::vector<DetectionWaiter> detectionWaiters;   // refreshes asked while detecting
        // operations queued or running, by id: a close() answers the ones that never ran
        std::map<uint64_t, Done> inflight;
        uint64_t nextOpId = 1;

        // shared with the threads
        mutable std::mutex providerMutex;
        std::shared_ptr<IVcsProvider> provider;
        std::mutex snapshotMutex;
        SnapshotPtr published;               // the poller's newest, until update() takes it
        std::string publishedError;
        std::mutex queueMutex;
        std::vector<std::function<void()>> mainQueue;
        std::atomic<int> pendingOps{0};
        std::atomic<bool> lockRefreshRequested{false};
        std::atomic<bool> ignoredRefreshRequested{true};
        std::atomic<uint64_t> polls{0};
        std::atomic<uint64_t> lockPolls{0};
        std::atomic<uint64_t> pollsStarted{0};
        uint64_t failedPoll = 0;             // the serial of the newest failed poll (snapshotMutex)
        // the cadences the poller reads (settings is the main thread's)
        std::atomic<int> lockPollMs{60000};
        std::atomic<int> autoFetchMs{0};
        // the poller thread's own
        SnapshotPtr pollerPrevious;
        std::chrono::steady_clock::time_point nextLockPoll;
        std::chrono::steady_clock::time_point nextFetch;
        std::mutex cancelMutex;
        std::vector<jobs::CancelToken> running;

        std::unique_ptr<jobs::JobChannel> worker;
        VcsStatusPoller poller;
        FileWatchSubscription watcherSubscription;
    };

}
