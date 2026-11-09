// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's AssetIOService (Engine/Source/Assets/AssetIOService.cpp),
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.
//
// A request's life: submitRead makes the decode task (Pending, gated on the read) and queues
// the request. A reader claims it together with a gate slot (or a waiter takes it to read it
// itself, without a slot), reads the bytes, and resolves the task, which enters the
// Background lane pre-admitted: its closure holds the slot and gives it back when it is
// destroyed (run or cancelled). A request that fails resolves the task Failed or Cancelled
// and fires onFailure instead.

#include "AssetIOService.h"

#include "Log.h"
#include "io/Data.h"
#include "io/File.h"
#include "io/FileData.h"
#include "jobs/detail/Scheduler.h"
#include "thread/ThreadUtil.h"
#include "jobs/detail/TimedWait.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::jobs;

namespace jasperengine { namespace detail {

    enum class IoState : int {
        Created,
        Queued,
        Claimed,
        Done,
    };

    enum SlotState : int {
        kSlotNone = 0,
        kSlotTaken = 1,
        kSlotReleased = 2,
    };

    struct IoRequest : jobs::detail::ExternalGate {
        AssetIOService* service = nullptr;
        AssetIOService::ReadRequest request;
        uint64_t id = 0;
        // held from publish until the request is resolved
        jobs::detail::TaskState* task = nullptr;
        std::vector<uint8_t> bytes;
        std::atomic<bool> hasBytes{false};
        std::atomic<int> slot{kSlotNone};
        std::atomic<int> state{(int)IoState::Created};
        std::atomic<bool> cancelled{false};
        // set when the service was not running at submit: the continuation fails
        bool rejected = false;
        // the gate a reader's slot comes from (set when the request is queued)
        jobs::BackgroundGate* gate = nullptr;

        // ExternalGate::help: a waiter runs the queued read itself
        static bool helpImpl(jobs::detail::ExternalGate* gate){
            auto* io = static_cast<IoRequest*>(gate);
            return io->service->helpRead(io);
        }

        bool isDead() const{
            return cancelled.load(std::memory_order_acquire) || request.cancel.isCancelled()
                || (task && jobs::detail::statusOf(task) != TaskStatus::Pending);
        }
    };

}}

using jasperengine::detail::IoRequest;
using jasperengine::detail::IoState;

namespace{
    constexpr int kMaxReaderThreads = 4;
    // a half-written or renamed file is retried for this long (S05 hot reload)
    constexpr double kRetryWindowMs = 200.0;
    constexpr int kRetrySleepMs = 10;

    jobs::detail::Scheduler* schedulerOf(jobs::JobSystem* system){
        return system->getScheduler();
    }

    void invokeFailure(IoRequest* io, const std::string& error){
        if (!io->request.onFailure)
            return;
        try{
            io->request.onFailure(error);
        }catch(const std::exception& e){
            Log::error("AssetIOService: onFailure of %s threw: %s", io->request.path.c_str(), e.what());
        }catch(...){
            Log::error("AssetIOService: onFailure of %s threw", io->request.path.c_str());
        }
    }

    void invokeSubmitted(IoRequest* io){
        if (!io->request.onSubmitted)
            return;
        try{
            jobs::detail::retainTask(io->task);
            const jobs::TaskRef continuation = jobs::TaskFactory::adopt<void>(io->task);
            io->request.onSubmitted(continuation);
        }catch(const std::exception& e){
            Log::error("AssetIOService: onSubmitted of %s threw: %s", io->request.path.c_str(), e.what());
        }catch(...){
            Log::error("AssetIOService: onSubmitted of %s threw", io->request.path.c_str());
        }
    }

    int defaultReaderCount(){
        if (const char* env = std::getenv("JE_ASSET_IO_THREADS")){
            if (env[0] != '\0')
                return std::clamp(std::atoi(env), 0, kMaxReaderThreads);
        }
        switch (thread::currentPlatform()){
            case thread::PlatformClass::Desktop:
                return 2;
            case thread::PlatformClass::Android:
            case thread::PlatformClass::IOS:
                return 1;
            default:
                return 0;
        }
    }

    void destroyGate(jobs::detail::ExternalGate* gate){
        delete static_cast<IoRequest*>(gate);
    }
}

const char* jasperengine::ioPriorityName(IOPriority priority){
    switch (priority){
        case IOPriority::Low: return "Low";
        case IOPriority::Normal: return "Normal";
        case IOPriority::High: return "High";
        case IOPriority::Critical: return "Critical";
    }
    return "Normal";
}

AssetIOError::AssetIOError(const std::string& message): std::runtime_error(message){
}

const char* AssetIOService::cancelledError(){
    return "Asset load cancelled";
}

bool AssetIOService::isCancellation(const std::string& error){
    return error == cancelledError();
}

// ---- requests

IoRequest* jasperengine::detail::ioCreate(AssetIOService* service, AssetIOService::ReadRequest&& request){
    auto* io = new IoRequest();
    io->service = service;
    io->request = std::move(request);
    io->help = &IoRequest::helpImpl;
    io->destroy = &destroyGate;
    return io;
}

void jasperengine::detail::ioRetain(IoRequest* request){
    jobs::detail::retainGate(request);
}

void jasperengine::detail::ioRelease(IoRequest* request){
    jobs::detail::releaseGate(request);
}

void jasperengine::detail::ioReleaseSlot(IoRequest* request){
    if (request->slot.exchange(kSlotReleased, std::memory_order_acq_rel) == kSlotTaken && request->gate)
        request->gate->release(request->request.gateClass);
}

void jasperengine::detail::ioTakeBytes(IoRequest* request, std::vector<uint8_t>& bytes){
    if (request->hasBytes.load(std::memory_order_acquire)){
        bytes = std::move(request->bytes);
        return;
    }
    if (request->rejected)
        throw AssetIOError("Asset read rejected: AssetIOService not running: " + request->request.path);
    // no reader threads: the read is part of the continuation
    std::string error;
    if (!AssetIOService::readFile(request->request.path, request->request.offset, request->request.size, bytes, error))
        throw AssetIOError(error);
}

void jasperengine::detail::ioPublish(AssetIOService* service, IoRequest* io, jobs::detail::TaskState* task){
    // the engine's service starts on its first read (a script may load before the
    // engine's own start); one that was stopped stays stopped until start()
    bool autoStart = false;
    {
        std::lock_guard<std::mutex> lock(service->mutex);
        autoStart = !service->running && !service->stopped;
    }
    if (autoStart)
        service->start(-1);
    if (!service->system)
        service->system = &jobs::system();
    jobs::detail::Scheduler* scheduler = schedulerOf(service->system);
    io->id = service->nextId.fetch_add(1, std::memory_order_relaxed);
    bool gated = false;
    {
        std::lock_guard<std::mutex> lock(service->mutex);
        gated = service->running && !service->inlineMode;
        io->rejected = !service->running;
    }
    if (!gated){
        // inline mode reads in the continuation; a stopped service fails it there
        if (io->rejected){
            invokeFailure(io, "Asset read rejected: AssetIOService not running");
        }else{
            io->task = task;
            invokeSubmitted(io);
            io->task = nullptr;
        }
        jobs::detail::publishTask(scheduler, task, nullptr, 0);
        return;
    }

    // gated on the read: the task holds the request, the request holds the task until resolved
    io->gate = &scheduler->gate;
    task->flags |= jobs::detail::kFlagIoGated | jobs::detail::kFlagPreAdmitted;
    jobs::detail::retainGate(io);
    task->owner = io;
    task->pendingDeps.store(1, std::memory_order_relaxed);
    jobs::detail::retainTask(task);
    io->task = task;
    jobs::detail::publishTask(scheduler, task, nullptr, 0);

    jobs::detail::retainGate(io);   // the queue's reference
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(service->mutex);
        if (service->running){
            io->state.store((int)IoState::Queued, std::memory_order_release);
            const size_t level = std::min((size_t)io->request.priority, kIOPriorityCount - 1);
            service->queues[level].push_back(io);
            service->changed.notify_one();
            queued = true;
        }
    }
    if (!queued){
        // stopped meanwhile
        io->state.store((int)IoState::Claimed, std::memory_order_release);
        service->fail(io, "Asset read rejected: AssetIOService not running", true);
    }
}

// ---- the service

AssetIOService::AssetIOService(JobSystem* jobSystem): system(jobSystem), followsProcessSystem(jobSystem == nullptr){
}

AssetIOService::~AssetIOService(){
    stop();
}

AssetIOService& AssetIOService::instance(){
    static AssetIOService* service = new AssetIOService(nullptr);
    return *service;
}

void AssetIOService::start(int readerThreads){
    // the process JobSystem may have been replaced since the last run
    if (followsProcessSystem)
        system = &jobs::system();
    int count = readerThreads >= 0 ? std::min(readerThreads, kMaxReaderThreads) : defaultReaderCount();
    if (system->isSingleThreaded())
        count = 0;
    if (thread::currentPlatform() == thread::PlatformClass::WebThreads){
        // out of the pthread pool: what the workers, the blocking thread and a video decoder
        // leave (thread::kWebPoolThreadsBesideWorkers)
        const int left = thread::webThreadPoolSize() - system->getWorkerCount() - (thread::kWebPoolThreadsBesideWorkers - 1);
        count = std::min(count, std::max(0, left));
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (running)
            return;
        running = true;
        stopped = false;
        inlineMode = count == 0;
    }
    if (count == 0)
        return;
    schedulerOf(system)->addGateListener(this, &AssetIOService::wakeReaders);
    readers.reserve((size_t)count);
    for (int i = 0; i < count; i++)
        readers.emplace_back([this, i](){ readerMain(i); });
}

void AssetIOService::stop(){
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopped = true;
        if (!running)
            return;
        running = false;
        changed.notify_all();
    }
    for (std::thread& reader : readers){
        if (reader.joinable())
            reader.join();
    }
    const bool hadReaders = !readers.empty();
    readers.clear();
    if (hadReaders && system)
        schedulerOf(system)->removeGateListener(this);

    std::vector<IoRequest*> abandoned;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& queue : queues){
            for (IoRequest* io : queue)
                abandoned.push_back(io);
            queue.clear();
        }
        inlineMode = false;
    }
    for (IoRequest* io : abandoned){
        io->state.store((int)IoState::Claimed, std::memory_order_release);
        fail(io, "Asset read abandoned: AssetIOService stopped", true);
    }
    if (!abandoned.empty())
        Log::debug("AssetIOService: abandoned %zu queued read(s) at stop", abandoned.size());
}

bool AssetIOService::isRunning() const{
    std::lock_guard<std::mutex> lock(mutex);
    return running;
}

void AssetIOService::wakeReaders(void* context){
    auto* service = static_cast<AssetIOService*>(context);
    std::lock_guard<std::mutex> lock(service->mutex);
    service->changed.notify_one();
}

IoRequest* AssetIOService::claimLocked(bool& returnSlot){
    // highest priority first, FIFO within a level, skipping requests whose class the gate
    // has full: a texture behind a full Heavy share must not hold up the material after it
    jobs::BackgroundGate& gate = schedulerOf(system)->gate;
    bool classFull[2] = {false, false};
    for (size_t level = kIOPriorityCount; level-- > 0;){
        auto& queue = queues[level];
        for (auto it = queue.begin(); it != queue.end(); ++it){
            IoRequest* io = *it;
            if (io->isDead()){
                // resolved without a slot
                queue.erase(it);
                return io;
            }
            const size_t gateClass = (size_t)io->request.gateClass;
            if (classFull[gateClass])
                continue;
            if (!gate.tryAcquire(io->request.gateClass)){
                classFull[gateClass] = true;
                if (classFull[0] && classFull[1])
                    return nullptr;
                continue;
            }
            // a slot taken: the request carries it until its continuation is destroyed. One
            // destroyed since isDead() leaves the slot to the caller, past the lock: a release
            // calls wakeReaders, which takes this mutex
            int expected = jasperengine::detail::kSlotNone;
            returnSlot = !io->slot.compare_exchange_strong(expected, jasperengine::detail::kSlotTaken, std::memory_order_acq_rel);
            queue.erase(it);
            return io;
        }
    }
    return nullptr;
}

void AssetIOService::readerMain(int index){
    char name[24];
    std::snprintf(name, sizeof(name), "je-io-%d", index);
    thread::setCurrentName(name);
    thread::setCurrentPriority(thread::Priority::BelowNormal);
    jobs::detail::currentSlotIndex(ThreadKind::IO, name);

    for (;;){
        IoRequest* io = nullptr;
        bool returnSlot = false;
        {
            std::unique_lock<std::mutex> lock(mutex);
            for (;;){
                if (!running)
                    return;   // stop() fails what is still queued
                io = claimLocked(returnSlot);
                if (io)
                    break;
                // a gate release or a new request wakes this; the backstop covers a missed one
                jobs::detail::timedWait(changed, lock, std::chrono::milliseconds(5));
            }
            io->state.store((int)IoState::Claimed, std::memory_order_release);
            claimedReads.push_back(io);
        }
        if (returnSlot)
            schedulerOf(system)->gate.release(io->request.gateClass);
        execute(io, true);
    }
}

bool AssetIOService::helpRead(IoRequest* io){
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (io->state.load(std::memory_order_acquire) != (int)IoState::Queued)
            return false;
        auto& queue = queues[std::min((size_t)io->request.priority, kIOPriorityCount - 1)];
        auto it = std::find(queue.begin(), queue.end(), io);
        if (it == queue.end())
            return false;
        queue.erase(it);
        io->state.store((int)IoState::Claimed, std::memory_order_release);
        claimedReads.push_back(io);
    }
    // the waiter reads without a gate slot: it may hold one itself
    execute(io, false);
    return true;
}

void AssetIOService::execute(IoRequest* io, bool holdsSlot){
    (void)holdsSlot;
    if (io->isDead()){
        fail(io, AssetIOService::cancelledError(), true);
        return;
    }
    std::vector<uint8_t> data;
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    if (!readFile(io->request.path, io->request.offset, io->request.size, data, error)){
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        Log::error("AssetIOService: read failed for %s after %.1f ms: %s", io->request.path.c_str(), ms, error.c_str());
        fail(io, error, false);
        return;
    }
    // a claimed read runs to completion, but a cancelled one is discarded here
    if (io->isDead()){
        fail(io, AssetIOService::cancelledError(), true);
        return;
    }
    io->bytes = std::move(data);
    io->hasBytes.store(true, std::memory_order_release);
    io->state.store((int)IoState::Done, std::memory_order_release);
    jobs::detail::TaskState* task = io->task;
    {
        std::lock_guard<std::mutex> lock(mutex);
        claimedReads.erase(std::remove(claimedReads.begin(), claimedReads.end(), io), claimedReads.end());
    }
    // the callback first, so a task seen terminal has had its callback; then the
    // continuation enters the Background lane holding the slot
    invokeSubmitted(io);
    schedulerOf(system)->resolveDep(task, false);
    io->task = nullptr;
    jobs::detail::releaseTask(task);
    jobs::detail::releaseGate(io);   // the queue's reference
}

void AssetIOService::fail(IoRequest* io, const std::string& error, bool cancelled){
    io->state.store((int)IoState::Done, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex);
        claimedReads.erase(std::remove(claimedReads.begin(), claimedReads.end(), io), claimedReads.end());
    }
    jobs::detail::TaskState* task = io->task;
    jobs::detail::Scheduler* scheduler = schedulerOf(system);
    // the callback first, so a task seen terminal has had its callback
    invokeFailure(io, error);
    if (task){
        if (cancelled)
            scheduler->cancelPending(task);
        else
            scheduler->failPending(task, std::make_exception_ptr(AssetIOError(error)));
    }
    if (task){
        io->task = nullptr;
        jobs::detail::releaseTask(task);
    }
    jobs::detail::releaseGate(io);   // the queue's reference
}

size_t AssetIOService::cancel(const std::string& path, const CancelToken& flag){
    std::vector<IoRequest*> removed;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& queue : queues){
            for (auto it = queue.begin(); it != queue.end();){
                IoRequest* io = *it;
                if (io->request.path == path && io->request.cancel == flag){
                    removed.push_back(io);
                    io->state.store((int)IoState::Claimed, std::memory_order_release);
                    it = queue.erase(it);
                }else{
                    ++it;
                }
            }
        }
    }
    for (IoRequest* io : removed)
        fail(io, AssetIOService::cancelledError(), true);
    return removed.size();
}

bool AssetIOService::cancelRead(uint64_t id){
    IoRequest* removed = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& queue : queues){
            for (auto it = queue.begin(); it != queue.end(); ++it){
                if ((*it)->id == id){
                    removed = *it;
                    removed->state.store((int)IoState::Claimed, std::memory_order_release);
                    queue.erase(it);
                    break;
                }
            }
            if (removed)
                break;
        }
        if (!removed){
            for (IoRequest* io : claimedReads){
                if (io->id == id){
                    io->cancelled.store(true, std::memory_order_release);
                    return true;
                }
            }
            return false;
        }
    }
    fail(removed, AssetIOService::cancelledError(), true);
    return true;
}

size_t AssetIOService::queuedCount() const{
    std::lock_guard<std::mutex> lock(mutex);
    size_t count = 0;
    for (const auto& queue : queues)
        count += queue.size();
    return count;
}

AssetIOService::IOStats AssetIOService::stats() const{
    IOStats result;
    std::lock_guard<std::mutex> lock(mutex);
    for (size_t level = 0; level < kIOPriorityCount; level++){
        result.queuedByPriority[level] = (uint32_t)queues[level].size();
        for (const IoRequest* io : queues[level])
            result.inFlight.push_back({io->id, io->request.path, io->request.priority, false});
    }
    for (const IoRequest* io : claimedReads)
        result.inFlight.push_back({io->id, io->request.path, io->request.priority, true});
    result.readers = (int)readers.size();
    result.inlineMode = inlineMode;
    result.running = running;
    return result;
}

// ---- the read

namespace{
    bool openRead(const std::string& path, std::unique_ptr<FileData>& file){
        file.reset(FileData::newFile(path.c_str(), true));
        if (!file)
            return false;
        if (File* handle = dynamic_cast<File*>(file.get()))
            return handle->getFilePtr() != nullptr;
        if (Data* memory = dynamic_cast<Data*>(file.get()))
            return memory->getMemPtr() != nullptr;
        return true;
    }
}

bool AssetIOService::readFile(const std::string& path, uint64_t offset, uint32_t size,
                              std::vector<uint8_t>& out, std::string& error){
    out.clear();
    error.clear();
    if (size > 0){
        // a range is read from a store only its writer replaces (by rename): no retry
        std::unique_ptr<FileData> file;
        if (!openRead(path, file)){
            error = "Cannot open file: " + path;
            return false;
        }
        const uint64_t length = file->length();
        if (offset > length || size > length - offset){
            error = "Short read: " + std::to_string(size) + " bytes at offset " + std::to_string(offset)
                + " lie past the end of " + path;
            return false;
        }
        out.resize(size);
        file->seek((int)offset);
        if (file->read(out.data(), size) != size){
            out.clear();
            error = "Short read of " + std::to_string(size) + " bytes at offset " + std::to_string(offset) + " of " + path;
            return false;
        }
        return true;
    }

    // a whole file: a half-written, truncated or renamed file is retried for a while
    const auto start = std::chrono::steady_clock::now();
    for (;;){
        std::unique_ptr<FileData> file;
        if (openRead(path, file)){
            const unsigned int length = file->length();
            if (length > 0){
                out.resize(length);
                file->seek(0);
                const unsigned int read = file->read(out.data(), length);
                if (read == length)
                    return true;
                out.clear();
                error = "Short read of " + path;
            }else{
                error = "Asset data is empty: " + path;
            }
        }else{
            error = "Failed to read file: " + path;
        }
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (elapsed >= kRetryWindowMs)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(kRetrySleepMs));
    }
}
