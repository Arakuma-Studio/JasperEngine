// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The scheduler. The lane contracts (Background polled last and never in local deques or
// steals, bounded spinners with a sleep backstop, the shutdown drain rules) follow
// OpenEngine's WorkStealingThreadPool (Modules/JobSystem/Include/JobSystem/
// WorkStealingThreadPool.h), Copyright (c) 2026 Gamecrafters Guild Limited and contributors,
// MIT License; the implementation is our own. See NOTICE.md.
//
// Task lifecycle. A TaskState is Pending until one thread wins the compare-exchange to
// Running (a worker that dequeued it, a waiter running it inline, a canceller), so it runs
// at most once. The thread that wins runs it (or discards it, for a cancel) and finalizes:
// stores the terminal status, wakes parked waiters, releases a pre-admitted gate slot and
// resolves the dependents. References: one per handle, one per queue holding it, one per
// dependency link naming it; the last release destroys it.
//
// Parking. Waiters park in a bucket of a global parking lot keyed by the task's address:
// the waiter counts itself in task->parked under the bucket mutex, then re-checks the
// status; the finalizer stores the status, then reads parked and notifies under the mutex.
// Both sides are seq_cst, so either the waiter sees the status or the finalizer sees the
// waiter. Every park also has a timed backstop.

#include "jobs/JobSystem.h"
#include "jobs/detail/Scheduler.h"

#include "Log.h"
#include "memory/MemoryStats.h"
#include "thread/ThreadUtil.h"
#include "jobs/detail/TimedWait.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__EMSCRIPTEN__)
    #include <emscripten.h>
    #include <emscripten/threading.h>
#endif

#if defined(_MSC_VER)
    #include <intrin.h>
#endif

using namespace jasperengine;
using namespace jasperengine::jobs;
using namespace jasperengine::jobs::detail;

namespace jasperengine { namespace jobs { namespace detail {
    uint64_t heapEnvelopesAllocated();
}}}

namespace{

    DepLink* const kSealed = reinterpret_cast<DepLink*>(uintptr_t(1));

    inline void cpuRelax(){
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        _mm_pause();
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
        __builtin_ia32_pause();
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__aarch64__) || defined(__arm__))
        __asm__ __volatile__("yield");
#endif
    }

    // ---- the parking lot

    struct alignas(64) ParkBucket {
        std::mutex mutex;
        std::condition_variable cv;
    };
    constexpr size_t kBucketCount = 64;

    ParkBucket* parkBuckets(){
        static ParkBucket* buckets = new ParkBucket[kBucketCount];
        return buckets;
    }

    ParkBucket& bucketOf(const void* key){
        const uintptr_t address = reinterpret_cast<uintptr_t>(key);
        return parkBuckets()[(address >> 6) % kBucketCount];
    }

    // ---- thread slots

    ThreadSlot* threadSlots(){
        static ThreadSlot* slots = new ThreadSlot[kMaxThreadSlots];
        return slots;
    }

    struct ThreadState {
        int slot = -1;
        Scheduler* workerOf = nullptr;
        int workerIndex = -1;
        bool isMain = false;
        BodyContext body;

        ~ThreadState(){
            if (slot >= 0){
                ThreadSlot& s = threadSlots()[slot];
                s.runningJob.store(nullptr, std::memory_order_relaxed);
                s.runningScheduler.store(nullptr, std::memory_order_relaxed);
                s.used.store(0, std::memory_order_release);
            }
        }
    };

    thread_local ThreadState threadState;

    // ---- hooks

    std::atomic<const ProfilerHooks*> installedHooks{nullptr};
    std::atomic<uint32_t> installedEpoch{1};

    // ---- the process instance

    std::mutex& instanceMutex(){
        static std::mutex* mutex = new std::mutex();
        return *mutex;
    }
    std::atomic<JobSystem*> processInstance{nullptr};

    // ---- postToMain

    std::mutex& mainQueueMutex(){
        static std::mutex* mutex = new std::mutex();
        return *mutex;
    }
    std::vector<std::function<void()>>& mainQueue(){
        static std::vector<std::function<void()>>* queue = new std::vector<std::function<void()>>();
        return *queue;
    }

    int defaultReserve(){
        switch (thread::currentPlatform()){
            case thread::PlatformClass::Desktop:
                return 2;
            default:
                return 1;
        }
    }

    int defaultBlockingBudget(int workers){
        if (workers == 0)
            return 0;
        switch (thread::currentPlatform()){
            case thread::PlatformClass::WebThreads:
                return 1;
            case thread::PlatformClass::Desktop:
                return 8;
            default:
                return 4;
        }
    }

    [[noreturn]] void fatal(const char* message){
        Log::error("%s", message);
        std::fprintf(stderr, "JobSystem: %s\n", message);
        std::fflush(stderr);
        std::abort();
    }
}

// ---- detail functions

uint64_t jobs::detail::nowNs(){
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

ThreadSlot* jobs::detail::slotAt(int index){
    return (index >= 0 && index < (int)kMaxThreadSlots) ? &threadSlots()[index] : nullptr;
}

BodyContext& jobs::detail::currentBody(){
    return threadState.body;
}

int jobs::detail::currentSlotIndex(ThreadKind kind, const char* name){
    ThreadState& state = threadState;
    if (state.slot >= 0)
        return state.slot;
    ThreadSlot* slots = threadSlots();
    for (uint32_t i = 0; i < kMaxThreadSlots; i++){
        uint32_t expected = 0;
        if (slots[i].used.load(std::memory_order_relaxed) == 0
            && slots[i].used.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)){
            ThreadSlot& slot = slots[i];
            char buffer[sizeof(slot.name)];
            if (name){
                std::snprintf(buffer, sizeof(buffer), "%s", name);
            }else if (state.isMain){
                std::snprintf(buffer, sizeof(buffer), "je-main");
            }else{
                std::snprintf(buffer, sizeof(buffer), "thread-%u", thread::currentThreadIndex());
            }
            std::memcpy(slot.name, buffer, sizeof(slot.name));
            slot.kind.store((uint8_t)(state.isMain ? ThreadKind::Main : kind), std::memory_order_relaxed);
            slot.runningJob.store(nullptr, std::memory_order_relaxed);
            slot.waitingOnRunner.store(-1, std::memory_order_relaxed);
            slot.hooksEpoch.store(0, std::memory_order_relaxed);
            // 2 publishes the name to snapshotOccupancy and stats
            slot.used.store(2, std::memory_order_release);
            state.slot = (int)i;
            return state.slot;
        }
    }
    return -1;
}

int jobs::detail::currentWorkerIndex(const Scheduler* scheduler){
    return threadState.workerOf == scheduler ? threadState.workerIndex : -1;
}

std::mutex& jobs::detail::bucketMutex(const void* key){
    return bucketOf(key).mutex;
}

std::condition_variable& jobs::detail::bucketCv(const void* key){
    return bucketOf(key).cv;
}

void jobs::detail::spinWait(){
#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
    // the browser main thread services the proxied calls a worker may wait on
    emscripten_main_thread_process_queued_calls();
#endif
    std::this_thread::yield();
}

const ProfilerHooks* jobs::detail::hooks(){
    // acquire: the reader dereferences the copy setProfilerHooks published
    return installedHooks.load(std::memory_order_acquire);
}

uint32_t jobs::detail::hooksEpoch(){
    return installedEpoch.load(std::memory_order_relaxed);
}

void jobs::detail::retainGate(ExternalGate* gate){
    gate->refs.fetch_add(1, std::memory_order_relaxed);
}

void jobs::detail::releaseGate(ExternalGate* gate){
    if (gate && gate->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        gate->destroy(gate);
}

// ---- BodyScope

BodyScope::BodyScope(Scheduler* scheduler, const Runnable* item, CancelState* cancel, ChannelState* channel){
    ThreadState& state = threadState;
    saved = state.body;
    state.body.scheduler = scheduler;
    state.body.lane = item->lane;
    state.body.gate = item->gate;
    state.body.cancel = cancel;
    state.body.channel = channel;
    state.body.inBody = true;

    owner = scheduler;
    worker = state.workerOf == scheduler ? state.workerIndex : -1;
    const bool collect = !scheduler || scheduler->config.collectStats;
    const int slotIndex = !collect ? -1 : state.slot >= 0 ? state.slot : currentSlotIndex();
    slot = slotAt(slotIndex);
    savedJob = nullptr;
    savedLane = 0;
    savedScheduler = nullptr;
    if (slot){
        savedJob = slot->runningJob.load(std::memory_order_relaxed);
        savedLane = slot->runningLane.load(std::memory_order_relaxed);
        savedScheduler = slot->runningScheduler.load(std::memory_order_relaxed);
        slot->runningJob.store(item->name, std::memory_order_relaxed);
        slot->runningLane.store((uint8_t)item->lane, std::memory_order_relaxed);
        slot->runningScheduler.store(scheduler, std::memory_order_relaxed);
        slot->jobSeq.store(slot->jobSeq.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    hooked = false;
    if (const ProfilerHooks* h = hooks()){
        hooked = true;
        if (slot && h->onThreadStart){
            const uint32_t epoch = hooksEpoch();
            if (slot->hooksEpoch.load(std::memory_order_relaxed) != epoch){
                slot->hooksEpoch.store(epoch, std::memory_order_relaxed);
                h->onThreadStart(slot->name);
            }
        }
        if (h->onJobBegin)
            h->onJobBegin(item->name, item->lane, state.workerIndex);
    }
}

BodyScope::~BodyScope(){
    if (hooked){
        if (const ProfilerHooks* h = hooks()){
            if (h->onJobEnd)
                h->onJobEnd();
        }
    }
    if (slot){
        slot->runningJob.store(savedJob, std::memory_order_relaxed);
        slot->runningLane.store(savedLane, std::memory_order_relaxed);
        slot->runningScheduler.store(savedScheduler, std::memory_order_relaxed);
        // counted here, before the task's result is published, where the worker is known
        if (owner)
            owner->countExecuted(worker);
    }
    threadState.body = saved;
}

namespace{
    struct WaitHookScope {
        bool hooked = false;
        explicit WaitHookScope(WaitKind kind){
            if (const ProfilerHooks* h = hooks()){
                if (h->onWaitBegin){
                    h->onWaitBegin(kind);
                    hooked = true;
                }
            }
        }
        ~WaitHookScope(){
            if (hooked){
                if (const ProfilerHooks* h = hooks()){
                    if (h->onWaitEnd)
                        h->onWaitEnd();
                }
            }
        }
    };
}

// ---- queues

bool LocalDeque::pushBack(Runnable* item){
    std::lock_guard<std::mutex> lock(mutex);
    if (tail - head == kLocalCapacity)
        return false;
    items[tail % kLocalCapacity] = item;
    tail++;
    size.store((uint32_t)(tail - head), std::memory_order_relaxed);
    return true;
}

void LocalDeque::pushBackBatch(Runnable* const* batch, size_t count){
    std::lock_guard<std::mutex> lock(mutex);
    for (size_t i = 0; i < count; i++){
        items[tail % kLocalCapacity] = batch[i];
        tail++;
    }
    size.store((uint32_t)(tail - head), std::memory_order_relaxed);
}

Runnable* LocalDeque::popBack(){
    if (size.load(std::memory_order_relaxed) == 0)
        return nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    if (tail == head)
        return nullptr;
    tail--;
    Runnable* item = items[tail % kLocalCapacity];
    size.store((uint32_t)(tail - head), std::memory_order_relaxed);
    return item;
}

Runnable* LocalDeque::popFront(){
    if (size.load(std::memory_order_relaxed) == 0)
        return nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    if (tail == head)
        return nullptr;
    Runnable* item = items[head % kLocalCapacity];
    head++;
    size.store((uint32_t)(tail - head), std::memory_order_relaxed);
    return item;
}

void LaneQueue::pushLocked(Runnable* item){
    item->next = nullptr;
    if (spillHead || size == ring.size()){
        // keeps FIFO: once spilled, everything newer spills until the spill drains
        if (spillTail)
            spillTail->next = item;
        else
            spillHead = item;
        spillTail = item;
    }else{
        ring[(head + size) % ring.size()] = item;
        size++;
    }
}

void LaneQueue::push(Runnable* item){
    {
        std::lock_guard<std::mutex> lock(mutex);
        pushLocked(item);
    }
    queued.fetch_add(1, std::memory_order_seq_cst);
}

void LaneQueue::pushBatch(Runnable* const* items, size_t count){
    if (count == 0)
        return;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (size_t i = 0; i < count; i++)
            pushLocked(items[i]);
    }
    queued.fetch_add((uint32_t)count, std::memory_order_seq_cst);
}

Runnable* LaneQueue::pop(){
    if (queued.load(std::memory_order_relaxed) == 0)
        return nullptr;
    Runnable* item = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (size > 0){
            item = ring[head];
            head = (head + 1) % ring.size();
            size--;
        }else if (spillHead){
            item = spillHead;
            spillHead = item->next;
            if (!spillHead)
                spillTail = nullptr;
        }
    }
    if (item){
        item->next = nullptr;
        queued.fetch_sub(1, std::memory_order_relaxed);
    }
    return item;
}

size_t LaneQueue::popBatch(Runnable** out, size_t max){
    if (max == 0 || queued.load(std::memory_order_relaxed) == 0)
        return 0;
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        while (count < max){
            Runnable* item = nullptr;
            if (size > 0){
                item = ring[head];
                head = (head + 1) % ring.size();
                size--;
            }else if (spillHead){
                item = spillHead;
                spillHead = item->next;
                if (!spillHead)
                    spillTail = nullptr;
            }else{
                break;
            }
            item->next = nullptr;
            out[count++] = item;
        }
    }
    if (count)
        queued.fetch_sub((uint32_t)count, std::memory_order_relaxed);
    return count;
}

namespace{
    // Immortal: the process JobSystem outlives static destruction.
    memory::TrackedBytes& queueBytes(){
        static memory::TrackedBytes* tracked = new memory::TrackedBytes("Jobs.Queues", memory::Residency::Cpu);
        return *tracked;
    }
}

FastLane::FastLane(): slots(new Slot[kCapacity]){
    for (size_t i = 0; i < kCapacity; i++)
        slots[i].sequence.store(i, std::memory_order_relaxed);
    queueBytes().add(kCapacity * sizeof(Slot));
}

FastLane::~FastLane(){
    queueBytes().subtract(kCapacity * sizeof(Slot));
}

bool FastLane::tryPush(Runnable* item){
    uint64_t position = enqueuePosition.load(std::memory_order_relaxed);
    for (;;){
        Slot& slot = slots[position & kMask];
        const uint64_t sequence = slot.sequence.load(std::memory_order_acquire);
        const int64_t lead = (int64_t)(sequence - position);
        if (lead == 0){
            if (enqueuePosition.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)){
                slot.item.store(item, std::memory_order_relaxed);
                slot.sequence.store(position + 1, std::memory_order_release);
                return true;
            }
        }else if (lead < 0){
            return false;  // full: the slot still holds the item from a lap ago
        }else{
            position = enqueuePosition.load(std::memory_order_relaxed);
        }
    }
}

void FastLane::push(Runnable* item){
    if (overflow.count() == 0 && tryPush(item))
        return;
    overflow.push(item);
}

void FastLane::pushBatch(Runnable* const* items, size_t count){
    for (size_t i = 0; i < count; i++)
        push(items[i]);
}

Runnable* FastLane::pop(){
    Runnable* item = nullptr;
    return popBatch(&item, 1) ? item : nullptr;
}

size_t FastLane::popBatch(Runnable** out, size_t max){
    if (max == 0)
        return 0;
    uint64_t position = dequeuePosition.load(std::memory_order_relaxed);
    for (;;){
        size_t ready = 0;
        while (ready < max){
            const Slot& slot = slots[(position + ready) & kMask];
            if (slot.sequence.load(std::memory_order_acquire) != position + ready + 1)
                break;
            ready++;
        }
        if (ready == 0)
            break;
        // the run is ours once the position moves past it (a failed CAS reloads it)
        if (dequeuePosition.compare_exchange_weak(position, position + ready, std::memory_order_relaxed)){
            for (size_t i = 0; i < ready; i++){
                Slot& slot = slots[(position + i) & kMask];
                out[i] = slot.item.load(std::memory_order_relaxed);
                slot.sequence.store(position + i + kCapacity, std::memory_order_release);
            }
            return ready;
        }
    }
    return overflow.count() ? overflow.popBatch(out, max) : 0;
}

bool FastLane::hasItems() const{
    const uint64_t position = dequeuePosition.load(std::memory_order_relaxed);
    return slots[position & kMask].sequence.load(std::memory_order_acquire) == position + 1 || overflow.count() > 0;
}

uint32_t FastLane::count() const{
    const uint64_t pushed = enqueuePosition.load(std::memory_order_relaxed);
    const uint64_t taken = dequeuePosition.load(std::memory_order_relaxed);
    return (uint32_t)(pushed > taken ? pushed - taken : 0) + overflow.count();
}

// ---- releasing

void jobs::detail::discardJob(Job* job) noexcept{
    job->ops->destroy(job);
    if (job->flags & kFlagHeap)
        releaseHeapEnvelope(job);
    else
        releaseSlab(SlabClass::Job, job);
}

void jobs::detail::releaseTask(TaskState* task) noexcept{
    if (task->refs.fetch_sub(1, std::memory_order_acq_rel) != 1)
        return;
    // the links to its dependencies: each holds a reference to the dependency
    DepLink* link = task->deps;
    while (link){
        DepLink* next = link->nextOfDependent;
        releaseTask(link->dep);
        releaseSlab(SlabClass::Link, link);
        link = next;
    }
    if (task->flags & kFlagIoGated)
        releaseGate(static_cast<ExternalGate*>(task->owner));
    if (task->cancel)
        releaseCancel(task->cancel);
    const bool heap = (task->flags & kFlagHeap) != 0;
    task->ops->destroy(task);
    if (heap)
        releaseHeapEnvelope(task);
    else
        releaseSlab(SlabClass::Task, task);
}

void jobs::detail::waitTask(TaskState* task){
    Scheduler* scheduler = task->scheduler;
    if (scheduler){
        scheduler->waitTask(task);
        return;
    }
    // never published (cannot happen through the public API): poll
    while (!isTerminal(statusOf(task)))
        std::this_thread::yield();
}

bool jobs::detail::cancelTask(TaskState* task){
    if (!task->scheduler){
        // not published yet: nothing can claim it concurrently
        uint8_t expected = (uint8_t)TaskStatus::Pending;
        if (!task->status.compare_exchange_strong(expected, (uint8_t)TaskStatus::Running, std::memory_order_acq_rel))
            return false;
        task->ops->discard(task);
        task->status.store((uint8_t)TaskStatus::Cancelled, std::memory_order_seq_cst);
        return true;
    }
    return task->scheduler->cancelPending(task);
}

void jobs::detail::throwTaskOutcome(TaskState* task){
    if (statusOf(task) == TaskStatus::Failed && task->error)
        std::rethrow_exception(task->error);
    throw TaskCancelled();
}

TaskCancelled::TaskCancelled(): std::runtime_error("job cancelled"){
}

const char* jobs::laneName(Lane lane){
    switch (lane){
        case Lane::High: return "High";
        case Lane::Normal: return "Normal";
        case Lane::Background: return "Background";
    }
    return "Normal";
}

// ---- Scheduler

Scheduler::Scheduler(JobSystem* owner, const Config& configuration):
    owner(owner), config(configuration),
    workerCount(thread::resolveWorkerCount(configuration.workers)),
    gate(thread::resolveWorkerCount(configuration.workers),
         configuration.reserveForFrame >= 0 ? configuration.reserveForFrame : defaultReserve(),
         [this](){ onGateRelease(); }){
    singleThread = workerCount == 0;
    blockingBudget = config.blockingBudget >= 0 ? config.blockingBudget : defaultBlockingBudget(workerCount);
    if (singleThread)
        blockingBudget = 0;
    else if (blockingBudget < 1)
        blockingBudget = 1;
    maxSpinners = std::min(2, std::max(workerCount, 1));
    startNs = nowNs();
    lastSampleNs = startNs;
}

Scheduler::~Scheduler(){
    shutdown();
}

void Scheduler::onGateRelease(){
    if (sleepers.load(std::memory_order_seq_cst) > 0 && (backgroundDefault.count() || backgroundHeavy.count()))
        wake(1);
    if (gateListenerCount.load(std::memory_order_acquire) > 0){
        std::lock_guard<std::mutex> lock(gateListenerMutex);
        for (const auto& listener : gateListeners)
            listener.second(listener.first);
    }
}

void Scheduler::addGateListener(void* context, void (*callback)(void*)){
    std::lock_guard<std::mutex> lock(gateListenerMutex);
    gateListeners.emplace_back(context, callback);
    gateListenerCount.store((int)gateListeners.size(), std::memory_order_release);
}

void Scheduler::removeGateListener(void* context){
    std::lock_guard<std::mutex> lock(gateListenerMutex);
    gateListeners.erase(std::remove_if(gateListeners.begin(), gateListeners.end(),
        [context](const std::pair<void*, void (*)(void*)>& listener){ return listener.first == context; }), gateListeners.end());
    gateListenerCount.store((int)gateListeners.size(), std::memory_order_release);
}

bool Scheduler::failPending(TaskState* task, std::exception_ptr error){
    if (!tryClaim(task))
        return false;
    task->ops->discard(task);
    task->error = std::move(error);
    finalize(task, TaskStatus::Failed);
    return true;
}

void Scheduler::start(){
    workers.reserve((size_t)workerCount);
    for (int i = 0; i < workerCount; i++){
        auto worker = std::make_unique<Worker>();
        worker->scheduler = this;
        worker->index = i;
        worker->backgroundHeavyFirst = (i % 2) == 1;
        workers.push_back(std::move(worker));
    }
    for (int i = 0; i < workerCount; i++){
        Worker* worker = workers[(size_t)i].get();
        worker->thread = std::thread([this, worker](){ workerMain(*worker); });
    }
}

Lane Scheduler::clampLane(Lane lane) const{
    const BodyContext& body = threadState.body;
    if (!body.inBody)
        return lane;
    // a fork never moves work into a more urgent lane than the body forking it
    return (uint8_t)body.lane > (uint8_t)lane ? body.lane : lane;
}

void Scheduler::wake(size_t count, bool force){
    if (singleThread)
        return;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (count == 1 && !force && spinners.load(std::memory_order_seq_cst) > 0)
        return;
    if (sleepers.load(std::memory_order_seq_cst) == 0)
        return;
    // one single wake in flight is enough: the woken worker clears the flag and wakes the
    // next one when it finds more work, so a producer pays one notify per wake-up, not per job
    if (count == 1 && (wakePending.load(std::memory_order_relaxed) || wakePending.exchange(true, std::memory_order_seq_cst)))
        return;
    std::lock_guard<std::mutex> lock(sleepMutex);
    if (sleepers.load(std::memory_order_seq_cst) == 0){
        // the sleeper seen above left (and cleared the flag first): nobody would clear it now
        wakePending.store(false, std::memory_order_seq_cst);
        return;
    }
    if (count > 1)
        sleepCv.notify_all();
    else
        sleepCv.notify_one();
}

void Scheduler::enqueue(Runnable* item){
    if (item->kind == RunnableKind::Job && (item->tags & Tag::Asset))
        assetJobs.fetch_add(1, std::memory_order_relaxed);
    switch (item->lane){
        case Lane::High:
            high.push(item);
            break;
        case Lane::Normal:{
            const int worker = currentWorkerIndex(this);
            if (worker < 0 || !workers[(size_t)worker]->local.pushBack(item))
                normal.push(item);
            break;
        }
        case Lane::Background:
            if (item->flags & kFlagPreAdmitted)
                backgroundAdmitted.push(item);
            else if (item->gate == GateClass::Heavy)
                backgroundHeavy.push(item);
            else
                backgroundDefault.push(item);
            break;
    }
    wake(1);
}

void Scheduler::publishJob(Job* job){
    if (stopWorkers.load(std::memory_order_acquire)){
        // after the drain: must-run work runs on the caller
        runBareJob(job);
        return;
    }
    enqueue(job);
    if (shuttingDown.load(std::memory_order_seq_cst) && stopWorkers.load(std::memory_order_seq_cst))
        drainQueues();
}

void Scheduler::publishBatch(Job* const* jobs, size_t count, Lane lane, GateClass gateClass){
    if (count == 0)
        return;
    if (lane == Lane::Background){
        LaneQueue& queue = gateClass == GateClass::Heavy ? backgroundHeavy : backgroundDefault;
        Runnable* items[kMaxHelpersPerRun];
        size_t done = 0;
        while (done < count){
            const size_t batch = std::min(count - done, kMaxHelpersPerRun);
            for (size_t i = 0; i < batch; i++)
                items[i] = jobs[done + i];
            queue.pushBatch(items, batch);
            done += batch;
        }
    }else{
        FastLane& queue = lane == Lane::High ? high : normal;
        for (size_t i = 0; i < count; i++)
            queue.push(jobs[i]);
    }
    wake(count);
    if (shuttingDown.load(std::memory_order_seq_cst) && stopWorkers.load(std::memory_order_seq_cst))
        drainQueues();
}

void Scheduler::publishTask(TaskState* task, const TaskRef* deps, size_t depCount){
    task->scheduler = this;
    if (task->tags & Tag::Asset)
        assetJobs.fetch_add(1, std::memory_order_relaxed);
    if (shuttingDown.load(std::memory_order_seq_cst)){
        cancelPending(task);
        return;
    }
    if (depCount == 0){
        if (task->flags & kFlagIoGated){
            // released by the read (resolveDep), or helped by a waiter
            return;
        }
        retainTask(task);
        enqueue(task);
    }else{
        // +1: the submit's own hold, so the task cannot become ready mid-registration
        task->pendingDeps.store((uint32_t)depCount + 1, std::memory_order_relaxed);
        for (size_t i = 0; i < depCount; i++){
            TaskState* dep = deps[i].getState();
            if (!dep){
                resolveDep(task, true);
                continue;
            }
            auto* link = ::new (acquireSlab(SlabClass::Link)) DepLink();
            link->dep = dep;
            link->dependent = task;
            retainTask(dep);
            link->nextOfDependent = task->deps;
            task->deps = link;
            // the dependency's list holds the dependent until it resolves it
            retainTask(task);
            DepLink* head = dep->dependents.load(std::memory_order_acquire);
            bool registered = false;
            while (head != kSealed){
                link->nextOfDep = head;
                if (dep->dependents.compare_exchange_weak(head, link, std::memory_order_acq_rel, std::memory_order_acquire)){
                    registered = true;
                    break;
                }
            }
            if (!registered){
                // already terminal
                resolveDep(task, statusOf(dep) != TaskStatus::Completed);
                releaseTask(task);
            }
        }
        resolveDep(task, false);
    }
    if (shuttingDown.load(std::memory_order_seq_cst)){
        // lost the race with shutdown: cancel it ourselves so a wait returns
        cancelPending(task);
        if (stopWorkers.load(std::memory_order_seq_cst))
            drainQueues();
    }
}

void Scheduler::resolveDep(TaskState* task, bool failed){
    if (failed && task->policy == DepPolicy::RequireSuccess){
        task->depFailed.store(1, std::memory_order_release);
        // cascades through finalize to its own dependents
        cancelPending(task);
    }
    if (task->pendingDeps.fetch_sub(1, std::memory_order_acq_rel) == 1){
        if (statusOf(task) != TaskStatus::Pending)
            return;
        if (task->depFailed.load(std::memory_order_acquire) && task->policy == DepPolicy::RequireSuccess){
            cancelPending(task);
            return;
        }
        onReady(task);
    }
}

void Scheduler::onReady(TaskState* task){
    if (shuttingDown.load(std::memory_order_seq_cst) && !(task->flags & kFlagPreAdmitted)){
        cancelPending(task);
        return;
    }
    if (stopWorkers.load(std::memory_order_seq_cst)){
        cancelPending(task);
        return;
    }
    retainTask(task);
    enqueue(task);
}

bool Scheduler::tryClaim(TaskState* task){
    uint8_t expected = (uint8_t)TaskStatus::Pending;
    return task->status.compare_exchange_strong(expected, (uint8_t)TaskStatus::Running, std::memory_order_acq_rel);
}

bool Scheduler::cancelPending(TaskState* task){
    if (!tryClaim(task))
        return false;
    task->ops->discard(task);
    finalize(task, TaskStatus::Cancelled);
    return true;
}

void Scheduler::runClaimed(TaskState* task){
    const int slot = currentSlotIndex();
    task->runner.store(slot, std::memory_order_release);
    if (shuttingDown.load(std::memory_order_acquire) && !(task->flags & (kFlagChannel))){
        task->ops->discard(task);
        finalize(task, TaskStatus::Cancelled);
        return;
    }
    bool ok;
    {
        ChannelState* channel = (task->flags & kFlagChannel) ? static_cast<ChannelState*>(task->owner) : nullptr;
        BodyScope scope(this, task, task->cancel, channel);
        ok = task->ops->invoke(task);
    }
    finalize(task, ok ? TaskStatus::Completed : TaskStatus::Failed);
}

void Scheduler::finalize(TaskState* task, TaskStatus status){
    if (task->tags & Tag::Asset)
        assetJobs.fetch_sub(1, std::memory_order_relaxed);
    if (status == TaskStatus::Failed)
        failedCount.fetch_add(1, std::memory_order_relaxed);
    else if (status == TaskStatus::Cancelled)
        cancelledCount.fetch_add(1, std::memory_order_relaxed);
    task->status.store((uint8_t)status, std::memory_order_seq_cst);
    if (task->parked.load(std::memory_order_seq_cst) > 0){
        std::lock_guard<std::mutex> lock(bucketMutex(task));
        bucketCv(task).notify_all();
    }
    DepLink* link = task->dependents.exchange(kSealed, std::memory_order_acq_rel);
    while (link){
        DepLink* next = link->nextOfDep;
        TaskState* dependent = link->dependent;
        resolveDep(dependent, status != TaskStatus::Completed);
        releaseTask(dependent);
        link = next;
    }
}

void Scheduler::runBareJob(Job* job){
    if (job->flags & kFlagInternal){
        // a counter visit or a strand drain: the job it runs opens its own body
        job->ops->run(job);
        discardJob(job);
        return;
    }
    {
        BodyScope scope(this, job, nullptr, nullptr);
        try{
            job->ops->run(job);
        }catch(const std::exception& e){
            failedCount.fetch_add(1, std::memory_order_relaxed);
            Log::error("Job '%s' threw: %s", job->name, e.what());
        }catch(...){
            failedCount.fetch_add(1, std::memory_order_relaxed);
            Log::error("Job '%s' threw an exception", job->name);
        }
    }
    if (job->tags & Tag::Asset)
        assetJobs.fetch_sub(1, std::memory_order_relaxed);
    discardJob(job);
}

void Scheduler::countExecuted(int worker){
    if (worker >= 0){
        // the worker's own counter: a plain increment, no locked instruction per job
        std::atomic<uint64_t>& executed = workers[(size_t)worker]->executed;
        executed.store(executed.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }else{
        externalExecuted.fetch_add(1, std::memory_order_relaxed);
    }
}

void Scheduler::execute(Runnable* item){
    if (item->kind == RunnableKind::Job){
        runBareJob(static_cast<Job*>(item));
        return;
    }
    TaskState* task = static_cast<TaskState*>(item);
    if (tryClaim(task))
        runClaimed(task);
    releaseTask(task);
}

// ---- workers

uint32_t Scheduler::localBacklog() const{
    // summed on demand: a shared counter would be written by every worker for every job
    uint32_t total = 0;
    for (const auto& worker : workers)
        total += worker->local.approxSize();
    return total;
}

bool Scheduler::hasWork() const{
    // pairs with the fence in wake(): a sleeper counted itself before looking, a producer
    // published before looking for sleepers, so one of them sees the other
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (high.hasItems() || normal.hasItems() || localBacklog() || backgroundAdmitted.count())
        return true;
    if (backgroundDefault.count() && gate.canAcquire(GateClass::Default))
        return true;
    if (backgroundHeavy.count() && gate.canAcquire(GateClass::Heavy))
        return true;
    return false;
}

Runnable* Scheduler::popBackground(Worker& worker, int& slotClass){
    slotClass = -1;
    if (Runnable* item = backgroundAdmitted.pop())
        return item;
    worker.backgroundHeavyFirst = !worker.backgroundHeavyFirst;
    for (int attempt = 0; attempt < 2; attempt++){
        const bool heavy = (attempt == 0) == worker.backgroundHeavyFirst;
        LaneQueue& queue = heavy ? backgroundHeavy : backgroundDefault;
        const GateClass gateClass = heavy ? GateClass::Heavy : GateClass::Default;
        if (queue.count() == 0)
            continue;
        if (!gate.tryAcquire(gateClass))
            continue;
        if (Runnable* item = queue.pop()){
            slotClass = (int)gateClass;
            return item;
        }
        gate.release(gateClass);
    }
    return nullptr;
}

Runnable* Scheduler::steal(Worker& worker){
    const int count = workerCount;
    for (int k = 1; k < count; k++){
        Worker& victim = *workers[(size_t)((worker.index + k) % count)];
        if (victim.local.approxSize() == 0)
            continue;
        if (Runnable* item = victim.local.popFront()){
            worker.stolen.fetch_add(1, std::memory_order_relaxed);
            return item;
        }
    }
    return nullptr;
}

Runnable* Scheduler::takeNormal(Worker& worker){
    if (!normal.hasItems())
        return nullptr;
    // a run of the backlog (up to a batch) moves to this worker's deque in one take, and the
    // other workers steal from there: the consumers then contend once per batch, not per job.
    // Only the owner pushes to its deque, so the room seen here can only grow.
    const size_t room = kLocalCapacity - std::min<size_t>(worker.local.approxSize(), kLocalCapacity);
    const size_t want = std::min(kNormalBatch, room + 1);
    Runnable* batch[kNormalBatch];
    const size_t count = normal.popBatch(batch, want);
    if (count == 0)
        return nullptr;
    if (count > 1){
        // newest first: the owner pops LIFO, so it runs the rest in submission order
        std::reverse(batch + 1, batch + count);
        worker.local.pushBackBatch(batch + 1, count - 1);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (sleepers.load(std::memory_order_seq_cst) > 0)
            wake(1, true);
    }
    return batch[0];
}

Runnable* Scheduler::findWork(Worker& worker, int& slotClass){
    slotClass = -1;
    if (Runnable* item = high.pop())
        return item;
    if (Runnable* item = worker.local.popBack())
        return item;
    // progress guarantee: the highest-index worker polls Background before Normal
    const bool backgroundFirst = workerCount >= 3 && worker.index == workerCount - 1;
    if (backgroundFirst){
        if (Runnable* item = popBackground(worker, slotClass))
            return item;
    }
    if (Runnable* item = takeNormal(worker))
        return item;
    if (Runnable* item = steal(worker))
        return item;
    if (!backgroundFirst)
        return popBackground(worker, slotClass);
    return nullptr;
}

void Scheduler::workerMain(Worker& worker){
    char name[24];
    std::snprintf(name, sizeof(name), "je-worker-%d", worker.index);
    thread::setCurrentName(name);
    thread::setCurrentPriority(thread::Priority::Normal);
    threadState.workerOf = this;
    threadState.workerIndex = worker.index;
    currentSlotIndex(ThreadKind::Worker, name);
    if (const ProfilerHooks* h = hooks()){
        if (h->onThreadStart){
            if (ThreadSlot* slot = slotAt(threadState.slot))
                slot->hooksEpoch.store(hooksEpoch(), std::memory_order_relaxed);
            h->onThreadStart(name);
        }
    }

    for (;;){
        int slotClass = -1;
        Runnable* item = findWork(worker, slotClass);
        if (item){
            // propagate: more backlog than this worker can take
            if (sleepers.load(std::memory_order_relaxed) > 0 && hasWork())
                wake(1, true);
            execute(item);
            if (slotClass >= 0)
                gate.release((GateClass)slotClass);
            continue;
        }
        if (stopWorkers.load(std::memory_order_acquire))
            break;

        const uint64_t idleStart = nowNs();
        worker.idleSince.store(idleStart, std::memory_order_relaxed);
        const ProfilerHooks* h = hooks();
        if (h && h->onIdleBegin)
            h->onIdleBegin();

        bool found = false;
        if (spinners.fetch_add(1, std::memory_order_seq_cst) < maxSpinners){
            const uint64_t deadline = idleStart + 50000;
            while (!found){
                for (int i = 0; i < 64; i++)
                    cpuRelax();
                found = hasWork() || stopWorkers.load(std::memory_order_relaxed);
                if (nowNs() >= deadline)
                    break;
            }
        }
        spinners.fetch_sub(1, std::memory_order_seq_cst);
        if (!found){
            std::unique_lock<std::mutex> lock(sleepMutex);
            sleepers.fetch_add(1, std::memory_order_seq_cst);
            if (!hasWork() && !stopWorkers.load(std::memory_order_acquire))
                timedWait(sleepCv, lock, std::chrono::milliseconds(1));
            sleepers.fetch_sub(1, std::memory_order_seq_cst);
            // awake (notified, timed out or never asleep): the next single wake notifies again
            wakePending.store(false, std::memory_order_seq_cst);
        }

        worker.idleSince.store(0, std::memory_order_relaxed);
        worker.idleNs.fetch_add(nowNs() - idleStart, std::memory_order_relaxed);
        h = hooks();
        if (h && h->onIdleEnd)
            h->onIdleEnd();
    }
}

// ---- waiting

TaskState* Scheduler::helpTarget(TaskState* task){
    // descends to the first unfinished dependency that is not itself blocked on others:
    // every task on the path stays alive (each holds its dependencies through its links)
    TaskState* current = task;
    for (int depth = 0; depth < 4096; depth++){
        if (statusOf(current) != TaskStatus::Pending)
            return current;
        if (current->flags & (kFlagChannel | kFlagStrand | kFlagIoGated))
            return current;
        if (current->pendingDeps.load(std::memory_order_acquire) == 0)
            return current;
        TaskState* next = nullptr;
        for (DepLink* link = current->deps; link; link = link->nextOfDependent){
            if (!isTerminal(statusOf(link->dep))){
                next = link->dep;
                break;
            }
        }
        if (!next)
            return current;  // its last dependency is resolving it right now
        current = next;
    }
    return current;
}

void Scheduler::reportWaitCycle(TaskState* task, const char* what){
    char message[256];
    std::snprintf(message, sizeof(message), "wait cycle: a wait on job '%s' %s; it can never complete",
                  task->name ? task->name : "job", what);
    fatal(message);
}

bool Scheduler::confirmWaitCycle(TaskState* task){
    // the chain: the thread running `task` -> the task it is parked on -> its runner ...
    const int self = threadState.slot;
    if (self < 0)
        return false;
    int runner = task->runner.load(std::memory_order_acquire);
    for (int hops = 0; hops < (int)kMaxThreadSlots && runner >= 0; hops++){
        if (runner == self)
            return true;
        ThreadSlot* slot = slotAt(runner);
        if (!slot)
            return false;
        runner = slot->waitingOnRunner.load(std::memory_order_acquire);
    }
    return false;
}

void Scheduler::park(TaskState* task, double timeoutMs){
    if (!thread::canBlock()){
        spinWait();
        return;
    }
    std::unique_lock<std::mutex> lock(bucketMutex(task));
    task->parked.fetch_add(1, std::memory_order_seq_cst);
    if (!isTerminal(statusOf(task)))
        timedWait(bucketCv(task), lock, std::chrono::microseconds((int64_t)(timeoutMs * 1000.0)));
    task->parked.fetch_sub(1, std::memory_order_seq_cst);
}

void Scheduler::waitTask(TaskState* task){
    if (isTerminal(statusOf(task)))
        return;
    WaitHookScope hookScope(WaitKind::Task);
    const int self = currentSlotIndex();
    ThreadSlot* selfSlot = slotAt(self);
    uint64_t parkedSince = 0;
    bool cycleSuspected = false;

    while (!isTerminal(statusOf(task))){
        TaskState* target = helpTarget(task);
        const TaskStatus status = statusOf(target);
        if (isTerminal(status))
            continue;

        if (status == TaskStatus::Pending){
            if (target->flags & kFlagStrand){
                if (strandHelp(static_cast<StrandState*>(target->owner)))
                    continue;
                park(target, 1.0);
                continue;
            }
            if (target->flags & kFlagChannel){
                if (config.detectWaitCycles && threadState.body.channel == target->owner)
                    reportWaitCycle(target, "comes from a job of the same channel (the channel's cap can be taken by the waiters)");
                if (singleThread && tryClaim(target)){
                    runClaimed(target);
                    continue;
                }
                park(target, 1.0);
                continue;
            }
            if (target->pendingDeps.load(std::memory_order_acquire) == 0){
                if (tryClaim(target))
                    runClaimed(target);
                continue;
            }
            if (target->flags & kFlagIoGated){
                ExternalGate* gateOwner = static_cast<ExternalGate*>(target->owner);
                if (gateOwner->help && gateOwner->help(gateOwner))
                    continue;
                park(target, 1.0);
                continue;
            }
            // transient: its last dependency is resolving it
            std::this_thread::yield();
            continue;
        }

        // Running on some thread
        const int runner = target->runner.load(std::memory_order_acquire);
        if (runner >= 0 && runner == self && config.detectWaitCycles)
            reportWaitCycle(target, "is made from inside that job (directly or through its dependencies)");
        if (selfSlot)
            selfSlot->waitingOnRunner.store(runner, std::memory_order_release);
        if (parkedSince == 0)
            parkedSince = nowNs();
        if (config.detectWaitCycles && nowNs() - parkedSince > 100000000ull){
            // a cross-thread cycle is stable: confirm it twice, 50 ms apart
            if (confirmWaitCycle(target)){
                if (cycleSuspected)
                    reportWaitCycle(target, "closes a cycle of waits across threads");
                cycleSuspected = true;
            }else{
                cycleSuspected = false;
            }
            park(target, 50.0);
        }else{
            park(target, 10.0);
        }
        if (selfSlot)
            selfSlot->waitingOnRunner.store(-1, std::memory_order_release);
    }
}

// ---- single-thread mode

void Scheduler::pump(double budgetMs){
    if (!singleThread)
        return;
    for (;;){
        Runnable* item = high.pop();
        if (!item)
            item = normal.pop();
        if (!item)
            break;
        execute(item);
    }
    runChannelsInline();
    const uint64_t start = nowNs();
    const uint64_t budget = (uint64_t)(std::max(budgetMs, 0.0) * 1.0e6);
    while (nowNs() - start < budget){
        Runnable* item = backgroundAdmitted.pop();
        if (!item)
            item = backgroundDefault.pop();
        if (!item)
            item = backgroundHeavy.pop();
        if (!item)
            break;
        execute(item);
        // urgent work a Background job queued runs before the next Background one
        for (;;){
            Runnable* urgent = high.pop();
            if (!urgent)
                urgent = normal.pop();
            if (!urgent)
                break;
            execute(urgent);
        }
    }
}

// ---- shutdown

void Scheduler::drainQueues(){
    for (;;){
        Runnable* item = high.pop();
        if (!item) item = normal.pop();
        if (!item) item = backgroundAdmitted.pop();
        if (!item) item = backgroundDefault.pop();
        if (!item) item = backgroundHeavy.pop();
        if (!item){
            for (auto& worker : workers){
                item = worker->local.popFront();
                if (item)
                    break;
            }
        }
        if (!item)
            break;
        if (item->kind == RunnableKind::Job){
            // bare work (detached jobs, counter jobs, helpers, strand drains) is must-run
            runBareJob(static_cast<Job*>(item));
        }else{
            TaskState* task = static_cast<TaskState*>(item);
            cancelPending(task);
            releaseTask(task);
        }
    }
}

void Scheduler::shutdown(){
    std::lock_guard<std::mutex> guard(shutdownMutex);
    if (shutdownDone.load(std::memory_order_acquire))
        return;
    shuttingDown.store(true, std::memory_order_seq_cst);

    {
        std::lock_guard<std::mutex> lock(sleepMutex);
        stopWorkers.store(true, std::memory_order_seq_cst);
        sleepCv.notify_all();
    }
    for (auto& worker : workers){
        if (worker->thread.joinable())
            worker->thread.join();
    }
    drainQueues();

    // blocking channels: cancel what has not started, let the running jobs finish
    std::vector<TaskState*> abandoned;
    {
        std::lock_guard<std::mutex> lock(blockingMutex);
        blockingStop = true;
        for (ChannelState* channel : channels){
            for (TaskState* task = channel->head; task; ){
                TaskState* next = static_cast<TaskState*>(task->next);
                task->next = nullptr;
                abandoned.push_back(task);
                task = next;
            }
            channel->head = channel->tail = nullptr;
            channel->queued = 0;
        }
        blockingCv.notify_all();
    }
    for (TaskState* task : abandoned){
        cancelPending(task);
        releaseTask(task);
    }
    for (std::thread& thread : blockingThreads){
        if (thread.joinable())
            thread.join();
    }
    blockingThreads.clear();

    // anything a finishing job published meanwhile
    drainQueues();
    shutdownDone.store(true, std::memory_order_release);
}

// ---- strands

void Scheduler::strandPush(StrandState* strand, TaskState* task){
    task->scheduler = this;
    task->owner = strand;
    if (task->tags & Tag::Asset)
        assetJobs.fetch_add(1, std::memory_order_relaxed);
    if (shuttingDown.load(std::memory_order_seq_cst)){
        cancelPending(task);
        return;
    }
    retainTask(task);
    bool needDrain = false;
    {
        std::lock_guard<std::mutex> lock(strand->mutex);
        task->next = nullptr;
        if (strand->tail)
            strand->tail->next = task;
        else
            strand->head = task;
        strand->tail = task;
        strand->pending++;
        if (!strand->scheduled && !strand->running){
            strand->scheduled = true;
            needDrain = true;
        }
    }
    if (needDrain)
        publishStrandDrain(strand);
}

void Scheduler::publishStrandDrain(StrandState* strand){
    strand->refs.fetch_add(1, std::memory_order_relaxed);
    Job* job = makeJob([strand](){
        strand->scheduler->strandDrain(strand);
    }, strand->lane, strand->gate, strand->name, 0);
    job->flags |= kFlagInternal;
    publishJob(job);
}

void Scheduler::strandRunItem(StrandState* strand, TaskState* task){
    if (tryClaim(task))
        runClaimed(task);
    {
        std::lock_guard<std::mutex> lock(strand->mutex);
        strand->pending--;
        if (strand->pending == 0)
            strand->idle.notify_all();
    }
    releaseTask(task);
}

namespace{
    void releaseStrand(StrandState* strand){
        if (strand->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete strand;
    }
}

void Scheduler::strandDrain(StrandState* strand){
    TaskState* task = nullptr;
    {
        std::lock_guard<std::mutex> lock(strand->mutex);
        strand->scheduled = false;
        if (!strand->running && strand->head){
            strand->running = true;
            task = strand->head;
            strand->head = static_cast<TaskState*>(task->next);
            if (!strand->head)
                strand->tail = nullptr;
            task->next = nullptr;
        }
    }
    if (task){
        strandRunItem(strand, task);
        bool reschedule = false;
        {
            std::lock_guard<std::mutex> lock(strand->mutex);
            strand->running = false;
            if (strand->head && !strand->scheduled){
                strand->scheduled = true;
                reschedule = true;
            }
            if (!strand->head)
                strand->idle.notify_all();
        }
        if (reschedule)
            publishStrandDrain(strand);
    }
    releaseStrand(strand);
}

bool Scheduler::strandHelp(StrandState* strand){
    TaskState* task = nullptr;
    {
        std::lock_guard<std::mutex> lock(strand->mutex);
        if (strand->running || !strand->head)
            return false;
        strand->running = true;
        task = strand->head;
        strand->head = static_cast<TaskState*>(task->next);
        if (!strand->head)
            strand->tail = nullptr;
        task->next = nullptr;
    }
    strandRunItem(strand, task);
    bool reschedule = false;
    {
        std::lock_guard<std::mutex> lock(strand->mutex);
        strand->running = false;
        if (strand->head && !strand->scheduled){
            strand->scheduled = true;
            reschedule = true;
        }
    }
    if (reschedule)
        publishStrandDrain(strand);
    return true;
}

void Scheduler::strandCancelPending(StrandState* strand){
    TaskState* list = nullptr;
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(strand->mutex);
        list = strand->head;
        strand->head = strand->tail = nullptr;
        for (TaskState* task = list; task; task = static_cast<TaskState*>(task->next))
            count++;
        strand->pending -= count;
        if (strand->pending == 0)
            strand->idle.notify_all();
    }
    while (list){
        TaskState* next = static_cast<TaskState*>(list->next);
        list->next = nullptr;
        cancelPending(list);
        releaseTask(list);
        list = next;
    }
}

void Scheduler::strandWaitIdle(StrandState* strand){
    WaitHookScope hookScope(WaitKind::Strand);
    for (;;){
        {
            std::unique_lock<std::mutex> lock(strand->mutex);
            if (strand->pending == 0)
                return;
            if (strand->running || !strand->head){
                if (!thread::canBlock()){
                    lock.unlock();
                    spinWait();
                }else{
                    timedWait(strand->idle, lock, std::chrono::milliseconds(1));
                }
                continue;
            }
        }
        strandHelp(strand);
    }
}

// ---- channels

void Scheduler::channelRegister(ChannelState* channel){
    std::lock_guard<std::mutex> lock(blockingMutex);
    channels.push_back(channel);
}

void Scheduler::channelUnregister(ChannelState* channel){
    std::vector<TaskState*> abandoned;
    {
        std::unique_lock<std::mutex> lock(blockingMutex);
        for (TaskState* task = channel->head; task; ){
            TaskState* next = static_cast<TaskState*>(task->next);
            task->next = nullptr;
            abandoned.push_back(task);
            task = next;
        }
        channel->head = channel->tail = nullptr;
        channel->queued = 0;
        while (channel->running > 0)
            blockingIdle.wait(lock);
        channels.erase(std::remove(channels.begin(), channels.end(), channel), channels.end());
    }
    for (TaskState* task : abandoned){
        cancelPending(task);
        releaseTask(task);
    }
}

void Scheduler::channelSubmit(ChannelState* channel, TaskState* task){
    task->scheduler = this;
    task->owner = channel;
    if (task->tags & Tag::Asset)
        assetJobs.fetch_add(1, std::memory_order_relaxed);
    if (shuttingDown.load(std::memory_order_seq_cst)){
        cancelPending(task);
        return;
    }
    retainTask(task);
    std::unique_lock<std::mutex> lock(blockingMutex);
    if (blockingStop){
        // shutdown took the queues already
        lock.unlock();
        cancelPending(task);
        releaseTask(task);
        return;
    }
    task->next = nullptr;
    if (channel->tail)
        channel->tail->next = task;
    else
        channel->head = task;
    channel->tail = task;
    channel->queued++;
    if (singleThread)
        return;
    uint32_t sumCaps = 0;
    for (ChannelState* registered : channels)
        sumCaps += registered->cap;
    const size_t limit = (size_t)std::min<int>(blockingBudget, (int)sumCaps);
    // The idle threads cover the jobs a thread may start now (queued, under their channel's cap),
    // this one and the ones submitted before it that no woken thread has picked yet; one more
    // thread when they don't. (Counting only "is a thread idle" let two submits in a row share
    // one idle thread: it could take the second and leave the first queued for good, as a
    // watcher job waiting for the job submitted just before it.)
    uint32_t startable = 0;
    for (ChannelState* registered : channels)
        startable += std::min(registered->queued, registered->cap > registered->running ? registered->cap - registered->running : 0u);
    if ((int)startable > blockingIdleThreads && blockingThreads.size() < limit && channel->running < channel->cap){
        const int index = (int)blockingThreads.size();
        blockingThreads.emplace_back([this, index](){ blockingThreadMain(index); });
    }else{
        blockingCv.notify_one();
    }
}

void Scheduler::blockingThreadMain(int index){
    char name[24];
    std::snprintf(name, sizeof(name), "je-block-%d", index);
    thread::setCurrentName(name);
    thread::setCurrentPriority(thread::Priority::BelowNormal);
    currentSlotIndex(ThreadKind::Blocking, name);

    std::unique_lock<std::mutex> lock(blockingMutex);
    for (;;){
        ChannelState* picked = nullptr;
        if (!channels.empty()){
            for (size_t k = 0; k < channels.size(); k++){
                ChannelState* channel = channels[(blockingNext + k) % channels.size()];
                if (channel->head && channel->running < channel->cap){
                    picked = channel;
                    blockingNext = (blockingNext + k + 1) % channels.size();
                    break;
                }
            }
        }
        if (!picked){
            if (blockingStop)
                return;
            blockingIdleThreads++;
            blockingCv.wait(lock);
            blockingIdleThreads--;
            continue;
        }
        TaskState* task = picked->head;
        picked->head = static_cast<TaskState*>(task->next);
        if (!picked->head)
            picked->tail = nullptr;
        task->next = nullptr;
        picked->queued--;
        picked->running++;
        lock.unlock();

        if (tryClaim(task))
            runClaimed(task);
        releaseTask(task);

        lock.lock();
        picked->running--;
        blockingIdle.notify_all();
        // the slot this freed may admit a job of the same channel
        blockingCv.notify_one();
    }
}

void Scheduler::runChannelsInline(){
    for (;;){
        TaskState* task = nullptr;
        {
            std::lock_guard<std::mutex> lock(blockingMutex);
            for (ChannelState* channel : channels){
                if (channel->head){
                    task = channel->head;
                    channel->head = static_cast<TaskState*>(task->next);
                    if (!channel->head)
                        channel->tail = nullptr;
                    task->next = nullptr;
                    channel->queued--;
                    channel->running++;
                    break;
                }
            }
        }
        if (!task)
            return;
        ChannelState* channel = static_cast<ChannelState*>(task->owner);
        if (tryClaim(task))
            runClaimed(task);
        releaseTask(task);
        std::lock_guard<std::mutex> lock(blockingMutex);
        channel->running--;
        blockingIdle.notify_all();
    }
}

// ---- counters

void Scheduler::publishCounterJob(CounterQueue* queue, Job* job, Lane lane){
    job->lane = clampLane(lane);
    {
        std::lock_guard<std::mutex> lock(queue->mutex);
        job->next = nullptr;
        if (queue->tail)
            queue->tail->next = job;
        else
            queue->head = job;
        queue->tail = job;
    }
    // one visit per job: runs the next job of the queue, wherever the counter's waiter is
    queue->refs.fetch_add(1, std::memory_order_relaxed);
    Job* visit = makeJob([queue, this](){
        Job* next = nullptr;
        {
            std::lock_guard<std::mutex> lock(queue->mutex);
            next = queue->head;
            if (next){
                queue->head = static_cast<Job*>(next->next);
                if (!queue->head)
                    queue->tail = nullptr;
                next->next = nullptr;
            }
        }
        if (next)
            runBareJob(next);
        releaseCounterQueue(queue);
    }, job->lane, GateClass::Default, job->name, 0);
    visit->flags |= kFlagInternal;
    publishJob(visit);
}

CounterQueue* jobs::detail::createCounterQueue(){
    return ::new (acquireSlab(SlabClass::Job)) CounterQueue();
}

void jobs::detail::releaseCounterQueue(CounterQueue* queue) noexcept{
    if (queue && queue->refs.fetch_sub(1, std::memory_order_acq_rel) == 1){
        queue->~CounterQueue();
        releaseSlab(SlabClass::Job, queue);
    }
}

// ---- stats

void Scheduler::sampleFrame(){
    std::lock_guard<std::mutex> lock(statsMutex);
    const uint64_t now = nowNs();
    const uint64_t wall = now > lastSampleNs ? now - lastSampleNs : 1;
    for (auto& worker : workers){
        uint64_t idle = worker->idleNs.load(std::memory_order_relaxed);
        const uint64_t since = worker->idleSince.load(std::memory_order_relaxed);
        if (since != 0 && now > since)
            idle += now - since;
        const uint64_t idleDelta = idle > worker->sampledIdleNs ? idle - worker->sampledIdleNs : 0;
        const uint64_t executed = worker->executed.load(std::memory_order_relaxed);
        const float busy = 100.0f * (1.0f - std::min(1.0f, (float)idleDelta / (float)wall));
        worker->busyHistory[historyCursor] = busy;
        worker->jobsHistory[historyCursor] = executed - worker->sampledExecuted;
        worker->sampledIdleNs = idle;
        worker->sampledExecuted = executed;
    }
    gatePeakHistory[historyCursor] = gate.takePeaks();
    historyCursor = (historyCursor + 1) % kHistoryFrames;
    sampledFrames = std::min(sampledFrames + 1, kHistoryFrames);
    lastSampleNs = now;
}

Stats Scheduler::stats(int windowFrames) const{
    Stats result;
    result.workers = workerCount;
    result.singleThread = singleThread;
    result.lanes[(size_t)Lane::High].queued = high.count();
    result.lanes[(size_t)Lane::Normal].queued = normal.count() + localBacklog();
    result.lanes[(size_t)Lane::Background].queued = backgroundAdmitted.count() + backgroundDefault.count() + backgroundHeavy.count();
    for (uint32_t i = 0; i < kMaxThreadSlots; i++){
        const ThreadSlot& slot = threadSlots()[i];
        if (slot.used.load(std::memory_order_acquire) != 2)
            continue;
        if (slot.runningScheduler.load(std::memory_order_relaxed) != this || !slot.runningJob.load(std::memory_order_relaxed))
            continue;
        const uint8_t lane = slot.runningLane.load(std::memory_order_relaxed);
        if (lane < kLaneCount)
            result.lanes[lane].running++;
    }
    result.gate.max = gate.maxBackground();
    result.gate.taken = gate.taken();
    result.gate.heavyMax = gate.heavyMax();
    result.gate.heavyTaken = gate.heavyTaken();
    result.gate.reserve = gate.getReserve();

    {
        std::lock_guard<std::mutex> lock(blockingMutex);
        for (const ChannelState* channel : channels){
            ChannelStats entry;
            entry.name = channel->name;
            entry.queued = channel->queued;
            entry.running = channel->running;
            entry.cap = channel->cap;
            result.channels.push_back(entry);
        }
        result.blockingThreads = (uint32_t)blockingThreads.size();
    }
    uint64_t executed = externalExecuted.load(std::memory_order_relaxed);
    uint64_t stolen = 0;
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        const int window = std::max(1, std::min(windowFrames, kHistoryFrames));
        const int frames = std::min(window, sampledFrames);
        result.windowFrames = frames;
        // the frame in progress, then the sampled ones
        const BackgroundGate::Peaks current = gate.peeks();
        result.gate.peakTaken = current.taken;
        result.gate.peakHeavyTaken = current.heavy;
        for (int k = 1; k <= frames; k++){
            const BackgroundGate::Peaks& peak = gatePeakHistory[(historyCursor - k + kHistoryFrames) % kHistoryFrames];
            result.gate.peakTaken = std::max(result.gate.peakTaken, peak.taken);
            result.gate.peakHeavyTaken = std::max(result.gate.peakHeavyTaken, peak.heavy);
        }
        const uint64_t now = nowNs();
        for (const auto& worker : workers){
            WorkerStats entry;
            if (frames > 0){
                float busy = 0.0f;
                uint64_t jobs = 0;
                for (int k = 1; k <= frames; k++){
                    const int index = (historyCursor - k + kHistoryFrames) % kHistoryFrames;
                    busy += worker->busyHistory[index];
                    jobs += worker->jobsHistory[index];
                }
                entry.busyPct = busy / (float)frames;
                entry.jobs = jobs;
            }else{
                uint64_t idle = worker->idleNs.load(std::memory_order_relaxed);
                const uint64_t since = worker->idleSince.load(std::memory_order_relaxed);
                if (since != 0 && now > since)
                    idle += now - since;
                const uint64_t wall = now > startNs ? now - startNs : 1;
                entry.busyPct = 100.0f * (1.0f - std::min(1.0f, (float)idle / (float)wall));
                entry.jobs = worker->executed.load(std::memory_order_relaxed);
            }
            result.perWorker.push_back(entry);
            executed += worker->executed.load(std::memory_order_relaxed);
            stolen += worker->stolen.load(std::memory_order_relaxed);
        }
    }
    result.totals.executed = executed;
    result.totals.stolen = stolen;
    result.totals.cancelled = cancelledCount.load(std::memory_order_relaxed);
    result.totals.failed = failedCount.load(std::memory_order_relaxed);
    result.totals.heapEnvelopes = heapEnvelopesAllocated();
    result.assetJobs = (uint32_t)std::max(0, assetJobs.load(std::memory_order_relaxed));
    return result;
}

std::vector<float> Scheduler::workerHistory(int worker) const{
    std::vector<float> history;
    std::lock_guard<std::mutex> lock(statsMutex);
    if (worker < 0 || worker >= (int)workers.size())
        return history;
    history.reserve((size_t)sampledFrames);
    for (int k = sampledFrames; k >= 1; k--){
        const int index = (historyCursor - k + kHistoryFrames) % kHistoryFrames;
        history.push_back(workers[(size_t)worker]->busyHistory[index]);
    }
    return history;
}

// ---- exported detail entry points

void jobs::detail::publishTask(Scheduler* scheduler, TaskState* task, const TaskRef* deps, size_t depCount){
    scheduler->publishTask(task, deps, depCount);
}

void jobs::detail::publishJob(Scheduler* scheduler, Job* job){
    job->lane = scheduler->clampLane(job->lane);
    scheduler->publishJob(job);
}

Scheduler* jobs::detail::schedulerOf(JobSystem* system){
    return (system ? system : &jobs::system())->getScheduler();
}

JobSystem* jobs::detail::systemOf(TaskState* task){
    return task->scheduler ? task->scheduler->owner : &jobs::system();
}

void jobs::detail::publishCounterJob(JobSystem* system, CounterQueue* queue, Job* job, Lane lane){
    schedulerOf(system)->publishCounterJob(queue, job, lane);
}

// ---- JobSystem

JobSystem::JobSystem(const Config& config): scheduler(new Scheduler(this, config)){
    scheduler->start();
    channels[0] = std::make_unique<JobChannel>("process", 2, this);
    channels[1] = std::make_unique<JobChannel>("io-write", 1, this);
    channels[2] = std::make_unique<JobChannel>("net", 2, this);
}

JobSystem::~JobSystem(){
    shutdown();
    for (auto& channel : channels)
        channel.reset();
    delete scheduler;
}

void JobSystem::shutdown(){
    scheduler->shutdown();
}

bool JobSystem::isShuttingDown() const{
    return scheduler->shuttingDown.load(std::memory_order_acquire);
}

int JobSystem::getWorkerCount() const{
    return scheduler->workerCount;
}

bool JobSystem::isSingleThreaded() const{
    return scheduler->singleThread;
}

uint32_t JobSystem::getAssetJobCount() const{
    return (uint32_t)std::max(0, scheduler->assetJobs.load(std::memory_order_relaxed));
}

void JobSystem::pump(double budgetMs){
    scheduler->pump(budgetMs);
}

BackgroundGate& JobSystem::getGate(){
    return scheduler->gate;
}

JobChannel& JobSystem::processChannel(){
    return *channels[0];
}

JobChannel& JobSystem::ioWriteChannel(){
    return *channels[1];
}

JobChannel& JobSystem::netChannel(){
    return *channels[2];
}

void JobSystem::sampleFrame(){
    scheduler->sampleFrame();
}

Stats JobSystem::stats(int windowFrames) const{
    return scheduler->stats(windowFrames);
}

std::vector<float> JobSystem::workerHistory(int worker) const{
    return scheduler->workerHistory(worker);
}

// ---- the process instance

JobSystem& jobs::system(){
    JobSystem* instance = processInstance.load(std::memory_order_acquire);
    if (instance)
        return *instance;
    return initialize(Config());
}

bool jobs::hasSystem(){
    return processInstance.load(std::memory_order_acquire) != nullptr;
}

JobSystem& jobs::initialize(const Config& config){
    std::lock_guard<std::mutex> lock(instanceMutex());
    JobSystem* instance = processInstance.load(std::memory_order_acquire);
    if (instance && instance->isShuttingDown()){
        // a new engine run in the same process (a recreated Android activity)
        processInstance.store(nullptr, std::memory_order_release);
        delete instance;
        instance = nullptr;
    }
    if (instance){
        const int wanted = thread::resolveWorkerCount(config.workers);
        if (wanted != instance->getWorkerCount())
            Log::verbose("JobSystem already runs %d workers; %d apply on the next launch", instance->getWorkerCount(), wanted);
        // -1 (Auto) is a value too: going back to Auto restores the platform's reserve
        instance->getGate().setReserve(config.reserveForFrame >= 0 ? config.reserveForFrame : defaultReserve());
        return *instance;
    }
    instance = new JobSystem(config);
    processInstance.store(instance, std::memory_order_release);
    return *instance;
}

void jobs::shutdown(){
    std::lock_guard<std::mutex> lock(instanceMutex());
    JobSystem* instance = processInstance.load(std::memory_order_acquire);
    if (!instance)
        return;
    // the object stays: work submitted later (static destructors) sees a shut-down system,
    // never a freed one or a new pool; initialize() replaces it for a new run
    instance->shutdown();
}

void jobs::pump(double budgetMs){
    if (JobSystem* instance = processInstance.load(std::memory_order_acquire))
        instance->pump(budgetMs);
}

Stats jobs::stats(int windowFrames){
    return system().stats(windowFrames);
}

void jobs::postToMain(std::function<void()> fn){
    if (!fn)
        return;
    std::lock_guard<std::mutex> lock(mainQueueMutex());
    mainQueue().push_back(std::move(fn));
}

size_t jobs::drainMainQueue(){
    std::vector<std::function<void()>> pending;
    {
        std::lock_guard<std::mutex> lock(mainQueueMutex());
        pending.swap(mainQueue());
    }
    for (auto& fn : pending){
        try{
            fn();
        }catch(const std::exception& e){
            Log::error("postToMain callback threw: %s", e.what());
        }catch(...){
            Log::error("postToMain callback threw an exception");
        }
    }
    return pending.size();
}

bool jobs::cancelRequested(){
    const CancelState* cancel = threadState.body.cancel;
    return cancel && cancel->cancelled.load(std::memory_order_acquire);
}

bool jobs::isMainThread(){
    return threadState.isMain;
}

void jobs::markMainThread(){
    // before the thread's first job, so its slot is claimed as je-main
    threadState.isMain = true;
    thread::setCurrentName("je-main");
}

bool jobs::isWorker(){
    return threadState.workerOf != nullptr;
}

bool jobs::isExecutingJob(){
    return threadState.body.inBody;
}

bool jobs::isInChannelJob(){
    return threadState.body.channel != nullptr;
}

void jobs::setProfilerHooks(const ProfilerHooks& value){
    const bool empty = !value.onThreadStart && !value.onJobBegin && !value.onJobEnd && !value.onWaitBegin
        && !value.onWaitEnd && !value.onIdleBegin && !value.onIdleEnd;
    // hooks are installed a handful of times per run; the copies are kept for the threads
    // that may still be reading an earlier one
    const ProfilerHooks* copy = empty ? nullptr : new ProfilerHooks(value);
    installedEpoch.fetch_add(1, std::memory_order_relaxed);
    installedHooks.store(copy, std::memory_order_release);
}

size_t jobs::snapshotOccupancy(ThreadOccupancy* out, size_t capacity){
    size_t count = 0;
    const uint64_t now = nowNs();
    ThreadSlot* slots = threadSlots();
    for (uint32_t i = 0; i < kMaxThreadSlots && count < capacity; i++){
        ThreadSlot& slot = slots[i];
        if (slot.used.load(std::memory_order_acquire) != 2)
            continue;
        ThreadOccupancy& entry = out[count++];
        entry.name = slot.name;
        entry.kind = (ThreadKind)slot.kind.load(std::memory_order_relaxed);
        entry.runningJob = slot.runningJob.load(std::memory_order_relaxed);
        const uint64_t seq = slot.jobSeq.load(std::memory_order_relaxed);
        if (slot.observedSeq.load(std::memory_order_relaxed) != seq){
            slot.observedSeq.store(seq, std::memory_order_relaxed);
            slot.observedSinceNs.store(now, std::memory_order_relaxed);
        }
        entry.sinceNs = entry.runningJob ? slot.observedSinceNs.load(std::memory_order_relaxed) : 0;
    }
    return count;
}
