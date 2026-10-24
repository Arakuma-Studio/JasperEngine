// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The counter contract is adapted from OpenEngine's JobCounter (Modules/JobSystem/Source/
// JobCounter.cpp), Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT
// License. See NOTICE.md.

#include "jobs/JobSystem.h"
#include "jobs/detail/Scheduler.h"

#include "Log.h"
#include "thread/ThreadUtil.h"
#include "jobs/detail/TimedWait.h"

#include <cassert>
#include <chrono>

using namespace jasperengine;
using namespace jasperengine::jobs;
using namespace jasperengine::jobs::detail;

JobCounter::JobCounter(JobSystem* system):
    system(system ? system : &jobs::system()), queue(createCounterQueue()){
}

JobCounter::~JobCounter(){
    assert(count.load(std::memory_order_acquire) == 0 && "JobCounter destroyed with outstanding jobs: wait() first");
    releaseCounterQueue(queue);
}

void JobCounter::add(uint32_t n){
    count.fetch_add(n, std::memory_order_seq_cst);
}

void JobCounter::done(){
    // under the mutex: the counter usually lives on the waiter's stack, and the waiter may
    // destroy it the moment it sees zero (see the header)
    std::lock_guard<std::mutex> lock(mutex);
    const uint32_t previous = count.fetch_sub(1, std::memory_order_acq_rel);
    assert(previous > 0 && "JobCounter::done without a matching add");
    if (previous == 1 && waiters > 0)
        cv.notify_all();
}

void JobCounter::recordFailure(const char* what){
    failed.store(true, std::memory_order_release);
    if (what)
        Log::error("JobCounter job failed: %s", what);
    else
        Log::error("JobCounter job failed with an exception");
}

void JobCounter::reset(){
    assert(count.load(std::memory_order_acquire) == 0 && "JobCounter::reset with outstanding jobs");
    failed.store(false, std::memory_order_release);
}

bool JobCounter::helpOne(){
    Job* job = nullptr;
    {
        std::lock_guard<std::mutex> lock(queue->mutex);
        job = queue->head;
        if (job){
            queue->head = static_cast<Job*>(job->next);
            if (!queue->head)
                queue->tail = nullptr;
            job->next = nullptr;
        }
    }
    if (!job)
        return false;
    // its visit, still queued in the pool, finds the queue empty and ends
    schedulerOf(system)->runBareJob(job);
    return true;
}

void JobCounter::wait(){
    const ProfilerHooks* h = hooks();
    const bool hooked = h && h->onWaitBegin;
    if (hooked)
        h->onWaitBegin(WaitKind::Counter);
    const bool mayBlock = thread::canBlock();
    for (;;){
        if (count.load(std::memory_order_acquire) == 0)
            break;
        // filtered help: this counter's jobs only
        if (helpOne())
            continue;
        if (!mayBlock){
            spinWait();
            continue;
        }
        std::unique_lock<std::mutex> lock(mutex);
        if (count.load(std::memory_order_acquire) == 0)
            break;
        waiters++;
        // the backstop re-checks the queue: a job counted here may still be queued
        timedWait(cv, lock, std::chrono::milliseconds(1));
        waiters--;
    }
    // orders the last done()'s critical section before the caller may destroy the counter
    { std::lock_guard<std::mutex> lock(mutex); }
    if (hooked){
        h = hooks();
        if (h && h->onWaitEnd)
            h->onWaitEnd();
    }
}
