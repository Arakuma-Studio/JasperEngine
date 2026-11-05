// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The RAII subscription that waits for an in-flight callback, the stop-outside-the-lock rule
// and the priorities follow OpenEngine's FileWatchingService (MIT, NOTICE.md).

#ifndef JASPERENGINE_FILEWATCHSERVICE_H
#define JASPERENGINE_FILEWATCHSERVICE_H

#include "Export.h"
#include "ExpectedWriteLedger.h"
#include "FileWatcher.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace jasperengine {

    class FileWatchService;

    // What a subscriber wants (S05 R8). Empty lists match everything.
    struct JASPERENGINE_API WatchFilter {
        std::filesystem::path root;             // only under this folder (empty: the watched root)
        std::vector<std::string> extensions;    // lower-case, with the dot
        std::vector<std::string> globs;         // relative to `root` (FileWatcher's globMatch)
        bool recursive = true;
        bool directories = false;               // folder events too
        bool deliverEditorWrites = false;       // the editor's own writes (origin Editor, R10)
    };

    enum class DeliverOn : uint8_t { MainThread, Worker };

    using FileChangeBatchFn = std::function<void(std::span<const FileChange>)>;

    // Move-only. Destroying (or reset()) returns only after a callback in flight on another
    // thread has finished, and the callback is never called again. A callback may destroy its
    // own subscription.
    class JASPERENGINE_API FileWatchSubscription {
    public:
        FileWatchSubscription() = default;
        FileWatchSubscription(FileWatchSubscription&& other) noexcept;
        FileWatchSubscription& operator=(FileWatchSubscription&& other) noexcept;
        FileWatchSubscription(const FileWatchSubscription&) = delete;
        FileWatchSubscription& operator=(const FileWatchSubscription&) = delete;
        ~FileWatchSubscription();

        void reset();
        explicit operator bool() const { return service != nullptr; }

    private:
        friend class FileWatchService;
        FileWatchSubscription(FileWatchService* service, uint64_t id);

        FileWatchService* service = nullptr;
        uint64_t id = 0;
    };

    // One watcher per project root, a coalescer, the self-write ledger, a journal and the
    // delivery of settled changes in batches (S05 R1-R10). The engine itself starts nothing:
    // the editor starts it when a project opens and stops it before a project switch.
    //
    // Threads: the backend's (raw events), the service's dispatcher (coalescing, the index,
    // worker deliveries) and the main thread (pump: main-thread deliveries). Started on
    // Android, iOS and the web it does nothing (R20).
    class JASPERENGINE_API FileWatchService {
    public:
        enum class State : uint8_t { Off, Watching, Polling, Degraded };

        struct Stats {
            std::string backend;      // "inotify", "ReadDirectoryChangesW", "FSEvents", "polling", "" (off)
            State state = State::Off;
            bool paused = false;      // Auto Refresh off
            size_t roots = 0;
            size_t watches = 0;
            uint64_t events = 0;      // raw events from the backend
            uint64_t delivered = 0;   // settled changes journaled
            uint64_t coalesced = 0;   // raw events folded into another
            uint64_t echoesSuppressed = 0;
            uint64_t overflows = 0;
            size_t pending = 0;       // not settled yet, or waiting for the main thread
            size_t indexed = 0;       // entries in the index
            bool indexReady = false;
            std::chrono::milliseconds pollInterval{1000};
        };

        static FileWatchService& get();

        FileWatchService();
        ~FileWatchService();
        FileWatchService(const FileWatchService&) = delete;
        FileWatchService& operator=(const FileWatchService&) = delete;

        // Watches `root` (absolute). True when a watcher runs (native, or polling for
        // Polling, or for Auto/Native when the native one fails); false for Off and on the
        // platforms without watching. With Off the root is still attached: the editor's own
        // writes (reportEditorWrite) are delivered. The calling thread is the main thread.
        bool start(const std::filesystem::path& root, WatchBackendKind kind,
                   std::chrono::milliseconds pollInterval = std::chrono::milliseconds(1000));
        // Stops and detaches; the backend and the dispatcher join outside the service's lock
        void stop();
        bool isAttached() const;
        std::filesystem::path getRoot() const;
        WatchBackendKind getRequestedKind() const;

        [[nodiscard]] FileWatchSubscription subscribe(WatchFilter filter, FileChangeBatchFn fn,
                                                      DeliverOn on = DeliverOn::MainThread, int priority = 0);

        // Main thread, every editor loop iteration: main-thread batches until budgetMs is
        // spent; the rest waits for the next call (R9, R22). True when it delivered some.
        bool pump(double budgetMs);
        // R7 barrier: when it returns true, every change that completed on disk before the
        // call has been delivered (main-thread subscribers too when called on the main
        // thread), the quiet window bypassed. False: no watcher, or the timeout passed.
        bool flush(std::chrono::milliseconds timeout);
        // The same barrier for the journal only: main-thread subscribers get their batches from
        // the next pump, so nothing they do runs in the caller (a settle on the way to a result)
        bool flushJournal(std::chrono::milliseconds timeout);
        // The barrier only drains what the system holds (inotify, ReadDirectoryChangesW), rather
        // than a marker's trip through fseventsd or a walk of the project
        bool flushIsDrain() const;

        // Auto Refresh off (R15): changes still reach the index and the journal, not the
        // subscribers. The editor's own writes are delivered all the same.
        void setPaused(bool paused);
        bool isPaused() const;

        // Called on the dispatcher when main-thread batches are waiting (the editor wakes its
        // idle loop with it, R9)
        void setMainThreadWaker(std::function<void()> waker);

        void setQuietWindow(std::chrono::milliseconds quiet);
        // Where the watcher's warnings go (each once: a fallback to polling, an overflow);
        // the "io" log category by default. Any thread.
        void setWarningHandler(std::function<void(const std::string& message)> handler);

        ExpectedWriteLedger& ledger() { return writeLedger; }

        // The editor wrote (or removed, or renamed) a file: the ledger learns its bytes, and
        // subscribers with deliverEditorWrites get the change with origin Editor now (R10).
        // ScopedExpectedWrite calls these.
        void reportEditorWrite(const std::filesystem::path& path, bool existedBefore);
        // The editor is about to replace `path` (the atomic writer, before its rename): what
        // another program wrote there and the watcher has not delivered yet goes out now, with
        // origin External. Once the editor's bytes are there, its pending events would look
        // like the echo of the editor's write (the ledger compares the bytes) and the edit
        // would be lost: an agent's .anim edit that the asset database's GUID line rewrites
        // before the coalescer settles it was never reloaded nor reported.
        void reportEditorReplacing(const std::filesystem::path& path);
        void reportEditorRemoval(const std::filesystem::path& path);
        void reportEditorMove(const std::filesystem::path& from, const std::filesystem::path& to);

        // The journal (D4): a ring of 65,536 changes with a monotonic sequence. `cursor` is
        // the next sequence to read; returns the changes from it (and moves it). A consumer
        // that fell behind the ring gets one Rescan of the root instead.
        std::vector<FileChange> journalSince(uint64_t& cursor, size_t max = SIZE_MAX) const;
        // The sequence the next change will get (a consumer starting now)
        uint64_t journalHead() const;
        // External changes in the journal from `cursor` on (Auto Refresh off: status bar)
        size_t externalChangesSince(uint64_t cursor) const;

        Stats stats() const;
        static const char* stateName(State state);   // "off", "watching", "polling", "degraded"

        // Test hooks (AC5): the backend's
        void setSuppressDeliveryForTesting(bool suppress);
        void injectOverflowForTesting();
        // Whether the index is exactly the disk now (it waits for the dispatcher to be idle)
        bool indexMatchesDiskForTesting();

    private:
        friend class FileWatchSubscription;
        struct Subscriber;
        class Sink;
        class CoalescerEnvironment;
        class LinkedSink;
        struct Shared;

        std::unique_ptr<Shared> shared;
        ExpectedWriteLedger writeLedger;

        void unsubscribe(uint64_t id);
        void dispatcherLoop();
        bool barrier(std::chrono::milliseconds timeout, bool deliverToMainHere);
        void switchToPolling(const std::string& reason);
        // A backend for each folder the index went through a link to (S68 R8: backends do not cross
        // links, so what such a folder holds is watched from its own real path)
        void syncLinkedWatches();
        // Turns settled changes into delivered ones (index, ledger, journal, queues). Holds
        // the state lock.
        void processLocked(std::vector<FileChange>& changes);
        void emitLocked(FileChange change);
        void deliverWorkers();
        bool deliverMain(double budgetMs);
        bool deliverOne(const std::shared_ptr<Subscriber>& subscriber, size_t maxBatch);
        void wakeMain();
        bool onMainThread() const;
    };

    // RAII around one editor write (S05 R10): announce, write, report, retire. Construct it
    // before touching the file; call reported() once the bytes are on disk.
    class JASPERENGINE_API ScopedExpectedWrite {
    public:
        explicit ScopedExpectedWrite(const std::filesystem::path& target, bool subtree = false);
        ~ScopedExpectedWrite();
        ScopedExpectedWrite(const ScopedExpectedWrite&) = delete;
        ScopedExpectedWrite& operator=(const ScopedExpectedWrite&) = delete;

        // `written` (the target by default) is on disk: dispatches origin=Editor
        void reported(const std::filesystem::path& written);
        void reported();
        // `removed` was deleted; `from` was renamed to `to`
        void reportedRemoved(const std::filesystem::path& removed);
        void reportedMoved(const std::filesystem::path& from, const std::filesystem::path& to);

        // A write already done (a writer that cannot be wrapped): announce + report + retire
        static void notePublished(const std::filesystem::path& target);

    private:
        std::filesystem::path target;
        bool existedBefore = false;
        ExpectedWriteLedger::Registration registration = ExpectedWriteLedger::noRegistration;
    };

}

#endif // JASPERENGINE_FILEWATCHSERVICE_H
