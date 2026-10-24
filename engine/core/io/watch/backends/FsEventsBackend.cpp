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

#include <mutex>

using namespace jasperengine;

namespace fs = std::filesystem;

namespace {

    constexpr CFAbsoluteTime LATENCY_SECONDS = 0.05;

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
        fs::path root;
        // what FSEvents names paths by: the root with its symlinks resolved (/var/folders/...
        // is reported as /private/var/folders/...)
        fs::path realRoot;
        WatchSink* sink = nullptr;
        FSEventStreamRef stream = nullptr;
        dispatch_queue_t queue = nullptr;
        std::timed_mutex flushMutex;

        static void callback(ConstFSEventStreamRef, void* info, size_t count, void* paths, const FSEventStreamEventFlags* flags,
                             const FSEventStreamEventId*);
        void handle(size_t count, const char* const* paths, const FSEventStreamEventFlags* flags);
        // an event path in the root's own spelling
        fs::path underRoot(const fs::path& path) const;
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
    std::vector<RawEvent> events;
    bool rootChanged = false;
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
        if (!probePath.empty() && path == probePath) {
            if (!(flag & kFSEventStreamEventFlagItemRemoved)) sink->onProbe();
            continue;
        }
        if (path == root || isExcludedPath(root, path)) continue;
        events.push_back(RawEvent{RawEvent::Type::Changed, path, {}, (flag & kFSEventStreamEventFlagItemIsDir) != 0});
    }
    if (!events.empty() && !suppressForTesting) sink->onRawEvents(std::move(events));
    if (rootChanged) sink->onBackendFailed("the project folder was moved or deleted");
}

fs::path FsEventsBackend::underRoot(const fs::path& path) const {
    if (realRoot == root) return path;
    const fs::path rel = path.lexically_relative(realRoot);
    if (rel.empty() || rel.is_absolute() || *rel.begin() == "..") return path;
    return rel == "." ? root : (root / rel).lexically_normal();
}

bool FsEventsBackend::flush(std::chrono::milliseconds timeout) {
    std::unique_lock<std::timed_mutex> lock(flushMutex, std::defer_lock);
    if (!lock.try_lock_for(timeout) || !stream) return false;
    // runs the callback for every pending event before it returns (S05 R7)
    FSEventStreamFlushSync(stream);
    return true;
}

void FsEventsBackend::injectOverflowForTesting() {
    // what handle() does with MustScanSubDirs on the root
    std::vector<RawEvent> events{RawEvent{RawEvent::Type::Rescan, root, {}, true}};
    sink->onRawEvents(std::move(events));
}

std::unique_ptr<FileWatcher> jasperengine::watch::createFsEventsBackend() {
    return std::make_unique<FsEventsBackend>();
}

#endif
