// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FileWatchService.h"

#include "AtomicFile.h"
#include "FileChangeCoalescer.h"
#include "ProjectFileIndex.h"
#include "jobs/detail/TimedWait.h"
#include "log/Logger.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <set>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

using namespace jasperengine;

namespace fs = std::filesystem;

namespace {

    using Clock = std::chrono::steady_clock;

    constexpr size_t JOURNAL_CAPACITY = 65536;
    // how many changes one main-thread callback gets at most (the rest: the next batch)
    constexpr size_t MAIN_BATCH_MAX = 512;
    constexpr size_t WORKER_BATCH_MAX = 4096;
    constexpr std::chrono::milliseconds PROBE_TIMEOUT{2000};
    // a polling watcher's removal waits one interval and this for the file to come back
    constexpr std::chrono::milliseconds POLL_HOLD_MARGIN{100};
    const char* const PROBE_NAME = "watch-probe";

#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    constexpr bool WATCHING_SUPPORTED = false;
#elif defined(__APPLE__) && !TARGET_OS_OSX
    constexpr bool WATCHING_SUPPORTED = false;   // iOS, tvOS
#else
    constexpr bool WATCHING_SUPPORTED = true;
#endif

    fs::path normalized(const fs::path& path) {
        std::error_code ec;
        const fs::path absolute = path.is_absolute() ? path : fs::absolute(path, ec);
        fs::path out = (ec ? path : absolute).lexically_normal();
        // "dir/" and "dir" are one folder
        if (!out.has_filename() && out.has_parent_path() && out != out.root_path()) out = out.parent_path();
        return out;
    }

}

// ------------------------------------------------------------------ shared state

struct FileWatchService::Subscriber {
    uint64_t id = 0;
    WatchFilter filter;
    FileChangeBatchFn fn;
    DeliverOn on = DeliverOn::MainThread;
    int priority = 0;
    std::deque<FileChange> queue;
    bool active = true;
    int inFlight = 0;
    std::thread::id callingThread{};
};

class FileWatchService::Sink : public watch::WatchSink {
public:
    explicit Sink(Shared& shared) : shared(shared) {}
    void onRawEvents(std::vector<watch::RawEvent>&& events) override;
    void onBackendFailed(const std::string& reason) override;
    void onProbe() override;

private:
    Shared& shared;
};

// A linked folder's backend: its events name the folder's real path; the rest of the editor
// knows them by the link's
class FileWatchService::LinkedSink : public watch::WatchSink {
public:
    LinkedSink(Shared& shared, fs::path real, fs::path logical) : shared(shared), real(std::move(real)), logical(std::move(logical)) {}
    void onRawEvents(std::vector<watch::RawEvent>&& events) override;
    void onBackendFailed(const std::string& reason) override;

private:
    fs::path mapped(const fs::path& path) const {
        std::string rel;
        if (path.empty() || !watch::relativeTo(real, path, rel)) return path;
        return rel.empty() ? logical : (logical / watch::pathFromUtf8(rel)).lexically_normal();
    }

    Shared& shared;
    fs::path real;
    fs::path logical;
};

class FileWatchService::CoalescerEnvironment : public FileChangeCoalescer::Environment {
public:
    explicit CoalescerEnvironment(ProjectFileIndex& index) : index(index) {}
    bool known(const fs::path& path, watch::PathStat& out) override { return index.find(path, out); }
    bool stat(const fs::path& path, watch::PathStat& out) override { return watch::statPath(path, out); }

private:
    ProjectFileIndex& index;
};

struct FileWatchService::Shared {
    // ---- state (stateMutex); lock order: stateMutex, then rawMutex, then subscribersMutex
    mutable std::mutex stateMutex;
    std::condition_variable stateCv;   // indexReady
    bool attached = false;
    fs::path root;
    WatchBackendKind requested = WatchBackendKind::Off;
    std::chrono::milliseconds pollInterval{1000};
    State state = State::Off;
    std::shared_ptr<FileWatcher> backend;
    struct LinkedWatch {
        std::string key;
        fs::path real;
        std::unique_ptr<LinkedSink> sink;
        std::shared_ptr<FileWatcher> backend;
    };
    std::vector<LinkedWatch> linked;
    uint64_t linkedVersion = 0;        // the index's version the backends above were made for
    std::mutex linkedSyncMutex;        // one syncLinkedWatches at a time; taken before stateMutex
    ProjectFileIndex index;
    bool indexReady = false;
    CoalescerEnvironment environment{index};
    FileChangeCoalescer coalescer{environment};
    bool paused = false;
    std::vector<FileChange> ring;
    uint64_t nextSequence = 1;
    uint64_t oldestSequence = 1;
    uint64_t delivered = 0;
    uint64_t echoes = 0;
    uint64_t overflows = 0;
    std::set<std::string> warned;      // each watcher warning once

    // ---- raw events from the backend (rawMutex)
    std::mutex rawMutex;
    std::condition_variable rawCv;
    std::vector<watch::RawEvent> raw;
    uint64_t rawCount = 0;
    bool stopRequested = false;
    bool fallbackRequested = false;
    std::string fallbackReason;
    bool probeSeen = false;
    std::atomic<bool> cancelBuild{false};
    std::thread dispatcher;
    Sink sink{*this};

    // ---- subscribers (subscribersMutex)
    std::mutex subscribersMutex;
    std::condition_variable subscribersCv;
    std::vector<std::shared_ptr<Subscriber>> subscribers;   // priority, then age
    uint64_t nextId = 1;
    std::atomic<bool> mainPending{false};

    std::mutex wakerMutex;
    std::function<void()> waker;
    std::atomic<std::thread::id> mainThread{std::thread::id()};

    std::function<void(const std::string&)> warning = [](const std::string& message) { JE_LOG_WARN(logCategoryIO(), "%s", message.c_str()); };

    void warnOnceLocked(const std::string& key, const std::string& message) {
        if (!warned.insert(key).second) return;
        if (warning) warning("[Hot Reload] " + message);
    }
};

void FileWatchService::Sink::onRawEvents(std::vector<watch::RawEvent>&& events) {
    {
        std::lock_guard<std::mutex> lock(shared.rawMutex);
        shared.rawCount += events.size();
        if (shared.raw.empty()) {
            shared.raw = std::move(events);
        } else {
            shared.raw.insert(shared.raw.end(), std::make_move_iterator(events.begin()), std::make_move_iterator(events.end()));
        }
    }
    shared.rawCv.notify_all();
}

void FileWatchService::Sink::onBackendFailed(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(shared.rawMutex);
        shared.fallbackRequested = true;
        shared.fallbackReason = reason;
    }
    shared.rawCv.notify_all();
}

void FileWatchService::LinkedSink::onRawEvents(std::vector<watch::RawEvent>&& events) {
    for (watch::RawEvent& event : events) {
        event.path = mapped(event.path);
        event.oldPath = mapped(event.oldPath);
    }
    shared.sink.onRawEvents(std::move(events));
}

void FileWatchService::LinkedSink::onBackendFailed(const std::string& reason) {
    // the linked folder went away (a drive unplugged): what it held is gone, the watch of the
    // project goes on. Anything else (out of watches) is the backend's failure like the project's.
    watch::PathStat stat;
    if (watch::statPath(real, stat, false)) {
        shared.sink.onBackendFailed(reason);
        return;
    }
    shared.sink.onRawEvents({watch::RawEvent{watch::RawEvent::Type::Rescan, logical, {}, true}});
}

void FileWatchService::Sink::onProbe() {
    {
        std::lock_guard<std::mutex> lock(shared.rawMutex);
        shared.probeSeen = true;
    }
    shared.rawCv.notify_all();
}

// ------------------------------------------------------------------ subscription

FileWatchSubscription::FileWatchSubscription(FileWatchService* service, uint64_t id)
    : service(service), id(id) {
}

FileWatchSubscription::FileWatchSubscription(FileWatchSubscription&& other) noexcept
    : service(other.service), id(other.id) {
    other.service = nullptr;
    other.id = 0;
}

FileWatchSubscription& FileWatchSubscription::operator=(FileWatchSubscription&& other) noexcept {
    if (this != &other) {
        reset();
        service = other.service;
        id = other.id;
        other.service = nullptr;
        other.id = 0;
    }
    return *this;
}

FileWatchSubscription::~FileWatchSubscription() {
    reset();
}

void FileWatchSubscription::reset() {
    if (service) service->unsubscribe(id);
    service = nullptr;
    id = 0;
}

// ------------------------------------------------------------------ service

FileWatchService& FileWatchService::get() {
    static FileWatchService* instance = []() {
        auto* service = new FileWatchService();   // outlives static teardown
        // every document write of the editor goes through the atomic writer (S03): its
        // publishes are the editor's own writes (R10)
        io::setPublishListener([](const fs::path& target, uintmax_t, fs::file_time_type) {
            FileWatchService::get().reportEditorWrite(target, false);
        });
        io::setPrePublishListener([](const fs::path& target) { FileWatchService::get().reportEditorReplacing(target); });
        return service;
    }();
    return *instance;
}

FileWatchService::FileWatchService() {
    // nothing is allocated where nothing is watched (S05 R20)
    if (WATCHING_SUPPORTED) shared = std::make_unique<Shared>();
}

FileWatchService::~FileWatchService() {
    stop();
}

const char* FileWatchService::stateName(State state) {
    switch (state) {
        case State::Off: return "off";
        case State::Watching: return "watching";
        case State::Polling: return "polling";
        case State::Degraded: return "degraded";
    }
    return "off";
}

bool FileWatchService::start(const fs::path& rootIn, WatchBackendKind kind, std::chrono::milliseconds pollInterval) {
    if (!shared) return false;
    stop();
    const fs::path root = normalized(rootIn);
    watch::PathStat rootStat;
    if (root.empty() || !watch::statPath(root, rootStat, false) || !rootStat.directory) return false;

    shared->mainThread = std::this_thread::get_id();
    {
        std::lock_guard<std::mutex> lock(shared->stateMutex);
        shared->attached = true;
        shared->root = root;
        shared->requested = kind;
        shared->pollInterval = std::clamp(pollInterval, std::chrono::milliseconds(250), std::chrono::milliseconds(10000));
        shared->state = State::Off;
        shared->index.reset(root);
        shared->index.setFollowLinks(true);
        shared->indexReady = false;
        shared->coalescer.clear();
        shared->warned.clear();
    }
    writeLedger.setEchoWindow(std::max(std::chrono::milliseconds(2000), 2 * shared->pollInterval));
    if (kind == WatchBackendKind::Off) return false;

    {
        std::lock_guard<std::mutex> lock(shared->rawMutex);
        shared->raw.clear();
        shared->stopRequested = false;
        shared->fallbackRequested = false;
        shared->probeSeen = false;
    }
    shared->cancelBuild = false;
    // the backend's tree walk and the index are built there, not on the caller's thread
    shared->dispatcher = std::thread([this]() { dispatcherLoop(); });
    return true;
}

void FileWatchService::stop() {
    if (!shared) return;
    {
        std::lock_guard<std::mutex> lock(shared->rawMutex);
        shared->stopRequested = true;
    }
    shared->cancelBuild = true;
    shared->rawCv.notify_all();
    if (shared->dispatcher.joinable()) shared->dispatcher.join();

    // joins outside the service's lock (the backend's thread may be inside the sink)
    std::shared_ptr<FileWatcher> backend;
    {
        std::lock_guard<std::mutex> lock(shared->stateMutex);
        backend = std::move(shared->backend);
    }
    if (backend) backend->stop();
    {
        std::lock_guard<std::mutex> syncLock(shared->linkedSyncMutex);
        std::vector<Shared::LinkedWatch> linked;
        {
            std::lock_guard<std::mutex> lock(shared->stateMutex);
            linked.swap(shared->linked);
        }
        for (Shared::LinkedWatch& entry : linked) {
            if (entry.backend) entry.backend->stop();
        }
    }

    {
        std::lock_guard<std::mutex> lock(shared->stateMutex);
        shared->attached = false;
        shared->linkedVersion = 0;
        shared->root.clear();
        shared->index.reset({});
        shared->indexReady = false;
        shared->coalescer.clear();
        shared->state = State::Off;
        // cursors from before point past the journal: their consumers start over
        shared->oldestSequence = shared->nextSequence;
        shared->ring.clear();
    }
    {
        std::lock_guard<std::mutex> lock(shared->rawMutex);
        shared->raw.clear();
        shared->stopRequested = false;
        shared->fallbackRequested = false;
        shared->probeSeen = false;
    }
    {
        std::lock_guard<std::mutex> lock(shared->subscribersMutex);
        for (auto& subscriber : shared->subscribers) subscriber->queue.clear();
    }
    shared->mainPending = false;
    shared->stateCv.notify_all();
}

bool FileWatchService::isAttached() const {
    if (!shared) return false;
    std::lock_guard<std::mutex> lock(shared->stateMutex);
    return shared->attached;
}

fs::path FileWatchService::getRoot() const {
    if (!shared) return {};
    std::lock_guard<std::mutex> lock(shared->stateMutex);
    return shared->root;
}

WatchBackendKind FileWatchService::getRequestedKind() const {
    if (!shared) return WatchBackendKind::Off;
    std::lock_guard<std::mutex> lock(shared->stateMutex);
    return shared->requested;
}

void FileWatchService::setQuietWindow(std::chrono::milliseconds quiet) {
    if (!shared) return;
    std::lock_guard<std::mutex> lock(shared->stateMutex);
    shared->coalescer.setQuietWindow(quiet);
}

void FileWatchService::setWarningHandler(std::function<void(const std::string& message)> handler) {
    if (!shared) return;
    std::lock_guard<std::mutex> lock(shared->stateMutex);
    shared->warning = std::move(handler);
}

void FileWatchService::setPaused(bool paused) {
    if (!shared) return;
    std::lock_guard<std::mutex> lock(shared->stateMutex);
    shared->paused = paused;
}

bool FileWatchService::isPaused() const {
    if (!shared) return false;
    std::lock_guard<std::mutex> lock(shared->stateMutex);
    return shared->paused;
}

void FileWatchService::setMainThreadWaker(std::function<void()> waker) {
    if (!shared) return;
    std::lock_guard<std::mutex> lock(shared->wakerMutex);
    shared->waker = std::move(waker);
}

void FileWatchService::wakeMain() {
    if (!shared || !shared->mainPending) return;
    std::function<void()> waker;
    {
        std::lock_guard<std::mutex> lock(shared->wakerMutex);
        waker = shared->waker;
    }
    if (waker) waker();
}

bool FileWatchService::onMainThread() const {
    return shared && shared->mainThread.load() == std::this_thread::get_id();
}

// ------------------------------------------------------------------ dispatcher

void FileWatchService::dispatcherLoop() {
    Shared& s = *shared;
    fs::path root;
    WatchBackendKind requested;
    std::chrono::milliseconds interval;
    {
        std::lock_guard<std::mutex> lock(s.stateMutex);
        root = s.root;
        requested = s.requested;
        interval = s.pollInterval;
    }

    // 1. the backend: native for Auto/Native (polling where it fails), polling for Polling
    const fs::path probe = root / ".jasperengine" / PROBE_NAME;
    std::shared_ptr<FileWatcher> backend;
    State state = State::Polling;
    if (requested != WatchBackendKind::Polling && FileWatcher::hasNative()) {
        backend = FileWatcher::createNative();
        state = State::Watching;
        if (requested == WatchBackendKind::Auto) {
            std::error_code ec;
            fs::create_directories(probe.parent_path(), ec);
            backend->setProbePath(probe);
        }
    } else {
        backend = FileWatcher::createPolling(interval);
    }
    std::string error;
    if (!backend->start(root, s.sink, error)) {
        const bool native = backend->isNative();
        if (native) {
            std::lock_guard<std::mutex> lock(s.stateMutex);
            s.warnOnceLocked("native", "File watcher: " + error + "; using polling (" + std::to_string(interval.count()) + " ms) instead");
        }
        backend = native ? std::shared_ptr<FileWatcher>(FileWatcher::createPolling(interval)) : nullptr;
        state = State::Degraded;
        if (backend && !backend->start(root, s.sink, error)) backend.reset();
        if (!backend) {
            std::lock_guard<std::mutex> lock(s.stateMutex);
            s.warnOnceLocked("off", "File watcher: " + error + "; external changes are not picked up (Assets > Refresh)");
            state = State::Off;
        }
    }
    {
        std::lock_guard<std::mutex> lock(s.stateMutex);
        s.backend = backend;
        s.state = state;
        s.coalescer.setDeleteHold(backend && !backend->isNative() ? interval + POLL_HOLD_MARGIN : std::chrono::milliseconds(0));
    }

    // 2. the index, off the lock (the editor's writes meanwhile go to the old, empty one
    // and to the journal; the walk sees their result)
    ProjectFileIndex built;
    built.reset(root);
    built.setFollowLinks(true);
    built.build(&s.cancelBuild);
    {
        std::lock_guard<std::mutex> lock(s.stateMutex);
        if (s.cancelBuild) return;
        s.index = std::move(built);
        s.indexReady = true;
    }
    syncLinkedWatches();
    s.stateCv.notify_all();

    // 3. Auto's self-test: does a native event arrive at all (network drives, WSL, Docker
    // bind mounts often miss them)?
    bool probing = false;
    Clock::time_point probeDeadline{};
    if (requested == WatchBackendKind::Auto && state == State::Watching) {
        io::AtomicWriteOptions options;
        options.durability = io::Durability::Cache;
        if (io::writeFileAtomic(probe, "file watcher self-test\n", options).ok()) {
            probing = true;
            probeDeadline = Clock::now() + PROBE_TIMEOUT;
        }
    }

    // 4. raw events in, settled changes out
    while (true) {
        Clock::time_point deadline;
        {
            std::lock_guard<std::mutex> lock(s.stateMutex);
            deadline = s.coalescer.nextDeadline();
        }
        if (probing) deadline = std::min(deadline, probeDeadline);
        {
            std::unique_lock<std::mutex> lock(s.rawMutex);
            auto ready = [&]() { return s.stopRequested || !s.raw.empty() || s.fallbackRequested || (probing && s.probeSeen); };
            // idle: no timeout, no wakeup (S05 R21)
            if (deadline == Clock::time_point::max()) s.rawCv.wait(lock, ready);
            else jobs::detail::timedWait(s.rawCv, lock, deadline - Clock::now(), ready);
            if (s.stopRequested) break;
        }

        bool fallback = false;
        std::string reason;
        bool probeDone = false, probeOk = false;
        {
            std::lock_guard<std::mutex> lock(s.rawMutex);
            fallback = s.fallbackRequested;
            reason = s.fallbackReason;
            s.fallbackRequested = false;
            if (probing && (s.probeSeen || Clock::now() >= probeDeadline)) {
                probeDone = true;
                probeOk = s.probeSeen;
            }
        }
        if (probeDone) {
            probing = false;
            std::error_code ec;
            fs::remove(probe, ec);
            if (!probeOk && !fallback) {
                fallback = true;
                reason = "native events did not arrive within 2 s (a network drive, WSL or a container mount?)";
            }
        }
        if (fallback) switchToPolling(reason);

        {
            std::lock_guard<std::mutex> stateLock(s.stateMutex);
            std::vector<watch::RawEvent> batch;
            {
                std::lock_guard<std::mutex> rawLock(s.rawMutex);
                batch.swap(s.raw);
            }
            const auto now = Clock::now();
            for (const watch::RawEvent& event : batch) s.coalescer.add(event, now);
            std::vector<FileChange> settled = s.coalescer.take(now);
            processLocked(settled);
        }
        syncLinkedWatches();
        deliverWorkers();
        wakeMain();
    }
}

void FileWatchService::syncLinkedWatches() {
    Shared& s = *shared;
    std::lock_guard<std::mutex> syncLock(s.linkedSyncMutex);
    std::map<std::string, fs::path> wanted;
    std::vector<Shared::LinkedWatch> retired;
    bool native = false;
    std::chrono::milliseconds interval{1000};
    fs::path root;
    {
        std::lock_guard<std::mutex> lock(s.stateMutex);
        if (s.linkedVersion == s.index.linkedFoldersVersion()) return;
        s.linkedVersion = s.index.linkedFoldersVersion();
        if (s.backend) wanted = s.index.linkedFolders();
        native = s.backend && s.backend->isNative();
        interval = s.pollInterval;
        root = s.root;
        for (auto it = s.linked.begin(); it != s.linked.end();) {
            auto same = wanted.find(it->key);
            if (same != wanted.end() && same->second == it->real && it->backend && it->backend->isNative() == native) {
                wanted.erase(same);
                ++it;
            } else {
                retired.push_back(std::move(*it));
                it = s.linked.erase(it);
            }
        }
    }
    // off the state lock: a stop joins a thread that may be in the sink, a start walks a tree
    for (Shared::LinkedWatch& entry : retired) {
        if (entry.backend) entry.backend->stop();
    }
    for (const auto& [key, real] : wanted) {
        Shared::LinkedWatch entry;
        entry.key = key;
        entry.real = real;
        entry.sink = std::make_unique<LinkedSink>(s, real, (root / watch::pathFromUtf8(key)).lexically_normal());
        entry.backend = native ? std::shared_ptr<FileWatcher>(FileWatcher::createNative())
                               : std::shared_ptr<FileWatcher>(FileWatcher::createPolling(interval));
        std::string error;
        if (!entry.backend || !entry.backend->start(real, *entry.sink, error)) {
            std::lock_guard<std::mutex> lock(s.stateMutex);
            s.warnOnceLocked("linked:" + key, "File watcher: the linked folder " + key + " is not watched: " + error);
            continue;
        }
        std::lock_guard<std::mutex> lock(s.stateMutex);
        s.linked.push_back(std::move(entry));
    }
}

void FileWatchService::switchToPolling(const std::string& reason) {
    Shared& s = *shared;
    std::shared_ptr<FileWatcher> old;
    fs::path root;
    std::chrono::milliseconds interval;
    {
        std::lock_guard<std::mutex> lock(s.stateMutex);
        if (s.backend && !s.backend->isNative()) return;   // polling already
        old = std::move(s.backend);
        root = s.root;
        interval = s.pollInterval;
        s.warnOnceLocked("fallback", "File watcher: " + reason + "; switched to polling (" + std::to_string(interval.count()) + " ms)");
    }
    if (old) old->stop();
    std::shared_ptr<FileWatcher> polling(FileWatcher::createPolling(interval));
    std::string error;
    const bool ok = polling->start(root, s.sink, error);
    {
        std::lock_guard<std::mutex> lock(s.stateMutex);
        s.backend = ok ? polling : nullptr;
        s.state = ok ? State::Degraded : State::Off;
        s.coalescer.setDeleteHold(interval + POLL_HOLD_MARGIN);
    }
    // the linked folders too (syncLinkedWatches compares each backend's kind)
    {
        std::lock_guard<std::mutex> lock(s.stateMutex);
        s.linkedVersion = s.index.linkedFoldersVersion() - 1;
    }
    syncLinkedWatches();
    // what changed while no watcher ran
    std::lock_guard<std::mutex> lock(s.rawMutex);
    s.raw.push_back(watch::RawEvent{watch::RawEvent::Type::Rescan, root, {}, true});
}

void FileWatchService::processLocked(std::vector<FileChange>& changes) {
    Shared& s = *shared;
    for (FileChange& change : changes) {
        change.origin = ChangeOrigin::External;
        switch (change.kind) {
            case FileChangeKind::Rescan: {
                s.overflows++;
                std::string rel;
                watch::relativeTo(s.root, change.path, rel);
                s.warnOnceLocked("overflow", "File watcher overflow: rescanned " + (rel.empty() ? std::string("the project") : rel));
                std::vector<FileChange> found = s.index.diff(change.path);
                emitLocked(change);
                for (FileChange& item : found) {
                    if (writeLedger.isEchoOfOurWrite(item.path)) {
                        s.echoes++;
                        continue;
                    }
                    emitLocked(std::move(item));
                }
                break;
            }
            case FileChangeKind::Added:
            case FileChangeKind::Modified: {
                watch::PathStat known;
                const bool isKnown = s.index.find(change.path, known);
                watch::PathStat now;
                now.size = change.size;
                now.mtimeNs = change.mtimeNs;
                now.identity = change.identity;
                now.directory = change.directory;
                if (writeLedger.isEchoOfOurWrite(change.path)) {
                    s.echoes++;
                    s.index.set(change.path, now);
                    break;
                }
                if (change.directory) {
                    s.index.set(change.path, now);
                    if (!isKnown) {
                        change.kind = FileChangeKind::Added;
                        emitLocked(change);
                    }
                    // what it holds (moved in from outside, or written before its watch existed)
                    for (FileChange& item : s.index.diff(change.path)) {
                        if (item.path == change.path) continue;
                        if (writeLedger.isEchoOfOurWrite(item.path)) {
                            s.echoes++;
                            continue;
                        }
                        emitLocked(std::move(item));
                    }
                    break;
                }
                // the same bytes as delivered: nothing happened (a touch of attributes, or
                // a change a folder's scan already delivered)
                if (isKnown && known.sameContent(now) && known.identity == now.identity) break;
                change.kind = isKnown ? FileChangeKind::Modified : FileChangeKind::Added;
                s.index.set(change.path, now);
                emitLocked(change);
                break;
            }
            case FileChangeKind::Removed: {
                watch::PathStat known;
                if (!s.index.find(change.path, known)) break;
                const bool echo = writeLedger.isEchoOfOurWrite(change.path);
                std::vector<std::pair<fs::path, watch::PathStat>> inside = s.index.under(change.path);
                s.index.erase(change.path);
                if (echo) {
                    s.echoes++;
                    break;
                }
                // what the folder held, children first, then the folder
                for (auto it = inside.rbegin(); it != inside.rend(); ++it) {
                    FileChange item;
                    item.kind = FileChangeKind::Removed;
                    item.path = it->first;
                    item.directory = it->second.directory;
                    item.size = it->second.size;
                    item.mtimeNs = it->second.mtimeNs;
                    item.identity = it->second.identity;
                    emitLocked(std::move(item));
                }
                change.directory = known.directory;
                change.size = known.size;
                change.mtimeNs = known.mtimeNs;
                change.identity = known.identity;
                emitLocked(change);
                break;
            }
            case FileChangeKind::Moved: {
                watch::PathStat known;
                watch::PathStat now;
                now.size = change.size;
                now.mtimeNs = change.mtimeNs;
                now.identity = change.identity;
                now.directory = change.directory;
                if (!s.index.find(change.oldPath, known)) {
                    // from a path never delivered: an addition here
                    FileChange added = change;
                    added.kind = FileChangeKind::Added;
                    added.oldPath.clear();
                    std::vector<FileChange> one{added};
                    processLocked(one);
                    break;
                }
                const bool echo = writeLedger.isEchoOfOurWrite(change.path) && writeLedger.isEchoOfOurWrite(change.oldPath);
                std::vector<std::pair<fs::path, watch::PathStat>> inside = s.index.under(change.oldPath);
                s.index.move(change.oldPath, change.path);
                s.index.set(change.path, now);
                if (echo) {
                    s.echoes++;
                    break;
                }
                emitLocked(change);
                // a folder's rename is one Moved per entry it held (S05 R5)
                for (const auto& [path, entry] : inside) {
                    std::string rel;
                    if (!watch::relativeTo(change.oldPath, path, rel)) continue;
                    FileChange item;
                    item.kind = FileChangeKind::Moved;
                    item.oldPath = path;
                    item.path = (change.path / watch::pathFromUtf8(rel)).lexically_normal();
                    item.directory = entry.directory;
                    item.size = entry.size;
                    item.mtimeNs = entry.mtimeNs;
                    item.identity = entry.identity;
                    emitLocked(std::move(item));
                }
                break;
            }
        }
    }
}

void FileWatchService::emitLocked(FileChange change) {
    Shared& s = *shared;
    change.sequence = s.nextSequence++;
    // the readers find a change at (sequence - 1) % capacity; after a restart the ring is empty
    // while the sequence goes on, so the slot can be past its end (a push_back put it at 0)
    const size_t slot = static_cast<size_t>((change.sequence - 1) % JOURNAL_CAPACITY);
    if (slot >= s.ring.size()) s.ring.resize(slot + 1);
    s.ring[slot] = change;
    if (s.nextSequence - s.oldestSequence > JOURNAL_CAPACITY) s.oldestSequence = s.nextSequence - JOURNAL_CAPACITY;
    s.delivered++;

    // Auto Refresh off: the journal only, but the editor's own writes still go out
    if (s.paused && change.origin == ChangeOrigin::External) return;

    const std::string ext = watch::lowerExtension(change.path);
    const std::string oldExt = change.kind == FileChangeKind::Moved ? watch::lowerExtension(change.oldPath) : std::string();
    std::lock_guard<std::mutex> lock(s.subscribersMutex);
    for (const std::shared_ptr<Subscriber>& subscriber : s.subscribers) {
        const WatchFilter& filter = subscriber->filter;
        if (change.origin == ChangeOrigin::Editor && !filter.deliverEditorWrites) continue;
        if (change.directory && !filter.directories && change.kind != FileChangeKind::Rescan) continue;
        const fs::path& base = filter.root.empty() ? s.root : filter.root;
        std::string rel, oldRel;
        const bool inside = watch::relativeTo(base, change.path, rel);
        const bool oldInside = change.kind == FileChangeKind::Moved && watch::relativeTo(base, change.oldPath, oldRel);
        if (change.kind == FileChangeKind::Rescan) {
            // a rescan of a folder above the filter's root covers it too
            std::string up;
            if (!inside && !watch::relativeTo(change.path, base, up)) continue;
        } else {
            if (!inside && !oldInside) continue;
            if (!filter.recursive) {
                const bool direct = (inside && rel.find('/') == std::string::npos) || (oldInside && oldRel.find('/') == std::string::npos);
                if (!direct) continue;
            }
            if (!filter.extensions.empty() &&
                std::find(filter.extensions.begin(), filter.extensions.end(), ext) == filter.extensions.end() &&
                (oldExt.empty() || std::find(filter.extensions.begin(), filter.extensions.end(), oldExt) == filter.extensions.end())) {
                continue;
            }
            if (!filter.globs.empty()) {
                bool any = false;
                for (const std::string& glob : filter.globs) {
                    if ((inside && watch::globMatch(glob, rel)) || (oldInside && watch::globMatch(glob, oldRel))) any = true;
                }
                if (!any) continue;
            }
        }
        subscriber->queue.push_back(change);
        if (subscriber->on == DeliverOn::MainThread) s.mainPending = true;
    }
}

// ------------------------------------------------------------------ delivery

FileWatchSubscription FileWatchService::subscribe(WatchFilter filter, FileChangeBatchFn fn, DeliverOn on, int priority) {
    if (!shared || !fn) return {};
    for (std::string& ext : filter.extensions) {
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!ext.empty() && ext[0] != '.') ext = "." + ext;
    }
    if (!filter.root.empty()) filter.root = normalized(filter.root);
    auto subscriber = std::make_shared<Subscriber>();
    subscriber->filter = std::move(filter);
    subscriber->fn = std::move(fn);
    subscriber->on = on;
    subscriber->priority = priority;
    std::lock_guard<std::mutex> lock(shared->subscribersMutex);
    subscriber->id = shared->nextId++;
    auto& list = shared->subscribers;
    // higher priority first; equal priorities in subscription order
    auto at = std::find_if(list.begin(), list.end(), [priority](const std::shared_ptr<Subscriber>& s) { return s->priority < priority; });
    list.insert(at, subscriber);
    return FileWatchSubscription(this, subscriber->id);
}

void FileWatchService::unsubscribe(uint64_t id) {
    if (!shared) return;
    std::unique_lock<std::mutex> lock(shared->subscribersMutex);
    auto& list = shared->subscribers;
    auto it = std::find_if(list.begin(), list.end(), [id](const std::shared_ptr<Subscriber>& s) { return s->id == id; });
    if (it == list.end()) return;
    std::shared_ptr<Subscriber> subscriber = *it;
    subscriber->active = false;
    subscriber->queue.clear();
    list.erase(it);
    // a callback in flight on another thread finishes first (one on this thread is the
    // callback unsubscribing itself)
    const std::thread::id self = std::this_thread::get_id();
    shared->subscribersCv.wait(lock, [&]() { return subscriber->inFlight == 0 || subscriber->callingThread == self; });
}

bool FileWatchService::deliverOne(const std::shared_ptr<Subscriber>& subscriber, size_t maxBatch) {
    std::vector<FileChange> batch;
    {
        std::lock_guard<std::mutex> lock(shared->subscribersMutex);
        if (!subscriber->active || subscriber->queue.empty() || subscriber->inFlight > 0) return false;
        const size_t count = std::min(maxBatch, subscriber->queue.size());
        batch.assign(std::make_move_iterator(subscriber->queue.begin()), std::make_move_iterator(subscriber->queue.begin() + static_cast<std::ptrdiff_t>(count)));
        subscriber->queue.erase(subscriber->queue.begin(), subscriber->queue.begin() + static_cast<std::ptrdiff_t>(count));
        subscriber->inFlight++;
        subscriber->callingThread = std::this_thread::get_id();
    }
    subscriber->fn(batch);
    {
        std::lock_guard<std::mutex> lock(shared->subscribersMutex);
        subscriber->inFlight--;
        subscriber->callingThread = std::thread::id();
    }
    shared->subscribersCv.notify_all();
    return true;
}

void FileWatchService::deliverWorkers() {
    std::vector<std::shared_ptr<Subscriber>> workers;
    {
        std::lock_guard<std::mutex> lock(shared->subscribersMutex);
        for (const auto& subscriber : shared->subscribers) {
            if (subscriber->on == DeliverOn::Worker && !subscriber->queue.empty()) workers.push_back(subscriber);
        }
    }
    for (const auto& subscriber : workers) {
        while (deliverOne(subscriber, WORKER_BATCH_MAX)) {
        }
        // another thread is delivering it: the barrier waits for that to end
        std::unique_lock<std::mutex> lock(shared->subscribersMutex);
        jobs::detail::timedWait(shared->subscribersCv, lock, std::chrono::seconds(5), [&]() { return subscriber->inFlight == 0 || !subscriber->active; });
        const bool more = subscriber->active && !subscriber->queue.empty();
        lock.unlock();
        if (more) {
            while (deliverOne(subscriber, WORKER_BATCH_MAX)) {
            }
        }
    }
}

bool FileWatchService::deliverMain(double budgetMs) {
    const auto start = Clock::now();
    auto spent = [&]() { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); };
    bool progress = true, delivered = false;
    while (progress) {
        progress = false;
        std::vector<std::shared_ptr<Subscriber>> mains;
        {
            std::lock_guard<std::mutex> lock(shared->subscribersMutex);
            for (const auto& subscriber : shared->subscribers) {
                if (subscriber->on == DeliverOn::MainThread && !subscriber->queue.empty()) mains.push_back(subscriber);
            }
            if (mains.empty()) shared->mainPending = false;
        }
        for (const auto& subscriber : mains) {
            if (spent() >= budgetMs) break;
            if (deliverOne(subscriber, MAIN_BATCH_MAX)) progress = delivered = true;
        }
        if (spent() >= budgetMs) break;
    }
    bool left = false;
    {
        std::lock_guard<std::mutex> lock(shared->subscribersMutex);
        for (const auto& subscriber : shared->subscribers) {
            if (subscriber->on == DeliverOn::MainThread && !subscriber->queue.empty()) left = true;
        }
        shared->mainPending = left;
    }
    // the rest waits for the next frame: make sure there is one
    if (left) wakeMain();
    return delivered;
}

bool FileWatchService::pump(double budgetMs) {
    if (!shared) return false;
    if (shared->mainThread.load() == std::thread::id()) shared->mainThread = std::this_thread::get_id();
    if (!shared->mainPending) return false;   // idle: nothing to do, nothing locked (S05 R21)
    return deliverMain(budgetMs);
}

bool FileWatchService::flush(std::chrono::milliseconds timeout) {
    if (!shared) return false;
    Shared& s = *shared;
    const auto deadline = Clock::now() + timeout;
    std::shared_ptr<FileWatcher> backend;
    {
        std::unique_lock<std::mutex> lock(s.stateMutex);
        if (!s.attached || s.requested == WatchBackendKind::Off) return false;
        // a watch just started: its index first
        if (!jobs::detail::timedWait(s.stateCv, lock, deadline - Clock::now(), [&]() { return s.indexReady || !s.attached; }) || !s.attached) return false;
        backend = s.backend;
    }
    if (!backend) return false;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    bool ok = backend->flush(std::max(left, std::chrono::milliseconds(1)));
    std::vector<std::shared_ptr<FileWatcher>> linked;
    {
        std::lock_guard<std::mutex> lock(s.stateMutex);
        for (const Shared::LinkedWatch& entry : s.linked) linked.push_back(entry.backend);
    }
    for (const std::shared_ptr<FileWatcher>& watcher : linked) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        ok = watcher->flush(std::max(remaining, std::chrono::milliseconds(1))) && ok;
    }
    {
        std::lock_guard<std::mutex> stateLock(s.stateMutex);
        std::vector<watch::RawEvent> batch;
        {
            std::lock_guard<std::mutex> rawLock(s.rawMutex);
            batch.swap(s.raw);
        }
        const auto now = Clock::now();
        for (const watch::RawEvent& event : batch) s.coalescer.add(event, now);
        std::vector<FileChange> settled = s.coalescer.take(now, true);
        processLocked(settled);
    }
    syncLinkedWatches();
    s.rawCv.notify_all();   // the dispatcher's next deadline changed
    deliverWorkers();
    if (onMainThread()) deliverMain(std::numeric_limits<double>::infinity());
    else wakeMain();
    return ok;
}

// ------------------------------------------------------------------ the editor's writes

void FileWatchService::reportEditorWrite(const fs::path& pathIn, bool existedBefore) {
    const fs::path path = normalized(pathIn);
    writeLedger.reported(path);
    if (!shared) return;
    {
        std::lock_guard<std::mutex> lock(shared->stateMutex);
        if (!shared->attached || watch::isExcludedPath(shared->root, path)) return;
        watch::PathStat now;
        if (!watch::statPath(path, now)) return;
        const bool isKnown = shared->index.contains(path);
        FileChange change;
        change.kind = (existedBefore || isKnown) ? FileChangeKind::Modified : FileChangeKind::Added;
        change.path = path;
        change.directory = now.directory;
        change.size = now.size;
        change.mtimeNs = now.mtimeNs;
        change.identity = now.identity;
        change.origin = ChangeOrigin::Editor;
        shared->index.set(path, now);
        if (now.directory && isKnown) return;   // a folder that was there: nothing to say
        emitLocked(change);
    }
    wakeMain();
}

void FileWatchService::reportEditorReplacing(const fs::path& pathIn) {
    if (!shared) return;
    const fs::path path = normalized(pathIn);
    {
        std::lock_guard<std::mutex> lock(shared->stateMutex);
        Shared& s = *shared;
        // the index is what was delivered (or written by the editor): only a complete one says
        // a file moved on since
        if (!s.attached || !s.indexReady || watch::isExcludedPath(s.root, path)) return;
        watch::PathStat now;
        if (!watch::statPath(path, now) || now.directory) return;
        watch::PathStat known;
        const bool isKnown = s.index.find(path, known);
        if (isKnown && known.sameContent(now)) return;
        // the editor's own earlier write (announced, or reported) whose events are still on
        // their way: not another program's
        if (writeLedger.isEchoOfOurWrite(path)) return;
        FileChange change;
        change.kind = isKnown ? FileChangeKind::Modified : FileChangeKind::Added;
        change.path = path;
        change.size = now.size;
        change.mtimeNs = now.mtimeNs;
        change.identity = now.identity;
        change.origin = ChangeOrigin::External;
        s.index.set(path, now);
        emitLocked(change);
    }
    wakeMain();
}

void FileWatchService::reportEditorRemoval(const fs::path& pathIn) {
    const fs::path path = normalized(pathIn);
    writeLedger.reportedRemoved(path);
    if (!shared) return;
    {
        std::lock_guard<std::mutex> lock(shared->stateMutex);
        if (!shared->attached || watch::isExcludedPath(shared->root, path)) return;
        watch::PathStat known;
        const bool isKnown = shared->index.find(path, known);
        std::vector<std::pair<fs::path, watch::PathStat>> inside = shared->index.under(path);
        shared->index.erase(path);
        for (auto it = inside.rbegin(); it != inside.rend(); ++it) {
            FileChange item;
            item.kind = FileChangeKind::Removed;
            item.path = it->first;
            item.directory = it->second.directory;
            item.origin = ChangeOrigin::Editor;
            emitLocked(std::move(item));
        }
        FileChange change;
        change.kind = FileChangeKind::Removed;
        change.path = path;
        change.directory = isKnown && known.directory;
        change.origin = ChangeOrigin::Editor;
        emitLocked(change);
    }
    wakeMain();
}

void FileWatchService::reportEditorMove(const fs::path& fromIn, const fs::path& toIn) {
    const fs::path from = normalized(fromIn), to = normalized(toIn);
    writeLedger.reportedRemoved(from);
    writeLedger.reported(to);
    if (!shared) return;
    {
        std::lock_guard<std::mutex> lock(shared->stateMutex);
        if (!shared->attached) return;
        const bool fromInside = !watch::isExcludedPath(shared->root, from);
        const bool toInside = !watch::isExcludedPath(shared->root, to);
        if (!fromInside && !toInside) return;
        watch::PathStat now;
        const bool exists = watch::statPath(to, now);
        if (!fromInside || !toInside) {
            // into or out of the watched tree: an addition, or a removal
            if (toInside && exists) {
                shared->index.set(to, now);
                FileChange change;
                change.kind = FileChangeKind::Added;
                change.path = to;
                change.directory = now.directory;
                change.size = now.size;
                change.mtimeNs = now.mtimeNs;
                change.identity = now.identity;
                change.origin = ChangeOrigin::Editor;
                emitLocked(change);
                if (now.directory) {
                    for (FileChange& item : shared->index.diff(to)) {
                        if (item.path == to) continue;
                        item.origin = ChangeOrigin::Editor;
                        emitLocked(std::move(item));
                    }
                }
            } else if (fromInside) {
                FileChange change;
                change.kind = FileChangeKind::Removed;
                change.path = from;
                change.origin = ChangeOrigin::Editor;
                watch::PathStat known;
                change.directory = shared->index.find(from, known) && known.directory;
                shared->index.erase(from);
                emitLocked(change);
            }
        } else {
            std::vector<std::pair<fs::path, watch::PathStat>> inside = shared->index.under(from);
            shared->index.move(from, to);
            if (exists) shared->index.set(to, now);
            FileChange change;
            change.kind = FileChangeKind::Moved;
            change.path = to;
            change.oldPath = from;
            change.directory = exists && now.directory;
            change.size = now.size;
            change.mtimeNs = now.mtimeNs;
            change.identity = now.identity;
            change.origin = ChangeOrigin::Editor;
            emitLocked(change);
            for (const auto& [path, entry] : inside) {
                std::string rel;
                if (!watch::relativeTo(from, path, rel)) continue;
                FileChange item;
                item.kind = FileChangeKind::Moved;
                item.oldPath = path;
                item.path = (to / watch::pathFromUtf8(rel)).lexically_normal();
                item.directory = entry.directory;
                item.size = entry.size;
                item.mtimeNs = entry.mtimeNs;
                item.identity = entry.identity;
                item.origin = ChangeOrigin::Editor;
                emitLocked(std::move(item));
            }
        }
    }
    wakeMain();
}

// ------------------------------------------------------------------ journal and stats

std::vector<FileChange> FileWatchService::journalSince(uint64_t& cursor, size_t max) const {
    std::vector<FileChange> out;
    if (!shared) return out;
    std::lock_guard<std::mutex> lock(shared->stateMutex);
    const Shared& s = *shared;
    if (cursor < s.oldestSequence) {
        // fell behind the ring (or the project changed): everything may have changed
        FileChange rescan;
        rescan.kind = FileChangeKind::Rescan;
        rescan.path = s.root;
        rescan.directory = true;
        rescan.sequence = s.nextSequence - 1;
        out.push_back(rescan);
        cursor = s.nextSequence;
        return out;
    }
    for (; cursor < s.nextSequence && out.size() < max; cursor++) {
        out.push_back(s.ring[(cursor - 1) % JOURNAL_CAPACITY]);
    }
    return out;
}

uint64_t FileWatchService::journalHead() const {
    if (!shared) return 1;
    std::lock_guard<std::mutex> lock(shared->stateMutex);
    return shared->nextSequence;
}

size_t FileWatchService::externalChangesSince(uint64_t cursor) const {
    if (!shared) return 0;
    std::lock_guard<std::mutex> lock(shared->stateMutex);
    const Shared& s = *shared;
    if (cursor < s.oldestSequence) return static_cast<size_t>(s.nextSequence - s.oldestSequence);
    size_t count = 0;
    for (uint64_t sequence = cursor; sequence < s.nextSequence; sequence++) {
        const FileChange& change = s.ring[(sequence - 1) % JOURNAL_CAPACITY];
        if (change.origin == ChangeOrigin::External && change.kind != FileChangeKind::Rescan && !change.directory) count++;
    }
    return count;
}

FileWatchService::Stats FileWatchService::stats() const {
    Stats out;
    if (!shared) return out;
    std::shared_ptr<FileWatcher> backend;
    {
        std::lock_guard<std::mutex> lock(shared->stateMutex);
        const Shared& s = *shared;
        backend = s.backend;
        out.state = s.state;
        out.paused = s.paused;
        out.roots = s.attached && backend ? 1 + s.linked.size() : 0;
        out.delivered = s.delivered;
        out.coalesced = s.coalescer.coalescedCount();
        out.echoesSuppressed = s.echoes;
        out.overflows = s.overflows;
        out.pending = s.coalescer.pendingCount();
        out.indexed = s.index.size();
        out.indexReady = s.indexReady;
        out.pollInterval = s.pollInterval;
    }
    {
        std::lock_guard<std::mutex> lock(shared->rawMutex);
        out.events = shared->rawCount;
        out.pending += shared->raw.size();
    }
    {
        std::lock_guard<std::mutex> lock(shared->subscribersMutex);
        for (const auto& subscriber : shared->subscribers) out.pending += subscriber->queue.size();
    }
    if (backend) {
        out.backend = backend->name();
        out.watches = backend->watchCount();
    }
    return out;
}

void FileWatchService::setSuppressDeliveryForTesting(bool suppress) {
    if (!shared) return;
    std::shared_ptr<FileWatcher> backend;
    {
        std::lock_guard<std::mutex> lock(shared->stateMutex);
        backend = shared->backend;
    }
    if (backend) backend->setSuppressDeliveryForTesting(suppress);
}

void FileWatchService::injectOverflowForTesting() {
    if (!shared) return;
    std::shared_ptr<FileWatcher> backend;
    {
        std::lock_guard<std::mutex> lock(shared->stateMutex);
        backend = shared->backend;
    }
    if (backend) backend->injectOverflowForTesting();
}

bool FileWatchService::indexMatchesDiskForTesting() {
    if (!shared) return false;
    flush(std::chrono::milliseconds(2000));
    std::lock_guard<std::mutex> lock(shared->stateMutex);
    return shared->indexReady && shared->index.matchesDisk();
}

// ------------------------------------------------------------------ ScopedExpectedWrite

ScopedExpectedWrite::ScopedExpectedWrite(const fs::path& targetIn, bool subtree)
    : target(normalized(targetIn)) {
    std::error_code ec;
    existedBefore = fs::exists(target, ec);
    registration = FileWatchService::get().ledger().expect(target, subtree);
}

ScopedExpectedWrite::~ScopedExpectedWrite() {
    FileWatchService::get().ledger().retire(registration);
}

void ScopedExpectedWrite::reported(const fs::path& written) {
    const fs::path path = normalized(written);
    FileWatchService::get().reportEditorWrite(path, path == target && existedBefore);
}

void ScopedExpectedWrite::reported() {
    reported(target);
}

void ScopedExpectedWrite::reportedRemoved(const fs::path& removed) {
    FileWatchService::get().reportEditorRemoval(removed);
}

void ScopedExpectedWrite::reportedMoved(const fs::path& from, const fs::path& to) {
    FileWatchService::get().reportEditorMove(from, to);
}

void ScopedExpectedWrite::notePublished(const fs::path& target) {
    // after the fact: whether it is new, the index says
    FileWatchService::get().reportEditorWrite(target, false);
}
