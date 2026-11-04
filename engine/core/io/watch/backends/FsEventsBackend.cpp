// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// macOS: one FSEvents stream per root with per-file events, 50 ms latency (S05 R18). The
// stream's flags are cumulative, so each reported path is a "something changed here" that
// the stat and the index decide (FileChangeCoalescer, RawEvent::Changed). The stream setup
// and the drain on stop follow OpenEngine's FileWatcher::StartChangeSignal (MIT, NOTICE.md).

#include "Backends.h"

#if defined(JASPERENGINE_WATCH_FSEVENTS)

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;

namespace fs = std::filesystem;

namespace {

    constexpr CFAbsoluteTime LATENCY_SECONDS = 0.05;
    // the barrier's marker files, in the root's own excluded folder
    const char* const FLUSH_MARKER_FOLDER = ".jasperengine";
    const char* const FLUSH_MARKER_PREFIX = "watch-flush-";
    // a marker not reported this long after it was written is written again (one written while
    // the stream was not live yet is never reported)
    constexpr std::chrono::milliseconds MARKER_REWRITE{100};
    // how long start() waits for a stream to report changes (the live ones took about a second at
    // most, over 900 streams on macOS 26), and how many streams it tries
    constexpr std::chrono::milliseconds START_LIVE_TIMEOUT{1500};
    constexpr int START_ATTEMPTS = 3;
    // setFsEventsDeadStreamsForTesting: how many of the next streams report nothing
    std::atomic<int> deadStreamsForTesting{0};
    // how often a barrier asks fseventsd again for what it holds
    constexpr std::chrono::milliseconds FLUSH_ASK{2};
    // a barrier without a marker: the longest a flush's events take to be reported
    constexpr std::chrono::milliseconds PLAIN_FLUSH_WAIT{100};

}

namespace jasperengine::watch {

    class FsEventsBackend : public FileWatcher {
    public:
        ~FsEventsBackend() override { stop(); }

        bool start(const fs::path& rootIn, WatchSink& sinkIn, std::string& error) override;
        void stop() override;
        bool flush(std::chrono::milliseconds timeout) override;
        const char* name() const override { return "FSEvents"; }
        bool isNative() const override { return true; }
        size_t watchCount() const override { return stream ? 1 : 0; }
        void injectOverflowForTesting() override;

    private:
        // the stream on `realRoot`, not yet known to be live; false when it cannot be made
        bool openStream(std::string& error);
        void closeStream();

        fs::path root;
        // what FSEvents names paths by: the root with its symlinks resolved (/var/folders/...
        // is reported as /private/var/folders/...)
        fs::path realRoot;
        WatchSink* sink = nullptr;
        FSEventStreamRef stream = nullptr;
        dispatch_queue_t queue = nullptr;
        bool deadForTesting = false;
        std::mutex flushMutex;
        // the barrier (flush, start): the markers of the barrier in progress are those numbered
        // from markerFloor (0: none in progress) up to markerGeneration, the last one written;
        // markerSeen once the stream reported one of them (markerMutex, markerSeenCv)
        std::mutex markerMutex;
        std::condition_variable markerSeenCv;
        uint64_t markerFloor = 0;
        uint64_t markerGeneration = 0;
        bool markerSeen = false;

        static void callback(ConstFSEventStreamRef, void* info, size_t count, void* paths, const FSEventStreamEventFlags* flags,
                             const FSEventStreamEventId*);
        void handle(size_t count, const char* const* paths, const FSEventStreamEventFlags* flags);
        // an event path in the root's own spelling
        fs::path underRoot(const fs::path& path) const;
        // the generation of `path` when it is a marker of the barrier in progress, else 0
        uint64_t flushMarkerOf(const fs::path& path);
        // that marker was reported, and the events before it handed to the sink
        void markerDelivered(uint64_t generation);
        // writes the next marker of the barrier in progress: its path, empty when it cannot
        fs::path writeMarker();
        // until the stream reports a marker written now, or the deadline (flushMutex held)
        bool barrier(std::chrono::steady_clock::time_point deadline);
    };

}

using namespace jasperengine::watch;

bool FsEventsBackend::start(const fs::path& rootIn, WatchSink& sinkIn, std::string& error) {
    stop();
    root = rootIn.lexically_normal();
    sink = &sinkIn;
    std::error_code ec;
    realRoot = fs::weakly_canonical(root, ec).lexically_normal();
    if (ec || realRoot.empty()) realRoot = root;

    // A stream that starts "since now" reports nothing until fseventsd registered it, which
    // FSEventStreamStart does not wait for: a change in the first moments was lost now and
    // then (FileWatcherTests.CreateModifyDeleteRename/Native on macOS 26). Started means live.
    // On macOS 26 some new streams, 1 to 9 in 100 from run to run, are not served at all (any
    // process, with or without a flush): 20 s later they still report nothing, so they are replaced,
    // and a later one on the same root is served. With none live the service polls.
    std::lock_guard<std::mutex> lock(flushMutex);
    for (int attempt = 0; attempt < START_ATTEMPTS; attempt++) {
        if (!openStream(error)) return false;
        if (barrier(std::chrono::steady_clock::now() + START_LIVE_TIMEOUT)) return true;
        closeStream();
    }
    error = "FSEvents reports nothing for this folder";
    return false;
}

bool FsEventsBackend::openStream(std::string& error) {
    deadForTesting = false;
    for (int dead = deadStreamsForTesting.load(); dead > 0;) {
        if (deadStreamsForTesting.compare_exchange_weak(dead, dead - 1)) {
            deadForTesting = true;
            break;
        }
    }
    CFStringRef path = CFStringCreateWithCString(nullptr, realRoot.c_str(), kCFStringEncodingUTF8);
    if (!path) {
        error = "the project path is not valid UTF-8";
        return false;
    }
    CFArrayRef paths = CFArrayCreate(nullptr, reinterpret_cast<const void**>(&path), 1, &kCFTypeArrayCallBacks);
    CFRelease(path);
    if (!paths) {
        error = "CFArrayCreate failed";
        return false;
    }

    FSEventStreamContext context{};
    context.info = this;
    stream = FSEventStreamCreate(nullptr, &FsEventsBackend::callback, &context, paths, kFSEventStreamEventIdSinceNow, LATENCY_SECONDS,
                                 kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer | kFSEventStreamCreateFlagWatchRoot);
    CFRelease(paths);
    if (!stream) {
        error = "FSEventStreamCreate failed";
        return false;
    }
    queue = dispatch_queue_create("org.jasperengine.filewatcher", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(stream, queue);
    if (!FSEventStreamStart(stream)) {
        FSEventStreamSetDispatchQueue(stream, nullptr);
        FSEventStreamInvalidate(stream);
        FSEventStreamRelease(stream);
        stream = nullptr;
        dispatch_release(queue);
        queue = nullptr;
        error = "FSEventStreamStart failed";
        return false;
    }
    return true;
}

void FsEventsBackend::stop() {
    closeStream();
}

void FsEventsBackend::closeStream() {
    if (stream) {
        FSEventStreamStop(stream);
        FSEventStreamSetDispatchQueue(stream, nullptr);
        FSEventStreamInvalidate(stream);
        FSEventStreamRelease(stream);
        stream = nullptr;
    }
    if (queue) {
        // serial queue: this empty block runs after a callback already on it returned, so no
        // callback touches `this` once it is done
        dispatch_sync_f(queue, nullptr, [](void*) {});
        dispatch_release(queue);
        queue = nullptr;
    }
}

void FsEventsBackend::callback(ConstFSEventStreamRef, void* info, size_t count, void* paths, const FSEventStreamEventFlags* flags,
                               const FSEventStreamEventId*) {
    static_cast<FsEventsBackend*>(info)->handle(count, static_cast<const char* const*>(paths), flags);
}

void FsEventsBackend::handle(size_t count, const char* const* paths, const FSEventStreamEventFlags* flags) {
    if (deadForTesting) return;
    std::vector<RawEvent> events;
    bool rootChanged = false;
    uint64_t marker = 0;
    for (size_t i = 0; i < count; i++) {
        const fs::path path = underRoot(fs::path(paths[i]).lexically_normal());
        const FSEventStreamEventFlags flag = flags[i];
        if (flag & kFSEventStreamEventFlagRootChanged) {
            rootChanged = true;
            continue;
        }
        if (flag & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped)) {
            // events were coalesced away or dropped: a rescan finds them (S05 R6)
            const bool inside = !isExcludedPath(root, path);
            events.push_back(RawEvent{RawEvent::Type::Rescan, inside ? path : root, {}, true});
            continue;
        }
        if (const uint64_t generation = flushMarkerOf(path)) {
            marker = generation;
            continue;
        }
        if (!probePath.empty() && path == probePath) {
            if (!(flag & kFSEventStreamEventFlagItemRemoved)) sink->onProbe();
            continue;
        }
        if (path == root || isExcludedPath(root, path)) continue;
        events.push_back(RawEvent{RawEvent::Type::Changed, path, {}, (flag & kFSEventStreamEventFlagItemIsDir) != 0});
    }
    if (!events.empty() && !suppressForTesting) sink->onRawEvents(std::move(events));
    // after the batch reached the sink: the flush that waits for it may return at once
    if (marker) markerDelivered(marker);
    if (rootChanged) sink->onBackendFailed("the project folder was moved or deleted");
}

fs::path FsEventsBackend::underRoot(const fs::path& path) const {
    if (realRoot == root) return path;
    const fs::path rel = path.lexically_relative(realRoot);
    if (rel.empty() || rel.is_absolute() || *rel.begin() == "..") return path;
    return rel == "." ? root : (root / rel).lexically_normal();
}

uint64_t FsEventsBackend::flushMarkerOf(const fs::path& path) {
    const std::string name = path.filename().string();
    const size_t prefix = std::char_traits<char>::length(FLUSH_MARKER_PREFIX);
    if (name.compare(0, prefix, FLUSH_MARKER_PREFIX) != 0 || path.parent_path() != root / FLUSH_MARKER_FOLDER) return 0;
    uint64_t generation = 0;
    for (size_t i = prefix; i < name.size(); i++) {
        if (name[i] < '0' || name[i] > '9') return 0;
        generation = generation * 10 + (uint64_t)(name[i] - '0');
    }
    std::lock_guard<std::mutex> lock(markerMutex);
    return (markerFloor != 0 && generation >= markerFloor && generation <= markerGeneration) ? generation : 0;
}

void FsEventsBackend::markerDelivered(uint64_t generation) {
    std::lock_guard<std::mutex> lock(markerMutex);
    // a barrier that gave up meanwhile does not count for the next one
    if (markerFloor == 0 || generation < markerFloor) return;
    markerSeen = true;
    markerSeenCv.notify_all();
}

fs::path FsEventsBackend::writeMarker() {
    fs::path path;
    {
        std::lock_guard<std::mutex> lock(markerMutex);
        path = root / FLUSH_MARKER_FOLDER / (FLUSH_MARKER_PREFIX + std::to_string(++markerGeneration));
    }
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return {};
    ::close(fd);
    return path;
}

// A flush alone is no barrier (S05 R7): it delivers what fseventsd holds, and a write that
// completed just before it has usually not reached fseventsd yet (999 of 1,000 misses in
// FileWatchServiceTests.FlushBarrier on macOS 26). FSEvents delivers in event id order, so once
// the stream reports a marker file written now, every change completed before it has been
// delivered too. Markers are numbered: a late one of an earlier barrier is never taken for this
// one's. Without a writable root it is the plain flush.
// FSEventStreamFlushAsync, never FlushSync: FlushSync has no timeout, and on a stream fseventsd
// does not serve (start()) it never returns, past the deadline, so that stop() never ended either.
bool FsEventsBackend::barrier(std::chrono::steady_clock::time_point deadline) {
    std::error_code ec;
    fs::create_directories(root / FLUSH_MARKER_FOLDER, ec);
    {
        std::lock_guard<std::mutex> lock(markerMutex);
        markerFloor = markerGeneration + 1;
        markerSeen = false;
    }
    std::vector<fs::path> written;
    fs::path path = writeMarker();
    if (path.empty()) {
        std::lock_guard<std::mutex> lock(markerMutex);
        markerFloor = 0;
        FSEventStreamFlushAsync(stream);
        std::this_thread::sleep_until(std::min(deadline, std::chrono::steady_clock::now() + PLAIN_FLUSH_WAIT));
        return true;
    }
    written.push_back(path);
    auto rewriteAt = std::chrono::steady_clock::now() + MARKER_REWRITE;

    bool seen = false;
    while (true) {
        // the callback reports what fseventsd holds, the marker among it, on the stream's queue
        FSEventStreamFlushAsync(stream);
        const auto now = std::chrono::steady_clock::now();
        {
            std::unique_lock<std::mutex> lock(markerMutex);
            markerSeenCv.wait_until(lock, std::min(deadline, now + FLUSH_ASK), [this]() { return markerSeen; });
            if (markerSeen || std::chrono::steady_clock::now() >= deadline) {
                seen = markerSeen;
                markerFloor = 0;
                break;
            }
        }
        if (std::chrono::steady_clock::now() >= rewriteAt) {
            path = writeMarker();
            if (!path.empty()) written.push_back(path);
            rewriteAt = std::chrono::steady_clock::now() + MARKER_REWRITE;
        }
    }
    for (const fs::path& marker : written) fs::remove(marker, ec);
    return seen;
}

bool FsEventsBackend::flush(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (!lockWithin(flushMutex, timeout)) return false;
    std::unique_lock<std::mutex> lock(flushMutex, std::adopt_lock);
    if (!stream) return false;
    return barrier(deadline);
}

void FsEventsBackend::injectOverflowForTesting() {
    // what handle() does with MustScanSubDirs on the root
    std::vector<RawEvent> events{RawEvent{RawEvent::Type::Rescan, root, {}, true}};
    sink->onRawEvents(std::move(events));
}

std::unique_ptr<FileWatcher> jasperengine::watch::createFsEventsBackend() {
    return std::make_unique<FsEventsBackend>();
}

void jasperengine::watch::setFsEventsDeadStreamsForTesting(int count) {
    deadStreamsForTesting = count;
}

#endif
