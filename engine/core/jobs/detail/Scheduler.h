// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The scheduler behind JobSystem. Engine-internal: nothing here is part of the public API.
//
// The lane design (Background polled last and never in local deques or steals, bounded
// spinners with a sleep backstop, the shutdown drain rules) follows the contracts of
// OpenEngine's WorkStealingThreadPool (Modules/JobSystem/Include/JobSystem/
// WorkStealingThreadPool.h), Copyright (c) 2026 Gamecrafters Guild Limited and contributors,
// MIT License; the implementation is our own. See NOTICE.md.

#ifndef JOBS_DETAIL_SCHEDULER_H
#define JOBS_DETAIL_SCHEDULER_H

#include "jobs/BackgroundGate.h"
#include "jobs/JobStats.h"
#include "jobs/Task.h"
#include "jobs/detail/JobCore.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace jasperengine { namespace jobs { namespace detail {

    constexpr uint32_t kMaxThreadSlots = 64;
    constexpr size_t kLocalCapacity = 1024;
    constexpr size_t kLaneCapacity = 4096;
    // the most Normal jobs a worker moves from the global queue to its deque at once
    constexpr size_t kNormalBatch = 32;
    // ParallelFor helpers a lane may hold published and not started, per worker.
    constexpr uint32_t kMaxUnstartedHelpersPerWorker = 4;
    // The most helpers one parallelFor publishes.
    constexpr size_t kMaxHelpersPerRun = 64;
    constexpr int kHistoryFrames = 120;

    uint64_t nowNs();

    // ---- queues (D2: mutex-guarded rings, and a lock-free ring in front of the hot lanes)

    // A worker's local deque: the owner pushes and pops at the back (LIFO), thieves take
    // from the front (FIFO).
    // Own cache lines: the owner, thieves and the producer feeding the lanes touch them all.
    class alignas(64) LocalDeque {
    public:
        bool pushBack(Runnable* item);
        // all of them, in order, under one lock; the caller made sure they fit
        void pushBackBatch(Runnable* const* batch, size_t count);
        Runnable* popBack();
        Runnable* popFront();
        uint32_t approxSize() const { return size.load(std::memory_order_relaxed); }

    private:
        std::mutex mutex;
        std::array<Runnable*, kLocalCapacity> items{};
        size_t head = 0;
        size_t tail = 0;
        std::atomic<uint32_t> size{0};
    };

    // A global lane: an MPMC FIFO ring that overflows into an intrusive spill list.
    class alignas(64) LaneQueue {
    public:
        void push(Runnable* item);
        void pushBatch(Runnable* const* items, size_t count);
        Runnable* pop();
        // up to `max` of the oldest items under one lock; returns how many
        size_t popBatch(Runnable** out, size_t max);
        uint32_t count() const { return queued.load(std::memory_order_seq_cst); }

    private:
        void pushLocked(Runnable* item);

        std::mutex mutex;
        std::vector<Runnable*> ring = std::vector<Runnable*>(kLaneCapacity, nullptr);
        size_t head = 0;
        size_t size = 0;
        Runnable* spillHead = nullptr;
        Runnable* spillTail = nullptr;
        std::atomic<uint32_t> queued{0};
    };

    // A global lane on the hot path (High, Normal): a bounded lock-free ring of sequenced
    // slots (D. Vyukov's MPMC queue) in front of a LaneQueue it overflows into. A push
    // touches its slot and the producers' position, a take one CAS on the consumers'
    // position for a run of ready slots: a producer feeding every worker shares no lock and
    // no counter with them, only the slots it hands over (D2's escape hatch for R23).
    // FIFO: once the ring is full, pushes go to the overflow until it drains, and takes
    // empty the ring first.
    class JASPERENGINE_API FastLane {
    public:
        FastLane();
        ~FastLane();
        FastLane(const FastLane&) = delete;
        FastLane& operator=(const FastLane&) = delete;

        void push(Runnable* item);
        void pushBatch(Runnable* const* items, size_t count);
        Runnable* pop();
        // up to `max` of the oldest items; returns how many
        size_t popBatch(Runnable** out, size_t max);
        // cheap emptiness test for idle workers: reads the next slot only
        bool hasItems() const;
        // approximate (stats)
        uint32_t count() const;

    private:
        // 512 KiB per lane: a producer flooding it stays on the lock-free path for a while
        static constexpr size_t kCapacity = 32768;
        static constexpr uint64_t kMask = kCapacity - 1;

        struct Slot {
            std::atomic<uint64_t> sequence{0};
            std::atomic<Runnable*> item{nullptr};
        };

        bool tryPush(Runnable* item);

        std::unique_ptr<Slot[]> slots;
        alignas(64) std::atomic<uint64_t> enqueuePosition{0};
        alignas(64) std::atomic<uint64_t> dequeuePosition{0};
        alignas(64) LaneQueue overflow;
    };

    // ---- per-thread identity (occupancy, stats, wait-cycle detection)

    struct alignas(64) ThreadSlot {
        std::atomic<uint32_t> used{0};
        char name[24] = {};
        std::atomic<uint8_t> kind{(uint8_t)ThreadKind::Other};
        std::atomic<const char*> runningJob{nullptr};
        std::atomic<uint8_t> runningLane{0};
        std::atomic<Scheduler*> runningScheduler{nullptr};
        std::atomic<uint64_t> jobSeq{0};
        // the runner slot of the task this thread is parked on, -1 when not parked on one
        std::atomic<int32_t> waitingOnRunner{-1};
        std::atomic<uint32_t> hooksEpoch{0};
        // snapshotOccupancy's view of the job running here
        std::atomic<uint64_t> observedSeq{0};
        std::atomic<uint64_t> observedSinceNs{0};
    };

    ThreadSlot* slotAt(int index);

    // ---- strands, channels, counters

    struct StrandState {
        Scheduler* scheduler = nullptr;
        const char* name = "serial";
        Lane lane = Lane::Background;
        GateClass gate = GateClass::Default;
        std::mutex mutex;
        std::condition_variable idle;
        TaskState* head = nullptr;
        TaskState* tail = nullptr;
        size_t pending = 0;
        bool running = false;
        bool scheduled = false;
        std::atomic<uint32_t> refs{1};
    };

    struct ChannelState {
        Scheduler* scheduler = nullptr;
        const char* name = "channel";
        uint32_t cap = 1;
        // guarded by the blocking pool's mutex
        TaskState* head = nullptr;
        TaskState* tail = nullptr;
        uint32_t queued = 0;
        uint32_t running = 0;
    };

    struct CounterQueue {
        std::atomic<uint32_t> refs{1};
        std::mutex mutex;
        Job* head = nullptr;
        Job* tail = nullptr;
    };
    static_assert(sizeof(CounterQueue) <= kJobSlabBytes, "a counter queue fits a job slab");

    // A gated task's external dependency (the AssetIOService read). See IoGate in
    // io/AssetIOService.cpp.
    struct ExternalGate {
        std::atomic<uint32_t> refs{1};
        // A waiter helping: resolves the gate on the calling thread when it can (runs a
        // queued read itself); false when it cannot now (the read is in progress elsewhere).
        bool (*help)(ExternalGate* gate) = nullptr;
        void (*destroy)(ExternalGate* gate) = nullptr;
    };
    JASPERENGINE_API void retainGate(ExternalGate* gate);
    JASPERENGINE_API void releaseGate(ExternalGate* gate);

    // ---- workers

    struct alignas(64) Worker {
        Scheduler* scheduler = nullptr;
        int index = 0;
        std::thread thread;
        LocalDeque local;
        std::atomic<uint64_t> executed{0};
        std::atomic<uint64_t> stolen{0};
        std::atomic<uint64_t> idleNs{0};
        // when the current idle span began, 0 while running
        std::atomic<uint64_t> idleSince{0};
        bool backgroundHeavyFirst = false;
        // sampleFrame's history (statsMutex)
        uint64_t sampledIdleNs = 0;
        uint64_t sampledExecuted = 0;
        float busyHistory[kHistoryFrames] = {};
        uint64_t jobsHistory[kHistoryFrames] = {};
    };

    class Scheduler {
    public:
        Scheduler(JobSystem* owner, const Config& config);
        ~Scheduler();

        void start();
        void shutdown();

        // publishing
        void publishTask(TaskState* task, const TaskRef* deps, size_t depCount);
        void publishJob(Job* job);
        void publishBatch(Job* const* jobs, size_t count, Lane lane, GateClass gate);
        // a ready task or a bare job into its lane
        void enqueue(Runnable* item);
        // wakes sleeping workers; one item is left to a spinning worker unless `force`
        void wake(size_t count, bool force = false);

        // running
        bool tryClaim(TaskState* task);
        void runClaimed(TaskState* task);
        void runBareJob(Job* job);
        // a body finished on `worker` (-1: a thread that is not one of ours)
        void countExecuted(int worker);
        // a dequeued item: claims and runs it (task) or runs it (job), releasing the queue's
        // hold on it
        void execute(Runnable* item);
        void finalize(TaskState* task, TaskStatus status);
        // claims a Pending task and ends it Cancelled; false when it was no longer Pending
        bool cancelPending(TaskState* task);
        void resolveDep(TaskState* task, bool failed);
        void onReady(TaskState* task);

        // waiting
        void waitTask(TaskState* task);
        // parks on `task` until it is terminal or `timeoutMs` passes
        void park(TaskState* task, double timeoutMs);

        // single-thread mode
        void pump(double budgetMs);

        // strands
        void strandPush(StrandState* strand, TaskState* task);
        void publishStrandDrain(StrandState* strand);
        void strandDrain(StrandState* strand);
        // runs the next item of the strand on the calling thread if no thread is running it;
        // false when the strand is busy or empty
        bool strandHelp(StrandState* strand);
        void strandRunItem(StrandState* strand, TaskState* task);
        void strandCancelPending(StrandState* strand);
        void strandWaitIdle(StrandState* strand);

        // channels
        void channelRegister(ChannelState* channel);
        void channelUnregister(ChannelState* channel);
        void channelSubmit(ChannelState* channel, TaskState* task);
        void blockingThreadMain(int index);
        // single-thread mode: runs every queued channel job
        void runChannelsInline();

        // counters
        void publishCounterJob(CounterQueue* queue, Job* job, Lane lane);

        // stats
        void sampleFrame();
        Stats stats(int windowFrames) const;
        std::vector<float> workerHistory(int worker) const;

        // the lane a fork from the calling thread takes when it asks for `lane`
        Lane clampLane(Lane lane) const;

        // claims a Pending task and ends it Failed with `error`, unrun
        bool failPending(TaskState* task, std::exception_ptr error);

        // called after every gate release (the asset readers wait for a slot)
        void onGateRelease();
        void addGateListener(void* context, void (*callback)(void*));
        void removeGateListener(void* context);

        JobSystem* owner;
        Config config;
        int workerCount = 0;
        bool singleThread = false;
        int blockingBudget = 0;
        BackgroundGate gate;

        FastLane high;
        FastLane normal;
        LaneQueue backgroundAdmitted;
        LaneQueue backgroundDefault;
        LaneQueue backgroundHeavy;

        std::vector<std::unique_ptr<Worker>> workers;

        alignas(64) std::mutex sleepMutex;
        std::condition_variable sleepCv;
        std::atomic<int> sleepers{0};
        std::atomic<int> spinners{0};
        // a single wake signalled and not yet taken by a worker (see wake())
        std::atomic<bool> wakePending{false};
        int maxSpinners = 2;

        // read on every loop of every worker: apart from the counters written on idle
        alignas(64) std::atomic<bool> shuttingDown{false};
        std::atomic<bool> stopWorkers{false};
        std::atomic<bool> shutdownDone{false};
        std::mutex shutdownMutex;

        std::atomic<uint64_t> cancelledCount{0};
        std::atomic<uint64_t> failedCount{0};
        std::atomic<uint64_t> externalExecuted{0};
        std::atomic<int32_t> assetJobs{0};
        std::atomic<uint32_t> unstartedHelpers[kLaneCount] = {};

        // blocking channels
        mutable std::mutex blockingMutex;
        std::condition_variable blockingCv;
        std::condition_variable blockingIdle;
        std::vector<ChannelState*> channels;
        std::vector<std::thread> blockingThreads;
        int blockingIdleThreads = 0;
        bool blockingStop = false;
        size_t blockingNext = 0;

        std::mutex gateListenerMutex;
        std::vector<std::pair<void*, void (*)(void*)>> gateListeners;
        std::atomic<int> gateListenerCount{0};

        mutable std::mutex statsMutex;
        uint64_t lastSampleNs = 0;
        int sampledFrames = 0;
        int historyCursor = 0;
        BackgroundGate::Peaks gatePeakHistory[kHistoryFrames] = {};
        uint64_t startNs = 0;

    private:
        void workerMain(Worker& worker);
        Runnable* findWork(Worker& worker, int& slotClass);
        Runnable* popBackground(Worker& worker, int& slotClass);
        Runnable* steal(Worker& worker);
        Runnable* takeNormal(Worker& worker);
        uint32_t localBacklog() const;
        bool hasWork() const;
        // shutdown: runs bare jobs, cancels tasks, until every queue is empty
        void drainQueues();
        TaskState* helpTarget(TaskState* task);
        void reportWaitCycle(TaskState* task, const char* what);
        bool confirmWaitCycle(TaskState* task);
    };

    // The thread-local view of the job body running on this thread.
    struct BodyContext {
        Scheduler* scheduler = nullptr;
        Lane lane = Lane::Normal;
        GateClass gate = GateClass::Default;
        CancelState* cancel = nullptr;
        ChannelState* channel = nullptr;
        bool inBody = false;
    };

    // Enters a job body on the calling thread (occupancy, hooks, cancel and lane context)
    // and restores the outer one on exit, so inline runs nest.
    class BodyScope {
    public:
        BodyScope(Scheduler* scheduler, const Runnable* item, CancelState* cancel, ChannelState* channel);
        ~BodyScope();

    private:
        BodyContext saved;
        const char* savedJob;
        uint8_t savedLane;
        Scheduler* savedScheduler;
        ThreadSlot* slot;
        Scheduler* owner;
        // the calling thread's worker index in `owner`, -1 when it is not one of its workers
        int worker;
        bool hooked;
    };

    BodyContext& currentBody();
    // The calling thread's slot, claimed on first use with `kind`.
    int currentSlotIndex(ThreadKind kind = ThreadKind::Other, const char* name = nullptr);
    // The worker index of the calling thread in `scheduler`, -1 when not one of its workers.
    int currentWorkerIndex(const Scheduler* scheduler);
    // Parking lot of the waits on tasks, run blocks and strands: buckets keyed by address.
    std::mutex& bucketMutex(const void* key);
    std::condition_variable& bucketCv(const void* key);
    // A wait step on a thread that must not block: services proxied calls, yields.
    void spinWait();

    // the hooks (null when none) and their epoch
    const ProfilerHooks* hooks();
    uint32_t hooksEpoch();

}}}

#endif // JOBS_DETAIL_SCHEDULER_H
