// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Export.h"
#include "SmallFunction.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace jasperengine {

    // The clocks timers run on (Scheduler::advance feeds them):
    //   Game        scaled by the time scale, only while game events run (not paused, held,
    //               or in the editor's Pause)
    //   Unscaled    like Game, ignoring the time scale
    //   Real        a steady wall clock, always (paused, loading)
    //   Frames      simulated frames, counted like Game
    //   FixedSteps  fixed-update iterations
    enum class TimeDomain : uint8_t { Game, Unscaled, Real, Frames, FixedSteps };
    constexpr size_t TIME_DOMAIN_COUNT = 5;

    // "game", "unscaled", "real", "frames", "fixed_steps"
    JASPERENGINE_API const char* timeDomainName(TimeDomain domain);
    JASPERENGINE_API bool parseTimeDomain(const std::string& name, TimeDomain& out);

    class Scheduler;

    // A scheduled task: copyable and weak (inert once the task is done or cancelled)
    struct TimerHandle {
        Scheduler* scheduler = nullptr;
        uint64_t id = 0;

        // From any thread. Before it starts the task never runs; while it runs this run
        // finishes, but a repeating task does not repeat. False when already done.
        bool cancel() const noexcept;
        bool active() const noexcept;
        explicit operator bool() const noexcept { return id != 0; }
    };

    struct TimerOpts {
        uint32_t count = 0;               // every(): how many times (0: until cancelled)
        bool catchUp = false;             // every(): after a stall, up to 8 missed periods at once
        uint64_t ownerKey = 0;            // cancelOwner / SubscriptionOwnerIndex::releaseOwner
        const char* debugName = nullptr;  // static text, for list_timers and crash reports
    };

    // Cancels everything added to it when destroyed (the C++ equivalent of a Subscription)
    class JASPERENGINE_API TimerGroup {
    public:
        TimerGroup() = default;
        TimerGroup(TimerGroup&& other) noexcept;
        TimerGroup& operator=(TimerGroup&& other) noexcept;
        TimerGroup(const TimerGroup&) = delete;
        TimerGroup& operator=(const TimerGroup&) = delete;
        ~TimerGroup();

        TimerHandle add(TimerHandle handle);
        void cancelAll();
        // the tasks still pending
        size_t size() const;

    private:
        std::vector<TimerHandle> handles;
    };

    struct SchedulerStats {
        size_t pending[TIME_DOMAIN_COUNT] = {0};
        size_t posted = 0;                // waiting in the inbox (post, and schedules from other threads)
        double now[TIME_DOMAIN_COUNT] = {0};
        uint64_t ran = 0;
        uint64_t cancelled = 0;
    };

    struct TimerInfo {
        uint64_t id = 0;
        TimeDomain domain = TimeDomain::Game;
        double due = 0.0;                 // in the domain's time
        double remaining = 0.0;           // due - now
        double period = 0.0;              // 0: once
        uint32_t remainingRuns = 0;       // repeating with a count: runs left (0: unlimited)
        uint64_t ownerKey = 0;
        std::string debugName;
    };

    // Delayed and repeating calls on one owner thread, on the clocks of TimeDomain:
    // - work scheduled during process() runs no earlier than the next process() of its domain;
    // - equal due times run in the order they were scheduled;
    // - a repeating task fires at most once per process() and keeps its phase (no burst
    //   after a stall) unless catchUp;
    // - post() and schedules from other threads go through an inbox the owner thread takes
    //   at processPosted() (FIFO);
    // - while a CrashHook is set tasks run under the crash guard (a faulting one is
    //   cancelled and reported); an exception thrown by a task is logged.
    // No allocation per task in steady state: tasks are SmallFunctions (48 bytes inline), slots
    // and heaps are reused.
    class JASPERENGINE_API Scheduler {
    public:
        using Task = SmallFunction<void(), 48>;

        explicit Scheduler(const char* name);
        ~Scheduler();
        Scheduler(const Scheduler&) = delete;
        Scheduler& operator=(const Scheduler&) = delete;

        // The engine's: processed by Engine::systemDraw, cleared when Play stops and at shutdown
        static Scheduler& game();
        // cancelOwner on every scheduler (SubscriptionOwnerIndex::releaseOwner). Owner thread.
        static size_t cancelOwnerEverywhere(uint64_t ownerKey);

        const char* name() const { return schedulerName; }

        // Any thread: runs at the next processPosted() on the owner thread, FIFO
        TimerHandle post(Task task, TimerOpts opts = {});
        // Any thread (from another one the delay counts from the next processPosted)
        TimerHandle after(double delay, Task task, TimeDomain domain = TimeDomain::Game, TimerOpts opts = {});
        TimerHandle every(double period, Task task, TimeDomain domain = TimeDomain::Game, TimerOpts opts = {});
        TimerHandle nextFrame(Task task, TimerOpts opts = {});
        TimerHandle afterFrames(uint32_t frames, Task task, TimerOpts opts = {});
        TimerHandle afterFixedSteps(uint32_t steps, Task task, TimerOpts opts = {});
        // at an absolute time of the domain (now(domain) + delay is after())
        TimerHandle at(TimeDomain domain, double time, Task task, TimerOpts opts = {});

        bool cancel(TimerHandle handle) noexcept;
        bool isActive(TimerHandle handle) const noexcept;
        // Owner thread: every task of the owner; returns how many
        size_t cancelOwner(uint64_t ownerKey);

        // Owner thread
        void advance(TimeDomain domain, double delta);
        double now(TimeDomain domain) const;
        // Runs the tasks of the domain due when it starts; returns how many ran
        size_t process(TimeDomain domain);
        // The inbox: posted tasks run, schedules from other threads join their domain
        size_t processPosted();
        bool hasPosted() const;
        // Cancels everything (tasks and inbox); the clocks keep their time
        void clear();
        // Seconds until the next Real task (an idle loop sleeps that long); a negative value
        // when none, 0 when something is posted
        double nextDueRealTime() const;
        SchedulerStats stats() const;
        // The nearest pending tasks (all domains when domain is null), at most max
        std::vector<TimerInfo> pending(const TimeDomain* domain = nullptr, size_t max = 100) const;

        // The task running now on the owner thread (an empty handle outside a task)
        TimerHandle current() const { return running; }

        // The thread that processes (default: the one that made the scheduler)
        void setOwnerThread();
        bool isOwnerThread() const;

    private:
        struct Slot;
        // a slot's seq changes with every push: an entry whose seq differs is stale
        struct HeapEntry {
            double due;
            uint64_t seq;
            uint32_t slot;
        };
        struct InboxEntry {
            uint32_t slot;
            uint64_t generation;
            bool posted;
            double delay;
        };

        static constexpr size_t CHUNK_SLOTS = 256;
        static constexpr size_t MAX_CHUNKS = 4096;

        const char* schedulerName;
        std::thread::id ownerThread;
        mutable std::mutex mutex;              // slot allocation, the inbox
        std::atomic<Slot*> chunks[MAX_CHUNKS];
        std::atomic<size_t> chunkCount{0};
        std::vector<uint32_t> freeSlots;
        std::vector<uint32_t> ownerFreed;      // freed by the owner thread, handed back in batches
        std::vector<InboxEntry> inbox;
        std::vector<InboxEntry> inboxWork;     // the swapped inbox (owner thread)
        std::atomic<size_t> inboxSize{0};
        std::vector<HeapEntry> heaps[TIME_DOMAIN_COUNT];
        std::vector<HeapEntry> deferred;       // scheduled during a process(): back after it
        double time[TIME_DOMAIN_COUNT] = {0};
        uint64_t nextSeq = 1;
        uint64_t ran = 0;                      // owner thread
        std::atomic<uint64_t> cancelledCount{0};
        std::atomic<uint64_t> remoteCancels{0};
        TimerHandle running;

        Slot& slotAt(uint32_t index) const;
        uint32_t allocateSlot();
        void freeSlot(uint32_t index, Slot& slot);
        TimerHandle schedule(TimeDomain domain, double dueOrDelay, bool absolute, double period, Task task, const TimerOpts& opts);
        void push(TimeDomain domain, uint32_t index, Slot& slot);
        bool runSlot(uint32_t index, Slot& slot, bool guarded);
        void sweepCancelled();
        void returnFreed();
    };

}
