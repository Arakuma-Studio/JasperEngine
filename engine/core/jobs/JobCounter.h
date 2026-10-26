// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The counter contract (add at submit, decrement under the mutex, filtered help, sticky
// failure) is adapted from OpenEngine's JobCounter (Modules/JobSystem/Include/JobSystem/
// JobCounter.h), Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License.
// See NOTICE.md.

#ifndef JOBS_JOBCOUNTER_H
#define JOBS_JOBCOUNTER_H

#include "jobs/detail/JobCore.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace jasperengine { namespace jobs {

    class JobSystem;

    namespace detail {
        struct CounterQueue;
        JASPERENGINE_API CounterQueue* createCounterQueue();
        JASPERENGINE_API void releaseCounterQueue(CounterQueue* queue) noexcept;
        // Queues `job` on the counter's own queue and publishes one visit to `lane`, which
        // runs the next job of that queue on whichever thread takes it.
        JASPERENGINE_API void publishCounterJob(JobSystem* system, CounterQueue* queue, Job* job, Lane lane);
    }

    // A fork-join counter for fire-and-wait work, which may live on the stack:
    //
    //     jobs::JobCounter counter;
    //     for (auto& chunk : chunks)
    //         counter.run([&chunk]{ process(chunk); });
    //     counter.wait();   // helps run the counter's own jobs, then parks
    //
    // - add() happens at submit, before the job is published, so a wait() after the submits
    //   on the same thread cannot miss one. Each counted unit is ended by exactly one done()
    //   (run() does it for its job, on every exit path).
    // - The decrement that reaches zero happens under the counter's mutex and wait() takes
    //   that mutex before returning, so the counter may be destroyed as soon as wait()
    //   returns. One thread joins a counter; it must be zero when destroyed.
    // - wait() helps only this counter's jobs, never arbitrary ones: a waiter holding a lock
    //   cannot be re-entered by unrelated work.
    // - A job that throws is logged and sets the sticky hasFailed() flag; reset() re-arms it
    //   once the counter is zero.
    class JASPERENGINE_API JobCounter {
    public:
        // null: the process JobSystem, the one current at each call (a long-lived counter, such
        // as JoltJobAdapter's, follows the instance a new engine run creates)
        explicit JobCounter(JobSystem* system = nullptr);
        ~JobCounter();

        JobCounter(const JobCounter&) = delete;
        JobCounter& operator=(const JobCounter&) = delete;

        void add(uint32_t count = 1);
        void done();

        // Counts and publishes `fn`. Inside a Background body the job is Background whatever
        // `lane` asks.
        template<class F>
        void run(F&& fn, Lane lane = Lane::Normal, const char* name = "job");

        void wait();
        bool isZero() const { return count.load(std::memory_order_acquire) == 0; }
        bool hasFailed() const { return failed.load(std::memory_order_acquire); }
        // Only at quiescence (zero, nobody waiting).
        void reset();

    private:
        void recordFailure(const char* what);
        // Runs the next queued job of this counter on the calling thread; false when none.
        bool helpOne();

        JobSystem* system;
        detail::CounterQueue* queue;
        std::atomic<uint32_t> count{0};
        std::atomic<bool> failed{false};
        std::mutex mutex;
        std::condition_variable cv;
        uint32_t waiters = 0;
    };

    template<class F>
    void JobCounter::run(F&& fn, Lane lane, const char* name){
        add(1);
        JobCounter* counter = this;
        detail::Job* job = nullptr;
        try{
            job = detail::makeJob([counter, f = std::forward<F>(fn)]() mutable {
                struct DoneGuard {
                    JobCounter* counter;
                    ~DoneGuard(){ counter->done(); }
                } guard{counter};
                try{
                    f();
                }catch(const std::exception& e){
                    counter->recordFailure(e.what());
                }catch(...){
                    counter->recordFailure(nullptr);
                }
            }, lane, GateClass::Default, name, 0);
        }catch(...){
            done();
            throw;
        }
        detail::publishCounterJob(system, queue, job, lane);
    }

}}

#endif // JOBS_JOBCOUNTER_H
