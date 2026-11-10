// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vcs/VcsService.h"

#include "vcs/GitProvider.h"
#include "vcs/ProcessExec.h"
#include "vcs/SvnProvider.h"

#include "Out.h"

#include <algorithm>
#include <exception>
#include <iterator>

namespace jasperengine::editor::vcs {

namespace {

    using Clock = std::chrono::steady_clock;

    struct Detection {
        VcsState state = VcsState::NoRepository;
        std::string message;
        std::shared_ptr<IVcsProvider> provider;
        VcsCaps caps;
        fs::path root;
    };

    bool sameDirectory(const fs::path& a, const fs::path& b) {
        std::error_code ec;
        const fs::path ca = fs::weakly_canonical(a, ec);
        const fs::path cb = fs::weakly_canonical(b, ec);
        return (ca.empty() ? a : ca).lexically_normal() == (cb.empty() ? b : cb).lexically_normal();
    }

    // R7: from the project folder up, the nearest .git (a folder, or a file for worktrees and
    // submodules) or .svn wins
    Detection detect(const fs::path& projectDir, const fs::path& internalDir, const VcsSettings& settings) {
        Detection found;
        if (settings.provider == ProviderChoice::None) {
            found.state = VcsState::Off;
            found.message = "Version control is off (Project Settings > Editor > Version Control)";
            return found;
        }
        enum class Kind { None, Git, Svn } kind = Kind::None;
        fs::path dir = projectDir.lexically_normal();
        while (!dir.empty()) {
            std::error_code ec;
            if (settings.provider != ProviderChoice::Svn && fs::exists(dir / ".git", ec)) {
                kind = Kind::Git;
                break;
            }
            if (settings.provider != ProviderChoice::Git && fs::is_directory(dir / ".svn", ec)) {
                kind = Kind::Svn;
                break;
            }
            const fs::path parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
        if (kind == Kind::None) {
            found.state = VcsState::NoRepository;
            found.message = settings.provider == ProviderChoice::Git ? "The project is not in a Git repository" :
                            settings.provider == ProviderChoice::Svn ? "The project is not in a Subversion working copy" :
                                                                       "The project is not in a Git or Subversion repository";
            return found;
        }
        ExecutableLookup lookup;
        lookup.projectDir = projectDir;
        lookup.configured = kind == Kind::Git ? settings.gitPath : settings.svnPath;
        lookup.usePathOverride = settings.usePathOverride;
        lookup.pathOverride = settings.pathOverride;
        lookup.probeMacLocations = !settings.usePathOverride;
        std::string why;
        const fs::path exe = findExecutable(kind == Kind::Git ? "git" : "svn", lookup, &why);
        if (exe.empty()) {
            found.state = VcsState::NotAvailable;
            found.message = "Version control: not available (" + why + ")";
            return found;
        }
        ProviderConfig config;
        config.projectDir = projectDir;
        config.internalDir = internalDir;
        config.executable = exe;
        config.userName = settings.userName;
        config.extraEnv = settings.extraEnv;
        config.svnGlobalArgs = settings.svnGlobalArgs;
        if (kind == Kind::Git) {
            const ToolVersion version = GitProvider::checkGit(exe, settings.extraEnv);
            if (!version.ok) {
                found.state = VcsState::NotAvailable;
                found.message = "Version control: not available (" + version.message + ")";
                return found;
            }
            fs::path top;
            const VcsResult toplevel = GitProvider::showToplevel(exe, projectDir, settings.extraEnv, top);
            if (!toplevel.ok) {
                found.state = VcsState::NoRepository;
                found.message = "Git does not see a repository here: " + toplevel.message;
                return found;
            }
            // the keys are lexical below the root: it keeps the form of the project's path
            config.repoRoot = sameDirectory(top, dir) ? dir : top;
            const ToolVersion lfs = GitProvider::checkLfs(exe, settings.extraEnv, projectDir);
            config.lfs = lfs.ok;
            found.provider = std::make_shared<GitProvider>(config);
            found.message = "Git " + version.version + (lfs.ok ? ", Git LFS " + lfs.version : " (" + lfs.message + ")");
        } else {
            const ToolVersion version = SvnProvider::checkSvn(exe, settings.extraEnv);
            if (!version.ok) {
                found.state = VcsState::NotAvailable;
                found.message = "Version control: not available (" + version.message + ")";
                return found;
            }
            fs::path wcRoot;
            const VcsResult info = SvnProvider::workingCopyRoot(exe, projectDir, settings.extraEnv, wcRoot);
            if (!info.ok) {
                found.state = VcsState::NoRepository;
                found.message = "Subversion does not see a working copy here: " + info.message;
                return found;
            }
            config.repoRoot = sameDirectory(wcRoot, dir) ? dir : wcRoot;
            found.provider = std::make_shared<SvnProvider>(config);
            found.message = "Subversion " + version.version;
        }
        found.state = VcsState::Active;
        found.caps = found.provider->capabilities();
        found.root = config.repoRoot;
        return found;
    }

}

const char* providerChoiceName(ProviderChoice choice) {
    switch (choice) {
        case ProviderChoice::Auto: return "auto";
        case ProviderChoice::Git: return "git";
        case ProviderChoice::Svn: return "svn";
        case ProviderChoice::None: return "none";
    }
    return "auto";
}

bool parseProviderChoice(std::string_view text, ProviderChoice& out) {
    for (ProviderChoice choice : {ProviderChoice::Auto, ProviderChoice::Git, ProviderChoice::Svn, ProviderChoice::None}) {
        if (text == providerChoiceName(choice)) {
            out = choice;
            return true;
        }
    }
    return false;
}

const char* vcsStateName(VcsState state) {
    switch (state) {
        case VcsState::Off: return "off";
        case VcsState::Detecting: return "detecting";
        case VcsState::NotAvailable: return "not_available";
        case VcsState::NoRepository: return "no_repository";
        case VcsState::Active: return "active";
    }
    return "off";
}

void VcsSettings::clamp() {
    pollSeconds = std::clamp(pollSeconds, kMinPollSeconds, kMaxPollSeconds);
    if (lockPollSeconds != 0) lockPollSeconds = std::clamp(lockPollSeconds, kMinLockPollSeconds, kMaxLockPollSeconds);
    autoFetchMinutes = std::clamp(autoFetchMinutes, 0, 24 * 60);
}

VcsService::VcsService() = default;

VcsService::~VcsService() {
    close();
}

void VcsService::open(const fs::path& projectDir, const fs::path& internalDir, VcsSettings newSettings, bool watchFiles) {
    close();
    settings = std::move(newSettings);
    settings.clamp();
    project = projectDir.lexically_normal();
    internal = internalDir;
    watch = watchFiles;
    generation++;
    startDetection();
}

void VcsService::startDetection() {
    currentState = VcsState::Detecting;
    message = "Looking for a repository...";
    worker = std::make_unique<jobs::JobChannel>("vcs", 1);
    const uint64_t openGeneration = generation;
    const fs::path projectDir = project;
    const fs::path internalDir = internal;
    const VcsSettings detectSettings = settings;
    pendingOps++;
    worker->submit([this, openGeneration, projectDir, internalDir, detectSettings]() {
        Detection found;
        try {
            found = detect(projectDir, internalDir, detectSettings);
        } catch (const std::exception& e) {
            found.state = VcsState::NotAvailable;
            found.message = std::string("Version control: not available (") + e.what() + ")";
        }
        post([this, openGeneration, found]() {
            pendingOps--;
            if (openGeneration != generation) return;
            currentState = found.state;
            message = found.message;
            if (found.state == VcsState::Active) {
                installProvider(found.provider, found.caps, found.root, found.message);
            } else if (found.state == VcsState::NotAvailable) {
                Out::warning("%s", found.message.c_str());
            }
            std::vector<DetectionWaiter> waiters;
            waiters.swap(detectionWaiters);
            for (DetectionWaiter& waiter : waiters) {
                if (active()) {
                    refresh(waiter.withLocks, std::move(waiter.done));
                } else if (waiter.done) {
                    waiter.done(VcsResult::failure("Version control is not active: " + message));
                }
            }
        });
    }, jobs::SubmitOptions{.name = "vcs-detect"});
}

void VcsService::installProvider(std::shared_ptr<IVcsProvider> newProvider, VcsCaps newCaps, fs::path repoRoot, std::string versionText) {
    {
        std::lock_guard<std::mutex> lock(providerMutex);
        provider = std::move(newProvider);
    }
    capabilities = std::move(newCaps);
    root = std::move(repoRoot);
    repoRelative(project, root, projectKeyValue);
    Out::info("Version control: %s at %s", versionText.c_str(), root.string().c_str());
    lockPollMs = settings.lockPollMilliseconds > 0 ? settings.lockPollMilliseconds : settings.lockPollSeconds * 1000;
    autoFetchMs = settings.autoFetchMinutes * 60 * 1000;
    ignoredRefreshRequested = true;
    // the first poll reads the locks too, so they show from the start
    lockRefreshRequested = true;
    const Clock::time_point now = Clock::now();
    nextLockPoll = now + std::chrono::milliseconds(lockPollMs.load());
    nextFetch = now + std::chrono::milliseconds(autoFetchMs.load());
    if (settings.pollMilliseconds > 0) poller.setIntervalMilliseconds(settings.pollMilliseconds);
    else poller.setIntervalSeconds(settings.pollSeconds);
    poller.start("je-vcs", [this]() { pollOnce(); });
    if (watch) {
        WatchFilter filter;
        filter.root = project;
        filter.deliverEditorWrites = true;   // the editor's own saves (R5)
        watcherSubscription = FileWatchService::get().subscribe(std::move(filter),
            [this](std::span<const FileChange> changes) { onFileChanges(changes); }, DeliverOn::Worker);
    }
}

void VcsService::close() {
    watcherSubscription.reset();
    {
        std::lock_guard<std::mutex> lock(cancelMutex);
        for (jobs::CancelToken& token : running) token.cancel();
    }
    poller.stop();
    // waits for the operation in flight; the queued ones never start
    worker.reset();
    {
        std::lock_guard<std::mutex> lock(providerMutex);
        provider.reset();
    }
    // what finished still reaches its callback; what never ran is answered
    std::vector<std::function<void()>> pending;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        pending.swap(mainQueue);
    }
    for (std::function<void()>& fn : pending) fn();
    std::map<uint64_t, Done> unanswered;
    unanswered.swap(inflight);
    for (auto& [id, done] : unanswered) {
        if (done) done(VcsResult::failure("cancelled: the project's version control closed"));
    }
    std::vector<RefreshWaiter> waiters;
    waiters.swap(refreshWaiters);
    for (RefreshWaiter& waiter : waiters) {
        if (waiter.done) waiter.done(VcsResult::failure("cancelled: the project's version control closed"));
    }
    std::vector<DetectionWaiter> detecting;
    detecting.swap(detectionWaiters);
    for (DetectionWaiter& waiter : detecting) {
        if (waiter.done) waiter.done(VcsResult::failure("cancelled: the project's version control closed"));
    }
    {
        std::lock_guard<std::mutex> lock(cancelMutex);
        running.clear();
    }
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        published.reset();
        publishedError.clear();
        failedPoll = 0;
    }
    pollerPrevious.reset();
    pendingOps = 0;
    currentState = VcsState::Off;
    message.clear();
    capabilities = VcsCaps();
    root.clear();
    projectKeyValue.clear();
    current.reset();
    error.clear();
    runningLabel.clear();
}

void VcsService::applySettings(VcsSettings newSettings) {
    newSettings.clamp();
    const bool reopen = newSettings.provider != settings.provider || newSettings.gitPath != settings.gitPath ||
                        newSettings.svnPath != settings.svnPath || newSettings.extraEnv != settings.extraEnv;
    if (reopen && !project.empty()) {
        open(project, internal, std::move(newSettings), watch);
        return;
    }
    settings = std::move(newSettings);
    if (settings.pollMilliseconds > 0) poller.setIntervalMilliseconds(settings.pollMilliseconds);
    else poller.setIntervalSeconds(settings.pollSeconds);
    lockPollMs = settings.lockPollMilliseconds > 0 ? settings.lockPollMilliseconds : settings.lockPollSeconds * 1000;
    autoFetchMs = settings.autoFetchMinutes * 60 * 1000;
}

void VcsService::post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(queueMutex);
    mainQueue.push_back(std::move(fn));
}

void VcsService::pollOnce() {
    const std::shared_ptr<IVcsProvider> reader = providerForReads();
    if (!reader) return;
    const uint64_t serial = ++pollsStarted;
    const Clock::time_point now = Clock::now();
    uint32_t depth = StatusDepth::Fast;
    const int lockMs = lockPollMs.load();
    const bool locksDue = lockMs > 0 && now >= nextLockPoll;
    if (lockRefreshRequested.exchange(false) || locksDue) {
        depth |= StatusDepth::WithLocksAndRemote;
        if (lockMs > 0) nextLockPoll = now + std::chrono::milliseconds(lockMs);
    }
    if (ignoredRefreshRequested.exchange(false)) depth |= StatusDepth::WithIgnored;
    const int fetchMs = autoFetchMs.load();
    if (fetchMs > 0 && now >= nextFetch && reader->capabilities().fetch) {
        nextFetch = now + std::chrono::milliseconds(fetchMs);
        const VcsResult fetched = reader->fetch();
        if (!fetched.ok) Out::warning("Version control: automatic fetch failed: %s", fetched.message.c_str());
        // what the fetch brought shows as server changes
        depth |= StatusDepth::WithLocksAndRemote;
    }

    VcsSnapshot snapshot;
    const VcsResult result = reader->status(snapshot, depth);
    polls++;
    if (depth & StatusDepth::WithLocksAndRemote) lockPolls++;
    if (!result.ok) {
        // the requests this poll carried come back for the next one
        if (depth & StatusDepth::WithIgnored) ignoredRefreshRequested = true;
        std::lock_guard<std::mutex> lock(snapshotMutex);
        publishedError = result.message.empty() ? std::string("status failed") : result.message;
        failedPoll = serial;
        return;
    }
    // a detached HEAD's tag: asked once per HEAD (a process), carried over while HEAD stays
    if (snapshot.branch.detached && !snapshot.branch.headOid.empty()) {
        if (pollerPrevious && pollerPrevious->branch.detached && pollerPrevious->branch.headOid == snapshot.branch.headOid) {
            snapshot.branch.tag = pollerPrevious->branch.tag;
        } else {
            std::string tag;
            if (reader->headTag(tag).ok) snapshot.branch.tag = tag;
        }
    }
    snapshot.finalize(pollerPrevious.get());
    snapshot.version = serial;
    SnapshotPtr fresh = std::make_shared<const VcsSnapshot>(std::move(snapshot));
    pollerPrevious = fresh;
    std::lock_guard<std::mutex> lock(snapshotMutex);
    published = std::move(fresh);
    publishedError.clear();
}

bool VcsService::update() {
    std::vector<std::function<void()>> pending;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        pending.swap(mainQueue);
    }
    for (std::function<void()>& fn : pending) fn();

    SnapshotPtr fresh;
    std::string pollError;
    uint64_t failed = 0;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        fresh = std::move(published);
        published.reset();
        pollError = publishedError;
        failed = failedPoll;
    }
    bool changed = false;
    if (fresh) {
        current = std::move(fresh);
        currentVersion++;
        changed = true;
        if (pollError.empty()) error.clear();
    }
    if (!pollError.empty()) error = pollError;
    if (!refreshWaiters.empty()) {
        const uint64_t done = current ? current->version : 0;
        std::vector<RefreshWaiter> ready;
        auto it = std::stable_partition(refreshWaiters.begin(), refreshWaiters.end(),
            [done, failed](const RefreshWaiter& waiter) { return done <= waiter.afterPoll && failed <= waiter.afterPoll; });
        std::move(it, refreshWaiters.end(), std::back_inserter(ready));
        refreshWaiters.erase(it, refreshWaiters.end());
        for (RefreshWaiter& waiter : ready) {
            if (!waiter.done) continue;
            if (done > waiter.afterPoll) waiter.done(VcsResult::success());
            else waiter.done(VcsResult::failure(pollError.empty() ? std::string("status failed") : pollError));
        }
    }
    return changed;
}

const std::string& VcsService::lockError() const {
    static const std::string none;
    return current ? current->lockError : none;
}

bool VcsService::locksPolled() const {
    return capabilities.locking && lockPollMs.load() > 0;
}

std::string VcsService::baselineRevision() const {
    if (!active()) return std::string();
    return capabilities.provider == "svn" ? std::string("BASE") : std::string("HEAD");
}

std::string VcsService::effectiveRevision(std::string_view rev) const {
    if (rev == "HEAD" && capabilities.provider == "svn") return "BASE";
    return std::string(rev);
}

std::string VcsService::contentMemoKey(const RepoPath& key, std::string_view rev) const {
    // (svn's HEAD and BASE read the same: one memo)
    std::string memo = key + "\n" + effectiveRevision(rev);
    if (current) {
        memo += "\n" + current->branch.headOid;
        if (const VcsChange* change = current->find(key)) {
            memo += "\n" + change->baseOid + (change->newFile() ? "\nnew" : "");
            if (!change->oldPath.empty()) memo += "\nfrom " + change->oldPath;
        }
    }
    return memo;
}

bool VcsService::toKey(const fs::path& absolute, RepoPath& out) const {
    if (root.empty()) return false;
    return repoRelative(absolute, root, out);
}

const VcsChange* VcsService::find(const fs::path& absolute) const {
    RepoPath key;
    if (!current || !toKey(absolute, key)) return nullptr;
    return current->find(key);
}

DirState VcsService::dirState(const fs::path& absolute) const {
    RepoPath key;
    if (!current || !toKey(absolute, key)) return DirState::Clean;
    const VcsDirEntry* dir = current->findDir(key);
    return dir ? dir->state : DirState::Clean;
}

DirState VcsService::dirStateKey(std::string_view key) const {
    const VcsDirEntry* dir = current ? current->findDir(key) : nullptr;
    return dir ? dir->state : DirState::Clean;
}

bool VcsService::isLockable(const fs::path& absolute) const {
    RepoPath key;
    return current && toKey(absolute, key) && current->isLockable(key);
}

bool VcsService::isIgnored(const fs::path& absolute) const {
    RepoPath key;
    return current && toKey(absolute, key) && current->isIgnored(key);
}

void VcsService::wake() {
    poller.wake();
}

void VcsService::requestLockRefresh() {
    lockRefreshRequested = true;
}

void VcsService::requestIgnoredRefresh() {
    ignoredRefreshRequested = true;
}

void VcsService::onFileChanges(std::span<const FileChange> changes) {
    bool relevant = false;
    for (const FileChange& change : changes) {
        RepoPath key;
        if (!repoRelative(change.path, project, key)) continue;
        const std::string_view first = std::string_view(key).substr(0, key.find('/'));
        // the editor's internal files and the repositories' own never change the status
        if (first == ".jasperengine" || first == ".git" || first == ".svn") continue;
        relevant = true;
        const std::string name = change.path.filename().string();
        if (name == ".gitignore" || name == ".gitattributes") requestIgnoredRefresh();
    }
    if (relevant) wake();
}

std::shared_ptr<IVcsProvider> VcsService::providerForReads() const {
    std::lock_guard<std::mutex> lock(providerMutex);
    return provider;
}

void VcsService::run(std::string label, Work work, Done done, bool refreshLocks) {
    const std::shared_ptr<IVcsProvider> target = providerForReads();
    const uint64_t id = nextOpId++;
    inflight[id] = std::move(done);
    if (!target || !worker) {
        post([this, id]() {
            auto it = inflight.find(id);
            if (it == inflight.end()) return;
            Done callback = std::move(it->second);
            inflight.erase(it);
            if (callback) callback(VcsResult::failure("Version control is not active: " + (message.empty() ? std::string("no repository") : message)));
        });
        return;
    }
    jobs::CancelToken token;
    {
        std::lock_guard<std::mutex> lock(cancelMutex);
        running.push_back(token);
    }
    pendingOps++;
    if (runningLabel.empty()) runningLabel = label;
    worker->submit([this, target, work = std::move(work), token, refreshLocks, id, label]() {
        post([this, label]() { runningLabel = label; });
        VcsResult result;
        try {
            result = work(*target, token);
        } catch (const std::exception& e) {
            result = VcsResult::failure(e.what());
        }
        if (refreshLocks) lockRefreshRequested = true;
        // an operation that finished changes what the status shows (D3)
        poller.wake();
        post([this, id, token, result]() {
            pendingOps--;
            {
                std::lock_guard<std::mutex> lock(cancelMutex);
                running.erase(std::remove(running.begin(), running.end(), token), running.end());
            }
            if (pendingOps.load() <= 0) runningLabel.clear();
            if (!result.ok) error = result.message;
            auto it = inflight.find(id);
            if (it == inflight.end()) return;
            Done callback = std::move(it->second);
            inflight.erase(it);
            if (callback) callback(result);
        });
    }, jobs::SubmitOptions{.name = "vcs-op"});
}

void VcsService::cancelRunning() {
    std::lock_guard<std::mutex> lock(cancelMutex);
    for (jobs::CancelToken& token : running) token.cancel();
}

void VcsService::refresh(bool withLocks, Done done) {
    if (currentState == VcsState::NoRepository || currentState == VcsState::NotAvailable) {
        // a repository made, or the tools installed, since the project opened: look again
        open(project, internal, settings, watch);
    }
    if (currentState == VcsState::Detecting) {
        detectionWaiters.push_back({withLocks, std::move(done)});
        return;
    }
    if (!active()) {
        if (done) post([this, done]() { done(VcsResult::failure("Version control is not active: " + message)); });
        return;
    }
    if (withLocks) lockRefreshRequested = true;
    refreshWaiters.push_back({pollsStarted.load(), std::move(done)});
    poller.wake();
}

void VcsService::commit(std::vector<RepoPath> paths, std::string commitMessage, bool pushAfter,
                        std::function<void(const VcsResult&, const std::string& revision)> done) {
    auto revision = std::make_shared<std::string>();
    const std::string label = "Committing " + std::to_string(paths.size()) + (paths.size() == 1 ? " file..." : " files...");
    run(label, [paths = std::move(paths), commitMessage = std::move(commitMessage), pushAfter, revision](IVcsProvider& provider, const jobs::CancelToken& cancel) {
        VcsResult result = provider.commit(paths, commitMessage, *revision, &cancel);
        if (!result.ok || !pushAfter) return result;
        const VcsResult pushed = provider.push(&cancel);
        if (!pushed.ok) return VcsResult::failure("Committed " + *revision + ", but the push failed: " + pushed.message, pushed.exitCode);
        return VcsResult::success(result.message + (pushed.message.empty() ? std::string() : "\n" + pushed.message));
    }, [revision, done = std::move(done)](const VcsResult& result) {
        if (done) done(result, *revision);
    }, true);
}

void VcsService::lock(std::vector<RepoPath> paths, Done done) {
    const std::string label = "Locking " + std::to_string(paths.size()) + (paths.size() == 1 ? " file..." : " files...");
    run(label, [paths = std::move(paths)](IVcsProvider& provider, const jobs::CancelToken&) { return provider.lock(paths); }, std::move(done), true);
}

void VcsService::unlock(std::vector<RepoPath> paths, bool force, Done done) {
    const std::string label = "Unlocking " + std::to_string(paths.size()) + (paths.size() == 1 ? " file..." : " files...");
    run(label, [paths = std::move(paths), force](IVcsProvider& provider, const jobs::CancelToken&) { return provider.unlock(paths, force); },
        std::move(done), true);
}

void VcsService::fetch(Done done) {
    run("Fetching...", [](IVcsProvider& provider, const jobs::CancelToken& cancel) { return provider.fetch(&cancel); }, std::move(done), true);
}

void VcsService::pull(Done done) {
    run("Pulling...", [](IVcsProvider& provider, const jobs::CancelToken& cancel) { return provider.pull(&cancel); }, std::move(done), true);
}

void VcsService::push(Done done) {
    run("Pushing...", [](IVcsProvider& provider, const jobs::CancelToken& cancel) { return provider.push(&cancel); }, std::move(done), true);
}

void VcsService::log(std::optional<RepoPath> path, int limit, int skip, std::function<void(const VcsResult&, std::vector<VcsLogEntry>)> done) {
    auto entries = std::make_shared<std::vector<VcsLogEntry>>();
    run("Reading the history...", [path = std::move(path), limit, skip, entries](IVcsProvider& provider, const jobs::CancelToken&) {
        return provider.log(path ? &*path : nullptr, limit, skip, *entries);
    }, [entries, done = std::move(done)](const VcsResult& result) {
        if (done) done(result, std::move(*entries));
    });
}

void VcsService::baseContent(RepoPath path, std::string rev, std::function<void(const VcsResult&, std::string)> done) {
    auto content = std::make_shared<std::string>();
    rev = effectiveRevision(rev);
    run("Reading " + path + "...", [path, rev = std::move(rev), content](IVcsProvider& provider, const jobs::CancelToken&) {
        return provider.baseContent(path, rev, *content);
    }, [content, done = std::move(done)](const VcsResult& result) {
        if (done) done(result, std::move(*content));
    });
}

void VcsService::textDiff(RepoPath path, std::string rev, std::function<void(const VcsResult&, std::string)> done) {
    auto diff = std::make_shared<std::string>();
    rev = effectiveRevision(rev);
    run("Diffing " + path + "...", [path, rev = std::move(rev), diff](IVcsProvider& provider, const jobs::CancelToken&) {
        return provider.textDiff(path, rev, *diff);
    }, [diff, done = std::move(done)](const VcsResult& result) {
        if (done) done(result, std::move(*diff));
    });
}

}
