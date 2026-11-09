// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's AssetIOService (Engine/Include/Assets/AssetIOService.h,
// Engine/Source/Assets/AssetIOService.cpp), Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors, MIT License. See NOTICE.md. Changes: reads through FileData (resource packs,
// Android assets), the decode continuation is a typed Task<T> handed back at submit, waiters
// can run a queued read themselves, and the gate is the job system's BackgroundGate.

#ifndef ASSETIOSERVICE_H
#define ASSETIOSERVICE_H

#include "Export.h"
#include "jobs/JobSystem.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace jasperengine {

    // A loader's intent (D11): a reader claims the highest admissible priority, FIFO within one.
    enum class IOPriority : uint8_t {
        Low,       // prefetch
        Normal,
        High,      // visible now
        Critical,  // the scene waits on it
    };
    constexpr size_t kIOPriorityCount = 4;

    JASPERENGINE_API const char* ioPriorityName(IOPriority priority);

    // Raised (as the decode task's exception) when the read failed or was rejected.
    class JASPERENGINE_API AssetIOError : public std::runtime_error {
    public:
        explicit AssetIOError(const std::string& message);
    };

    class AssetIOService;
    namespace detail {
        struct IoRequest;
        JASPERENGINE_API void ioPublish(AssetIOService* service, IoRequest* request, jobs::detail::TaskState* task);
    }

    // Blocking file reads off the job system's workers. A few reader threads do one thing:
    // claim the highest-priority read whose gate class has a free BackgroundGate slot, read
    // it (retrying a half-written or renamed file for up to 200 ms), then release the decode
    // continuation as a Background job that holds that slot. Blocking I/O never runs on a
    // worker, and an asset storm cannot take the workers the frame needs.
    //
    //     AssetIOService::ReadRequest request;
    //     request.path = path;
    //     request.priority = IOPriority::High;
    //     jobs::Task<Image> image = AssetIOService::instance().submitRead(request,
    //         [](std::vector<uint8_t>&& bytes){ return decodeImage(bytes); });
    //     ... poll image.isReady() on the main thread, or image.cancel()
    //
    // - Exactly one of onSubmitted (the continuation is released) and onFailure (the read
    //   failed, was cancelled, or the service stopped) fires per request, before the task can
    //   be seen terminal; after onFailure the task ends Failed (AssetIOError) or Cancelled.
    // - cancel(path, flag) removes the queued requests whose CancelToken is that token
    //   (identity, so a stale canceller never withdraws a newer load); a claimed read discards
    //   its result when its token is cancelled.
    // - A wait on the task runs a still-queued read on the waiting thread, so it never waits
    //   for a gate slot the waiter itself may hold.
    // - With no reader threads (single-thread mode, web) the read runs inside the
    //   continuation job; onSubmitted fires at submit and a read failure fails the task.
    class JASPERENGINE_API AssetIOService {
    public:
        struct ReadRequest {
            std::string path;
            // a byte range: `size` bytes from `offset` (size 0 reads the whole file); the
            // range must lie in the file, a short read fails
            uint64_t offset = 0;
            uint32_t size = 0;
            IOPriority priority = IOPriority::Normal;
            // the load's cancellation, shared with its other jobs; its identity is what
            // cancel() matches
            jobs::CancelToken cancel;
            jobs::GateClass gateClass = jobs::GateClass::Default;
            const char* name = "asset-decode";
            uint32_t tags = jobs::Tag::Asset;
            // on the reader thread, once the continuation is released
            std::function<void(const jobs::TaskRef& continuation)> onSubmitted;
            // when no continuation will run: read failure, cancel, stop
            std::function<void(const std::string& error)> onFailure;
        };

        struct ReadInfo {
            uint64_t id = 0;
            std::string path;
            IOPriority priority = IOPriority::Normal;
            bool claimed = false;   // being read
        };

        struct IOStats {
            uint32_t queuedByPriority[kIOPriorityCount] = {};
            std::vector<ReadInfo> inFlight;   // queued and claimed reads
            int readers = 0;
            bool inlineMode = false;
            bool running = false;
        };

        // null: the process JobSystem
        explicit AssetIOService(jobs::JobSystem* system = nullptr);
        ~AssetIOService();

        AssetIOService(const AssetIOService&) = delete;
        AssetIOService& operator=(const AssetIOService&) = delete;

        // The engine's service, started by Engine::systemInit.
        static AssetIOService& instance();

        // Reader threads: -1 = env JE_ASSET_IO_THREADS, else 2 on desktop, 1 on mobile,
        // 0 on web and in single-thread mode (reads run in the continuation). At most 4.
        void start(int readerThreads = -1);
        // The first read starts a service that was never started (with the default count).
        // Stops accepting reads, joins the readers (a claimed read finishes), then fails
        // every queued read. Idempotent; start() may run again after it.
        void stop();
        bool isRunning() const;

        template<class F>
        auto submitRead(ReadRequest request, F&& onData)
            -> jobs::Task<std::invoke_result_t<std::decay_t<F>&, std::vector<uint8_t>&&>>;

        // Removes the queued reads of `path` whose token is `flag`; they end Cancelled and
        // their onFailure fires on the calling thread. Returns how many.
        size_t cancel(const std::string& path, const jobs::CancelToken& flag);
        // Cancels one read of stats().inFlight (queued: removed; claimed: discarded).
        bool cancelRead(uint64_t id);

        size_t queuedCount() const;
        IOStats stats() const;

        // The error onFailure receives for a cancelled request (not worth a log line).
        static const char* cancelledError();
        static bool isCancellation(const std::string& error);

        // The read itself, with the hot-reload retry window for whole files. Non-throwing;
        // false with `error` set on failure.
        static bool readFile(const std::string& path, uint64_t offset, uint32_t size,
                             std::vector<uint8_t>& out, std::string& error);

    private:
        friend struct detail::IoRequest;
        friend void detail::ioPublish(AssetIOService* service, detail::IoRequest* request, jobs::detail::TaskState* task);

        void readerMain(int index);
        detail::IoRequest* claimLocked(bool& returnSlot);
        void execute(detail::IoRequest* request, bool holdsSlot);
        // resolves the request's task Failed (or Cancelled) and fires onFailure
        void fail(detail::IoRequest* request, const std::string& error, bool cancelled);
        bool helpRead(detail::IoRequest* request);
        static void wakeReaders(void* service);

        jobs::JobSystem* system;
        bool followsProcessSystem;
        mutable std::mutex mutex;
        std::condition_variable changed;
        std::deque<detail::IoRequest*> queues[kIOPriorityCount];
        std::vector<detail::IoRequest*> claimedReads;
        std::vector<std::thread> readers;
        bool running = false;
        // stop() ran: no automatic start on the next read
        bool stopped = false;
        bool inlineMode = false;
        std::atomic<uint64_t> nextId{1};
    };

    namespace detail {
        // exported pieces of submitRead (the request type is private to AssetIOService.cpp)
        JASPERENGINE_API IoRequest* ioCreate(AssetIOService* service, AssetIOService::ReadRequest&& request);
        JASPERENGINE_API void ioRetain(IoRequest* request);
        JASPERENGINE_API void ioRelease(IoRequest* request);
        // The continuation's bytes: what the reader read, or (no readers) the read done here.
        // Throws AssetIOError when that read fails.
        JASPERENGINE_API void ioTakeBytes(IoRequest* request, std::vector<uint8_t>& bytes);
        // Gives back the gate slot the reader took for the request, if any (once).
        JASPERENGINE_API void ioReleaseSlot(IoRequest* request);

        // The continuation's closure: a reference to the request and the decode. The gate
        // slot of the read is released when the closure is destroyed, whether it ran or not.
        template<class F>
        struct IoContinuation {
            IoRequest* request;
            F onData;

            IoContinuation(IoRequest* io, F&& fn): request(io), onData(std::move(fn)){ ioRetain(request); }
            IoContinuation(IoContinuation&& other) noexcept: request(other.request), onData(std::move(other.onData)){
                other.request = nullptr;
            }
            IoContinuation(const IoContinuation&) = delete;
            IoContinuation& operator=(const IoContinuation&) = delete;
            IoContinuation& operator=(IoContinuation&&) = delete;
            ~IoContinuation(){
                if (request){
                    ioReleaseSlot(request);
                    ioRelease(request);
                }
            }

            auto operator()(){
                std::vector<uint8_t> bytes;
                ioTakeBytes(request, bytes);
                // the slot goes as soon as the decode ends
                return onData(std::move(bytes));
            }
        };
    }

    template<class F>
    auto AssetIOService::submitRead(ReadRequest request, F&& onData)
        -> jobs::Task<std::invoke_result_t<std::decay_t<F>&, std::vector<uint8_t>&&>>{
        using G = std::decay_t<F>;
        using R = std::invoke_result_t<G&, std::vector<uint8_t>&&>;
        jobs::CancelToken token = request.cancel;
        jobs::SubmitOptions options;
        options.lane = jobs::Lane::Background;
        options.gate = request.gateClass;
        options.name = request.name ? request.name : "asset-decode";
        options.tags = request.tags;
        options.cancel = &token;
        detail::IoRequest* io = detail::ioCreate(this, std::move(request));
        jobs::detail::TaskState* task = nullptr;
        try{
            task = jobs::detail::makeTask<detail::IoContinuation<G>, R>(detail::IoContinuation<G>(io, G(std::forward<F>(onData))), options);
        }catch(...){
            detail::ioRelease(io);
            throw;
        }
        jobs::Task<R> handle = jobs::TaskFactory::adopt<R>(task);
        detail::ioPublish(this, io, task);
        detail::ioRelease(io);
        return handle;
    }

}

#endif // ASSETIOSERVICE_H
